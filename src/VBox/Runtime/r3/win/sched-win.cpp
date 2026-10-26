/* $Id: sched-win.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * IPRT - Scheduling, Win32.
 */

/*
 * Copyright (C) 2006-2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL), a copy of it is provided in the "COPYING.CDDL" file included
 * in the VirtualBox distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
 */

/** @def WIN32_SCHED_ENABLED
 * Enables the priority scheme. */
#define WIN32_SCHED_ENABLED

/** @page pg_rt_sched_win   IPRT Windows Scheduling, Power Throttling and VM Processes
 *
 * Besides mapping thread types to priorities, this module takes care of
 * Windows power throttling (EcoQoS, W10 1709+).  Windows 11 applies EcoQoS to
 * processes it considers to be in the background (minimized, occluded or
 * windowless, e.g. headless VMs), which means running their threads on
 * efficiency cores at reduced clock speeds.  It may also ignore the timer
 * resolution requests of a process whose windows are all minimized or
 * occluded.  For a process emulating a VM this is quite harmful, as virtual
 * CPUs (EMTs) and timer/IO threads are latency sensitive and a starved vCPU can
 * make a guest panic (e.g. spinlock timeouts).
 *
 * To avoid affecting every process using IPRT (services, tools, guest
 * additions), nothing is changed until the process creates its first
 * RTTHREADTYPE_EMULATION thread, which only VM processes do (EMTs).  At that
 * point rtSchedWinInitVmProcess:
 *      - opts the whole process out of EcoQoS and out of timer resolution
 *        throttling (SetProcessInformation / ProcessPowerThrottling);
 *      - enables thread level EcoQoS opt-out for RTTHREADTYPE_EMULATION,
 *        RTTHREADTYPE_TIMER and RTTHREADTYPE_IO threads (SetThreadInformation /
 *        ThreadPowerThrottling), which also holds should something later put
 *        the process into EcoQoS (e.g. task manager's efficiency mode).
 *
 * RTTHREADTYPE_TIMER and RTTHREADTYPE_IO threads that got their type before the
 * first emulation thread was created don't get the thread level opt-out, only
 * the process level one.  This only matters if something later forces EcoQoS
 * on the process.
 *
 * Setting the VBOX_NO_POWER_THROTTLING_OPTOUT environment variable to anything
 * but "0" disables all of this, leaving it to Windows to trade VM performance
 * for battery life.
 *
 * There is intentionally no CPU set (P-core) affinity: with the EcoQoS opt-out
 * the Windows scheduler already prefers performance cores for these threads,
 * and restricting vCPUs to a subset of the cores increases the risk of starving
 * them when the host is busy.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP RTLOGGROUP_THREAD
#include <iprt/win/windows.h>

#include <iprt/thread.h>
#include <iprt/asm.h>
#include <iprt/env.h>
#include <iprt/log.h>
#include <iprt/assert.h>
#include <iprt/errcore.h>
#include <iprt/string.h>
#include "internal/sched.h"
#include "internal/thread.h"
#include "internal-r3-win.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @name Power throttling (EcoQoS) definitions.
 * Own copies since older SDKs lack them.  The information classes and the
 * execution speed control are W10 1709+, the timer resolution one is W11+.
 * @{ */
/** THREAD_INFORMATION_CLASS::ThreadPowerThrottling. */
#define RTSCHEDWIN_THREAD_POWER_THROTTLING              3
/** PROCESS_INFORMATION_CLASS::ProcessPowerThrottling. */
#define RTSCHEDWIN_PROCESS_POWER_THROTTLING             4
/** THREAD_POWER_THROTTLING_CURRENT_VERSION and PROCESS_POWER_THROTTLING_CURRENT_VERSION. */
#define RTSCHEDWIN_POWER_THROTTLING_VERSION             1
/** THREAD_POWER_THROTTLING_EXECUTION_SPEED and PROCESS_POWER_THROTTLING_EXECUTION_SPEED. */
#define RTSCHEDWIN_POWER_THROTTLING_EXECUTION_SPEED     UINT32_C(0x00000001)
/** PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION. */
#define RTSCHEDWIN_POWER_THROTTLING_IGNORE_TIMER_RES    UINT32_C(0x00000004)
/** @} */

/** Environment variable which, when set to anything but "0", disables the
 * VM process power throttling opt-out. */
#define RTSCHEDWIN_ENV_NO_POWER_THROTTLING_OPTOUT       "VBOX_NO_POWER_THROTTLING_OPTOUT"

/** @name VM process initialization states (g_uVmProcessState).
 * @{ */
#define RTSCHEDWIN_VMPROC_STATE_UNINIT                  UINT32_C(0)
#define RTSCHEDWIN_VMPROC_STATE_BUSY                    UINT32_C(1)
#define RTSCHEDWIN_VMPROC_STATE_DONE                    UINT32_C(2)
/** @} */


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Configuration of one priority.
 */
typedef struct
{
    /** The priority. */
    RTPROCPRIORITY  enmPriority;
    /** The name of this priority. */
    const char     *pszName;
    /** The Win32 process priority class. If ANY_PROCESS_PRIORITY_CLASS the
     * process priority class is left unchanged. */
    DWORD dwProcessPriorityClass;
    /** Array scheduler attributes corresponding to each of the thread types. */
    struct
    {
        /** For sanity include the array index. */
        RTTHREADTYPE    enmType;
        /** The Win32 thread priority. */
        int             iThreadPriority;
    } aTypes[RTTHREADTYPE_END];
} PROCPRIORITY;

/** Matches any process priority class. */
#define ANY_PROCESS_PRIORITY_CLASS  (~0U)

/**
 * THREAD_POWER_THROTTLING_STATE and PROCESS_POWER_THROTTLING_STATE.
 */
typedef struct RTSCHEDWINPOWERTHROTTLING
{
    /** RTSCHEDWIN_POWER_THROTTLING_VERSION. */
    ULONG   Version;
    /** Which policies are explicitly controlled; the others are up to Windows. */
    ULONG   ControlMask;
    /** The state of the controlled policies, a clear bit means not throttled. */
    ULONG   StateMask;
} RTSCHEDWINPOWERTHROTTLING;

/** SetThreadInformation and SetProcessInformation (W8+); the information class
 * enum is passed as an int. */
typedef BOOL (WINAPI *PFNRTSCHEDWINSETINFORMATION)(HANDLE, int, LPVOID, DWORD);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/**
 * Array of static priority configurations.
 */
static const PROCPRIORITY g_aPriorities[] =
{
    {
        RTPROCPRIORITY_FLAT, "Flat", ANY_PROCESS_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_NORMAL }
        }
    },
    {
        RTPROCPRIORITY_LOW, "Low - Below Normal", BELOW_NORMAL_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    },
    {
        RTPROCPRIORITY_LOW, "Low", ANY_PROCESS_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_NORMAL }
        }
    },
    {
        RTPROCPRIORITY_NORMAL, "Normal - Normal", NORMAL_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    },
    {
        RTPROCPRIORITY_NORMAL, "Normal", ANY_PROCESS_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    },
    {
        RTPROCPRIORITY_HIGH, "High - High", HIGH_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    },
    {
        RTPROCPRIORITY_HIGH, "High - Above Normal", ABOVE_NORMAL_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_LOWEST },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    },
    {
        RTPROCPRIORITY_HIGH, "High", ANY_PROCESS_PRIORITY_CLASS,
        {
            { RTTHREADTYPE_INVALID,                 ~0 },
            { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_BELOW_NORMAL },
            { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_NORMAL },
            { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_ABOVE_NORMAL },
            { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_IO,                      THREAD_PRIORITY_HIGHEST },
            { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
        }
    }
};

/**
 * The dynamic default priority configuration.
 *
 * This can be recalulated at runtime depending on what the
 * system allow us to do. Presently we don't do this as it's
 * generally not a bit issue on Win32 hosts.
 */
static PROCPRIORITY g_aDefaultPriority =
{
    RTPROCPRIORITY_LOW, "Default", ANY_PROCESS_PRIORITY_CLASS,
    {
        { RTTHREADTYPE_INVALID,                 ~0 },
        { RTTHREADTYPE_INFREQUENT_POLLER,       THREAD_PRIORITY_LOWEST },
        { RTTHREADTYPE_MAIN_HEAVY_WORKER,       THREAD_PRIORITY_BELOW_NORMAL },
        { RTTHREADTYPE_EMULATION,               THREAD_PRIORITY_NORMAL },
        { RTTHREADTYPE_DEFAULT,                 THREAD_PRIORITY_NORMAL },
        { RTTHREADTYPE_GUI,                     THREAD_PRIORITY_NORMAL },
        { RTTHREADTYPE_MAIN_WORKER,             THREAD_PRIORITY_NORMAL },
        { RTTHREADTYPE_VRDP_IO,                 THREAD_PRIORITY_NORMAL },
        { RTTHREADTYPE_DEBUGGER,                THREAD_PRIORITY_ABOVE_NORMAL },
        { RTTHREADTYPE_MSG_PUMP,                THREAD_PRIORITY_ABOVE_NORMAL },
        { RTTHREADTYPE_IO,                      THREAD_PRIORITY_HIGHEST },
        { RTTHREADTYPE_TIMER,                   THREAD_PRIORITY_HIGHEST }
    }
};


/** Pointer to the current priority configuration. */
static const PROCPRIORITY *g_pProcessPriority = &g_aDefaultPriority;

/** The VM process initialization state (RTSCHEDWIN_VMPROC_STATE_XXX), see
 * rtSchedWinInitVmProcess. */
static uint32_t volatile                        g_uVmProcessState = RTSCHEDWIN_VMPROC_STATE_UNINIT;
/** SetThreadInformation, only set once the VM process has opted out of power
 * throttling and thread level opt-outs should be done. */
static PFNRTSCHEDWINSETINFORMATION volatile     g_pfnSetThreadInformation = NULL;


/**
 * Calculate the scheduling properties for all the threads in the default
 * process priority, assuming the current thread have the type enmType.
 *
 * @returns iprt status code.
 * @param   enmType     The thread type to be assumed for the current thread.
 */
DECLHIDDEN(int) rtSchedNativeCalcDefaultPriority(RTTHREADTYPE enmType)
{
    Assert(enmType > RTTHREADTYPE_INVALID && enmType < RTTHREADTYPE_END); RT_NOREF_PV(enmType);
    return VINF_SUCCESS;
}


DECLHIDDEN(int) rtProcNativeSetPriority(RTPROCPRIORITY enmPriority)
{
    Assert(enmPriority > RTPROCPRIORITY_INVALID && enmPriority < RTPROCPRIORITY_LAST); RT_NOREF_PV(enmPriority);

    if (enmPriority == RTPROCPRIORITY_DEFAULT)
    {
        g_pProcessPriority = &g_aDefaultPriority;
        return VINF_SUCCESS;
    }

    for (size_t i = 0; i < RT_ELEMENTS(g_aPriorities); i++)
        if (   g_aPriorities[i].enmPriority == enmPriority
            && g_aPriorities[i].dwProcessPriorityClass == ANY_PROCESS_PRIORITY_CLASS)
        {
            g_pProcessPriority = &g_aPriorities[i];
            return VINF_SUCCESS;
        }

    AssertFailedReturn(VERR_INTERNAL_ERROR);
}


/**
 * Worker for rtSchedWinInitVmProcess that does the actual work.
 */
static void rtSchedWinInitVmProcessWorker(void)
{
    /*
     * Allow laptop users to keep the power saving behaviour.
     */
    char szValue[16];
    int rc = RTEnvGetEx(RTENV_DEFAULT, RTSCHEDWIN_ENV_NO_POWER_THROTTLING_OPTOUT, szValue, sizeof(szValue), NULL);
    if (   rc == VERR_BUFFER_OVERFLOW
        || (RT_SUCCESS(rc) && strcmp(szValue, "0") != 0))
    {
        LogRel(("IPRT: %s is set, leaving power throttling to Windows.\n",
                RTSCHEDWIN_ENV_NO_POWER_THROTTLING_OPTOUT));
        return;
    }

    /*
     * Opt the process out of EcoQoS and out of having its timer resolution
     * requests ignored while minimized or occluded.  The latter is W11+ and
     * makes the call fail on older systems, so retry without it.
     */
    PFNRTSCHEDWINSETINFORMATION pfnSetProcessInformation
        = (PFNRTSCHEDWINSETINFORMATION)GetProcAddress(g_hModKernel32, "SetProcessInformation");
    if (pfnSetProcessInformation)
    {
        RTSCHEDWINPOWERTHROTTLING State;
        State.Version     = RTSCHEDWIN_POWER_THROTTLING_VERSION;
        State.ControlMask = RTSCHEDWIN_POWER_THROTTLING_EXECUTION_SPEED | RTSCHEDWIN_POWER_THROTTLING_IGNORE_TIMER_RES;
        State.StateMask   = 0;
        BOOL fRc = pfnSetProcessInformation(GetCurrentProcess(), RTSCHEDWIN_PROCESS_POWER_THROTTLING, &State, sizeof(State));
        if (!fRc)
        {
            State.ControlMask = RTSCHEDWIN_POWER_THROTTLING_EXECUTION_SPEED;
            fRc = pfnSetProcessInformation(GetCurrentProcess(), RTSCHEDWIN_PROCESS_POWER_THROTTLING, &State, sizeof(State));
        }
        if (fRc)
            LogRel(("IPRT: Opted the VM process out of power throttling (control mask %#x).\n", State.ControlMask));
        else
            LogRel(("IPRT: Failed to opt the VM process out of power throttling: %u\n", GetLastError()));
    }

    /*
     * Enable the thread level opt-out, see rtSchedWinUpdatePowerThrottling.
     * (Thread power throttling is W10 1709+, failures are ignored there.)
     */
    g_pfnSetThreadInformation = (PFNRTSCHEDWINSETINFORMATION)GetProcAddress(g_hModKernel32, "SetThreadInformation");
}


/**
 * Called when an emulation thread is created to turn this into a VM process.
 *
 * See @ref pg_rt_sched_win for details.  Only the first call does anything,
 * concurrent callers wait for it to complete.
 *
 * @note    May be called with the thread tree lock held (RTThreadSetType), so
 *          the worker must not need it.
 */
static void rtSchedWinInitVmProcess(void)
{
    if (ASMAtomicReadU32(&g_uVmProcessState) == RTSCHEDWIN_VMPROC_STATE_DONE)
        return;
    if (ASMAtomicCmpXchgU32(&g_uVmProcessState, RTSCHEDWIN_VMPROC_STATE_BUSY, RTSCHEDWIN_VMPROC_STATE_UNINIT))
    {
        rtSchedWinInitVmProcessWorker();
        ASMAtomicWriteU32(&g_uVmProcessState, RTSCHEDWIN_VMPROC_STATE_DONE);
    }
    else
        /* Sleep(1) rather than Sleep(0): the latter only yields to threads of
           equal or higher priority and could starve a lower priority initializer. */
        while (ASMAtomicReadU32(&g_uVmProcessState) != RTSCHEDWIN_VMPROC_STATE_DONE)
            Sleep(1);
}


/**
 * Checks if threads of the given type are latency sensitive in a VM process and
 * should be opted out of power throttling at the thread level.
 *
 * @returns true if they should, false if not.
 * @param   enmType     The thread type.
 */
DECLINLINE(bool) rtSchedWinIsLatencySensitiveType(RTTHREADTYPE enmType)
{
    return enmType == RTTHREADTYPE_EMULATION
        || enmType == RTTHREADTYPE_TIMER
        || enmType == RTTHREADTYPE_IO;
}


/**
 * Updates the thread level power throttling (EcoQoS) state when a thread gets
 * its (new) type.
 *
 * Latency sensitive thread types are opted out in VM processes, while threads
 * changing to other types are handed back to the system.
 *
 * @param   hThread     The thread handle.
 * @param   enmOldType  The current type of the thread, same as @a enmNewType
 *                      when called for a new thread.
 * @param   enmNewType  The new thread type.
 */
static void rtSchedWinUpdatePowerThrottling(HANDLE hThread, RTTHREADTYPE enmOldType, RTTHREADTYPE enmNewType)
{
    if (enmNewType == RTTHREADTYPE_EMULATION)
        rtSchedWinInitVmProcess();

    PFNRTSCHEDWINSETINFORMATION const pfnSetThreadInformation = g_pfnSetThreadInformation;
    if (pfnSetThreadInformation)
    {
        bool const fOptOut = rtSchedWinIsLatencySensitiveType(enmNewType);
        if (fOptOut || rtSchedWinIsLatencySensitiveType(enmOldType))
        {
            RTSCHEDWINPOWERTHROTTLING State;
            State.Version     = RTSCHEDWIN_POWER_THROTTLING_VERSION;
            State.ControlMask = fOptOut ? RTSCHEDWIN_POWER_THROTTLING_EXECUTION_SPEED : 0;
            State.StateMask   = 0;
            if (!pfnSetThreadInformation(hThread, RTSCHEDWIN_THREAD_POWER_THROTTLING, &State, sizeof(State)))
                Log(("rtSchedWinUpdatePowerThrottling: SetThreadInformation(%p, ThreadPowerThrottling, %#x) failed: %u\n",
                     hThread, State.ControlMask, GetLastError()));
        }
    }
}


/**
 * Gets the win32 thread handle.
 *
 * @returns Valid win32 handle for the specified thread.
 * @param   pThread     The thread.
 */
DECLINLINE(HANDLE) rtThreadNativeGetHandle(PRTTHREADINT pThread)
{
    if ((uintptr_t)pThread->Core.Key == GetCurrentThreadId())
        return GetCurrentThread();
    return (HANDLE)pThread->hThread;
}


DECLHIDDEN(int) rtThreadNativeSetPriority(PRTTHREADINT pThread, RTTHREADTYPE enmType)
{
    Assert(enmType > RTTHREADTYPE_INVALID && enmType < RTTHREADTYPE_END);
    AssertMsg(g_pProcessPriority && g_pProcessPriority->aTypes[enmType].enmType == enmType,
              ("enmType=%d entry=%d\n", enmType, g_pProcessPriority->aTypes[enmType].enmType));

#ifdef WIN32_SCHED_ENABLED
    HANDLE hThread = rtThreadNativeGetHandle(pThread);
    if (hThread == NULL) /* No handle for alien threads. */
        return VINF_SUCCESS;
    if (SetThreadPriority(hThread, g_pProcessPriority->aTypes[enmType].iThreadPriority))
    {
        rtSchedWinUpdatePowerThrottling(hThread, pThread->enmType, enmType);
        return VINF_SUCCESS;
    }

    DWORD dwLastError = GetLastError();
    int rc = RTErrConvertFromWin32(dwLastError);
    AssertMsgFailed(("SetThreadPriority(%p, %d) failed, dwLastError=%d rc=%Rrc\n",
                     rtThreadNativeGetHandle(pThread), g_pProcessPriority->aTypes[enmType].iThreadPriority, dwLastError, rc));
    return rc;
#else
    return VINF_SUCCESS;
#endif
}

