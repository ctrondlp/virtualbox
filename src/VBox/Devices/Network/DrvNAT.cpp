/* $Id: DrvNAT.cpp 114578 2026-07-01 08:34:03Z alexander.eichner@oracle.com $ */
/** @file
 * DrvNATlibslirp - NATlibslirp network transport driver.
 */

/*
 * Copyright (C) 2022-2026 Oracle and/or its affiliates.
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
 * SPDX-License-Identifier: GPL-3.0-only
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_NAT
#define RTNET_INCL_IN_ADDR
#include "VBoxDD.h"

#ifdef RT_OS_WINDOWS
# include <iprt/win/winsock2.h>
# include <iprt/win/ws2tcpip.h>
# include "winutils.h"
# define inet_aton(x, y) inet_pton(2, x, y)
# define AF_INET6 23
#endif

#include <slirp/libslirp.h>

#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/pdmdrv.h>
#include <VBox/vmm/pdmnetifs.h>
#include <VBox/vmm/pdmnetinline.h>

#ifndef RT_OS_WINDOWS
# include <unistd.h>
# include <fcntl.h>
# include <poll.h>
# include <errno.h>
# include <netdb.h>
#endif

#ifdef RT_OS_FREEBSD
# include <netinet/in.h>
#endif

#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/cidr.h>
#include <iprt/ctype.h>
#include <iprt/file.h>
#include <limits.h>
#include <iprt/mem.h>
#include <iprt/net.h>
#include <iprt/pipe.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/time.h>
#include <iprt/uuid.h>
#include <iprt/vector.h>

#include <iprt/asm.h>

#include <iprt/semaphore.h>
#include <iprt/req.h>
#ifdef RT_OS_DARWIN
# include <SystemConfiguration/SystemConfiguration.h>
# include <CoreFoundation/CoreFoundation.h>
#endif

#define COUNTERS_INIT
#include "slirp/counters.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
#define DRVNAT_MAXFRAMESIZE (16 * 1024)
/** The maximum (default) poll/WSAPoll timeout. */
#define DRVNAT_DEFAULT_TIMEOUT (int)RT_MS_1HOUR
#define MAX_IP_ADDRESS_STR_LEN_W_NULL 16
#define BOOTP_FILE_MAX_LEN 127

#define IPV4_MAX_MTU 65521
#define IPV4_MIN_MTU 68
/** Number of recent diagnostic events kept for the debugger info item. */
#define DRVNAT_DIAG_EVENT_COUNT 64
/** Maximum length of one recent diagnostic event. */
#define DRVNAT_DIAG_EVENT_MSG_MAX 160
/** Number of port-forwarding rules tracked for diagnostic output. */
#define DRVNAT_DIAG_PF_COUNT 64

#ifdef DEBUG
# define DRVNAT_DIAG_COUNTER_INC(a_pCounter)      ASMAtomicIncU64(a_pCounter)
# define DRVNAT_DIAG_COUNTER_ADD(a_pCounter, a_u) ASMAtomicAddU64(a_pCounter, (uint64_t)(a_u))
#else
/** NAT diagnostic counters are debug-build only. */
# define DRVNAT_DIAG_COUNTER_INC(a_pCounter)      do { } while (0)
# define DRVNAT_DIAG_COUNTER_ADD(a_pCounter, a_u) do { } while (0)
#endif


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/** Slirp Timer */
typedef struct slirpTimer
{
    /**
     * Pointer to next timer in linked list.
     * @note: This currently is not in use. See note in struct DRVNAT.
     */
    struct slirpTimer *next;
    /** The time deadline (milliseconds, RTTimeMilliTS).   */
    int64_t msExpire;
    /** Callback to be called on timer expiry. Supplied by libslirp. */
    SlirpTimerCb pHandler;
    /** Opaque object passed to callback. */
    void *opaque;
} SlirpTimer;

/** A recent NAT diagnostic event. */
typedef struct DRVNATDIAGEVENT
{
    /** Event timestamp, from RTTimeMilliTS. */
    uint64_t msTimestamp;
    /** Human-readable event text. */
    char     szMsg[DRVNAT_DIAG_EVENT_MSG_MAX];
} DRVNATDIAGEVENT;

/** Port-forwarding rule as applied to libslirp, for diagnostic output. */
typedef struct DRVNATDIAGPF
{
    /** Whether this entry is in use. */
    bool           fInUse;
    /** Whether this is UDP, otherwise TCP. */
    bool           fUdp;
    /** Host bind address. */
    struct in_addr HostIp;
    /** Host bind port. */
    uint16_t       u16HostPort;
    /** Guest target address. */
    struct in_addr GuestIp;
    /** Guest target port. */
    uint16_t       u16GuestPort;
} DRVNATDIAGPF;

#ifdef DEBUG
/** NAT diagnostic counters for debugger info output. */
typedef struct DRVNATDIAGCOUNTERS
{
    /** Frames submitted by the guest to libslirp. */
    volatile uint64_t cGuestToNatFrames;
    /** Bytes submitted by the guest to libslirp. */
    volatile uint64_t cbGuestToNat;
    /** Frames queued by libslirp for delivery to the guest. */
    volatile uint64_t cNatToGuestFrames;
    /** Bytes queued by libslirp for delivery to the guest. */
    volatile uint64_t cbNatToGuest;
    /** Frames dropped because the link was down. */
    volatile uint64_t cDropLinkDown;
    /** Packets/frames dropped because a NAT/RX thread was not running. */
    volatile uint64_t cDropNatThreadDown;
    /** Oversized guest frames rejected before reaching libslirp. */
    volatile uint64_t cDropOversized;
    /** Guest GSO frames rejected because the GSO descriptor was invalid. */
    volatile uint64_t cDropInvalidGso;
    /** Queueing failures on the NAT or RX request queues. */
    volatile uint64_t cQueueFailures;
    /** Successful wakeups sent to the NAT thread. */
    volatile uint64_t cWakeupsSent;
    /** Failed wakeups sent to the NAT thread. */
    volatile uint64_t cWakeupFailures;
    /** Successful port-forward additions. */
    volatile uint64_t cPortForwardAddSuccesses;
    /** Failed port-forward additions. */
    volatile uint64_t cPortForwardAddFailures;
    /** Successful port-forward removals. */
    volatile uint64_t cPortForwardRemoveSuccesses;
    /** Failed port-forward removals. */
    volatile uint64_t cPortForwardRemoveFailures;
    /** DNS update notifications received. */
    volatile uint64_t cDnsUpdates;
    /** IPv4 DNS fallbacks to the libslirp proxy. */
    volatile uint64_t cDnsIpv4Fallbacks;
    /** IPv6 DNS fallbacks to the libslirp proxy. */
    volatile uint64_t cDnsIpv6Fallbacks;
    /** Libslirp guest-error callbacks. */
    volatile uint64_t cGuestErrors;
} DRVNATDIAGCOUNTERS;
#endif

/** NAT diagnostic state for debugger info output. */
typedef struct DRVNATDIAG
{
    /** Configured IPv4 network address. */
    RTNETADDRIPV4          Ipv4Network;
    /** Configured IPv4 network mask. */
    RTNETADDRIPV4          Ipv4Netmask;
    /** Virtual host/gateway IPv4 address. */
    struct in_addr         Ipv4Host;
    /** DHCP start IPv4 address. */
    struct in_addr         Ipv4DhcpStart;
    /** Virtual IPv4 nameserver address. */
    struct in_addr         Ipv4NameServer;
    /** Configured IPv6 prefix. */
    struct in6_addr        Ipv6Prefix;
    /** Configured virtual IPv6 host/gateway address. */
    struct in6_addr        Ipv6Host;
    /** Configured virtual IPv6 nameserver address. */
    struct in6_addr        Ipv6NameServer;
    /** Configured IPv6 prefix length. */
    uint8_t                bIpv6PrefixLen;
    /** Configured interface MTU. */
    uint32_t               u32Mtu;
    /** Configured interface MRU. */
    uint32_t               u32Mru;
    /** Whether localhost is reachable from the guest. */
    bool                   fLocalhostReachable;
    /** Whether broadcast forwarding is enabled. */
    bool                   fForwardBroadcast;
    /** Whether TFTP was enabled. */
    bool                   fTftpEnabled;
    /** Whether an outbound bind address was configured. */
    bool                   fBindIp;
    /** Configured outbound bind IPv4 address. */
    RTNETADDRIPV4          BindIp;
    /** Configured listen backlog. */
    int                    iSoMaxConn;
#ifdef DEBUG
    /** Diagnostic counters, debug builds only to avoid hot-path overhead. */
    DRVNATDIAGCOUNTERS     Counters;
#endif
    /** Last DNS update timestamp, from RTTimeMilliTS. */
    uint64_t               msDnsLastUpdate;
    /** Last DNS domain name. */
    char                   szDnsDomainName[256];
    /** Last DNS update search-domain count. */
    uint32_t               cDnsSearchDomains;
    /** Last DNS update configured IPv4 nameserver count. */
    uint32_t               cDnsIpv4NameServersConfigured;
    /** Last DNS update accepted IPv4 nameserver count. */
    uint32_t               cDnsIpv4NameServersAccepted;
    /** Whether the last IPv4 DNS update fell back to the libslirp proxy. */
    bool                   fDnsIpv4Fallback;
    /** Last DNS update configured IPv6 nameserver count. */
    uint32_t               cDnsIpv6NameServersConfigured;
    /** Last DNS update accepted IPv6 nameserver count. */
    uint32_t               cDnsIpv6NameServersAccepted;
    /** Whether the last IPv6 DNS update fell back to the libslirp proxy. */
    bool                   fDnsIpv6Fallback;
    /** Active port-forwarding rules. */
    DRVNATDIAGPF           aPf[DRVNAT_DIAG_PF_COUNT];
    /** Recent event ring. */
    DRVNATDIAGEVENT        aEvents[DRVNAT_DIAG_EVENT_COUNT];
    /** Next event slot. */
    volatile uint32_t      iEvent;
} DRVNATDIAG;

/**
 * NAT network transport driver instance data.
 *
 * @implements  PDMINETWORKUP
 */
typedef struct DRVNAT
{
    /** The network interface. */
    PDMINETWORKUP           INetworkUp;
    /** The network NAT Engine configuration. */
    PDMINETWORKNATCONFIG    INetworkNATCfg;
    /** The port we're attached to. */
    PPDMINETWORKDOWN        pIAboveNet;
    /** The network config of the port we're attached to. */
    PPDMINETWORKCONFIG      pIAboveConfig;
    /** Pointer to the driver instance. */
    PPDMDRVINS              pDrvIns;
    /** Link state */
    PDMNETWORKLINKSTATE     enmLinkState;
    /** tftp server name to provide in the DHCP server response. */
    char                   *pszNextServer;
    /** Polling thread. */
    PPDMTHREAD              pSlirpThread;
    /** Queue for NAT-thread-external events. */
    RTREQQUEUE              hSlirpReqQueue;
    /** The guest IP for port-forwarding. */
    uint32_t                GuestIP;
    /** Link state set when the VM is suspended. */
    PDMNETWORKLINKSTATE     enmLinkStateWant;

#ifdef RT_OS_WINDOWS
    /** Wakeup socket pair for NAT thread.
     * Entry #0 is write, entry #1 is read. */
    SOCKET                  ahWakeupSockPair[2];
#else
    /** The write end of the control pipe. */
    RTPIPE                  hPipeWrite;
    /** The read end of the control pipe. */
    RTPIPE                  hPipeRead;
#endif
    /** count of unconsumed bytes sent to notify NAT thread */
    volatile uint64_t       cbWakeupNotifs;

#define DRV_PROFILE_COUNTER(name, dsc)     STAMPROFILE Stat ## name
#define DRV_COUNTING_COUNTER(name, dsc)    STAMCOUNTER Stat ## name
#include "slirp/counters.h"

    /** thread delivering packets for receiving by the guest */
    PPDMTHREAD              pRecvThread;
    /** event to wakeup the guest receive thread */
    RTSEMEVENT              hEventRecv;
    /** Receive Req queue (deliver packets to the guest) */
    RTREQQUEUE              hRecvReqQueue;
    /** makes access to device func RecvAvail and Recv atomical. */
    RTCRITSECT              DevAccessLock;
    /** Number of in-flight packets. */
    volatile uint32_t       cPkts;
    /** Transmit lock taken by BeginXmit and released by EndXmit. */
    RTCRITSECT              XmitLock;

    /**  Pointer to Slirp NAT engine instance */
    Slirp *pSlirp;
    /** Count of open socket connections as of last poll fill */
    unsigned int cSockets;
    /**
     * A cSockets length list of file descriptors to poll, filled by slirp
     *
     * @note: Array is allocated to fit uPollCap number of pollfd structs.
     * It is only populated with cSockets number of pollfd structs.
     */
    struct pollfd *aPolls;
    /** Cap of the number of file descriptors to poll, can be increased when needed */
    unsigned int uPollCap = 0;
    /** List of timers (in reverse creation order).
     * @note There is currently only one libslirp timer (v4.8 / 2025-01-16).  */
    SlirpTimer *pTimerHead;
    /** Flag from Main API that determines if we pass search domain via DHCP */
    bool fPassDomain;
    /** Whether guest TCP data may be queued instead of being written to the host socket at once when more frames
     * are known to follow (the DeferHostWrites config value). */
    bool fDeferHostWrites;
    /** Number of guest frames handed to the NAT thread that it hasn't picked up yet. */
    volatile uint32_t cSendsPending;
    /** Diagnostic state for debugger info output. */
    DRVNATDIAG Diag;
} DRVNAT;
AssertCompileMemberAlignment(DRVNAT, StatNATRecvWakeups, 8);
/** Pointer to the NAT driver instance data. */
typedef DRVNAT *PDRVNAT;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static void drvNATNotifyNATThread(PDRVNAT pThis, const char *pszWho);
static void drvNATDiagEvent(PDRVNAT pThis, const char *pszFormat, ...) RT_IPRT_FORMAT_ATTR(2, 3);
static int  drvNATTimersAdjustTimeoutDown(PDRVNAT pThis, int cMsTimeout);
static void drvNATTimersRunExpired(PDRVNAT pThis);
static DECLCALLBACK(int) drvNAT_AddPollCb(slirp_os_socket hFd, int iEvents, void *opaque);
static int64_t drvNAT_ClockGetNsCb(void *opaque);
static DECLCALLBACK(int) drvNAT_GetREventsCb(int idx, void *opaque);
static DECLCALLBACK(int) drvNATNotifyApplyPortForwardCommand(PDRVNAT pThis, bool fRemove, bool fUdp, const char *pszHostIp,
                                                             uint16_t u16HostPort, const char *pszGuestIp, uint16_t u16GuestPort);


/**
 * Adds an entry to the recent diagnostic event ring.
 *
 * @param   pThis       The NAT instance.
 * @param   pszFormat   Format string.
 * @param   ...         Format arguments.
 *
 * @thread  any
 */
static void drvNATDiagEvent(PDRVNAT pThis, const char *pszFormat, ...)
{
    if (!pThis)
        return;

    uint32_t const iEvent = ASMAtomicIncU32(&pThis->Diag.iEvent) - 1;
    DRVNATDIAGEVENT * const pEvent = &pThis->Diag.aEvents[iEvent % DRVNAT_DIAG_EVENT_COUNT];
    pEvent->msTimestamp = RTTimeMilliTS();

    va_list va;
    va_start(va, pszFormat);
    RTStrPrintfV(pEvent->szMsg, sizeof(pEvent->szMsg), pszFormat, va);
    va_end(va);
}


/**
 * Checks whether the DBGF info argument contains the given section name.
 *
 * @returns true if present, false if not.
 * @param   pszArgs     Info argument string, optional.
 * @param   pszSection  Section name to look for.
 */
static bool drvNATInfoHasSection(const char *pszArgs, const char *pszSection)
{
    if (!pszArgs || !*pszArgs)
        return false;

    size_t const cchSection = strlen(pszSection);
    const char *pszCur = pszArgs;
    while (*pszCur)
    {
        while (RT_C_IS_SPACE(*pszCur) || *pszCur == ',' || *pszCur == ';' || *pszCur == '|')
            pszCur++;

        const char * const pszStart = pszCur;
        while (*pszCur && !RT_C_IS_SPACE(*pszCur) && *pszCur != ',' && *pszCur != ';' && *pszCur != '|')
            pszCur++;

        if (   (size_t)(pszCur - pszStart) == cchSection
            && RTStrNICmpAscii(pszStart, pszSection, cchSection) == 0)
            return true;
    }

    return false;
}


/**
 * Checks whether a DBGF info argument string is empty or only contains separators.
 *
 * @returns true if empty, false if not.
 * @param   pszArgs     Info argument string, optional.
 */
static bool drvNATInfoArgsAreEmpty(const char *pszArgs)
{
    if (!pszArgs)
        return true;

    while (*pszArgs)
    {
        if (!RT_C_IS_SPACE(*pszArgs) && *pszArgs != ',' && *pszArgs != ';' && *pszArgs != '|')
            return false;
        pszArgs++;
    }

    return true;
}


/**
 * Converts link state to a stable diagnostic string.
 *
 * @returns Link-state string.
 * @param   enmLinkState    The link state.
 */
static const char *drvNATLinkStateName(PDMNETWORKLINKSTATE enmLinkState)
{
    switch (enmLinkState)
    {
        case PDMNETWORKLINKSTATE_UP:          return "up";
        case PDMNETWORKLINKSTATE_DOWN:        return "down";
        case PDMNETWORKLINKSTATE_DOWN_RESUME: return "down-resume";
        case PDMNETWORKLINKSTATE_INVALID:     return "invalid";
        default:                              return "unknown";
    }
}


/**
 * Updates the diagnostic copy of active port-forwarding rules.
 *
 * @param   pThis           The NAT instance.
 * @param   fRemove         Whether a rule is being removed.
 * @param   fUdp            Whether this is UDP, otherwise TCP.
 * @param   HostIp          Host bind address.
 * @param   u16HostPort     Host bind port.
 * @param   GuestIp         Guest target address.
 * @param   u16GuestPort    Guest target port.
 */
static void drvNATDiagPortForwardUpdate(PDRVNAT pThis, bool fRemove, bool fUdp, struct in_addr HostIp, uint16_t u16HostPort,
                                        struct in_addr GuestIp, uint16_t u16GuestPort)
{
    if (fRemove)
    {
        for (uint32_t i = 0; i < RT_ELEMENTS(pThis->Diag.aPf); i++)
            if (   pThis->Diag.aPf[i].fInUse
                && pThis->Diag.aPf[i].fUdp == fUdp
                && pThis->Diag.aPf[i].HostIp.s_addr == HostIp.s_addr
                && pThis->Diag.aPf[i].u16HostPort == u16HostPort)
            {
                pThis->Diag.aPf[i].fInUse = false;
                return;
            }
        return;
    }

    for (uint32_t i = 0; i < RT_ELEMENTS(pThis->Diag.aPf); i++)
        if (   pThis->Diag.aPf[i].fInUse
            && pThis->Diag.aPf[i].fUdp == fUdp
            && pThis->Diag.aPf[i].HostIp.s_addr == HostIp.s_addr
            && pThis->Diag.aPf[i].u16HostPort == u16HostPort)
        {
            pThis->Diag.aPf[i].GuestIp      = GuestIp;
            pThis->Diag.aPf[i].u16GuestPort = u16GuestPort;
            return;
        }

    for (uint32_t i = 0; i < RT_ELEMENTS(pThis->Diag.aPf); i++)
        if (!pThis->Diag.aPf[i].fInUse)
        {
            pThis->Diag.aPf[i].fInUse       = true;
            pThis->Diag.aPf[i].fUdp         = fUdp;
            pThis->Diag.aPf[i].HostIp       = HostIp;
            pThis->Diag.aPf[i].u16HostPort  = u16HostPort;
            pThis->Diag.aPf[i].GuestIp      = GuestIp;
            pThis->Diag.aPf[i].u16GuestPort = u16GuestPort;
            return;
        }

    drvNATDiagEvent(pThis, "port-forward diagnostic table full; newest rule not tracked");
}


/*
 * PDM Function Implementations
 */

/**
 * @callback_method_impl{FNPDMTHREADDRV}
 *
 * Queues guest process received packet. Triggered by drvNATRecvWakeup.
 */
static DECLCALLBACK(int) drvNATRecv(PPDMDRVINS pDrvIns, PPDMTHREAD pThread)
{
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        RTReqQueueProcess(pThis->hRecvReqQueue, 0);
        if (ASMAtomicReadU32(&pThis->cPkts) == 0)
            RTSemEventWait(pThis->hEventRecv, RT_INDEFINITE_WAIT);
    }
    return VINF_SUCCESS;
}

/**
 * @callback_method_impl{FNPDMTHREADWAKEUPDRV}
 */
static DECLCALLBACK(int) drvNATRecvWakeup(PPDMDRVINS pDrvIns, PPDMTHREAD pThread)
{
    RT_NOREF(pThread);
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);
    int rc = RTSemEventSignal(pThis->hEventRecv);

    STAM_COUNTER_INC(&pThis->StatNATRecvWakeups);
    return rc;
}

/**
 * @brief Processes incoming packet (to guest).
 *
 * @param   pThis   Pointer to DRVNAT state for current context.
 * @param   pBuf    Pointer to packet buffer.
 * @param   cb      Size of packet in buffer.
 *
 * @thread  NATRX
 */
static DECLCALLBACK(void) drvNATRecvWorker(PDRVNAT pThis, void *pBuf, size_t cb)
{
    int rc;
    STAM_PROFILE_START(&pThis->StatNATRecv, a);

    rc = RTCritSectEnter(&pThis->DevAccessLock);
    AssertRC(rc);

    STAM_PROFILE_START(&pThis->StatNATRecvWait, b);
    rc = pThis->pIAboveNet->pfnWaitReceiveAvail(pThis->pIAboveNet, RT_INDEFINITE_WAIT);
    STAM_PROFILE_STOP(&pThis->StatNATRecvWait, b);

    if (RT_SUCCESS(rc))
    {
        rc = pThis->pIAboveNet->pfnReceive(pThis->pIAboveNet, pBuf, cb);
        AssertRC(rc);
        RTMemFree(pBuf);
        pBuf = NULL;
    }
    else if (   rc != VERR_TIMEOUT
             && rc != VERR_INTERRUPTED)
    {
        AssertRC(rc);
    }

    rc = RTCritSectLeave(&pThis->DevAccessLock);
    AssertRC(rc);
    ASMAtomicDecU32(&pThis->cPkts);
    STAM_PROFILE_STOP(&pThis->StatNATRecv, a);
}

/**
 * Frees a S/G buffer allocated by drvNATNetworkUp_AllocBuf.
 *
 * @param   pThis               Pointer to the NAT instance.
 * @param   pSgBuf              The S/G buffer to free.
 *
 * @thread  NAT
 */
static void drvNATFreeSgBuf(PDRVNAT pThis, PPDMSCATTERGATHER pSgBuf)
{
    RT_NOREF(pThis);
    Assert((pSgBuf->fFlags & PDMSCATTERGATHER_FLAGS_MAGIC_MASK) == PDMSCATTERGATHER_FLAGS_MAGIC);
    pSgBuf->fFlags = 0;
    if (pSgBuf->pvAllocator)
    {
        Assert(!pSgBuf->pvUser);
        RTMemFree(pSgBuf->aSegs[0].pvSeg);
    }
    else if (pSgBuf->pvUser)
    {
        RTMemFree(pSgBuf->aSegs[0].pvSeg);
        pSgBuf->aSegs[0].pvSeg = NULL;
        RTMemFree(pSgBuf->pvUser);
        pSgBuf->pvUser = NULL;
    }
    RTMemFree(pSgBuf);
}

/**
 * Worker function for drvNATSend().
 *
 * @param   pThis               Pointer to the NAT instance.
 * @param   pSgBuf              The scatter/gather buffer.
 *
 * @thread  NAT
 */
static DECLCALLBACK(void) drvNATSendWorker(PDRVNAT pThis, PPDMSCATTERGATHER pSgBuf)
{
    LogFlowFunc(("pThis=%p pSgBuf=%p\n", pThis, pSgBuf));

    /* Frames queued behind this one: while there are some (or this is a GSO frame that is carved into several
       segments) it is cheaper to let libslirp write the queued guest data in one go than to write each segment. */
    bool const fMoreToCome = ASMAtomicDecU32(&pThis->cSendsPending) > 0;

    if (pThis->enmLinkState != PDMNETWORKLINKSTATE_UP)
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropLinkDown);
        drvNATFreeSgBuf(pThis, pSgBuf);
        LogFlowFuncLeave();
        return;
    }

    if (pSgBuf->pvAllocator)
    {
        /*
         * A normal frame.
         */
        LogFlowFunc(("Normal Frame -> pvAllocator=%p LB %#zx\n", pSgBuf->pvAllocator, pSgBuf->cbUsed));
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cGuestToNatFrames);
        DRVNAT_DIAG_COUNTER_ADD(&pThis->Diag.Counters.cbGuestToNat, (uint64_t)pSgBuf->cbUsed);
        slirp_set_defer_host_writes(pThis->pSlirp, pThis->fDeferHostWrites && fMoreToCome);
        slirp_input(pThis->pSlirp, (uint8_t const *)pSgBuf->pvAllocator, (int)pSgBuf->cbUsed);
        drvNATFreeSgBuf(pThis, pSgBuf);
        LogFlowFuncLeave();
        return;
    }

    /*
     * Do segmentation offloading.
     * Do not attempt to segment frames with invalid GSO parameters.
     */
    PCPDMNETWORKGSO pGso = (PCPDMNETWORKGSO)pSgBuf->pvUser;
    if (PDMNetGsoIsValid(pGso, sizeof(*pGso), pSgBuf->cbUsed))
    {
        uint32_t const cSegs = PDMNetGsoCalcSegmentCount(pGso, pSgBuf->cbUsed);
        Assert(cSegs > 1);
        uint8_t pbSeg[DRVNAT_MAXFRAMESIZE];
        uint8_t const * const pbFrame = (uint8_t const *)pSgBuf->aSegs[0].pvSeg;
        slirp_set_defer_host_writes(pThis->pSlirp, pThis->fDeferHostWrites);
        LogFlowFunc(("GSO %p LB %#zx - creating %u segments out of it\n", pbFrame, pSgBuf->cbUsed, cSegs));
        for (uint32_t iSeg = 0; iSeg < cSegs; iSeg++)
        {
            uint32_t cbPayload, cbHdrs;
            uint32_t offPayload = PDMNetGsoCarveSegment(pGso, pbFrame, pSgBuf->cbUsed,
                                                        iSeg, cSegs, pbSeg, &cbHdrs, &cbPayload);
            Assert(cbHdrs > 0);
            Assert(cbHdrs < DRVNAT_MAXFRAMESIZE);
            Assert(cbPayload > 0);
            Assert(cbPayload < DRVNAT_MAXFRAMESIZE);
            AssertBreak((uint64_t)cbHdrs + cbPayload <= DRVNAT_MAXFRAMESIZE);

            memcpy(&pbSeg[cbHdrs], &pbFrame[offPayload], cbPayload);

            size_t const cbSeg = cbPayload + cbHdrs;
            DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cGuestToNatFrames);
            DRVNAT_DIAG_COUNTER_ADD(&pThis->Diag.Counters.cbGuestToNat, (uint64_t)cbSeg);
            slirp_input(pThis->pSlirp, pbSeg, (int)cbSeg);
        }
    }
    else
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropInvalidGso);

    drvNATFreeSgBuf(pThis, pSgBuf);
    LogFlowFuncLeave();
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnBeginXmit}
 */
static DECLCALLBACK(int) drvNATNetworkUp_BeginXmit(PPDMINETWORKUP pInterface, bool fOnWorkerThread)
{
    RT_NOREF(fOnWorkerThread);
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);
    int rc = RTCritSectTryEnter(&pThis->XmitLock);
    if (RT_FAILURE(rc))
    {
        rc = VERR_TRY_AGAIN;
        drvNATNotifyNATThread(pThis, "drvNATNetworkUp_BeginXmit");
    }
    LogFlowFunc(("Beginning xmit...\n"));
    return rc;
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnAllocBuf}
 */
static DECLCALLBACK(int) drvNATNetworkUp_AllocBuf(PPDMINETWORKUP pInterface, size_t cbMin,
                                                  PCPDMNETWORKGSO pGso, PPPDMSCATTERGATHER ppSgBuf)
{
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);
    Assert(RTCritSectIsOwner(&pThis->XmitLock));

    LogFlowFuncEnter();

    /*
     * Drop the incoming frame if the NAT thread isn't running.
     */
    if (pThis->pSlirpThread->enmState != PDMTHREADSTATE_RUNNING)
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropNatThreadDown);
        Log(("drvNATNetowrkUp_AllocBuf: returns VERR_NET_DOWN\n"));
        return VERR_NET_DOWN;
    }

    /*
     * Allocate a scatter/gather buffer and an mbuf.
     */
    PPDMSCATTERGATHER pSgBuf = (PPDMSCATTERGATHER)RTMemAllocZ(sizeof(PDMSCATTERGATHER));
    if (!pSgBuf)
        return VERR_NO_MEMORY;
    if (!pGso)
    {
        /*
         * Drop the frame if it is too big.
         */
        if (cbMin >= DRVNAT_MAXFRAMESIZE)
        {
            DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropOversized);
            Log(("drvNATNetowrkUp_AllocBuf: drops over-sized frame (%u bytes), returns VERR_INVALID_PARAMETER\n",
                 cbMin));
            RTMemFree(pSgBuf);
            return VERR_INVALID_PARAMETER;
        }

        pSgBuf->pvUser      = NULL;
        pSgBuf->aSegs[0].cbSeg = RT_ALIGN_Z(cbMin, 128);
        pSgBuf->aSegs[0].pvSeg = RTMemAlloc(pSgBuf->aSegs[0].cbSeg);
        pSgBuf->pvAllocator = pSgBuf->aSegs[0].pvSeg;

        if (!pSgBuf->pvAllocator)
        {
            RTMemFree(pSgBuf);
            return VERR_TRY_AGAIN;
        }
    }
    else
    {
        /*
         * Drop the frame if its segment is too big.
         */
        if (pGso->cbHdrsTotal + pGso->cbMaxSeg >= DRVNAT_MAXFRAMESIZE)
        {
            DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropOversized);
            Log(("drvNATNetowrkUp_AllocBuf: drops over-sized frame (%u bytes), returns VERR_INVALID_PARAMETER\n",
                 pGso->cbHdrsTotal + pGso->cbMaxSeg));
            RTMemFree(pSgBuf);
            return VERR_INVALID_PARAMETER;
        }

        pSgBuf->pvUser      = RTMemDup(pGso, sizeof(*pGso));
        pSgBuf->pvAllocator = NULL;

        pSgBuf->aSegs[0].cbSeg = RT_ALIGN_Z(cbMin, 128);
        pSgBuf->aSegs[0].pvSeg = RTMemAlloc(pSgBuf->aSegs[0].cbSeg);
        if (!pSgBuf->pvUser || !pSgBuf->aSegs[0].pvSeg)
        {
            RTMemFree(pSgBuf->aSegs[0].pvSeg);
            RTMemFree(pSgBuf->pvUser);
            RTMemFree(pSgBuf);
            return VERR_TRY_AGAIN;
        }
    }

    /*
     * Initialize the S/G buffer and return.
     */
    pSgBuf->fFlags      = PDMSCATTERGATHER_FLAGS_MAGIC | PDMSCATTERGATHER_FLAGS_OWNER_1;
    pSgBuf->cbUsed      = 0;
    pSgBuf->cbAvailable = pSgBuf->aSegs[0].cbSeg;
    pSgBuf->cSegs       = 1;

    *ppSgBuf = pSgBuf;
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnFreeBuf}
 */
static DECLCALLBACK(int) drvNATNetworkUp_FreeBuf(PPDMINETWORKUP pInterface, PPDMSCATTERGATHER pSgBuf)
{
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);
    Assert(RTCritSectIsOwner(&pThis->XmitLock));
    drvNATFreeSgBuf(pThis, pSgBuf);
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnSendBuf}
 */
static DECLCALLBACK(int) drvNATNetworkUp_SendBuf(PPDMINETWORKUP pInterface, PPDMSCATTERGATHER pSgBuf, bool fOnWorkerThread)
{
    RT_NOREF(fOnWorkerThread);
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);
    Assert((pSgBuf->fFlags & PDMSCATTERGATHER_FLAGS_OWNER_MASK) == PDMSCATTERGATHER_FLAGS_OWNER_1);
    Assert(RTCritSectIsOwner(&pThis->XmitLock));

    LogFlowFunc(("enter\n"));

    int rc;
    if (pThis->pSlirpThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        ASMAtomicIncU32(&pThis->cSendsPending);
        rc = RTReqQueueCallEx(pThis->hSlirpReqQueue, NULL /*ppReq*/, 0 /*cMillies*/, RTREQFLAGS_VOID | RTREQFLAGS_NO_WAIT,
                              (PFNRT)drvNATSendWorker, 2, pThis, pSgBuf);
        if (RT_SUCCESS(rc))
        {
            drvNATNotifyNATThread(pThis, "drvNATNetworkUp_SendBuf");
            LogFlowFunc(("leave success\n"));
            return VINF_SUCCESS;
        }
        ASMAtomicDecU32(&pThis->cSendsPending);

        rc = VERR_NET_NO_BUFFER_SPACE;
    }
    else
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropNatThreadDown);
        rc = VERR_NET_DOWN;
    }
    if (rc == VERR_NET_NO_BUFFER_SPACE)
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cQueueFailures);
    drvNATFreeSgBuf(pThis, pSgBuf);
    LogFlowFunc(("leave rc=%Rrc\n", rc));
    return rc;
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnEndXmit}
 */
static DECLCALLBACK(void) drvNATNetworkUp_EndXmit(PPDMINETWORKUP pInterface)
{
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);
    RTCritSectLeave(&pThis->XmitLock);
}

/**
 * Get the NAT thread out of poll/WSAWaitForMultipleEvents
 */
static void drvNATNotifyNATThread(PDRVNAT pThis, const char *pszWho)
{
#ifndef LOG_ENABLED
    RT_NOREF(pszWho);
#endif
    Log3(("Notifying NAT Thread. Culprit: %s\n", pszWho));
#ifdef RT_OS_WINDOWS
    int cbWritten = send(pThis->ahWakeupSockPair[0], "", 1, NULL);
    if (RT_LIKELY(cbWritten != SOCKET_ERROR))
    {
        /* Count how many bites we send down the socket */
        ASMAtomicIncU64(&pThis->cbWakeupNotifs);
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cWakeupsSent);
    }
    else
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cWakeupFailures);
        Log4(("Notify NAT Thread Error %d\n", WSAGetLastError()));
    }
#else
    /* kick poll() */
    size_t cbIgnored;
    int rc = RTPipeWrite(pThis->hPipeWrite, "", 1, &cbIgnored);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        /* Count how many bites we send down the socket */
        ASMAtomicIncU64(&pThis->cbWakeupNotifs);
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cWakeupsSent);
    }
    else
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cWakeupFailures);
#endif
}

/**
 * @interface_method_impl{PDMINETWORKUP,pfnSetPromiscuousMode}
 */
static DECLCALLBACK(void) drvNATNetworkUp_SetPromiscuousMode(PPDMINETWORKUP pInterface, bool fPromiscuous)
{
    RT_NOREF(pInterface, fPromiscuous);
    LogFlow(("drvNATNetworkUp_SetPromiscuousMode: fPromiscuous=%d\n", fPromiscuous));
    /* nothing to do */
}

/**
 * Worker function for drvNATNetworkUp_NotifyLinkChanged().
 * @thread "NAT" thread.
 *
 * @param   pThis           Pointer to DRVNAT state for current context.
 * @param   enmLinkState    Enum value of link state.
 *
 * @thread  NAT
 */
static DECLCALLBACK(void) drvNATNotifyLinkChangedWorker(PDRVNAT pThis, PDMNETWORKLINKSTATE enmLinkState)
{
    pThis->enmLinkState = pThis->enmLinkStateWant = enmLinkState;
    switch (enmLinkState)
    {
        case PDMNETWORKLINKSTATE_UP:
            LogRel(("NAT: Link up\n"));
            drvNATDiagEvent(pThis, "link up");
            break;

        case PDMNETWORKLINKSTATE_DOWN:
        case PDMNETWORKLINKSTATE_DOWN_RESUME:
            LogRel(("NAT: Link down\n"));
            drvNATDiagEvent(pThis, "link down (%s)", drvNATLinkStateName(enmLinkState));
            break;

        default:
            AssertMsgFailed(("drvNATNetworkUp_NotifyLinkChanged: unexpected link state %d\n", enmLinkState));
    }
}

/**
 * Notification on link status changes.
 *
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   enmLinkState    The new link state.
 *
 * @thread  EMT
 */
static DECLCALLBACK(void) drvNATNetworkUp_NotifyLinkChanged(PPDMINETWORKUP pInterface, PDMNETWORKLINKSTATE enmLinkState)
{
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkUp);

    LogFlow(("drvNATNetworkUp_NotifyLinkChanged: enmLinkState=%d\n", enmLinkState));

    /* Don't queue new requests if the NAT thread is not running (e.g. paused,
     * stopping), otherwise we would deadlock. Memorize the change. */
    if (pThis->pSlirpThread->enmState != PDMTHREADSTATE_RUNNING)
    {
        pThis->enmLinkStateWant = enmLinkState;
        return;
    }

    PRTREQ pReq;
    int rc = RTReqQueueCallEx(pThis->hSlirpReqQueue, &pReq, 0 /*cMillies*/, RTREQFLAGS_VOID,
                              (PFNRT)drvNATNotifyLinkChangedWorker, 2, pThis, enmLinkState);
    if (rc == VERR_TIMEOUT)
    {
        drvNATNotifyNATThread(pThis, "drvNATNetworkUp_NotifyLinkChanged");
        rc = RTReqWait(pReq, RT_INDEFINITE_WAIT);
        AssertRC(rc);
    }
    else
        AssertRC(rc);
    RTReqRelease(pReq);
}

/**
 * NAT thread handling the slirp stuff.
 *
 * The slirp implementation is single-threaded so we execute this enginre in a
 * dedicated thread. We take care that this thread does not become the
 * bottleneck: If the guest wants to send, a request is enqueued into the
 * hSlirpReqQueue and handled asynchronously by this thread. If this thread
 * wants to deliver packets to the guest, it enqueues a request into
 * hRecvReqQueue which is later handled by the Recv thread.
 *
 * @param   pDrvIns     Pointer to PDM driver context.
 * @param   pThread     Pointer to calling thread context.
 *
 * @returns VBox status code
 *
 * @thread  NAT
 */
static DECLCALLBACK(int) drvNATAsyncIoThread(PPDMDRVINS pDrvIns, PPDMTHREAD pThread)
{
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    /* The first polling entry is for the control/wakeup pipe. */
#ifdef RT_OS_WINDOWS
    drvNAT_AddPollCb(pThis->ahWakeupSockPair[1], SLIRP_POLL_IN | SLIRP_POLL_HUP, pThis);
#else
    RTHCINTPTR const i64NativeReadPipe = RTPipeToNative(pThis->hPipeRead);
    int const        fdNativeReadPipe  = (int)i64NativeReadPipe;
    Assert(fdNativeReadPipe == i64NativeReadPipe);
    Assert(fdNativeReadPipe >= 0);
    drvNAT_AddPollCb(fdNativeReadPipe,
                     SLIRP_POLL_IN | SLIRP_POLL_PRI | SLIRP_POLL_HUP, pThis);
#endif /* !RT_OS_WINDOWS */

    LogFlowFunc(("pThis=%p\n", pThis));

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    if (pThis->enmLinkStateWant != pThis->enmLinkState)
        drvNATNotifyLinkChangedWorker(pThis, pThis->enmLinkStateWant);

    /*
     * Polling loop.
     */
    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        /*
         * To prevent concurrent execution of sending/receiving threads
         */
        pThis->cSockets = 1;

        uint32_t cMsTimeout = DRVNAT_DEFAULT_TIMEOUT;
        slirp_pollfds_fill_socket(pThis->pSlirp, &cMsTimeout, drvNAT_AddPollCb /* SlirpAddPollCb */, pThis /* opaque */);
        cMsTimeout = drvNATTimersAdjustTimeoutDown(pThis, cMsTimeout);
        Log4Func(("Timeout adjust to: %d\n", cMsTimeout));

#ifdef RT_OS_WINDOWS
        int cChangedFDs = WSAPoll(pThis->aPolls, pThis->cSockets, cMsTimeout);
#else
        int cChangedFDs = poll(pThis->aPolls, pThis->cSockets, cMsTimeout);
#endif
        if (RT_LIKELY(!(cChangedFDs >= 0)))
        {
            Log4Func(("Poll error\n"));
#ifdef RT_OS_WINDOWS
            int const iLastErr = WSAGetLastError(); /* (In debug builds LogRel translates to two RTLogLoggerExWeak calls.) */
            LogRel(("NAT: RTWinPoll returned error=%Rrc (cChangedFDs=%d)\n", iLastErr, cChangedFDs));
            Log4(("NAT: cSockets = %d\n", pThis->cSockets));
#else
            if (errno == EINTR)
            {
                Log2(("NAT: signal was caught while sleep on poll\n"));
                /* No error, just process all outstanding requests but don't wait */
                cChangedFDs = 0;
            }
#endif
        }

        Log4Func(("poll\n"));
        slirp_pollfds_poll(pThis->pSlirp, cChangedFDs < 0, drvNAT_GetREventsCb, pThis /* opaque */);
        Log4Func(("management pipe revents = %d\n", pThis->aPolls[0].revents));

        /*
         * Drain the control pipe if necessary.
         *
         * Note! drvNATSend is decoupled. We use a control pipe to track how
         *       many times DrvNAT has been notified that there are packets to
         *       process. Every time we find that there are, we will drain the
         *       control pipe of all the notifications. If there is an error
         *       on reading the pipe, we try again next time around.
         */
        if (pThis->aPolls[0].revents & (POLLIN|POLLRDNORM|POLLPRI|POLLRDBAND))   /* POLLPRI won't be seen with WSAPoll. */
        {
            Log4(("Draining control pipe.\n"));
            char achBuf[1024];
            size_t cbRead = 0;
            uint64_t cbWakeupNotifs = ASMAtomicReadU64(&pThis->cbWakeupNotifs);
            int rc = VINF_SUCCESS;
#ifdef RT_OS_WINDOWS
            cbRead = recv(pThis->ahWakeupSockPair[1], &achBuf[0], RT_MIN(cbWakeupNotifs, sizeof(achBuf)), NULL);
            int iError = WSAGetLastError();
            if(RT_LIKELY(!(cbRead != SOCKET_ERROR)))
            {
                LogRelFunc(("Wakeup socket read error in poll loop: %d\n", iError));
                rc = VERR_PIPE_IO_ERROR;
            }
#else
            rc = RTPipeRead(pThis->hPipeRead, &achBuf[0], RT_MIN(cbWakeupNotifs, sizeof(achBuf)), &cbRead);
            if (RT_FAILURE(rc))
                LogRelFunc(("Wakup socket read error in poll loop (%Rrc)\n", rc));
#endif
            if(RT_SUCCESS(rc))
                ASMAtomicSubU64(&pThis->cbWakeupNotifs, cbRead);
        }

        /* process _all_ outstanding requests but don't wait */
        RTReqQueueProcess(pThis->hSlirpReqQueue, 0);
        drvNATTimersRunExpired(pThis);
    }

    LogFlowFunc(("Exiting poll loop...\n"));

    return VINF_SUCCESS;
}

/**
 * Unblock the send thread so it can respond to a state change.
 *
 * @returns VBox status code.
 * @param   pDrvIns     The pcnet device instance.
 * @param   pThread     The send thread.
 *
 * @thread  NAT
 */
static DECLCALLBACK(int) drvNATAsyncIoWakeup(PPDMDRVINS pDrvIns, PPDMTHREAD pThread)
{
    LogFlowFuncEnter();
    RT_NOREF(pThread);
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    drvNATNotifyNATThread(pThis, "drvNATAsyncIoWakeup");
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvNATQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS  pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVNAT     pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMINETWORKUP, &pThis->INetworkUp);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMINETWORKNATCONFIG, &pThis->INetworkNATCfg);
    return NULL;
}

/**
 * Prints NAT debugger-info help.
 *
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoHelp(PCDBGFINFOHLP pHlp)
{
    pHlp->pfnPrintf(pHlp,
                    "NAT info arguments:\n"
                    "  help           Show this help.\n"
                    "  summary   | s  Show configuration and thread/link summary.\n"
                    "  flows     | f  Show libslirp TCP/UDP/ICMP connection state.\n"
                    "  flowsr    | fr Show libslirp TCP/UDP/ICMP connection state with destination reverse DNS.\n"
                    "  neighbors | n  Show libslirp ARP/NDP neighbor state.\n"
                    "  pf             Show VirtualBox-tracked active port-forwarding rules.\n"
                    "  counters  | c  Show diagnostic counters (debug builds only).\n"
                    "  dns            Show DNS update/proxy state.\n"
                    "  poll      | p  Show poll/wakeup state.\n"
                    "  timers    | t  Show libslirp timer state.\n"
                    "  events    | e  Show recent NAT diagnostic events.\n"
                    "  all            Show all diagnostic sections.\n");
}


/**
 * Prints NAT summary information.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoSummary(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    char szIpv6Prefix[INET6_ADDRSTRLEN] = "";
    char szIpv6Host[INET6_ADDRSTRLEN] = "";
    char szIpv6NameServer[INET6_ADDRSTRLEN] = "";
    inet_ntop(AF_INET6, &pThis->Diag.Ipv6Prefix, szIpv6Prefix, sizeof(szIpv6Prefix));
    inet_ntop(AF_INET6, &pThis->Diag.Ipv6Host, szIpv6Host, sizeof(szIpv6Host));
    inet_ntop(AF_INET6, &pThis->Diag.Ipv6NameServer, szIpv6NameServer, sizeof(szIpv6NameServer));

    pHlp->pfnPrintf(pHlp,
                    "NAT summary:\n"
                    "  libslirp:             %s\n"
                    "  link-state:           %s (wanted %s)\n"
                    "  NAT thread state:     %d\n"
                    "  RX thread state:      %d\n"
                    "  in-flight RX packets: %u\n"
                    "  IPv4 network:         %RTnaipv4/%RTnaipv4\n"
                    "  IPv4 host/gateway:    %RTnaipv4\n"
                    "  IPv4 DHCP start:      %RTnaipv4\n"
                    "  IPv4 DNS proxy:       %RTnaipv4\n"
                    "  IPv6 prefix:          %s/%u\n"
                    "  IPv6 host/gateway:    %s\n"
                    "  IPv6 DNS proxy:       %s\n"
                    "  MTU/MRU:              %u/%u\n"
                    "  localhost reachable:  %RTbool\n"
                    "  forward broadcast:    %RTbool\n"
                    "  pass domain:          %RTbool\n"
                    "  TFTP enabled:         %RTbool\n"
                    "  outbound bind IPv4:   %s%RTnaipv4\n"
                    "  SO max connection:    %d\n",
                    slirp_version_string(),
                    drvNATLinkStateName(pThis->enmLinkState), drvNATLinkStateName(pThis->enmLinkStateWant),
                    pThis->pSlirpThread ? pThis->pSlirpThread->enmState : -1,
                    pThis->pRecvThread ? pThis->pRecvThread->enmState : -1,
                    ASMAtomicReadU32(&pThis->cPkts),
                    pThis->Diag.Ipv4Network.u, pThis->Diag.Ipv4Netmask.u,
                    pThis->Diag.Ipv4Host.s_addr, pThis->Diag.Ipv4DhcpStart.s_addr, pThis->Diag.Ipv4NameServer.s_addr,
                    szIpv6Prefix[0] ? szIpv6Prefix : "<unknown>", pThis->Diag.bIpv6PrefixLen,
                    szIpv6Host[0] ? szIpv6Host : "<unknown>", szIpv6NameServer[0] ? szIpv6NameServer : "<unknown>",
                    pThis->Diag.u32Mtu, pThis->Diag.u32Mru,
                    pThis->Diag.fLocalhostReachable, pThis->Diag.fForwardBroadcast, pThis->fPassDomain, pThis->Diag.fTftpEnabled,
                    pThis->Diag.fBindIp ? "" : "<not configured> ", pThis->Diag.BindIp.u,
                    pThis->Diag.iSoMaxConn);
}


/**
 * Prints debug-build-only NAT diagnostic counters.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoCounters(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
#ifdef DEBUG
    DRVNATDIAGCOUNTERS * const pCtr = &pThis->Diag.Counters;
    pHlp->pfnPrintf(pHlp,
                    "NAT diagnostic counters:\n"
                    "  guest -> NAT:             %RU64 frames, %RU64 bytes\n"
                    "  NAT -> guest:             %RU64 frames, %RU64 bytes\n"
                    "  drops, link down:         %RU64\n"
                    "  drops, thread down:       %RU64\n"
                    "  drops, oversized:         %RU64\n"
                    "  drops, invalid GSO:       %RU64\n"
                    "  queue failures:           %RU64\n"
                    "  NAT-thread wakeups:       %RU64 sent, %RU64 failed\n"
                    "  port-forward add:         %RU64 ok, %RU64 failed\n"
                    "  port-forward remove:      %RU64 ok, %RU64 failed\n"
                    "  DNS updates:              %RU64\n"
                    "  DNS fallback:             %RU64 IPv4, %RU64 IPv6\n"
                    "  libslirp guest errors:    %RU64\n",
                    ASMAtomicReadU64(&pCtr->cGuestToNatFrames), ASMAtomicReadU64(&pCtr->cbGuestToNat),
                    ASMAtomicReadU64(&pCtr->cNatToGuestFrames), ASMAtomicReadU64(&pCtr->cbNatToGuest),
                    ASMAtomicReadU64(&pCtr->cDropLinkDown), ASMAtomicReadU64(&pCtr->cDropNatThreadDown),
                    ASMAtomicReadU64(&pCtr->cDropOversized), ASMAtomicReadU64(&pCtr->cDropInvalidGso),
                    ASMAtomicReadU64(&pCtr->cQueueFailures),
                    ASMAtomicReadU64(&pCtr->cWakeupsSent), ASMAtomicReadU64(&pCtr->cWakeupFailures),
                    ASMAtomicReadU64(&pCtr->cPortForwardAddSuccesses), ASMAtomicReadU64(&pCtr->cPortForwardAddFailures),
                    ASMAtomicReadU64(&pCtr->cPortForwardRemoveSuccesses), ASMAtomicReadU64(&pCtr->cPortForwardRemoveFailures),
                    ASMAtomicReadU64(&pCtr->cDnsUpdates),
                    ASMAtomicReadU64(&pCtr->cDnsIpv4Fallbacks), ASMAtomicReadU64(&pCtr->cDnsIpv6Fallbacks),
                    ASMAtomicReadU64(&pCtr->cGuestErrors));
#else
    RT_NOREF(pThis);
    pHlp->pfnPrintf(pHlp,
                    "NAT diagnostic counters:\n"
                    "  (only available in debug builds)\n");
#endif
}


/**
 * Prints DNS diagnostic state.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoDns(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    char szIpv6NameServer[INET6_ADDRSTRLEN] = "";
    inet_ntop(AF_INET6, &pThis->Diag.Ipv6NameServer, szIpv6NameServer, sizeof(szIpv6NameServer));

    pHlp->pfnPrintf(pHlp,
                    "DNS state:\n"
                    "  virtual IPv4 DNS proxy:       %RTnaipv4\n"
                    "  virtual IPv6 DNS proxy:       %s\n"
                    "  pass domain via DHCP:         %RTbool\n",
                    pThis->Diag.Ipv4NameServer.s_addr,
                    szIpv6NameServer[0] ? szIpv6NameServer : "<unknown>",
                    pThis->fPassDomain);
#ifdef DEBUG
    pHlp->pfnPrintf(pHlp,
                    "  updates:                      %RU64\n"
                    "  IPv4 fallbacks:               %RU64\n"
                    "  IPv6 fallbacks:               %RU64\n",
                    ASMAtomicReadU64(&pThis->Diag.Counters.cDnsUpdates),
                    ASMAtomicReadU64(&pThis->Diag.Counters.cDnsIpv4Fallbacks),
                    ASMAtomicReadU64(&pThis->Diag.Counters.cDnsIpv6Fallbacks));
#else
    pHlp->pfnPrintf(pHlp, "  counters:                     <DEBUG builds only>\n");
#endif

    if (pThis->Diag.msDnsLastUpdate)
        pHlp->pfnPrintf(pHlp,
                        "  last update:                  %RU64 ms\n"
                        "  last domain:                  %s\n"
                        "  last search domains:          %u\n"
                        "  last IPv4 nameservers:        %u configured, %u accepted, fallback %RTbool\n"
                        "  last IPv6 nameservers:        %u configured, %u accepted, fallback %RTbool\n",
                        pThis->Diag.msDnsLastUpdate,
                        pThis->Diag.szDnsDomainName[0] ? pThis->Diag.szDnsDomainName : "<none>",
                        pThis->Diag.cDnsSearchDomains,
                        pThis->Diag.cDnsIpv4NameServersConfigured, pThis->Diag.cDnsIpv4NameServersAccepted,
                        pThis->Diag.fDnsIpv4Fallback,
                        pThis->Diag.cDnsIpv6NameServersConfigured, pThis->Diag.cDnsIpv6NameServersAccepted,
                        pThis->Diag.fDnsIpv6Fallback);
    else
        pHlp->pfnPrintf(pHlp, "  last update:                  <none>\n");
}


/**
 * Extracts the last IPv4 address token from a line of libslirp connection info.
 *
 * The last IPv4 token is the destination address in the current libslirp output.
 *
 * @returns true if an IPv4 address was found, false if not.
 * @param   pszLine     The line to parse, not necessarily zero-terminated.
 * @param   cchLine     The line length.
 * @param   pszAddr     Where to return the address string.
 * @param   cbAddr      Size of the output buffer.
 */
static bool drvNATInfoExtractDestIpv4(const char *pszLine, size_t cchLine, char *pszAddr, size_t cbAddr)
{
    bool fFound = false;
    for (size_t off = 0; off < cchLine; )
    {
        while (   off < cchLine
               && !RT_C_IS_DIGIT(pszLine[off]))
            off++;

        size_t const offStart = off;
        while (   off < cchLine
               && (RT_C_IS_DIGIT(pszLine[off]) || pszLine[off] == '.'))
            off++;

        size_t const cchToken = off - offStart;
        if (   cchToken > 0
            && cchToken < cbAddr
            && memchr(&pszLine[offStart], '.', cchToken))
        {
            char szTmp[INET_ADDRSTRLEN];
            memcpy(szTmp, &pszLine[offStart], cchToken);
            szTmp[cchToken] = '\0';

            struct in_addr Addr;
            if (inet_pton(AF_INET, szTmp, &Addr) == 1)
            {
                RTStrCopy(pszAddr, cbAddr, szTmp);
                fFound = true;
            }
        }
    }

    return fFound;
}


/**
 * Reverse-resolves an IPv4 address for optional debugger output.
 *
 * @returns true if a host name was resolved, false if not.
 * @param   pszAddr     IPv4 address string.
 * @param   pszHost     Where to return the host name.
 * @param   cbHost      Size of the output buffer.
 */
static bool drvNATInfoResolveIpv4(const char *pszAddr, char *pszHost, size_t cbHost)
{
    AssertReturn(cbHost > 0, false);
    pszHost[0] = '\0';

    struct sockaddr_in Addr;
    RT_ZERO(Addr);
    Addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, pszAddr, &Addr.sin_addr) != 1)
        return false;

    int const rc = getnameinfo((struct sockaddr *)&Addr, sizeof(Addr), pszHost, (socklen_t)cbHost,
                               NULL /*pszService*/, 0 /*cbService*/, NI_NAMEREQD);
    if (rc == 0 && pszHost[0])
        return true;

    pszHost[0] = '\0';
    return false;
}


/**
 * Prints libslirp connection information, optionally with destination names.
 *
 * @param   pHlp            Info helper callbacks.
 * @param   pszInfo         Formatted libslirp connection info.
 * @param   fResolveDest    Whether to reverse-resolve destination addresses.
 */
static void drvNATInfoPrintConnectionInfo(PCDBGFINFOHLP pHlp, const char *pszInfo, bool fResolveDest)
{
    if (!fResolveDest)
    {
        pHlp->pfnPrintf(pHlp, "%s", pszInfo);
        return;
    }

    for (const char *pszLine = pszInfo; *pszLine; )
    {
        const char *pszEol = strchr(pszLine, '\n');
        size_t const cchLine = pszEol ? (size_t)(pszEol - pszLine) : strlen(pszLine);
        pHlp->pfnPrintf(pHlp, "%.*s", (int)cchLine, pszLine);

        if (RTStrStr(pszLine, "Dest. Address"))
            pHlp->pfnPrintf(pHlp, " Dest. Name");
        else if (cchLine > 0)
        {
            char szAddr[INET_ADDRSTRLEN];
            if (drvNATInfoExtractDestIpv4(pszLine, cchLine, szAddr, sizeof(szAddr)))
            {
                char szHost[256];
                if (drvNATInfoResolveIpv4(szAddr, szHost, sizeof(szHost)))
                    pHlp->pfnPrintf(pHlp, "  %s", szHost);
                else
                    pHlp->pfnPrintf(pHlp, "  <unresolved>");
            }
        }

        if (!pszEol)
            return;
        pHlp->pfnPrintf(pHlp, "\n");
        pszLine = pszEol + 1;
    }
}


/**
 * Prints libslirp connection information.
 *
 * @param   pThis           The NAT instance.
 * @param   pHlp            Info helper callbacks.
 * @param   fResolveDest    Whether to reverse-resolve destination addresses.
 */
static void drvNATInfoFlows(PDRVNAT pThis, PCDBGFINFOHLP pHlp, bool fResolveDest)
{
    pHlp->pfnPrintf(pHlp, "libslirp connection info%s:\n",
                    fResolveDest ? " (destination reverse DNS enabled)" : "");
    char *pszInfo = slirp_connection_info(pThis->pSlirp);
    if (pszInfo)
        drvNATInfoPrintConnectionInfo(pHlp, pszInfo, fResolveDest);
    else
        pHlp->pfnPrintf(pHlp, "  <unavailable>\n");
}


/**
 * Prints libslirp neighbor information.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoNeighbors(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    pHlp->pfnPrintf(pHlp, "libslirp neighbor info:\n");
    char *pszInfo = slirp_neighbor_info(pThis->pSlirp);
    if (pszInfo)
        pHlp->pfnPrintf(pHlp, "%s", pszInfo);
    else
        pHlp->pfnPrintf(pHlp, "  <unavailable>\n");
}


/**
 * Prints VirtualBox-tracked port-forwarding rules.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoPortForwarding(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    pHlp->pfnPrintf(pHlp, "VirtualBox port-forwarding rules:\n");
    pHlp->pfnPrintf(pHlp, "  Proto  Host address  Port  Guest address  Port\n");

    uint32_t cRules = 0;
    for (uint32_t i = 0; i < RT_ELEMENTS(pThis->Diag.aPf); i++)
        if (pThis->Diag.aPf[i].fInUse)
        {
            pHlp->pfnPrintf(pHlp, "  %-5s  %RTnaipv4  %5u  %RTnaipv4  %5u\n",
                            pThis->Diag.aPf[i].fUdp ? "UDP" : "TCP",
                            pThis->Diag.aPf[i].HostIp.s_addr, pThis->Diag.aPf[i].u16HostPort,
                            pThis->Diag.aPf[i].GuestIp.s_addr, pThis->Diag.aPf[i].u16GuestPort);
            cRules++;
        }

    if (!cRules)
        pHlp->pfnPrintf(pHlp, "  <none tracked>\n");
}


/**
 * Prints poll/wakeup state.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoPoll(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    pHlp->pfnPrintf(pHlp,
                    "Poll/wakeup state:\n"
                    "  poll entries:          %u\n"
                    "  poll capacity:         %u\n"
                    "  pending wakeups:       %RU64\n",
                    pThis->cSockets, pThis->uPollCap, ASMAtomicReadU64(&pThis->cbWakeupNotifs));

    uint32_t const cEntries = RT_MIN(pThis->cSockets, pThis->uPollCap);
    pHlp->pfnPrintf(pHlp, "  Idx  FD        Events  REvents\n");
    for (uint32_t i = 0; i < cEntries; i++)
    {
#ifdef RT_OS_WINDOWS
        pHlp->pfnPrintf(pHlp, "  %3u  %p  %#06x  %#07x\n", i, (void *)(uintptr_t)pThis->aPolls[i].fd,
                        pThis->aPolls[i].events, pThis->aPolls[i].revents);
#else
        pHlp->pfnPrintf(pHlp, "  %3u  %-8d  %#06x  %#07x\n", i, pThis->aPolls[i].fd,
                        pThis->aPolls[i].events, pThis->aPolls[i].revents);
#endif
    }
}


/**
 * Prints timer state.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoTimers(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    uint32_t cTimers = 0;
    uint32_t cActive = 0;
    int64_t msNext = INT64_MAX;
    int64_t const msNow = drvNAT_ClockGetNsCb(pThis) / RT_NS_1MS;

    for (SlirpTimer *pTimer = pThis->pTimerHead; pTimer; pTimer = pTimer->next)
    {
        cTimers++;
        if (pTimer->msExpire > 0)
        {
            cActive++;
            if (pTimer->msExpire < msNext)
                msNext = pTimer->msExpire;
        }
    }

    pHlp->pfnPrintf(pHlp,
                    "Timer state:\n"
                    "  timers:        %u\n"
                    "  active timers: %u\n",
                    cTimers, cActive);
    if (msNext != INT64_MAX)
        pHlp->pfnPrintf(pHlp, "  next expiry:   %RI64 ms from now\n", msNext - msNow);
    else
        pHlp->pfnPrintf(pHlp, "  next expiry:   <none>\n");
}


/**
 * Prints recent diagnostic events.
 *
 * @param   pThis       The NAT instance.
 * @param   pHlp        Info helper callbacks.
 */
static void drvNATInfoEvents(PDRVNAT pThis, PCDBGFINFOHLP pHlp)
{
    uint32_t const iNext = ASMAtomicReadU32(&pThis->Diag.iEvent);
    uint32_t const cEvents = RT_MIN(iNext, (uint32_t)DRVNAT_DIAG_EVENT_COUNT);

    pHlp->pfnPrintf(pHlp, "Recent NAT events (newest first):\n");
    if (!cEvents)
    {
        pHlp->pfnPrintf(pHlp, "  <none>\n");
        return;
    }

    for (uint32_t i = 0; i < cEvents; i++)
    {
        DRVNATDIAGEVENT const * const pEvent = &pThis->Diag.aEvents[(iNext - 1 - i) % DRVNAT_DIAG_EVENT_COUNT];
        if (pEvent->szMsg[0])
            pHlp->pfnPrintf(pHlp, "  %RU64 ms: %s\n", pEvent->msTimestamp, pEvent->szMsg);
    }
}


/**
 * Info handler.
 *
 * @param   pDrvIns     The PDM driver context.
 * @param   pHlp        Info helper callbacks.
 * @param   pszArgs     Optional section list.
 *
 * @thread  any
 */
static DECLCALLBACK(void) drvNATInfo(PPDMDRVINS pDrvIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    if (drvNATInfoHasSection(pszArgs, "help"))
    {
        drvNATInfoHelp(pHlp);
        return;
    }

    bool const fAll = drvNATInfoArgsAreEmpty(pszArgs) || drvNATInfoHasSection(pszArgs, "all");
    bool const fResolveDest =    drvNATInfoHasSection(pszArgs, "flowsr")
                              || drvNATInfoHasSection(pszArgs, "fr");
    bool       fAny = false;

    if (   fAll
        || drvNATInfoHasSection(pszArgs, "s")
        || drvNATInfoHasSection(pszArgs, "summary"))
    {
        drvNATInfoSummary(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "pf"))
    {
        drvNATInfoPortForwarding(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "c")
        || drvNATInfoHasSection(pszArgs, "counter")
        || drvNATInfoHasSection(pszArgs, "counters"))
    {
        drvNATInfoCounters(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "d")
        || drvNATInfoHasSection(pszArgs, "dns"))
    {
        drvNATInfoDns(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || fResolveDest
        || drvNATInfoHasSection(pszArgs, "f")
        || drvNATInfoHasSection(pszArgs, "flows"))
    {
        drvNATInfoFlows(pThis, pHlp, fResolveDest);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "n")
        || drvNATInfoHasSection(pszArgs, "neighbors")
        || drvNATInfoHasSection(pszArgs, "neighbours"))
    {
        drvNATInfoNeighbors(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "p")
        || drvNATInfoHasSection(pszArgs, "poll"))
    {
        drvNATInfoPoll(pThis, pHlp);
        fAny = true;
    }
    if (fAll
        || drvNATInfoHasSection(pszArgs, "t")
        || drvNATInfoHasSection(pszArgs, "timers"))
    {
        drvNATInfoTimers(pThis, pHlp);
        fAny = true;
    }
    if (   fAll
        || drvNATInfoHasSection(pszArgs, "e")
        || drvNATInfoHasSection(pszArgs, "events"))
    {
        drvNATInfoEvents(pThis, pHlp);
        fAny = true;
    }

    if (!fAny)
    {
        pHlp->pfnPrintf(pHlp, "Unknown NAT info argument: %s\n", pszArgs);
        drvNATInfoHelp(pHlp);
    }
}

/**
 * Sets up the redirectors.
 *
 * @returns VBox status code.
 * @param   uInstance       Index of the redirection being constructed.
 * @param   pThis           Pointer to NAT context.
 * @param   pCfg            The configuration handle.
 * @param   pNetwork        Unused.
 *
 * @thread  EMT
 */
static int drvNATConfigPreDefPortForward(unsigned iInstance, PDRVNAT pThis, PCFGMNODE pCfg, PRTNETADDRIPV4 pNetwork)
{
    /** @todo r=jack: rewrite to support IPv6? */
    PPDMDRVINS const    pDrvIns = pThis->pDrvIns;
    PCPDMDRVHLPR3 const pHlp    = pDrvIns->pHlpR3;

    RT_NOREF(pNetwork); /** @todo figure why pNetwork isn't used */

    PCFGMNODE pPFTree = pHlp->pfnCFGMGetChild(pCfg, "PortForwarding");
    if (pPFTree == NULL)
        return VINF_SUCCESS;

    /*
     * Enumerate redirections.
     */
    for (PCFGMNODE pNode = pHlp->pfnCFGMGetFirstChild(pPFTree); pNode; pNode = pHlp->pfnCFGMGetNextChild(pNode))
    {
        char szNodeNm[128] = {0};
        int rc = pHlp->pfnCFGMGetName(pNode, szNodeNm, sizeof(szNodeNm));
        if (RT_FAILURE(rc))
            RTStrCopy(szNodeNm, sizeof(szNodeNm), "xxxx");

        /*
         * Validate the port forwarding config.
         */
        if (!pHlp->pfnCFGMAreValuesValid(pNode, "Name\0Protocol\0UDP\0HostPort\0GuestPort\0GuestIP\0BindIP\0"))
            return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES,
                                    N_("Unknown configuration in port forwarding"));

        /* protocol type */
        bool fUDP = false;
        char szProtocol[32];
        rc = pHlp->pfnCFGMQueryString(pNode, "Protocol", szProtocol, sizeof(szProtocol));
        if (RT_SUCCESS(rc))
        {
            if (!RTStrICmp(szProtocol, "TCP"))
                fUDP = false;
            else if (!RTStrICmp(szProtocol, "UDP"))
                fUDP = true;
            else
                return PDMDrvHlpVMSetError(pDrvIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                           N_("NAT#%d: Invalid configuration value for \"%s/Protocol\": \"%s\""),
                                           iInstance, szNodeNm, szProtocol);
        }
        else if (rc == VERR_CFGM_VALUE_NOT_FOUND)
        {
            rc = pHlp->pfnCFGMQueryBoolDef(pNode, "UDP", &fUDP, false);
            if (RT_FAILURE(rc))
                return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                           N_("NAT#%d: configuration query for \"%s/UDP\" as boolan failed"),
                                           iInstance, szNodeNm);
        }
        else
            return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                       N_("NAT#%d: configuration query for \"%s/Protocol\" as failed"),  iInstance, szNodeNm);
        /* host port - required */
        uint16_t uHostPort;
        rc = pHlp->pfnCFGMQueryU16(pNode, "HostPort", &uHostPort);
        if (RT_FAILURE(rc))
            return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                       N_("NAT#%d: configuration query for \"%s/HostPort\" failed"), iInstance, szNodeNm);

        /* guest port - required */
        uint16_t uGuestPort;
        rc = pHlp->pfnCFGMQueryU16(pNode, "GuestPort", &uGuestPort);
        if (RT_FAILURE(rc))
            return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                       N_("NAT#%d: configuration query for \"%s/GuestPort\" failed"), iInstance, szNodeNm);

        /* host address ("BindIP" name is rather unfortunate given "HostPort" to go with it) */
        char szHostIp[MAX_IP_ADDRESS_STR_LEN_W_NULL] = {0};
        rc = pHlp->pfnCFGMQueryStringDef(pNode, "BindIP", szHostIp, sizeof(szHostIp), "");
        if (RT_FAILURE(rc) && rc != VERR_CFGM_VALUE_NOT_FOUND)
            return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                       N_("NAT#%d: configuration query for \"%s/BindIP\" failed"), iInstance, szNodeNm);

        /* guest address */
        char szGuestIp[MAX_IP_ADDRESS_STR_LEN_W_NULL] = {0};
        rc = pHlp->pfnCFGMQueryStringDef(pNode, "GuestIP", szGuestIp, sizeof(szGuestIp), "");
        if (RT_FAILURE(rc) && rc != VERR_CFGM_VALUE_NOT_FOUND)
            return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                       N_("NAT#%d: configuration query for \"%s/GuestIP\" failed"), iInstance, szNodeNm);

        LogRelMax(256, ("Preconfigured port forward rule discovered on startup: fUdp=%d, HostIp=%s, u16HostPort=%u, GuestIp=%s, u16GuestPort=%u\n",
                        fUDP, szHostIp, uHostPort, szGuestIp, uGuestPort));

        /*
         * Apply port forward.
         */
        rc = drvNATNotifyApplyPortForwardCommand(pThis, false /* fRemove */, fUDP, szHostIp, uHostPort, szGuestIp, uGuestPort);
        if (RT_FAILURE(rc))
            LogFlowFunc(("NAT#%d: configuration error: failed to set up redirection of %d to %d (probably a conflict with existing services or other rules): %Rrc\n",
                         iInstance, uHostPort, uGuestPort, rc));
    } /* for each redir rule */

    return VINF_SUCCESS;
}

/**
 * Applies port forwarding between guest and host.
 *
 * @param   pThis           Pointer to DRVNAT state for current context.
 * @param   fRemove         Flag to remove port forward instead of create.
 * @param   fUdp            Flag specifying if UDP. If false, TCP.
 * @param   pszHostIp       String of host IP address.
 * @param   u16HostPort     Host port to forward to.
 * @param   pszGuestIp      String of guest IP address.
 * @param   u16GuestPort    Guest port to forward.
 *
 * @thread  EMT
 */
static DECLCALLBACK(int) drvNATNotifyApplyPortForwardCommand(PDRVNAT pThis, bool fRemove, bool fUdp, const char *pszHostIp,
                                                             uint16_t u16HostPort, const char *pszGuestIp, uint16_t u16GuestPort)
{
    /** @todo r=jack:
     * - rewrite for IPv6
     * - do we want to lock the guestIp to the VMs IP?
     */
    struct in_addr guestIp, hostIp;

    if (   pszHostIp == NULL
        || inet_aton(pszHostIp, &hostIp) == 0)
        hostIp.s_addr = INADDR_ANY;

    if (   pszGuestIp == NULL
        || inet_aton(pszGuestIp, &guestIp) == 0)
        guestIp.s_addr = pThis->GuestIP;

    int rc;
    if (fRemove)
        rc = slirp_remove_hostfwd(pThis->pSlirp, fUdp, hostIp, u16HostPort);
    else
        rc = slirp_add_hostfwd(pThis->pSlirp, fUdp, hostIp,
                               u16HostPort, guestIp, u16GuestPort);
    if (rc < 0)
    {
        DRVNAT_DIAG_COUNTER_INC(fRemove ? &pThis->Diag.Counters.cPortForwardRemoveFailures
                                : &pThis->Diag.Counters.cPortForwardAddFailures);
        LogRelFunc(("Port forward modify FAIL! Details: fRemove=%d, fUdp=%d, pszHostIp=%s, u16HostPort=%u, pszGuestIp=%s, u16GuestPort=%u\n",
                    fRemove, fUdp, pszHostIp, u16HostPort, pszGuestIp, u16GuestPort));
        drvNATDiagEvent(pThis, "port-forward %s failed: %s %RTnaipv4:%u -> %RTnaipv4:%u",
                        fRemove ? "remove" : "add", fUdp ? "UDP" : "TCP",
                        hostIp.s_addr, u16HostPort, guestIp.s_addr, u16GuestPort);
        return PDMDrvHlpVMSetError(pThis->pDrvIns, VERR_NAT_REDIR_SETUP, RT_SRC_POS,
                                   N_("NAT#%d: configuration error: failed to set up redirection of %d to %d. Probably a conflict with existing services or other rules"),
                                   pThis->pDrvIns->iInstance, u16HostPort, u16GuestPort);
    }

    DRVNAT_DIAG_COUNTER_INC(fRemove ? &pThis->Diag.Counters.cPortForwardRemoveSuccesses
                            : &pThis->Diag.Counters.cPortForwardAddSuccesses);
    drvNATDiagPortForwardUpdate(pThis, fRemove, fUdp, hostIp, u16HostPort, guestIp, u16GuestPort);
    drvNATDiagEvent(pThis, "port-forward %s: %s %RTnaipv4:%u -> %RTnaipv4:%u",
                    fRemove ? "removed" : "added", fUdp ? "UDP" : "TCP",
                    hostIp.s_addr, u16HostPort, guestIp.s_addr, u16GuestPort);
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMINETWORKNATCONFIG,pfnRedirectRuleCommand}
 */
static DECLCALLBACK(int) drvNATAddRedirect(PPDMINETWORKNATCONFIG pInterface, bool fRemove,
                                                        bool fUdp, const char *pHostIp, uint16_t u16HostPort,
                                                        const char *pGuestIp, uint16_t u16GuestPort)
{
    LogRelMax(256, ("New port forwarded added: "
                    "fRemove=%d, fUdp=%d, pHostIp=%s, u16HostPort=%u, pGuestIp=%s, u16GuestPort=%u\n",
                        RT_BOOL(fRemove), RT_BOOL(fUdp), pHostIp, u16HostPort, pGuestIp, u16GuestPort));
    PDRVNAT pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkNATCfg);
    /* Execute the command directly if the VM is not running. */
    int rc;
    if (pThis->pSlirpThread->enmState != PDMTHREADSTATE_RUNNING)
        rc = drvNATNotifyApplyPortForwardCommand(pThis, fRemove, fUdp, pHostIp, u16HostPort, pGuestIp,u16GuestPort);
    else
    {
        PRTREQ pReq;
        rc = RTReqQueueCallEx(pThis->hSlirpReqQueue, &pReq, 0 /*cMillies*/, RTREQFLAGS_VOID,
                              (PFNRT)drvNATNotifyApplyPortForwardCommand, 7,
                              pThis, fRemove, fUdp, pHostIp, u16HostPort, pGuestIp, u16GuestPort);
        if (rc == VERR_TIMEOUT)
        {
            drvNATNotifyNATThread(pThis, "drvNATAddRedirect");
            rc = RTReqWait(pReq, RT_INDEFINITE_WAIT);
            AssertRC(rc);
        }
        else
            AssertRC(rc);

        RTReqRelease(pReq);
    }
    return rc;
}

/**
 * @interface_method_impl{PDMINETWORKNATCONFIG,pfnNotifyDnsChanged}
 */
static DECLCALLBACK(void) drvNATNotifyDnsChanged(PPDMINETWORKNATCONFIG pInterface, PCPDMINETWORKNATDNSCONFIG pDnsConf)
{
    PDRVNAT const pThis = RT_FROM_MEMBER(pInterface, DRVNAT, INetworkNATCfg);
    AssertPtrReturnVoid(pDnsConf);
    AssertReturnVoid(pThis->pSlirp);

    LogRel(("NAT: DNS settings changed, triggering update\n"));
    DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDnsUpdates);
    pThis->Diag.msDnsLastUpdate = RTTimeMilliTS();
    RTStrCopy(pThis->Diag.szDnsDomainName, sizeof(pThis->Diag.szDnsDomainName), pDnsConf->szDomainName);
    pThis->Diag.cDnsSearchDomains = (uint32_t)pDnsConf->cSearchDomains;
    pThis->Diag.cDnsIpv4NameServersConfigured = (uint32_t)pDnsConf->cNameServers;
    pThis->Diag.cDnsIpv4NameServersAccepted = 0;
    pThis->Diag.fDnsIpv4Fallback = false;
    pThis->Diag.cDnsIpv6NameServersConfigured = (uint32_t)pDnsConf->cIPv6NameServers;
    pThis->Diag.cDnsIpv6NameServersAccepted = 0;
    pThis->Diag.fDnsIpv6Fallback = false;
    drvNATDiagEvent(pThis, "DNS settings changed");

    int rc = 0;

    if (pThis->fPassDomain)
    {
        if (pDnsConf->szDomainName[0] == '\0')
            slirp_set_vdomainname(pThis->pSlirp, NULL);
        else
            slirp_set_vdomainname(pThis->pSlirp, pDnsConf->szDomainName);
    }

    if (pDnsConf->papszSearchDomains)
        slirp_set_vdnssearch(pThis->pSlirp, pDnsConf->papszSearchDomains);

    if (pDnsConf->cNameServers > 0)
    {
        struct in_addr* aIPv4Nameservers = (struct in_addr *)RTMemAllocZ(sizeof(struct in_addr) * pDnsConf->cNameServers);

        /* Loop through and store in array if not on 127/8 network. */
        size_t uStoredNameservers = 0;
        for (size_t i = 0; i < pDnsConf->cNameServers; i++)
        {
            RTNETADDRIPV4 tmpNameserver;
            rc = RTNetStrToIPv4Addr(pDnsConf->papszNameServers[i], &tmpNameserver);

            if (RT_FAILURE(rc))
            {
                Log3Func(("Failed to convert IPv4 nameserver %s. Check for errors.\n", pDnsConf->papszNameServers[i]));
                continue;
            }

            if (!((tmpNameserver.u & RT_H2N_U32_C(IN_CLASSA_NET))
                == RT_H2N_U32_C(INADDR_LOOPBACK & IN_CLASSA_NET)))
            {
                aIPv4Nameservers[uStoredNameservers].s_addr = tmpNameserver.u;
                uStoredNameservers++;
                LogRelMax(256, ("NAT DNS Update: Stored %RTnaipv4 as nameserver #%u\n", tmpNameserver, i));
                drvNATDiagEvent(pThis, "DNS IPv4 nameserver accepted: %RTnaipv4", tmpNameserver.u);
            }
        }

        pThis->Diag.cDnsIpv4NameServersAccepted = (uint32_t)uStoredNameservers;
        if (aIPv4Nameservers[0].s_addr == 0)
        {
            pThis->Diag.fDnsIpv4Fallback = true;
            DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDnsIpv4Fallbacks);
            LogRel(("Nameserver is either on 127/8 network or failed to obtain from host. "
                    "Falling back to libslirp DNS proxy.\n"));
            drvNATDiagEvent(pThis, "DNS IPv4 fallback to libslirp proxy");

            /* Free the unused allocation before falling back. */
            RTMemFree(aIPv4Nameservers);
            aIPv4Nameservers = NULL;

            struct in_addr mProxyNameserver;
            mProxyNameserver.s_addr = slirp_get_vnetwork_addr(pThis->pSlirp).s_addr | RT_H2N_U32_C(0x00000003);

            slirp_set_vnameserver(pThis->pSlirp, mProxyNameserver);
            slirp_set_RealNameservers(pThis->pSlirp, 0, NULL);

            LogRel(("fallback virtual nameserver: %RTnaipv4", mProxyNameserver));
        }
        else
        {
            LogRelMax(256, ("NAT DNS Update: Stored %u total IPv4 nameservers\n", uStoredNameservers));
            drvNATDiagEvent(pThis, "DNS IPv4 nameservers active: %u", uStoredNameservers);
            slirp_set_RealNameservers(pThis->pSlirp, uStoredNameservers, aIPv4Nameservers);
        }
    }

    if (pDnsConf->cIPv6NameServers > 0)
    {
        struct in6_addr* aIPv6Nameservers = (struct in6_addr *)RTMemAllocZ(sizeof(struct in6_addr) * pDnsConf->cIPv6NameServers);
        size_t uStoredNameservers6 = 0;

        /* Storing all IPv6 nameservers */
        for (size_t i = 0; i < pDnsConf->cIPv6NameServers; i++)
        {
            RTNETADDRIPV6 tmpNameserver;
            rc = RTNetStrToIPv6AddrEx(pDnsConf->papszIPv6NameServers[i], &tmpNameserver, NULL); /** @todo r=jack: IPv6 zones. */

            if (RT_FAILURE(rc))
            {
                Log3Func(("Failed to convert IPv6 nameserver %s. Check for errors.\n", pDnsConf->papszIPv6NameServers[i]));
                continue;
            }

            memcpy(&aIPv6Nameservers[uStoredNameservers6], &tmpNameserver, sizeof(RTNETADDRIPV6));
            LogRelMax(256, ("NAT DNS Update: Stored %RTnaipv6 as nameserver #%u\n", &tmpNameserver, (unsigned)uStoredNameservers6));
            drvNATDiagEvent(pThis, "DNS IPv6 nameserver accepted: %RTnaipv6", &tmpNameserver);
            uStoredNameservers6++;
        }

        pThis->Diag.cDnsIpv6NameServersAccepted = (uint32_t)uStoredNameservers6;
        if (uStoredNameservers6 == 0)
        {
            pThis->Diag.fDnsIpv6Fallback = true;
            DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDnsIpv6Fallbacks);
            LogRel(("Failed to obtain nameserver from host. "
                    "Falling back to libslirp DNS proxy for IPv6.\n"));
            drvNATDiagEvent(pThis, "DNS IPv6 fallback to libslirp proxy");

            /* Free the unused allocation before falling back. */
            RTMemFree(aIPv6Nameservers);
            aIPv6Nameservers = NULL;

            struct in6_addr mProxyNameserver;
            inet_pton(AF_INET6, "fd17:625c:f037:0::3", &mProxyNameserver.s6_addr);

            slirp_set_vnameserver6(pThis->pSlirp, mProxyNameserver);
            slirp_set_IPv6RealNameservers(pThis->pSlirp, 0, NULL);

            LogRel(("fallback IPv6 virtual nameserver: %RTnaipv6", &mProxyNameserver));
        }
        else
        {
            LogRelMax(256, ("NAT DNS Update: Stored %u total IPv6 nameservers\n", (unsigned)uStoredNameservers6));
            drvNATDiagEvent(pThis, "DNS IPv6 nameservers active: %u", (unsigned)uStoredNameservers6);
            slirp_set_IPv6RealNameservers(pThis->pSlirp, uStoredNameservers6, aIPv6Nameservers);
        }
    }
}


/*
 * Libslirp Utility Functions
 */

/**
 * Reduce the given timeout to match the earliest timer deadline.
 *
 * @returns Updated cMsTimeout value.
 * @param   pThis       Pointer to NAT State context.
 * @param   cMsTimeout  The timeout to adjust, in milliseconds.
 *
 * @thread  NAT
 */
static int drvNATTimersAdjustTimeoutDown(PDRVNAT pThis, int cMsTimeout)
{
    /** @todo r=bird: This and a most other stuff would be easier if msExpire was
     *                unsigned and we used UINT64_MAX for stopped timers.  */
    /** @todo The timer code isn't thread safe, it assumes a single user thread
     *        (NAT). */

    /* Find the first (lowest) deadline. */
    int64_t msDeadline = INT64_MAX;
    for (SlirpTimer *pCurrent = pThis->pTimerHead; pCurrent; pCurrent = pCurrent->next)
        if (pCurrent->msExpire < msDeadline && pCurrent->msExpire > 0)
            msDeadline = pCurrent->msExpire;

    /* Adjust the timeout if there is a timer with a deadline. */
    if (msDeadline < INT64_MAX)
    {
        int64_t const msNow = drvNAT_ClockGetNsCb(pThis) / RT_NS_1MS;
        if (msNow < msDeadline)
        {
            int64_t cMilliesToDeadline = msDeadline - msNow;
            if (cMilliesToDeadline < cMsTimeout)
                cMsTimeout = (int)cMilliesToDeadline;
        }
        else
            cMsTimeout = 0;
    }

    return cMsTimeout;
}

/**
 * Run expired timers.
 *
 * @param   opaque  Pointer to NAT State context.
 *
 * @thread  NAT
 */
static void drvNATTimersRunExpired(PDRVNAT pThis)
{
    int64_t const msNow    = drvNAT_ClockGetNsCb(pThis) / RT_NS_1MS;
    SlirpTimer   *pCurrent = pThis->pTimerHead;
    while (pCurrent != NULL)
    {
        SlirpTimer * const pNext = pCurrent->next; /* (in case the timer is destroyed from the callback) */
        if (pCurrent->msExpire <= msNow && pCurrent->msExpire > 0)
        {
            pCurrent->msExpire = 0;
            pCurrent->pHandler(pCurrent->opaque);
        }
        pCurrent = pNext;
    }
}

/**
 * Converts slirp representation of poll events to host representation.
 *
 * @param   iEvents     Integer representing slirp type poll events.
 *
 * @returns Integer representing host type poll events.
 *
 * @thread ?
 */
static short drvNAT_PollEventSlirpToHost(int iEvents)
{
    short iRet = 0;
#ifndef RT_OS_WINDOWS
    if (iEvents & SLIRP_POLL_IN)  iRet |= POLLIN;
    if (iEvents & SLIRP_POLL_OUT) iRet |= POLLOUT;
    if (iEvents & SLIRP_POLL_PRI) iRet |= POLLPRI;
    if (iEvents & SLIRP_POLL_ERR) iRet |= POLLERR;
    if (iEvents & SLIRP_POLL_HUP) iRet |= POLLHUP;
#else
    if (iEvents & SLIRP_POLL_IN)  iRet |= (POLLRDNORM | POLLRDBAND);
    if (iEvents & SLIRP_POLL_OUT) iRet |= POLLWRNORM;
    if (iEvents & SLIRP_POLL_PRI) iRet |= (POLLIN);
    if (iEvents & SLIRP_POLL_ERR) iRet |= 0;
    if (iEvents & SLIRP_POLL_HUP) iRet |= 0;
#endif
    return iRet;
}

/**
 * Converts host representation of poll events to slirp representation.
 *
 * @param   iEvents     Integer representing host type poll events.
 *
 * @returns integer     representing slirp type poll events.
 *
 * @thread  NAT
 */
static int drvNAT_PollEventHostToSlirp(int iEvents) {
    int iRet = 0;
#ifndef RT_OS_WINDOWS
    if (iEvents & POLLIN)  iRet |= SLIRP_POLL_IN;
    if (iEvents & POLLOUT) iRet |= SLIRP_POLL_OUT;
    if (iEvents & POLLPRI) iRet |= SLIRP_POLL_PRI;
    if (iEvents & POLLERR) iRet |= SLIRP_POLL_ERR;
    if (iEvents & POLLHUP) iRet |= SLIRP_POLL_HUP;
#else
    if (iEvents & (POLLRDNORM | POLLRDBAND))  iRet |= SLIRP_POLL_IN;
    if (iEvents & POLLWRNORM) iRet |= SLIRP_POLL_OUT;
    if (iEvents & (POLLPRI)) iRet |= SLIRP_POLL_PRI;
    if (iEvents & POLLERR) iRet |= SLIRP_POLL_ERR;
    if (iEvents & POLLHUP) iRet |= SLIRP_POLL_HUP;
#endif
    return iRet;
}


/*
 * Libslirp Callbacks
 */

/**
 * Callback called by libslirp to send packet into guest.
 *
 * @param   pvBuf   Pointer to packet buffer.
 * @param   cb      Size of packet.
 * @param   pvUser  Pointer to NAT State context.
 *
 * @returns Size of packet received or -1 on error.
 *
 * @thread  NAT
 */
static ssize_t drvNAT_SendPacketCb(const void *pvBuf, ssize_t cb, void *pvUser /* PDRVNAT */)
{
    PDRVNAT const pThis = (PDRVNAT)pvUser;
    AssertPtr(pThis);

    /* Don't queue new requests when the NAT thread is about to stop. */
    if (pThis->pSlirpThread->enmState != PDMTHREADSTATE_RUNNING)
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cDropNatThreadDown);
        return -1;
    }

    void * const pvNewBuf = RTMemDup(pvBuf, cb);
    AssertPtrReturn(pvNewBuf, -1);

    Log6Func(("pvNewBuf=%p cb=%#x (pThis=%p)\n"
              "%.*Rhxd\n", pvNewBuf, cb, pThis, cb, pvNewBuf));

    ASMAtomicIncU32(&pThis->cPkts);
    int rc = RTReqQueueCallEx(pThis->hRecvReqQueue, NULL /*ppReq*/, 0 /*cMillies*/, RTREQFLAGS_VOID | RTREQFLAGS_NO_WAIT,
                              (PFNRT)drvNATRecvWorker, 3, pThis, pvNewBuf, cb);
    if (RT_FAILURE(rc))
    {
        DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cQueueFailures);
        drvNATDiagEvent(pThis, "RX queueing failed: cb=%zd rc=%Rrc", cb, rc);
        STAM_COUNTER_INC(&pThis->StatQueuePktDropped);
        ASMAtomicDecU32(&pThis->cPkts);
        RTMemFree(pvNewBuf);
        return -1;
    }

    drvNATRecvWakeup(pThis->pDrvIns, pThis->pRecvThread);

    DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cNatToGuestFrames);
    DRVNAT_DIAG_COUNTER_ADD(&pThis->Diag.Counters.cbNatToGuest, (uint64_t)cb);
    STAM_COUNTER_INC(&pThis->StatQueuePktSent);

    LogFlowFuncLeave();
    return cb;
}

/**
 * Callback called by libslirp when the guest does something wrong.
 *
 * @param   pMsg    Error message string.
 * @param   pvUser  Pointer to NAT State context.
 *
 * @thread  NAT
 */
static void drvNAT_GuestErrorCb(const char *pszMsg, void *pvUser)
{
    /* Note! This is _just_ libslirp complaining about odd guest behaviour.
             It is nothing we need to create popup messages in the GUI about. */
    LogRelMax(250, ("NAT Guest Error: %s\n", pszMsg));
    PDRVNAT const pThis = (PDRVNAT)pvUser;
    AssertPtrReturnVoid(pThis);
    DRVNAT_DIAG_COUNTER_INC(&pThis->Diag.Counters.cGuestErrors);
    drvNATDiagEvent(pThis, "libslirp guest error: %s", pszMsg);
}

/**
 * Callback called by libslirp to get the current timestamp in nanoseconds.
 *
 * @param   pvUser  Pointer to NAT State context.
 *
 * @returns 64-bit signed integer representing time in nanoseconds.
 *
 * @thread  EMT/NAT
 *
 * @note    This function is called both during the initialization of the Slirp
 *          instance and by Slirp's polling function.
 */
static int64_t drvNAT_ClockGetNsCb(void *pvUser)
{
    RT_NOREF(pvUser);
    return (int64_t)RTTimeNanoTS();
}

/**
 * Callback called by slirp to create a new timer and insert it into the given list.
 *
 * @param   slirpTimeCb     Callback function supplied to the new timer upon timer expiry.
 *                          Called later by the timeout handler.
 * @param   cb_opaque       Opaque object supplied to slirpTimeCb when called. Should be
 *                          Identical to the opaque parameter.
 * @param   opaque          Pointer to NAT State context.
 *
 * @returns Pointer to new timer.
 *
 * @thread EMT
 */
static void * drvNAT_TimerNewCb(SlirpTimerCb slirpTimeCb, void *cb_opaque, void *opaque)
{
    PDRVNAT pThis = (PDRVNAT)opaque;
    Assert(pThis);

    SlirpTimer * const pNewTimer = (SlirpTimer *)RTMemAlloc(sizeof(SlirpTimer));
    if (pNewTimer)
    {
        pNewTimer->msExpire = 0;
        pNewTimer->pHandler = slirpTimeCb;
        pNewTimer->opaque = cb_opaque;
        /** @todo r=bird: Not thread safe. Assumes NAT */
        pNewTimer->next = pThis->pTimerHead;
        pThis->pTimerHead = pNewTimer;
    }
    return pNewTimer;
}

/**
 * Callback called by slirp to free a timer.
 *
 * @param   pvTimer Pointer to slirpTimer object to be freed.
 * @param   pvUser  Pointer to NAT State context.
 *
 * @thread EMT
 */
static void drvNAT_TimerFreeCb(void *pvTimer, void *pvUser)
{
    PDRVNAT const      pThis  = (PDRVNAT)pvUser;
    SlirpTimer * const pTimer = (SlirpTimer *)pvTimer;
    Assert(pThis);
    /** @todo r=bird: Not thread safe. Assumes NAT */

    SlirpTimer *pPrev    = NULL;
    SlirpTimer *pCurrent = pThis->pTimerHead;
    while (pCurrent != NULL)
    {
        if (pCurrent == pTimer)
        {
            /* unlink it. */
            if (!pPrev)
                pThis->pTimerHead = pCurrent->next;
            else
                pPrev->next                  = pCurrent->next;
            pCurrent->next = NULL;
            RTMemFree(pCurrent);
            return;
        }

        /* advance */
        pPrev = pCurrent;
        pCurrent = pCurrent->next;
    }
    Assert(!pTimer);
}

/**
 * Callback called by slirp to modify a timer.
 *
 * @param   pvTimer         Pointer to slirpTimer object to be modified.
 * @param   msNewDeadlineTs The new absolute expiration time in milliseconds.
 *                          Zero stops it.
 * @param   pvUser          Pointer to NAT State context.
 *
 * @thread  EMT
 *
 * @note    This function is called both during the initialization of the Slirp
 *          instance.
 */
static void drvNAT_TimerModCb(void *pvTimer, int64_t msNewDeadlineTs, void *pvUser)
{
    SlirpTimer * const pTimer = (SlirpTimer *)pvTimer;
    /** @todo r=bird: ASSUMES NAT, otherwise it may need to be woken up! */
    pTimer->msExpire = msNewDeadlineTs;
    RT_NOREF(pvUser);
}

/**
 * Callback called by slirp when there is I/O that needs to happen.
 *
 * @param   opaque  Pointer to NAT State context.
 *
 * @thread  NAT
 */
static void drvNAT_NotifyCb(void *opaque)
{
    LogFlowFuncEnter();
    PDRVNAT pThis = (PDRVNAT)opaque;
    drvNATNotifyNATThread(pThis, "drvNAT_NotifyCb");
}

/**
 * Registers poll. Unused function (other than logging).
 *
 * @param   socket  Slirp's OS specific socket tpye.
 * @param   opaque  Pointer to NAT State context.
 *
 * @thread  NAT
 */
static void drvNAT_RegisterPoll(slirp_os_socket socket, void *opaque)
{
    RT_NOREF(socket, opaque);
#ifdef RT_OS_WINDOWS
    Log4(("Poll registered: fd=%p\n", socket));
#else
    Log4(("Poll registered: fd=%d\n", socket));
#endif
}

/**
 * Unregisters poll. Unused function (other than logging).
 *
 * @param   socket  Slirp's OS specific socket tpye.
 * @param   opaque  Pointer to NAT State context.
 *
 * @thread  NAT
 */
static void drvNAT_UnregisterPoll(slirp_os_socket socket, void *opaque)
{
    RT_NOREF(socket, opaque);
#ifdef RT_OS_WINDOWS
    Log4(("Poll unregistered: fd=%p\n", socket));
#else
    Log4(("Poll unregistered: fd=%d\n", socket));
#endif
}

/**
 * Callback function to add entry to pollfd array.
 *
 * @param   iFd     Integer of system file descriptor of socket.
 *                  (on windows, this is a VBox internal, not system, value).
 * @param   iEvents Integer of slirp type poll events.
 * @param   opaque  Pointer to NAT State context.
 *
 * @returns Index of latest pollfd entry.
 *
 * @thread  NAT
 */
static DECLCALLBACK(int) drvNAT_AddPollCb(slirp_os_socket hFd, int iEvents, void *opaque)
{
    PDRVNAT pThis = (PDRVNAT)opaque;

    if (pThis->cSockets + 1 >= pThis->uPollCap)
    {
        size_t cbNew = pThis->uPollCap * 2 * sizeof(struct pollfd);
        struct pollfd *pvNew = (struct pollfd *)RTMemRealloc(pThis->aPolls, cbNew);
        if (pvNew)
        {
            pThis->aPolls = pvNew;
            pThis->uPollCap *= 2;
        }
        else
            return -1;
    }

    unsigned int uIdx = pThis->cSockets;
    Assert(uIdx < INT_MAX);
    pThis->aPolls[uIdx].fd = hFd;
    pThis->aPolls[uIdx].events = drvNAT_PollEventSlirpToHost(iEvents);
    pThis->aPolls[uIdx].revents = 0;
    pThis->cSockets += 1;
    return uIdx;
}

/**
 * Get translated revents from a poll at a given index.
 *
 * @param   idx     Integer index of poll.
 * @param   opaque  Pointer to NAT State context.
 *
 * @returns Integer representing transalted revents.
 *
 * @thread  NAT
 */
static DECLCALLBACK(int) drvNAT_GetREventsCb(int idx, void *opaque)
{
    PDRVNAT pThis = (PDRVNAT)opaque;
    struct pollfd* aPolls = pThis->aPolls;
    return drvNAT_PollEventHostToSlirp(aPolls[idx].revents);
}

/**
 * Contructor/Destructor
 */
/**
 * Destruct a driver instance.
 *
 * Most VM resources are freed by the VM. This callback is provided so that any non-VM
 * resources can be freed correctly.
 *
 * @param   pDrvIns     The driver instance data.
 *
 * @thread EMT
 */
static DECLCALLBACK(void) drvNATDestruct(PPDMDRVINS pDrvIns)
{
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);
    LogFlow(("drvNATDestruct:\n"));
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);

    if (pThis)
    {
        if (pThis->pSlirp)
        {
            slirp_cleanup(pThis->pSlirp);
            pThis->pSlirp = NULL;
        }

#ifdef VBOX_WITH_STATISTICS
# define DRV_PROFILE_COUNTER(name, dsc)     DEREGISTER_COUNTER(name, pThis)
# define DRV_COUNTING_COUNTER(name, dsc)    DEREGISTER_COUNTER(name, pThis)
# include "slirp/counters.h"
#endif
        RTMemFree(pThis->aPolls);
        pThis->aPolls = NULL;
    }

    RTReqQueueDestroy(pThis->hSlirpReqQueue);
    pThis->hSlirpReqQueue = NIL_RTREQQUEUE;

    RTReqQueueDestroy(pThis->hRecvReqQueue);
    pThis->hRecvReqQueue = NIL_RTREQQUEUE;

    RTSemEventDestroy(pThis->hEventRecv);
    pThis->hEventRecv = NIL_RTSEMEVENT;

    if (RTCritSectIsInitialized(&pThis->DevAccessLock))
        RTCritSectDelete(&pThis->DevAccessLock);

    if (RTCritSectIsInitialized(&pThis->XmitLock))
        RTCritSectDelete(&pThis->XmitLock);

#ifndef RT_OS_WINDOWS
    RTPipeClose(pThis->hPipeRead);
    RTPipeClose(pThis->hPipeWrite);
    pThis->hPipeRead = NIL_RTPIPE;
    pThis->hPipeWrite = NIL_RTPIPE;
#endif
}

/**
 * Construct a NAT network transport driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvNATConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVNAT pThis = PDMINS_2_DATA(pDrvIns, PDRVNAT);

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                      = pDrvIns;
    pThis->hSlirpReqQueue               = NIL_RTREQQUEUE;
    pThis->hEventRecv                   = NIL_RTSEMEVENT;
    pThis->hRecvReqQueue                = NIL_RTREQQUEUE;
#ifndef RT_OS_WINDOWS
    pThis->hPipeRead                    = NIL_RTPIPE;
    pThis->hPipeWrite                   = NIL_RTPIPE;
#endif
    pThis->cSockets                     = 0;
    pThis->pTimerHead                   = NULL;
    pThis->aPolls                       = (struct pollfd *)RTMemAllocZ(64 * sizeof(struct pollfd));
    AssertReturn(pThis->aPolls, VERR_NO_MEMORY);
    pThis->uPollCap                 = 64;

    /* IBase */
    pDrvIns->IBase.pfnQueryInterface    = drvNATQueryInterface;

    /* INetwork */
    pThis->INetworkUp.pfnBeginXmit          = drvNATNetworkUp_BeginXmit;
    pThis->INetworkUp.pfnAllocBuf           = drvNATNetworkUp_AllocBuf;
    pThis->INetworkUp.pfnFreeBuf            = drvNATNetworkUp_FreeBuf;
    pThis->INetworkUp.pfnSendBuf            = drvNATNetworkUp_SendBuf;
    pThis->INetworkUp.pfnEndXmit            = drvNATNetworkUp_EndXmit;
    pThis->INetworkUp.pfnSetPromiscuousMode = drvNATNetworkUp_SetPromiscuousMode;
    pThis->INetworkUp.pfnNotifyLinkChanged  = drvNATNetworkUp_NotifyLinkChanged;

    /* NAT engine configuration */
    pThis->INetworkNATCfg.pfnRedirectRuleCommand = drvNATAddRedirect;
    pThis->INetworkNATCfg.pfnNotifyDnsChanged    = drvNATNotifyDnsChanged;

    /*
     * Query the network port interface.
     */
    pThis->pIAboveNet = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMINETWORKDOWN);
    if (!pThis->pIAboveNet)
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_MISSING_INTERFACE_ABOVE,
                                N_("Configuration error: the above device/driver didn't export the network port interface"));
    pThis->pIAboveConfig = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMINETWORKCONFIG);
    if (!pThis->pIAboveConfig)
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_MISSING_INTERFACE_ABOVE,
                                N_("Configuration error: the above device/driver didn't export the network config interface"));

    /*
     * Validate the config.
     */
    PDMDRV_VALIDATE_CONFIG_RETURN(pDrvIns,
                                  "PassDomain"
                                  "|TFTPPrefix"
                                  "|BootFile"
                                  "|Network"
                                  "|NextServer"
                                  "|DNSProxy"
                                  "|BindIP"
                                  "|UseHostResolver"
                                  "|SlirpMTU"
                                  "|AliasMode"
                                  "|SockRcv"
                                  "|SockSnd"
                                  "|TcpRcv"
                                  "|TcpSnd"
                                  "|ICMPCacheLimit"
                                  "|SoMaxConnection"
                                  "|LocalhostReachable"
                                  "|HostResolverMappings"
                                  "|ForwardBroadcast"
                                  "|EnableTFTP"
                                  "|RestrictInternet"
                                  "|DeferHostWrites"
                                  , "PortForwarding");

    /*
     * Get the configuration settings and build Slirp Config.
     */
    int  rc;
    SlirpConfig slirpCfg = { 0 };

    slirpCfg.version = 6;
    slirpCfg.in_enabled = true;
    slirpCfg.in6_enabled = true;
    slirpCfg.vhostname = NULL;

    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "PassDomain", &pThis->fPassDomain, true);
    AssertLogRelRCReturn(rc, rc);

    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "ForwardBroadcast", &slirpCfg.fForwardBroadcast, false);
    AssertLogRelRCReturn(rc, rc);
    pThis->Diag.fForwardBroadcast = slirpCfg.fForwardBroadcast;

    bool fRestricted = false;
    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "RestrictInternet", &fRestricted, false);
    AssertLogRelRCReturn(rc, rc);

    slirpCfg.restricted = fRestricted ? 1 : 0;

    /*
     * Keep default as true to preserve functionality for old VMs.
     * All new VMs will have TFTP functionality disabled by default.
     */
    bool fEnableTFTP;
    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "EnableTFTP", &fEnableTFTP, true);
    AssertLogRelRCReturn(rc, rc);

    pThis->Diag.fTftpEnabled = fEnableTFTP;

    if (fEnableTFTP)
    {
        char *pszTmp = NULL;
        rc = pDrvIns->pHlpR3->pfnCFGMQueryStringAllocDef(pCfg, "TFTPPrefix", &pszTmp, "");
        AssertLogRelRCReturn(rc, rc);
        slirpCfg.tftp_path = pszTmp;

        rc = pDrvIns->pHlpR3->pfnCFGMQueryStringAllocDef(pCfg, "BootFile", &pszTmp, "");
        AssertLogRelRCReturn(rc, rc);
        slirpCfg.bootfile = pszTmp;

        rc = pDrvIns->pHlpR3->pfnCFGMQueryStringAllocDef(pCfg, "NextServer", &pszTmp, "");
        AssertLogRelRCReturn(rc, rc);
        slirpCfg.tftp_server_name = pszTmp;
    }

    uint32_t uTmpMtu = 0;
    rc = pDrvIns->pHlpR3->pfnCFGMQueryU32Def(pCfg, "SlirpMTU", &uTmpMtu, 1500);
    AssertLogRelRCReturn(rc, rc);
    if (uTmpMtu < IPV4_MIN_MTU || uTmpMtu > IPV4_MAX_MTU)
        return PDMDrvHlpVMSetError(pDrvIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                    N_("NAT#%d: Configuration error: MTU is not a valid length. "
                                        "Ensure that the MTU is at above the IPv4 minimum of %u bytes "
                                        "and at or below the maximum of %u.\n"),
                                    pDrvIns->iInstance, IPV4_MIN_MTU, IPV4_MAX_MTU);
    slirpCfg.if_mtu = (size_t)uTmpMtu;
    pThis->Diag.u32Mtu = uTmpMtu;

    /** @todo r=jack: make this configurable with the right cfgm key, currently just using MTU */
    uint32_t uTmpMru = 0;
    rc = pDrvIns->pHlpR3->pfnCFGMQueryU32Def(pCfg, "SlirpMTU", &uTmpMru, 1500);
    AssertLogRelRCReturn(rc, rc);
    if (uTmpMru < IPV4_MIN_MTU || uTmpMru > IPV4_MAX_MTU)
        return PDMDrvHlpVMSetError(pDrvIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                    N_("NAT#%d: Configuration error: MRU is not a valid length. "
                                        "Ensure that the MRU is at above the IPv4 minimum of %u bytes "
                                        "and at or below the maximum of %u.\n"),
                                    pDrvIns->iInstance, IPV4_MIN_MTU, IPV4_MAX_MTU);
    slirpCfg.if_mru = uTmpMru;
    pThis->Diag.u32Mru = uTmpMru;

    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "LocalhostReachable", &slirpCfg.disable_host_loopback, false);
    pThis->Diag.fLocalhostReachable = slirpCfg.disable_host_loopback;
    // Invert the input since the libslirp config is "disable" not "is reachable"
    slirpCfg.disable_host_loopback = !slirpCfg.disable_host_loopback;
    AssertLogRelRCReturn(rc, rc);

    rc = pDrvIns->pHlpR3->pfnCFGMQuerySIntDef(pCfg, "SoMaxConnection", &slirpCfg.iSoMaxConn, 10);
    AssertLogRelRCReturn(rc, rc);
    pThis->Diag.iSoMaxConn = slirpCfg.iSoMaxConn;

    /* Generate a network address for this network card. */
    char szNetwork[32]; /* xxx.xxx.xxx.xxx/yy */
    rc = pDrvIns->pHlpR3->pfnCFGMQueryStringDef(pCfg, "Network", szNetwork, sizeof(szNetwork), "10.0.2.0/24");
    AssertLogRelRCReturn(rc, rc);

    RTNETADDRIPV4 Network, Netmask;
    rc = RTCidrStrToIPv4(szNetwork, &Network, &Netmask);
    if (RT_FAILURE(rc))
        return PDMDrvHlpVMSetError(pDrvIns, rc, RT_SRC_POS,
                                   N_("NAT#%d: Configuration error: network '%s' describes not a valid IPv4 network"),
                                   pDrvIns->iInstance, szNetwork);

    LogFlow(("Basic NAT config (NAT#%d):\n"
             "  Network: %RTnaipv4\n"
             "  Netmask: %RTnaipv4\n",
             pDrvIns->iInstance, RT_H2BE_U32(Network.u), RT_H2BE_U32(Netmask.u)));

    slirpCfg.vnetwork = RTNetIPv4AddrHEToInAddr(&Network);
    slirpCfg.vnetmask = RTNetIPv4AddrHEToInAddr(&Netmask);
    pThis->Diag.Ipv4Network.u = slirpCfg.vnetwork.s_addr;
    pThis->Diag.Ipv4Netmask.u = slirpCfg.vnetmask.s_addr;

    RTNETADDRIPV4 NetTemp = Network;
    NetTemp.u |= 2;  /* Usually 10.0.2.2 */
    slirpCfg.vhost       = RTNetIPv4AddrHEToInAddr(&NetTemp);
    pThis->Diag.Ipv4Host = slirpCfg.vhost;

    NetTemp = Network;
    NetTemp.u |= 15; /* Usually 10.0.2.15 */
    slirpCfg.vdhcp_start = RTNetIPv4AddrHEToInAddr(&NetTemp);
    pThis->Diag.Ipv4DhcpStart = slirpCfg.vdhcp_start;

    NetTemp = Network;
    NetTemp.u |= 3;  /* Usually 10.0.2.3 */
    slirpCfg.vnameserver = RTNetIPv4AddrHEToInAddr(&NetTemp);
    pThis->Diag.Ipv4NameServer = slirpCfg.vnameserver;

    /* IPv6: Use the same prefix as the NAT Network default:
       [fd17:625c:f037:XXXX::/64] - RFC 4193 (ULA) Locally Assigned
       Global ID where XXXX, 16 bit Subnet ID, are two bytes from the
       middle of the IPv4 address, e.g. :0002: for 10.0.2.1. */
    inet_pton(AF_INET6, "fd17:625c:f037:0::",  &slirpCfg.vprefix_addr6);
    inet_pton(AF_INET6, "fd17:625c:f037:0::2", &slirpCfg.vhost6);
    inet_pton(AF_INET6, "fd17:625c:f037:0::3", &slirpCfg.vnameserver6);
    slirpCfg.vprefix_len = 64;
    pThis->Diag.bIpv6PrefixLen = slirpCfg.vprefix_len;

    /* Copy the middle of the IPv4 addresses to the IPv6 addresses. */
    slirpCfg.vprefix_addr6.s6_addr[6] = RT_BYTE2(slirpCfg.vhost.s_addr);
    slirpCfg.vprefix_addr6.s6_addr[7] = RT_BYTE3(slirpCfg.vhost.s_addr);
    slirpCfg.vhost6.s6_addr[6]        = RT_BYTE2(slirpCfg.vhost.s_addr);
    slirpCfg.vhost6.s6_addr[7]        = RT_BYTE3(slirpCfg.vhost.s_addr);
    slirpCfg.vnameserver6.s6_addr[6]  = RT_BYTE2(slirpCfg.vnameserver.s_addr);
    slirpCfg.vnameserver6.s6_addr[7]  = RT_BYTE3(slirpCfg.vnameserver.s_addr);
    pThis->Diag.Ipv6Prefix     = slirpCfg.vprefix_addr6;
    pThis->Diag.Ipv6Host       = slirpCfg.vhost6;
    pThis->Diag.Ipv6NameServer = slirpCfg.vnameserver6;

    slirpCfg.vdnssearch = NULL;
    slirpCfg.vdomainname = NULL;
    slirpCfg.aRealNameservers = NULL;
    slirpCfg.cRealNameservers = 0;

    /* Pull Bind IP for outgoing traffic (if applicable). */
    char szTmpBindIp[32]; /* xxx.xxx.xxx.xxx/yy */
    rc = pDrvIns->pHlpR3->pfnCFGMQueryString(pCfg, "BindIP", szTmpBindIp, sizeof(szNetwork));
    if (rc != VERR_CFGM_VALUE_NOT_FOUND)
    {
        RTNETADDRIPV4 mOutboundAddr;
        int iPrefixLength;
        rc = RTNetStrToIPv4Cidr(szTmpBindIp, &mOutboundAddr, &iPrefixLength);
        AssertLogRelRCReturn(rc, rc);
        slirpCfg.outbound_addr = (struct sockaddr_in *)RTMemAlloc(sizeof(struct sockaddr_in));
        slirpCfg.outbound_addr->sin_addr = RTNetIPv4AddrToInAddr(&mOutboundAddr);
        slirpCfg.outbound_addr->sin_family = AF_INET;
        slirpCfg.outbound_addr->sin_port = 0;
        pThis->Diag.fBindIp = true;
        pThis->Diag.BindIp = mOutboundAddr;
    }
    else
        rc = VINF_SUCCESS;

    AssertLogRelRCReturn(rc, rc);

    /** @todo r=jack: add IPv6 support for BindIP. */

    /*
     * Slirp Callbacks
     */
    static SlirpCb slirpCallbacks = { 0 };

    slirpCallbacks.send_packet = drvNAT_SendPacketCb;
    slirpCallbacks.guest_error = drvNAT_GuestErrorCb;
    slirpCallbacks.clock_get_ns = drvNAT_ClockGetNsCb;
    slirpCallbacks.timer_new = drvNAT_TimerNewCb;
    slirpCallbacks.timer_free = drvNAT_TimerFreeCb;
    slirpCallbacks.timer_mod = drvNAT_TimerModCb;
    slirpCallbacks.notify = drvNAT_NotifyCb;
    slirpCallbacks.init_completed = NULL;
    slirpCallbacks.timer_new_opaque = NULL;
    slirpCallbacks.register_poll_socket = drvNAT_RegisterPoll;
    slirpCallbacks.unregister_poll_socket = drvNAT_UnregisterPoll;

    /*
     * Initialize Slirp
     */
    Slirp * const pSlirp = slirp_new(/* cfg */ &slirpCfg, /* callbacks */ &slirpCallbacks, /* opaque */ pThis);
    if (!pSlirp)
        return PDMDRV_SET_ERROR(pDrvIns, VERR_INTERNAL_ERROR_4,
                                N_("Configuration error: libslirp failed to create new instance - probably misconfiguration"));

    pThis->pSlirp = pSlirp;

    /* Let libslirp write all the guest TCP data that is pending to the host socket in one go instead of
       once for every guest packet (the host send() is the bottleneck of guest to host transfers). */
    rc = pDrvIns->pHlpR3->pfnCFGMQueryBoolDef(pCfg, "DeferHostWrites", &pThis->fDeferHostWrites, false);
    AssertLogRelRCReturn(rc, rc);
    if (pThis->fDeferHostWrites)
        LogRel(("NAT#%d: Deferring writes to host TCP sockets while more guest frames are queued\n", pDrvIns->iInstance));

    rc = drvNATConfigPreDefPortForward(pDrvIns->iInstance, pThis, pCfg, &Network);
    AssertLogRelRCReturn(rc, rc);

    rc = PDMDrvHlpSSMRegisterLoadDone(pDrvIns, NULL);
    AssertLogRelRCReturn(rc, rc);

    rc = RTReqQueueCreate(&pThis->hSlirpReqQueue);
    AssertLogRelRCReturn(rc, rc);

    rc = RTReqQueueCreate(&pThis->hRecvReqQueue);
    AssertLogRelRCReturn(rc, rc);

    rc = PDMDrvHlpThreadCreate(pDrvIns, &pThis->pRecvThread, pThis, drvNATRecv,
                               drvNATRecvWakeup, 256 * _1K, RTTHREADTYPE_IO, "NATRX");
    AssertRCReturn(rc, rc);

    rc = RTSemEventCreate(&pThis->hEventRecv);
    AssertRCReturn(rc, rc);

    rc = RTCritSectInit(&pThis->DevAccessLock);
    AssertRCReturn(rc, rc);

    rc = RTCritSectInit(&pThis->XmitLock);
    AssertRCReturn(rc, rc);

    char szTmp[128];
    RTStrPrintf(szTmp, sizeof(szTmp), "nat%d", pDrvIns->iInstance);
    PDMDrvHlpDBGFInfoRegister(pDrvIns, szTmp,
                               "NAT info. Accepts 'help' for more information.",
                               drvNATInfo);
    drvNATDiagEvent(pThis, "NAT diagnostics initialized: network %RTnaipv4/%RTnaipv4, MTU %u",
                    pThis->Diag.Ipv4Network.u, pThis->Diag.Ipv4Netmask.u, pThis->Diag.u32Mtu);

#ifdef VBOX_WITH_STATISTICS
# define DRV_PROFILE_COUNTER(name, dsc)     REGISTER_COUNTER(name, pThis, STAMTYPE_PROFILE, STAMUNIT_TICKS_PER_CALL, dsc)
# define DRV_COUNTING_COUNTER(name, dsc)    REGISTER_COUNTER(name, pThis, STAMTYPE_COUNTER, STAMUNIT_COUNT,          dsc)
# include "slirp/counters.h"
#endif

#ifdef RT_OS_WINDOWS
    /* Create the wakeup socket pair (idx=0 is write, idx=1 is read). */
    pThis->ahWakeupSockPair[0] = INVALID_SOCKET;
    pThis->ahWakeupSockPair[1] = INVALID_SOCKET;
    rc = RTWinSocketPair(AF_INET, SOCK_DGRAM, 0, pThis->ahWakeupSockPair);
    AssertRCReturn(rc, rc);
#else
    /* Create the control pipe. */
    rc = RTPipeCreate(&pThis->hPipeRead, &pThis->hPipeWrite, 0 /*fFlags*/);
    AssertRCReturn(rc, rc);
#endif
    /* initalize the notifier counter */
    pThis->cbWakeupNotifs = 0;

    rc = PDMDrvHlpThreadCreate(pDrvIns, &pThis->pSlirpThread, pThis, drvNATAsyncIoThread,
                               drvNATAsyncIoWakeup, 256 * _1K, RTTHREADTYPE_IO, "NAT");
    AssertRCReturn(rc, rc);

    pThis->enmLinkState = pThis->enmLinkStateWant = PDMNETWORKLINKSTATE_UP;

    return rc;
}

/**
 * NAT network transport driver registration record.
 */
const PDMDRVREG g_DrvNAT =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "NAT",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "NAT Transport Driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_NETWORK,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVNAT),
    /* pfnConstruct */
    drvNATConstruct,
    /* pfnDestruct */
    drvNATDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
