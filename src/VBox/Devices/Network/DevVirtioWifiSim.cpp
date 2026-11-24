/* $Id$ */
/** @file
 * VBox network devices - Virtio simulated Wi-Fi radio medium (virtio 1.x, device id 0x105d).
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
 * SPDX-License-Identifier: GPL-3.0-only
 */

/** @page pg_dev_virtio_wifi_sim    Virtio Simulated Wi-Fi Radio Medium
 *
 * Some guests expect a Wi-Fi adapter to exist and misbehave without one, for
 * instance by taking the whole network stack down when configuring Wi-Fi
 * fails.  This device gives Linux guests one without emulating any radio
 * hardware: it is the virtio transport of the Linux simulated 802.11 radio
 * driver (mac80211_hwsim, virtio device type 29).  The guest kernel loads that
 * driver because the device is there, and the driver creates its simulated
 * radios and wireless interfaces by itself.
 *
 * The driver sends every frame its radios transmit to the device (queue 0)
 * and expects the device to hand back the frames the radios should receive
 * (queue 1).  This device is an empty radio medium: transmitted frames are
 * taken and dropped, and nothing is ever received.  So the guest has working
 * Wi-Fi interfaces that never find a network, and keeps using its wired
 * adapter.
 *
 * The frames are never looked at, so nothing the guest puts into them is
 * interpreted by the host.  The device has no feature bits and no meaningful
 * configuration space, and runs in ring-3 only; the queues are served on the
 * EMT that delivers the notification, which is cheap because nothing is
 * copied.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_VIRTIO
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/stam.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/string.h>

#include "../VirtIO/VirtioCore.h"
#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** The virtio device type of the mac80211_hwsim transport (VIRTIO_ID_MAC80211_HWSIM). */
#define VWIFISIM_DEVICE_TYPE            29
/** The PCI device id of a modern virtio device of that type. */
#define VWIFISIM_PCI_DEVICE_ID          UINT16_C(0x105d)
/** PCI class: network controller, other. */
#define VWIFISIM_PCI_CLASS_BASE         0x02
#define VWIFISIM_PCI_CLASS_SUB          0x80
#define VWIFISIM_PCI_CLASS_PROG         0x00

/** The queue the driver sends transmitted frames on. */
#define VWIFISIM_VIRTQ_TX               0
/** The queue the driver receives frames on (never used by this device). */
#define VWIFISIM_VIRTQ_RX               1
/** The number of queues. */
#define VWIFISIM_VIRTQ_COUNT            2

/** The saved state version. */
#define VWIFISIM_SAVED_STATE_VERSION    1


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * The shared device state.
 */
typedef struct VWIFISIM
{
    /** The core virtio state (must be first). */
    VIRTIOCORE              Virtio;
    /** The device specific configuration space.  The device type has none, but
     * the virtio core wants one; it reads as zero. */
    uint32_t                u32Config;
    /** Set while the driver is ready. */
    bool volatile           fVirtioReady;
    /** The instance name for the release log. */
    char                    szInstance[16];
    /** The number of transmitted frames taken from the driver and dropped. */
    STAMCOUNTER             StatTxDropped;
} VWIFISIM;
/** Pointer to the shared device state. */
typedef VWIFISIM *PVWIFISIM;

/**
 * The ring-3 device state.
 */
typedef struct VWIFISIMCC
{
    /** The core virtio ring-3 state (must be first). */
    VIRTIOCORECC            Virtio;
    /** The device instance. */
    PPDMDEVINS              pDevIns;
    /** Keeps a device reset by the driver out of the transmit queue while
     * frames are taken from it.  The device critical section PDM holds around
     * the callbacks already does that; this states the requirement locally. */
    RTCRITSECT              CritSectTx;
} VWIFISIMCC;
/** Pointer to the ring-3 device state. */
typedef VWIFISIMCC *PVWIFISIMCC;


/**
 * Takes all the frames the driver queued for transmission and gives the
 * buffers back unused.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 */
static void vwifiSimR3TxDrain(PPDMDEVINS pDevIns, PVWIFISIM pThis, PVWIFISIMCC pThisCC)
{
    /* One buffer object serves all the frames: fetching a frame sets it up afresh. */
    PVIRTQBUF pVirtqBuf = virtioCoreR3VirtqBufAlloc();
    if (!pVirtqBuf)
        return;

    RTCritSectEnter(&pThisCC->CritSectTx);
    /* The queue size bounds the work one notification can cause. */
    bool fPut = false;
    for (uint32_t cLeft = VIRTQ_SIZE; cLeft > 0; cLeft--)
    {
        if (   !ASMAtomicReadBool(&pThis->fVirtioReady)
            || !virtioCoreIsVirtqEnabled(&pThis->Virtio, VWIFISIM_VIRTQ_TX))
            break;
        int rc = virtioCoreR3VirtqAvailBufGet(pDevIns, &pThis->Virtio, VWIFISIM_VIRTQ_TX, pVirtqBuf, true /*fRemove*/);
        if (RT_SUCCESS(rc))
            rc = virtioCoreR3VirtqUsedBufPut(pDevIns, &pThis->Virtio, VWIFISIM_VIRTQ_TX, (PRTSGBUF)NULL, pVirtqBuf,
                                             true /*fFence*/);
        if (RT_FAILURE(rc)) /* VERR_NOT_AVAILABLE: the queue is empty. */
        {
            if (rc != VERR_NOT_AVAILABLE)
                LogRelMax(16, ("%s: Taking a frame from the transmit queue failed: %Rrc\n", pThis->szInstance, rc));
            break;
        }
        fPut = true;
        STAM_REL_COUNTER_INC(&pThis->StatTxDropped);
    }
    if (fPut)
        virtioCoreVirtqUsedRingSync(pDevIns, &pThis->Virtio, VWIFISIM_VIRTQ_TX);
    RTCritSectLeave(&pThisCC->CritSectTx);

    virtioCoreR3VirtqBufRelease(&pThis->Virtio, pVirtqBuf);
}


/*********************************************************************************************************************************
*   VirtioCore callbacks                                                                                                         *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{VIRTIOCORER3,pfnVirtqNotified}
 */
static DECLCALLBACK(void) vwifiSimR3VirtqNotified(PPDMDEVINS pDevIns, PVIRTIOCORE pVirtio, uint16_t uVirtq)
{
    PVWIFISIM   pThis   = RT_FROM_MEMBER(pVirtio, VWIFISIM, Virtio);
    PVWIFISIMCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVWIFISIMCC);
    /* The receive queue only gets buffers this device never fills. */
    if (uVirtq == VWIFISIM_VIRTQ_TX)
        vwifiSimR3TxDrain(pDevIns, pThis, pThisCC);
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnStatusChanged}
 */
static DECLCALLBACK(void) vwifiSimR3StatusChanged(PVIRTIOCORE pVirtio, PVIRTIOCORECC pVirtioCC, uint32_t fDriverOk)
{
    PVWIFISIM   pThis   = RT_FROM_MEMBER(pVirtio, VWIFISIM, Virtio);
    PVWIFISIMCC pThisCC = RT_FROM_MEMBER(pVirtioCC, VWIFISIMCC, Virtio);

    if (fDriverOk)
    {
        /* VirtioCore does not mask the driver features with the offered ones. */
        pVirtio->uDriverFeatures &= pVirtio->uDeviceFeatures;
        ASMAtomicWriteBool(&pThis->fVirtioReady, true);
        LogRelMax(16, ("%s: Driver ready\n", pThis->szInstance));
    }
    else
    {
        /* VirtioCore resets the queue state right after this returns: nobody may be in the queue then. */
        ASMAtomicWriteBool(&pThis->fVirtioReady, false);
        if (RTCritSectIsInitialized(&pThisCC->CritSectTx))
        {
            RTCritSectEnter(&pThisCC->CritSectTx);
            RTCritSectLeave(&pThisCC->CritSectTx);
        }
    }
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnDevCapRead}
 */
static DECLCALLBACK(int) vwifiSimR3DevCapRead(PPDMDEVINS pDevIns, uint32_t offCap, void *pvBuf, uint32_t cbToRead)
{
    RT_NOREF(pDevIns, offCap);
    memset(pvBuf, 0, cbToRead);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnDevCapWrite}
 */
static DECLCALLBACK(int) vwifiSimR3DevCapWrite(PPDMDEVINS pDevIns, uint32_t offCap, const void *pvBuf, uint32_t cbToWrite)
{
    RT_NOREF(pDevIns, offCap, pvBuf, cbToWrite);
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Saved state                                                                                                                  *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 */
static DECLCALLBACK(int) vwifiSimR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PVWIFISIM pThis = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    return virtioCoreR3SaveExec(&pThis->Virtio, pDevIns->pHlpR3, pSSM, VWIFISIM_SAVED_STATE_VERSION, VWIFISIM_VIRTQ_COUNT);
}


/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) vwifiSimR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PVWIFISIM pThis = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    AssertReturn(uPass == SSM_PASS_FINAL, VERR_SSM_UNEXPECTED_PASS);
    if (uVersion != VWIFISIM_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    int rc = virtioCoreR3ModernDeviceLoadExec(&pThis->Virtio, pDevIns->pHlpR3, pSSM, uVersion, VWIFISIM_SAVED_STATE_VERSION,
                                              VWIFISIM_VIRTQ_COUNT);
    if (RT_SUCCESS(rc))
        ASMAtomicWriteBool(&pThis->fVirtioReady, RT_BOOL(pThis->Virtio.fDeviceStatus & VIRTIO_STATUS_DRIVER_OK));
    return rc;
}


/*********************************************************************************************************************************
*   Device interface                                                                                                             *
*********************************************************************************************************************************/

/**
 * @interface_method_impl{PDMDEVREGR3,pfnReset}
 */
static DECLCALLBACK(void) vwifiSimR3Reset(PPDMDEVINS pDevIns)
{
    PVWIFISIM   pThis   = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    PVWIFISIMCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVWIFISIMCC);
    virtioCoreR3ResetDevice(pDevIns, &pThis->Virtio, &pThisCC->Virtio);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnSuspend}
 */
static DECLCALLBACK(void) vwifiSimR3Suspend(PPDMDEVINS pDevIns)
{
    PVWIFISIM pThis = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    virtioCoreR3VmStateChanged(&pThis->Virtio, kvirtIoVmStateChangedSuspend);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnPowerOff}
 */
static DECLCALLBACK(void) vwifiSimR3PowerOff(PPDMDEVINS pDevIns)
{
    PVWIFISIM pThis = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    virtioCoreR3VmStateChanged(&pThis->Virtio, kvirtIoVmStateChangedPowerOff);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnResume}
 */
static DECLCALLBACK(void) vwifiSimR3Resume(PPDMDEVINS pDevIns)
{
    PVWIFISIM   pThis   = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    PVWIFISIMCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVWIFISIMCC);
    virtioCoreR3VmStateChanged(&pThis->Virtio, kvirtIoVmStateChangedResume);
    /* Frames queued while the VM was not running. */
    vwifiSimR3TxDrain(pDevIns, pThis, pThisCC);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnDestruct}
 */
static DECLCALLBACK(int) vwifiSimR3Destruct(PPDMDEVINS pDevIns)
{
    PDMDEV_CHECK_VERSIONS_RETURN_QUIET(pDevIns);
    PVWIFISIM   pThis   = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    PVWIFISIMCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVWIFISIMCC);

    virtioCoreR3Term(pDevIns, &pThis->Virtio, &pThisCC->Virtio);
    if (RTCritSectIsInitialized(&pThisCC->CritSectTx))
        RTCritSectDelete(&pThisCC->CritSectTx);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnConstruct}
 */
static DECLCALLBACK(int) vwifiSimR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PVWIFISIM   pThis   = PDMDEVINS_2_DATA(pDevIns, PVWIFISIM);
    PVWIFISIMCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVWIFISIMCC);
    RT_NOREF(pCfg);

    pThisCC->pDevIns = pDevIns;
    RTStrPrintf(pThis->szInstance, sizeof(pThis->szInstance), "VIRTIOWIFI%d", iInstance);

    /* "MmioBase" and "Irq" are read by the virtio core. */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "MmioBase|Irq", "");

    int rc = RTCritSectInit(&pThisCC->CritSectTx);
    AssertRCReturn(rc, rc);

    /*
     * The virtio core.  The callbacks must be set before the init call.
     */
    pThisCC->Virtio.pfnVirtqNotified = vwifiSimR3VirtqNotified;
    pThisCC->Virtio.pfnStatusChanged = vwifiSimR3StatusChanged;
    pThisCC->Virtio.pfnDevCapRead    = vwifiSimR3DevCapRead;
    pThisCC->Virtio.pfnDevCapWrite   = vwifiSimR3DevCapWrite;

    VIRTIOPCIPARAMS VirtioPciParams;
    RT_ZERO(VirtioPciParams);
    VirtioPciParams.uDeviceId      = VWIFISIM_PCI_DEVICE_ID;
    VirtioPciParams.uClassBase     = VWIFISIM_PCI_CLASS_BASE;
    VirtioPciParams.uClassSub      = VWIFISIM_PCI_CLASS_SUB;
    VirtioPciParams.uClassProg     = VWIFISIM_PCI_CLASS_PROG;
    VirtioPciParams.uSubsystemId   = VWIFISIM_PCI_DEVICE_ID;
    VirtioPciParams.uInterruptLine = 0x00;
    VirtioPciParams.uInterruptPin  = 0x01;
    VirtioPciParams.uDeviceType    = VWIFISIM_DEVICE_TYPE;

    rc = virtioCoreR3Init(pDevIns, &pThis->Virtio, &pThisCC->Virtio, &VirtioPciParams, pThis->szInstance,
                          0 /*fDevSpecificFeatures*/, 0 /*fOfferLegacy*/, &pThis->u32Config, sizeof(pThis->u32Config));
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-wifi-sim: Failed to initialize the virtio core"));

    static const char * const s_apszVirtqNames[VWIFISIM_VIRTQ_COUNT] = { "tx", "rx" };
    for (uint16_t uVirtq = 0; uVirtq < VWIFISIM_VIRTQ_COUNT; uVirtq++)
    {
        rc = virtioCoreR3VirtqAttach(&pThis->Virtio, uVirtq, s_apszVirtqNames[uVirtq]);
        if (RT_FAILURE(rc))
            return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-wifi-sim: Failed to attach a virtqueue"));
    }

    rc = PDMDevHlpSSMRegister(pDevIns, VWIFISIM_SAVED_STATE_VERSION, sizeof(*pThis), vwifiSimR3SaveExec, vwifiSimR3LoadExec);
    AssertRCReturn(rc, rc);

    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatTxDropped, STAMTYPE_COUNTER, "TxDropped", STAMUNIT_OCCURENCES,
                          "Transmitted frames taken from the driver and dropped.");

    LogRel(("%s: Simulated Wi-Fi radio medium for Linux guests (mac80211_hwsim), no networks in range\n", pThis->szInstance));
    return VINF_SUCCESS;
}


/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceVirtioWifiSim =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "virtio-wifi-sim",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_NETWORK,
    /* .cMaxInstances = */          1,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(VWIFISIM),
    /* .cbInstanceCC = */           sizeof(VWIFISIMCC),
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         1,
    /* .cMaxMsixVectors = */        VBOX_MSIX_MAX_ENTRIES,
    /* .pszDescription = */         "Virtio simulated Wi-Fi radio medium (mac80211_hwsim), no networks in range.\n",
#if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           vwifiSimR3Construct,
    /* .pfnDestruct = */            vwifiSimR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               vwifiSimR3Reset,
    /* .pfnSuspend = */             vwifiSimR3Suspend,
    /* .pfnResume = */              vwifiSimR3Resume,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            vwifiSimR3PowerOff,
    /* .pfnSoftReset = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RING0)
    /* .pfnEarlyConstruct = */      NULL,
    /* .pfnConstruct = */           NULL,
    /* .pfnDestruct = */            NULL,
    /* .pfnFinalDestruct = */       NULL,
    /* .pfnRequest = */             NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RC)
    /* .pfnConstruct = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#else
# error "Not in IN_RING3, IN_RING0 or IN_RC!"
#endif
    /* .u32VersionEnd = */          PDM_DEVREG_VERSION
};
