/* $Id$ */
/** @file
 * IPRT Testcase - Windows thread QoS (power throttling) and semaphore wait precision.
 *
 * Checks that IPRT opts emulation threads and the process hosting them out of
 * Windows power throttling (EcoQoS) and measures how precisely short relative
 * semaphore waits are honoured.
 */

/*
 * Copyright (C) 2026 Oracle and/or its affiliates.
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


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <iprt/win/windows.h>

#include <iprt/semaphore.h>

#include <iprt/env.h>
#include <iprt/errcore.h>
#include <iprt/path.h>
#include <iprt/process.h>
#include <iprt/string.h>
#include <iprt/test.h>
#include <iprt/thread.h>
#include <iprt/time.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** THREAD_INFORMATION_CLASS::ThreadPowerThrottling (W10 1709+). */
#define TST_THREAD_INFO_CLASS_POWER_THROTTLING          3
/** PROCESS_INFORMATION_CLASS::ProcessPowerThrottling (W10 1709+). */
#define TST_PROCESS_INFO_CLASS_POWER_THROTTLING         4
/** THREAD/PROCESS_POWER_THROTTLING_CURRENT_VERSION. */
#define TST_POWER_THROTTLING_CURRENT_VERSION            1
/** THREAD/PROCESS_POWER_THROTTLING_EXECUTION_SPEED. */
#define TST_POWER_THROTTLING_EXECUTION_SPEED            UINT32_C(0x00000001)
/** PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION (W11). */
#define TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION    UINT32_C(0x00000004)
/** The environment variable which disables the IPRT power throttling opt-out. */
#define TST_OPT_OUT_ENV_VAR                             "VBOX_NO_POWER_THROTTLING_OPTOUT"
/** The argument telling us we're the opt-out child process. */
#define TST_CHILD_ARG                                   "--child-opt-out"
/** Command line option enabling the (slow) wait latency and core placement
 * measurements; by default only the deterministic checks run. */
#define TST_BENCHMARK_ARG                               "--benchmark"


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/** Layout of THREAD_POWER_THROTTLING_STATE and PROCESS_POWER_THROTTLING_STATE. */
typedef struct TSTPOWERTHROTTLINGSTATE
{
    ULONG   Version;
    ULONG   ControlMask;
    ULONG   StateMask;
} TSTPOWERTHROTTLINGSTATE;

/** Our own SYSTEM_CPU_SET_INFORMATION subset (CpuSetInformation type only). */
typedef struct TSTCPUSETINFO
{
    DWORD   Size;
    DWORD   Type;
    DWORD   Id;
    WORD    Group;
    BYTE    LogicalProcessorIndex;
    BYTE    CoreIndex;
    BYTE    LastLevelCacheIndex;
    BYTE    NumaNodeIndex;
    BYTE    EfficiencyClass;
    BYTE    AllFlags;
    DWORD   SchedulingClass;
    DWORD64 AllocationTag;
} TSTCPUSETINFO;

/** GetThreadInformation (W8+). */
typedef BOOL (WINAPI *PFNGETTHREADINFORMATION)(HANDLE, int, LPVOID, DWORD);
/** SetProcessInformation (W8+). */
typedef BOOL (WINAPI *PFNSETPROCESSINFORMATION)(HANDLE, int, LPVOID, DWORD);
/** GetProcessInformation (W8+). */
typedef BOOL (WINAPI *PFNGETPROCESSINFORMATION)(HANDLE, int, LPVOID, DWORD);
/** GetSystemCpuSetInformation (W10+). */
typedef BOOL (WINAPI *PFNGETSYSTEMCPUSETINFORMATION)(PVOID, ULONG, PULONG, HANDLE, ULONG);

/** Worker callback for tstRunInThread. */
typedef void FNTSTWORKER(const char *pszWhat);
/** Pointer to a worker callback. */
typedef FNTSTWORKER *PFNTSTWORKER;

/** Arguments for tstThreadProc. */
typedef struct TSTTHREADARGS
{
    PFNTSTWORKER    pfnWorker;
    const char     *pszWhat;
} TSTTHREADARGS;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** The test handle. */
static RTTEST                   g_hTest;
static PFNGETTHREADINFORMATION  g_pfnGetThreadInformation  = NULL;
static PFNSETPROCESSINFORMATION g_pfnSetProcessInformation = NULL;
static PFNGETPROCESSINFORMATION g_pfnGetProcessInformation = NULL;
/** Efficiency class per logical processor, indexed by group * 64 + number. */
static uint8_t                  g_abEfficiencyClass[4 * 64];
/** The highest efficiency class seen (performance cores). */
static uint8_t                  g_bMaxEfficiencyClass = 0;
/** Set if the host has more than one efficiency class (hybrid CPU). */
static bool                     g_fHybrid = false;
/** Set if the measurements should be done (TST_BENCHMARK_ARG). */
static bool                     g_fBenchmark = false;


/**
 * Queries the power throttling state of the calling thread.
 *
 * @returns true on success, false if not supported.
 * @param   pState  Where to return the state.
 */
static bool tstQueryThreadState(TSTPOWERTHROTTLINGSTATE *pState)
{
    RT_ZERO(*pState);
    pState->Version = TST_POWER_THROTTLING_CURRENT_VERSION;
    return g_pfnGetThreadInformation
        && g_pfnGetThreadInformation(GetCurrentThread(), TST_THREAD_INFO_CLASS_POWER_THROTTLING, pState, sizeof(*pState));
}


/**
 * Queries the power throttling state of the current process.
 *
 * @returns true on success, false if not supported.
 * @param   pState  Where to return the state.
 */
static bool tstQueryProcessState(TSTPOWERTHROTTLINGSTATE *pState)
{
    RT_ZERO(*pState);
    pState->Version = TST_POWER_THROTTLING_CURRENT_VERSION;
    return g_pfnGetProcessInformation
        && g_pfnGetProcessInformation(GetCurrentProcess(), TST_PROCESS_INFO_CLASS_POWER_THROTTLING, pState, sizeof(*pState));
}


/**
 * Sets the power throttling state of the current process.
 *
 * Used to simulate the EcoQoS treatment Windows 11 applies to background and
 * minimized processes.
 */
static void tstSetProcessState(ULONG fControlMask, ULONG fStateMask)
{
    TSTPOWERTHROTTLINGSTATE State;
    State.Version     = TST_POWER_THROTTLING_CURRENT_VERSION;
    State.ControlMask = fControlMask;
    State.StateMask   = fStateMask;
    if (!g_pfnSetProcessInformation)
        return;
    if (g_pfnSetProcessInformation(GetCurrentProcess(), TST_PROCESS_INFO_CLASS_POWER_THROTTLING, &State, sizeof(State)))
        return;
    /* Windows 10 doesn't know IGNORE_TIMER_RESOLUTION, retry without it (as sched-win.cpp does). */
    State.ControlMask &= ~TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    State.StateMask   &= ~TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (!g_pfnSetProcessInformation(GetCurrentProcess(), TST_PROCESS_INFO_CLASS_POWER_THROTTLING, &State, sizeof(State)))
        RTTestIPrintf(RTTESTLVL_ALWAYS, "SetProcessInformation(%#x,%#x) failed: %u\n", fControlMask, fStateMask, GetLastError());
}


/**
 * Gathers the efficiency class of each logical processor.
 */
static void tstInitCpuSets(void)
{
    PFNGETSYSTEMCPUSETINFORMATION pfnGetSystemCpuSetInformation
        = (PFNGETSYSTEMCPUSETINFORMATION)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    if (!pfnGetSystemCpuSetInformation)
        return;

    static uint8_t s_abBuf[64 * 1024];
    ULONG          cbRet = 0;
    if (!pfnGetSystemCpuSetInformation(s_abBuf, sizeof(s_abBuf), &cbRet, GetCurrentProcess(), 0))
        return;

    uint8_t bMin = UINT8_MAX;
    for (ULONG off = 0; off + RT_UOFFSETOF(TSTCPUSETINFO, AllFlags) <= cbRet;)
    {
        TSTCPUSETINFO const *pInfo = (TSTCPUSETINFO const *)&s_abBuf[off];
        if (pInfo->Size == 0)
            break;
        if (pInfo->Type == 0 /*CpuSetInformation*/)
        {
            uint32_t const idx = pInfo->Group * 64 + pInfo->LogicalProcessorIndex;
            if (idx < RT_ELEMENTS(g_abEfficiencyClass))
                g_abEfficiencyClass[idx] = pInfo->EfficiencyClass;
            g_bMaxEfficiencyClass = RT_MAX(g_bMaxEfficiencyClass, pInfo->EfficiencyClass);
            bMin                  = RT_MIN(bMin, pInfo->EfficiencyClass);
        }
        off += pInfo->Size;
    }
    g_fHybrid = bMin != UINT8_MAX && bMin != g_bMaxEfficiencyClass;
}


/**
 * Measures how late short relative waits on event semaphores return.
 *
 * @param   pszWhat     The test phase.
 */
static void tstMeasureWaits(const char *pszWhat)
{
    static struct { uint64_t cNsWait; uint32_t cIterations; bool fMulti; } const s_aTests[] =
    {
        {   500000, 100, false },
        {  1000000, 100, false },
        {  2000000,  50, false },
        { 10000000,  20, false },
        {  1000000,  50, true  },
    };

    RTSEMEVENT      hEvt;
    RTTESTI_CHECK_RC_RETV(RTSemEventCreate(&hEvt), VINF_SUCCESS);
    RTSEMEVENTMULTI hEvtMulti;
    int const rcCreate = RTSemEventMultiCreate(&hEvtMulti);
    if (RT_FAILURE(rcCreate))
    {
        RTTestIFailed("RTSemEventMultiCreate -> %Rrc", rcCreate);
        RTSemEventDestroy(hEvt);
        return;
    }

    uint32_t const fFlags = RTSEMWAIT_FLAGS_RELATIVE | RTSEMWAIT_FLAGS_NANOSECS | RTSEMWAIT_FLAGS_NORESUME;
    for (size_t iTest = 0; iTest < RT_ELEMENTS(s_aTests); iTest++)
    {
        uint64_t       acNsLate[128];
        uint32_t const cIterations = RT_MIN(s_aTests[iTest].cIterations, RT_ELEMENTS(acNsLate));
        uint64_t const cNsWait     = s_aTests[iTest].cNsWait;
        uint64_t       cNsTotal    = 0;
        uint32_t       cEarly      = 0;
        for (uint32_t i = 0; i < cIterations; i++)
        {
            uint64_t const nsStart = RTTimeNanoTS();
            int rc;
            if (!s_aTests[iTest].fMulti)
                rc = RTSemEventWaitEx(hEvt, fFlags, cNsWait);
            else
                rc = RTSemEventMultiWaitEx(hEvtMulti, fFlags, cNsWait);
            uint64_t const cNsElapsed = RTTimeNanoTS() - nsStart;
            if (rc != VERR_TIMEOUT)
                RTTestIFailed("%s: wait returned %Rrc, expected VERR_TIMEOUT", pszWhat, rc);
            /* Plain NT waits may time out up to a clock tick early as seen by
               RTTimeNanoTS, so this is only reported (allowing for the 100ns
               rounding). */
            if (cNsElapsed + 100 < cNsWait)
                cEarly++;

            /* Insertion sort the lateness. */
            uint64_t const cNsLate = cNsElapsed > cNsWait ? cNsElapsed - cNsWait : 0;
            uint32_t       j       = i;
            while (j > 0 && acNsLate[j - 1] > cNsLate)
            {
                acNsLate[j] = acNsLate[j - 1];
                j--;
            }
            acNsLate[j] = cNsLate;
            cNsTotal   += cNsLate;
        }

        RTTestIPrintf(RTTESTLVL_ALWAYS,
                      "%-22s %-5s %6RU64 us wait: late by min %7RU64 median %7RU64 avg %7RU64 max %7RU64 ns (n=%u, early %u)\n",
                      pszWhat, s_aTests[iTest].fMulti ? "multi" : "event", cNsWait / RT_NS_1US, acNsLate[0],
                      acNsLate[cIterations / 2], cNsTotal / cIterations, acNsLate[cIterations - 1], cIterations, cEarly);
        RTTestIValueF(acNsLate[cIterations / 2], RTTESTUNIT_NS, "%s %s %RU64us median late",
                      pszWhat, s_aTests[iTest].fMulti ? "multi" : "event", cNsWait / RT_NS_1US);
    }

    RTSemEventMultiDestroy(hEvtMulti);
    RTSemEventDestroy(hEvt);
}


/**
 * Spins for a while and samples which kind of core the thread runs on.
 *
 * @param   pszWhat     The test phase.
 */
static void tstMeasurePlacement(const char *pszWhat)
{
    if (!g_fHybrid)
        return;

    uint64_t const      nsStart   = RTTimeNanoTS();
    uint64_t            cSamples  = 0;
    uint64_t            cOnPerf   = 0;
    uint32_t volatile   uDummy    = 0;
    while (RTTimeNanoTS() - nsStart < 300 * RT_NS_1MS)
    {
        for (uint32_t i = 0; i < 4096; i++)
            uDummy = uDummy * 1664525 + 1013904223;
        PROCESSOR_NUMBER ProcNum;
        GetCurrentProcessorNumberEx(&ProcNum);
        uint32_t const idx = ProcNum.Group * 64 + ProcNum.Number;
        if (idx < RT_ELEMENTS(g_abEfficiencyClass) && g_abEfficiencyClass[idx] == g_bMaxEfficiencyClass)
            cOnPerf++;
        cSamples++;
    }
    uint64_t const cNsElapsed = RTTimeNanoTS() - nsStart;
    RTTestIPrintf(RTTESTLVL_ALWAYS, "%-22s placement: %3u%% of %RU64 samples on P-cores, %RU64 spin blocks/ms\n",
                  pszWhat, (unsigned)(cOnPerf * 100 / RT_MAX(cSamples, 1)), cSamples, cSamples * RT_NS_1MS / cNsElapsed);
    RTTestIValueF(cOnPerf * 100 / RT_MAX(cSamples, 1), RTTESTUNIT_PCT, "%s on P-cores", pszWhat);
}


/**
 * Worker: Measures waits and placement only.
 */
static void tstWorkerMeasure(const char *pszWhat)
{
    TSTPOWERTHROTTLINGSTATE State;
    if (tstQueryThreadState(&State))
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%-22s thread power throttling: control=%#x state=%#x\n",
                      pszWhat, State.ControlMask, State.StateMask);
    if (g_fBenchmark)
    {
        tstMeasureWaits(pszWhat);
        tstMeasurePlacement(pszWhat);
    }
}


/**
 * Worker: Checks that an emulation thread has opted out of EcoQoS and that
 * the process has too, then measures.
 */
static void tstWorkerEmulation(const char *pszWhat)
{
    TSTPOWERTHROTTLINGSTATE State;
    if (tstQueryThreadState(&State))
    {
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%-22s thread power throttling: control=%#x state=%#x\n",
                      pszWhat, State.ControlMask, State.StateMask);
        if (   !(State.ControlMask & TST_POWER_THROTTLING_EXECUTION_SPEED)
            || (State.StateMask & TST_POWER_THROTTLING_EXECUTION_SPEED))
            RTTestIFailed("%s: thread not opted out of EcoQoS: control=%#x state=%#x",
                          pszWhat, State.ControlMask, State.StateMask);
    }
    else
        RTTestIPrintf(RTTESTLVL_ALWAYS, "GetThreadInformation(ThreadPowerThrottling) failed: %u\n", GetLastError());

    if (tstQueryProcessState(&State))
    {
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%-22s process power throttling: control=%#x state=%#x\n",
                      pszWhat, State.ControlMask, State.StateMask);
        if (   !(State.ControlMask & TST_POWER_THROTTLING_EXECUTION_SPEED)
            || (State.StateMask & (TST_POWER_THROTTLING_EXECUTION_SPEED | TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)))
            RTTestIFailed("%s: process not opted out of EcoQoS: control=%#x state=%#x",
                          pszWhat, State.ControlMask, State.StateMask);
    }
    else
        RTTestIPrintf(RTTESTLVL_ALWAYS, "GetProcessInformation(ProcessPowerThrottling) failed: %u\n", GetLastError());

    if (g_fBenchmark)
    {
        tstMeasureWaits(pszWhat);
        tstMeasurePlacement(pszWhat);
    }
}


/**
 * Worker: Checks that a thread type we don't touch has no explicit thread
 * level power throttling state.
 */
static void tstWorkerUntouched(const char *pszWhat)
{
    TSTPOWERTHROTTLINGSTATE State;
    if (tstQueryThreadState(&State))
    {
        RTTestIPrintf(RTTESTLVL_ALWAYS, "%-22s thread power throttling: control=%#x state=%#x\n",
                      pszWhat, State.ControlMask, State.StateMask);
        if (State.ControlMask != 0)
            RTTestIFailed("%s: unexpected thread power throttling control mask %#x", pszWhat, State.ControlMask);
    }
}


/**
 * Worker: Checks that an emulation thread in the opt-out child didn't change
 * anything.
 */
static void tstWorkerChildOptOut(const char *pszWhat)
{
    TSTPOWERTHROTTLINGSTATE State;
    if (tstQueryThreadState(&State) && State.ControlMask != 0)
        RTTestIFailed("%s: thread power throttling changed despite " TST_OPT_OUT_ENV_VAR ": control=%#x state=%#x",
                      pszWhat, State.ControlMask, State.StateMask);
    if (tstQueryProcessState(&State) && State.ControlMask != 0)
        RTTestIFailed("%s: process power throttling changed despite " TST_OPT_OUT_ENV_VAR ": control=%#x state=%#x",
                      pszWhat, State.ControlMask, State.StateMask);
}


/**
 * Thread procedure for tstRunInThread.
 *
 * @returns VINF_SUCCESS.
 * @param   hThreadSelf The thread handle.
 * @param   pvUser      Pointer to the TSTTHREADARGS.
 */
static DECLCALLBACK(int) tstThreadProc(RTTHREAD hThreadSelf, void *pvUser)
{
    TSTTHREADARGS const *pArgs = (TSTTHREADARGS const *)pvUser;
    RT_NOREF(hThreadSelf);
    RTTestSetDefault(g_hTest, NULL); /* The RTTestI* APIs use a per thread default. */
    pArgs->pfnWorker(pArgs->pszWhat);
    return VINF_SUCCESS;
}


/**
 * Runs @a pfnWorker on a new IPRT thread of the given type.
 */
static void tstRunInThread(RTTHREADTYPE enmType, PFNTSTWORKER pfnWorker, const char *pszWhat)
{
    TSTTHREADARGS Args = { pfnWorker, pszWhat };
    RTTHREAD      hThread;
    RTTESTI_CHECK_RC_RETV(RTThreadCreate(&hThread, tstThreadProc, &Args, 0, enmType, RTTHREADFLAGS_WAITABLE, "tstQoS"),
                          VINF_SUCCESS);
    RTTESTI_CHECK_RC(RTThreadWait(hThread, RT_INDEFINITE_WAIT, NULL), VINF_SUCCESS);
}


/**
 * Runs this executable with the opt-out environment variable set.
 */
static void tstSpawnOptOutChild(void)
{
    RTTestISub("Opt-out environment variable");

    char szExe[RTPATH_MAX];
    RTTESTI_CHECK_RETV(RTProcGetExecutablePath(szExe, sizeof(szExe)) != NULL);
    RTENV hEnv;
    RTTESTI_CHECK_RC_RETV(RTEnvClone(&hEnv, RTENV_DEFAULT), VINF_SUCCESS);
    RTTESTI_CHECK_RC(RTEnvSetEx(hEnv, TST_OPT_OUT_ENV_VAR, "1"), VINF_SUCCESS);
    /* The child reports through its exit code, not into a test driver pipe or file of ours. */
    RTTESTI_CHECK(RT_SUCCESS(RTEnvUnsetEx(hEnv, "IPRT_TEST_PIPE")));
    RTTESTI_CHECK(RT_SUCCESS(RTEnvUnsetEx(hEnv, "IPRT_TEST_FILE")));

    const char *apszArgs[] = { szExe, TST_CHILD_ARG, NULL };
    RTPROCESS   hProcess;
    int rc = RTProcCreate(szExe, apszArgs, hEnv, 0 /*fFlags*/, &hProcess);
    RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
    if (RT_SUCCESS(rc))
    {
        RTPROCSTATUS Status;
        Status.iStatus   = -1;
        Status.enmReason = RTPROCEXITREASON_ABEND;
        rc = RTProcWait(hProcess, RTPROCWAIT_FLAGS_BLOCK, &Status);
        RTTESTI_CHECK_RC(rc, VINF_SUCCESS);
        if (RT_SUCCESS(rc) && (Status.enmReason != RTPROCEXITREASON_NORMAL || Status.iStatus != 0))
            RTTestIFailed("Opt-out child failed: enmReason=%d iStatus=%d", Status.enmReason, Status.iStatus);
    }
    RTEnvDestroy(hEnv);
}


int main(int argc, char **argv)
{
    bool const fChild = argc == 2 && !strcmp(argv[1], TST_CHILD_ARG);
    g_fBenchmark      = argc == 2 && !strcmp(argv[1], TST_BENCHMARK_ARG);

    RTTEST hTest;
    RTEXITCODE rcExit = RTTestInitAndCreate(fChild ? "tstRTWinThreadQoS-child" : "tstRTWinThreadQoS", &hTest);
    if (rcExit != RTEXITCODE_SUCCESS)
        return rcExit;
    RTTestBanner(hTest);
    g_hTest = hTest;

    HMODULE const hModKernel32 = GetModuleHandleW(L"kernel32.dll");
    g_pfnGetThreadInformation  = (PFNGETTHREADINFORMATION)GetProcAddress(hModKernel32, "GetThreadInformation");
    g_pfnSetProcessInformation = (PFNSETPROCESSINFORMATION)GetProcAddress(hModKernel32, "SetProcessInformation");
    g_pfnGetProcessInformation = (PFNGETPROCESSINFORMATION)GetProcAddress(hModKernel32, "GetProcessInformation");
    if (!g_pfnGetThreadInformation || !g_pfnSetProcessInformation)
        return RTTestSkipAndDestroy(hTest, "Requires Windows 8 or later");

    if (fChild)
    {
        RTTestSub(hTest, "Child");
        tstRunInThread(RTTHREADTYPE_EMULATION, tstWorkerChildOptOut, "child EMULATION");
        return RTTestSummaryAndDestroy(hTest);
    }

    /* The parent checks the opt-out itself, so make sure it is not disabled. */
    RTEnvUnset(TST_OPT_OUT_ENV_VAR);

    tstInitCpuSets();
    RTTestPrintf(hTest, RTTESTLVL_ALWAYS, "Hybrid CPU: %RTbool (max efficiency class %u)\n", g_fHybrid, g_bMaxEfficiencyClass);

    /*
     * The baseline must be measured before the first emulation thread is
     * created, since that makes the whole process a "VM process".
     */
    if (g_fBenchmark)
    {
        RTTestSub(hTest, "Baseline");
        tstRunInThread(RTTHREADTYPE_DEFAULT, tstWorkerMeasure, "baseline");

        RTTestSub(hTest, "Baseline with forced EcoQoS");
        tstSetProcessState(TST_POWER_THROTTLING_EXECUTION_SPEED | TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                           TST_POWER_THROTTLING_EXECUTION_SPEED | TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
        tstRunInThread(RTTHREADTYPE_DEFAULT, tstWorkerMeasure, "baseline-ecoqos");
        tstSetProcessState(0, 0); /* back to letting the system decide */
    }

    /*
     * Now create an emulation thread.
     */
    RTTestSub(hTest, "Emulation thread");
    tstRunInThread(RTTHREADTYPE_EMULATION, tstWorkerEmulation, "EMULATION");

    RTTestSub(hTest, "Other thread types");
    tstRunInThread(RTTHREADTYPE_DEFAULT, tstWorkerUntouched, "DEFAULT");
    if (g_fBenchmark)
        tstRunInThread(RTTHREADTYPE_DEFAULT, tstWorkerMeasure, "DEFAULT in VM process");

    /*
     * Force EcoQoS on the process again (simulating whatever might do that
     * behind our back) and check that the emulation thread level opt-out
     * still keeps the thread fast.
     */
    if (g_fBenchmark)
    {
        RTTestSub(hTest, "Emulation thread with forced process EcoQoS");
        tstSetProcessState(TST_POWER_THROTTLING_EXECUTION_SPEED | TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                           TST_POWER_THROTTLING_EXECUTION_SPEED | TST_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
        tstRunInThread(RTTHREADTYPE_EMULATION, tstWorkerMeasure, "EMULATION-ecoqos");
        tstRunInThread(RTTHREADTYPE_DEFAULT,   tstWorkerMeasure, "DEFAULT-ecoqos");
        tstSetProcessState(0, 0);
    }

    tstSpawnOptOutChild();

    return RTTestSummaryAndDestroy(hTest);
}

