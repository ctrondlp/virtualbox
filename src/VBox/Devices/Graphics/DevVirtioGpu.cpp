/* $Id$ */
/** @file
 * VBox graphics devices - Virtio GPU, 2D only (virtio 1.x, device id 0x1050).
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

/** @page pg_dev_virtio_gpu    Virtio GPU Device (2D)
 *
 * @section sec_dev_virtio_gpu_notes    NOTES
 *
 * Design:
 *  - A modern-only (virtio 1.x) PCI device with the PCI device id 0x1050, built
 *    on the VirtioCore transport and running in ring-3 only.  The PCI class is
 *    "display controller, other" (0x0380) and not VGA (0x0300), because the
 *    OVMF VirtioGpuDxe driver does not bind to a virtio GPU claiming to be VGA.
 *  - No device specific feature bits are offered (no virgl, EDID, blob
 *    resources or context init), so only the 2D command set is implemented:
 *    GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_UNREF, SET_SCANOUT,
 *    RESOURCE_FLUSH, TRANSFER_TO_HOST_2D, RESOURCE_ATTACH_BACKING and
 *    RESOURCE_DETACH_BACKING.  Everything else gets VIRTIO_GPU_RESP_ERR_UNSPEC.
 *  - Every resource has a host shadow copy, 32 bpp and always stored as
 *    B8G8R8X8 (the layout Main expects).  The other supported 32-bit formats
 *    are converted while TRANSFER_TO_HOST_2D copies the guest backing pages
 *    into the shadow.  RESOURCE_FLUSH marks the displayed part dirty.
 *  - There is one worker thread per virtqueue.  The control queue worker owns
 *    the resources: it is the only thread creating, changing or freeing them
 *    (a device reset only asks it to drop them).  It never holds the state
 *    lock while touching guest memory, because a guest memory access from a
 *    non-EMT thread may have to be serviced by an EMT.
 *  - The display uses the "copy into Main's buffer" model of the QEMU RAM
 *    framebuffer: Main's refresh timer calls pfnUpdateDisplay, which resizes
 *    Main's framebuffer when the scanout size changed and copies the dirty
 *    rectangle from the shadow into PDMIDISPLAYCONNECTOR::pbData.  The
 *    connector is never called with the state lock held.
 *
 * Locking:
 *  - The device critical section (VirtioCore MMIO handlers, PDM callbacks)
 *    protects the virtio configuration space; it may be followed by
 *    VIRTIOGPU::CritSectState, never the other way around.
 *  - VIRTIOGPU::CritSectState protects the resource list, the scanout and the
 *    display bookkeeping.  It is only held for short host memory operations.
 *  - VIRTIOGPUWORKERR3::CritSectVirtq serializes the used ring updates of a
 *    queue (the VirtioCore used ring bookkeeping is not thread safe).
 *
 * Limitations:
 *  - One scanout only.  No hardware cursor: cursor queue commands are
 *    completed without effect, so the guest has to draw its own cursor.
 *  - No 3D, EDID, blob resources or capsets.
 *  - Saving the VM state (and thus teleporting) is refused.
 *  - VirtioCore only uses INTx interrupts, not MSI-X.
 *  - Resources are limited to 8192x8192 pixels, 512 resources, 256 MB of host
 *    shadow memory per VM and 16384 backing entries per resource.
 *  - Little-endian hosts only: the wire structures are used as they are.
 *
 * @section sec_dev_virtio_gpu_config   Configuration
 *
 *  - "MonitorCount": number of scanouts, must be 1 for now (default 1).
 *  - "MmioBase", "Irq": use the virtio-over-MMIO transport instead of PCI
 *    (read by VirtioCore).
 *  - LUN#0: the display driver (MainDisplay).  Without one the device runs
 *    headless.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_VIRTIO
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/pdmcritsect.h>
#include <VBox/vmm/pdmifs.h>
#include <VBox/vmm/stam.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <VBox/msi.h>
#include <VBox/sup.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/list.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/uuid.h>

#include "../VirtIO/VirtioCore.h"
#include "DevVirtioGpu.h"
#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** The saved state version (saving is refused, see vgpuR3SavePrep). */
#define VIRTIOGPU_SAVED_STATE_VERSION           UINT32_C(1)
/** The device specific features offered to the driver: none, 2D only. */
#define VIRTIOGPU_FEATURES_OFFERED              UINT64_C(0)

/** @name PCI class code: display controller, other (deliberately not VGA).
 * @{ */
#define VIRTIOGPU_PCI_CLASS_BASE                0x03
#define VIRTIOGPU_PCI_CLASS_SUB                 0x80
#define VIRTIOGPU_PCI_CLASS_PROG                0x00
/** @} */

/** The maximum number of scanouts (monitors) currently supported. */
#define VIRTIOGPU_MAX_MONITORS                  1
/** Bytes per pixel of all supported resource formats and of the shadow. */
#define VIRTIOGPU_BPP_BYTES                     4
/** The maximum width and height of a resource in pixels. */
#define VIRTIOGPU_MAX_DIMENSION                 8192
/** The maximum number of resources per VM. */
#define VIRTIOGPU_MAX_RESOURCES                 512
/** The maximum amount of host memory for resource shadows per VM. */
#define VIRTIOGPU_MAX_SHADOW_MEMORY             (256 * _1M)
/** The maximum size of the guest backing of one resource. */
#define VIRTIOGPU_MAX_BACKING_SIZE              (256 * _1M)
/** The maximum number of backing entries of one resource (QEMU uses the same limit). */
#define VIRTIOGPU_MAX_BACKING_ENTRIES           16384
/** The maximum number of backing entries of all resources together. */
#define VIRTIOGPU_MAX_TOTAL_BACKING_ENTRIES     65536
/** The preferred width reported before Main sends a mode hint. */
#define VIRTIOGPU_DEFAULT_WIDTH                 1024
/** The preferred height reported before Main sends a mode hint. */
#define VIRTIOGPU_DEFAULT_HEIGHT                768

AssertCompile((uint64_t)VIRTIOGPU_MAX_DIMENSION * VIRTIOGPU_MAX_DIMENSION * VIRTIOGPU_BPP_BYTES <= VIRTIOGPU_MAX_SHADOW_MEMORY);
AssertCompile(VIRTIOGPU_MAX_BACKING_ENTRIES <= VIRTIOGPU_MAX_TOTAL_BACKING_ENTRIES);
AssertCompile(VIRTIOGPU_MAX_MONITORS <= VIRTIOGPU_MAX_SCANOUTS);
AssertCompile(VIRTIOGPU_DEVICE_TYPE == VIRTIO_DEVICE_TYPE_GPU);
AssertCompile(VIRTIOGPU_VIRTQ_COUNT <= VIRTQ_MAX_COUNT);
/* VirtioCore maps the device specific configuration in 4 byte units. */
AssertCompile(sizeof(VIRTIOGPUCONFIG) % 4 == 0);


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * A 2D resource.
 *
 * Resources are created, changed and freed only by the control queue worker
 * (and by the destructor).  The list membership, the backing description and
 * the scanout pointer are changed while holding VIRTIOGPU::CritSectState, so
 * the display and info code can safely look at them under that lock.
 */
typedef struct VIRTIOGPURESOURCE
{
    /** Node in VIRTIOGPUR3::ListResources. */
    RTLISTNODE              Node;
    /** The resource ID chosen by the guest, never 0. */
    uint32_t                idResource;
    /** The guest format (VIRTIOGPU_FORMAT_XXX). */
    uint32_t                uFormat;
    /** The width in pixels, 1..VIRTIOGPU_MAX_DIMENSION. */
    uint32_t                cx;
    /** The height in pixels, 1..VIRTIOGPU_MAX_DIMENSION. */
    uint32_t                cy;
    /** Bytes per row of the shadow and of the guest backing (cx * 4). */
    uint32_t                cbPitch;
    /** Number of entries in paEntries, 0 when no backing is attached. */
    uint32_t                cEntries;
    /** Size of the shadow in bytes (cbPitch * cy). */
    size_t                  cbShadow;
    /** The host copy of the resource, always B8G8R8X8. */
    uint8_t                *pbShadow;
    /** The validated guest backing entries (host copy). */
    VIRTIOGPUMEMENTRY      *paEntries;
    /** The sum of the sizes of all backing entries. */
    uint64_t                cbBacking;
} VIRTIOGPURESOURCE;
/** Pointer to a 2D resource. */
typedef VIRTIOGPURESOURCE *PVIRTIOGPURESOURCE;

/**
 * Position in the backing entry list of a resource, so that reading a
 * rectangle row by row does not rescan the list for every row.
 */
typedef struct VIRTIOGPUBACKINGCURSOR
{
    /** Index of the current entry. */
    uint32_t                iEntry;
    /** The backing offset at which the current entry starts. */
    uint64_t                offEntry;
} VIRTIOGPUBACKINGCURSOR;
/** Pointer to a backing cursor. */
typedef VIRTIOGPUBACKINGCURSOR *PVIRTIOGPUBACKINGCURSOR;

/**
 * Host copy of a control queue request, large enough for every fixed size
 * request.  All variants start with the common header.
 */
typedef union VIRTIOGPUCTRLREQ
{
    VIRTIOGPUCTRLHDR                Hdr;
    VIRTIOGPURESOURCECREATE2D       Create2d;
    VIRTIOGPURESOURCEUNREF          Unref;
    VIRTIOGPUSETSCANOUT             SetScanout;
    VIRTIOGPURESOURCEFLUSH          Flush;
    VIRTIOGPUTRANSFERTOHOST2D       Transfer2d;
    VIRTIOGPURESOURCEATTACHBACKING  AttachBacking;
    VIRTIOGPURESOURCEDETACHBACKING  DetachBacking;
} VIRTIOGPUCTRLREQ;

/**
 * Worker thread state, shared edition.
 */
typedef struct VIRTIOGPUWORKER
{
    /** The semaphore the worker sleeps on. */
    SUPSEMEVENT             hEvtProcess;
    /** Set while the worker is (about to go) sleeping. */
    bool volatile           fSleeping;
    /** Set when the worker was notified and must look at its queue again. */
    bool volatile           fNotified;
    /** Explicit padding. */
    bool                    afPadding[6];
} VIRTIOGPUWORKER;
/** Pointer to the shared worker state. */
typedef VIRTIOGPUWORKER *PVIRTIOGPUWORKER;

/**
 * Worker thread state, ring-3 edition.
 */
typedef struct VIRTIOGPUWORKERR3
{
    /** The worker thread. */
    R3PTRTYPE(PPDMTHREAD)   pThread;
    /** Serializes the used ring updates of the queue. */
    RTCRITSECT              CritSectVirtq;
} VIRTIOGPUWORKERR3;
/** Pointer to the ring-3 worker state. */
typedef VIRTIOGPUWORKERR3 *PVIRTIOGPUWORKERR3;

/**
 * The virtio GPU device state, shared edition.
 *
 * @extends     VIRTIOCORE
 */
typedef struct VIRTIOGPU
{
    /** The core virtio state, must be first. */
    VIRTIOCORE              Virtio;
    /** The device specific configuration space (protected by the device critical section). */
    VIRTIOGPUCONFIG         Config;
    /** Per queue worker state. */
    VIRTIOGPUWORKER         aWorkers[VIRTIOGPU_VIRTQ_COUNT];
    /** The instance name. */
    char                    szInstance[16];
    /** Set while the driver is ready (DRIVER_OK). */
    bool volatile           fVirtioReady;
    /** Set while the VM is being suspended or powered off: workers take no new buffers. */
    bool volatile           fQuiescing;
    /** Set by a device reset: the control worker must free all resources. */
    bool volatile           fDropResources;
    /** Explicit padding. */
    bool                    afPadding0[1];
    /** Number of workers currently inside their processing section. */
    uint32_t volatile       cActiveCmds;
    /** What the device is quiescing for. */
    VIRTIOVMSTATECHANGED    enmQuiescingFor;
    /** The display refresh interval requested by Main, 0 if stopped. */
    uint32_t volatile       cMilliesRefreshInterval;
    /** The negotiated features (masked with the offered ones), latched at DRIVER_OK. */
    uint64_t                fNegotiatedFeatures;
    /** The display refresh timer (TMCLOCK_REAL, no critical section). */
    TMTIMERHANDLE           hRefreshTimer;
    /** Protects the resources, the scanout and the display state, see the NOTES. */
    PDMCRITSECT             CritSectState;

    /** @name Statistics.
     * @{ */
    STAMCOUNTER             StatCtrlCmds;
    STAMCOUNTER             StatCursorCmds;
    STAMCOUNTER             StatCmdErrors;
    STAMCOUNTER             StatTransferBytes;
    /** @} */
} VIRTIOGPU;
/** Pointer to the shared state of the virtio GPU device. */
typedef VIRTIOGPU *PVIRTIOGPU;

/**
 * The virtio GPU device state, ring-3 edition.
 *
 * @extends     VIRTIOCORER3
 */
typedef struct VIRTIOGPUR3
{
    /** The core virtio ring-3 state, must be first. */
    VIRTIOCORER3            Virtio;
    /** The device instance. */
    PPDMDEVINSR3            pDevIns;
    /** The base interface. */
    PDMIBASE                IBase;
    /** The display port interface. */
    PDMIDISPLAYPORT         IPort;
    /** The base interface of the attached display driver. */
    R3PTRTYPE(PPDMIBASE)    pDrvBase;
    /** The display connector of the attached display driver, NULL if headless. */
    R3PTRTYPE(PPDMIDISPLAYCONNECTOR) pDrv;
    /** Per queue worker state. */
    VIRTIOGPUWORKERR3       aWorkers[VIRTIOGPU_VIRTQ_COUNT];

    /** @name Resources: owned by the control worker, changed under CritSectState.
     * @{ */
    /** The resource list (VIRTIOGPURESOURCE). */
    RTLISTANCHOR            ListResources;
    /** Number of resources in the list. */
    uint32_t                cResources;
    /** Number of backing entries of all resources. */
    uint32_t                cTotalBackingEntries;
    /** Host memory used by all resource shadows. */
    size_t                  cbShadowTotal;
    /** @} */

    /** @name Display state, protected by CritSectState.
     * @{ */
    /** The resource shown on scanout 0, NULL if the scanout is disabled. */
    PVIRTIOGPURESOURCE      pScanoutRes;
    /** The part of pScanoutRes shown, inside the resource; its size is the display size. */
    VIRTIOGPURECT           ScanoutRect;
    /** Incremented whenever the display size or the enabled state changes. */
    uint32_t                uScanoutGen;
    /** The uScanoutGen value Main's framebuffer was last resized for. */
    uint32_t                uScanoutGenMain;
    /** The dirty rectangle in scanout coordinates (start inclusive, end exclusive). */
    uint32_t                xDirty;
    uint32_t                yDirty;
    uint32_t                xDirtyEnd;
    uint32_t                yDirtyEnd;
    /** The preferred width reported to the guest. */
    uint32_t                cxHint;
    /** The preferred height reported to the guest. */
    uint32_t                cyHint;
    /** Whether Main currently shows a real mode (we only blank after one). */
    bool                    fMainHasMode;
    /** Whether Main wants us to render into PDMIDISPLAYCONNECTOR::pbData. */
    bool                    fRenderVRAM;
    /** @} */
} VIRTIOGPUR3;
/** Pointer to the ring-3 state of the virtio GPU device. */
typedef VIRTIOGPUR3 *PVIRTIOGPUR3;

/** The device instance data for the current context (ring-3 only). */
typedef VIRTIOGPUR3 VIRTIOGPUCC;
/** Pointer to the device instance data for the current context. */
typedef VIRTIOGPUR3 *PVIRTIOGPUCC;


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Enters the state lock.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 */
DECLINLINE(void) vgpuR3StateLock(PPDMDEVINS pDevIns, PVIRTIOGPU pThis)
{
    int const rcLock = PDMDevHlpCritSectEnter(pDevIns, &pThis->CritSectState, VERR_SEM_BUSY);
    PDM_CRITSECT_RELEASE_ASSERT_RC_DEV(pDevIns, &pThis->CritSectState, rcLock);
}


/**
 * Leaves the state lock.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 */
DECLINLINE(void) vgpuR3StateUnlock(PPDMDEVINS pDevIns, PVIRTIOGPU pThis)
{
    PDMDevHlpCritSectLeave(pDevIns, &pThis->CritSectState);
}


/**
 * Checks that a rectangle lies inside a cx by cy area, without overflowing.
 *
 * @returns true if inside (an empty rectangle at the edge counts as inside).
 * @param   pRect       The rectangle (guest supplied).
 * @param   cx          The width of the area.
 * @param   cy          The height of the area.
 */
DECLINLINE(bool) vgpuR3RectIsInside(PCVIRTIOGPURECT pRect, uint32_t cx, uint32_t cy)
{
    return pRect->x  <= cx
        && pRect->cx <= cx - pRect->x
        && pRect->y  <= cy
        && pRect->cy <= cy - pRect->y;
}


/**
 * Empties the dirty rectangle.
 *
 * @param   pThisCC     The ring-3 device state.  Caller holds the state lock.
 */
DECLINLINE(void) vgpuR3DirtyClear(PVIRTIOGPUCC pThisCC)
{
    pThisCC->xDirty    = UINT32_MAX;
    pThisCC->yDirty    = UINT32_MAX;
    pThisCC->xDirtyEnd = 0;
    pThisCC->yDirtyEnd = 0;
}


/**
 * Marks the whole scanout dirty.
 *
 * @param   pThisCC     The ring-3 device state.  Caller holds the state lock.
 */
DECLINLINE(void) vgpuR3DirtyAll(PVIRTIOGPUCC pThisCC)
{
    pThisCC->xDirty    = 0;
    pThisCC->yDirty    = 0;
    pThisCC->xDirtyEnd = pThisCC->ScanoutRect.cx;
    pThisCC->yDirtyEnd = pThisCC->ScanoutRect.cy;
}


/**
 * Adds a rectangle to the dirty rectangle.
 *
 * @param   pThisCC     The ring-3 device state.  Caller holds the state lock.
 * @param   x           Left edge in scanout coordinates.
 * @param   y           Top edge in scanout coordinates.
 * @param   cx          Width, inside the scanout.
 * @param   cy          Height, inside the scanout.
 */
static void vgpuR3DirtyAdd(PVIRTIOGPUCC pThisCC, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    if (!cx || !cy)
        return;
    pThisCC->xDirty    = RT_MIN(pThisCC->xDirty, x);
    pThisCC->yDirty    = RT_MIN(pThisCC->yDirty, y);
    pThisCC->xDirtyEnd = RT_MAX(pThisCC->xDirtyEnd, x + cx);
    pThisCC->yDirtyEnd = RT_MAX(pThisCC->yDirtyEnd, y + cy);
}


/**
 * Disables the scanout, making the next display update blank Main's screen.
 *
 * @param   pThisCC     The ring-3 device state.  Caller holds the state lock.
 */
static void vgpuR3ScanoutDisableLocked(PVIRTIOGPUCC pThisCC)
{
    if (pThisCC->pScanoutRes)
    {
        pThisCC->pScanoutRes = NULL;
        RT_ZERO(pThisCC->ScanoutRect);
        pThisCC->uScanoutGen++;
        vgpuR3DirtyClear(pThisCC);
    }
}


/**
 * Copies a rectangle of the current scanout into a 32 bpp buffer.
 *
 * @param   pThisCC     The ring-3 device state.  Caller holds the state lock.
 * @param   x           Left edge in scanout coordinates.
 * @param   y           Top edge in scanout coordinates.
 * @param   cx          Width, the rectangle must be inside the scanout.
 * @param   cy          Height, the rectangle must be inside the scanout.
 * @param   pbDst       The destination; it holds the whole scanout, origin at (0,0).
 * @param   cbDstLine   Bytes per destination line, at least ScanoutRect.cx * 4.
 */
static void vgpuR3ScanoutCopyLocked(PVIRTIOGPUCC pThisCC, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy,
                                    uint8_t *pbDst, size_t cbDstLine)
{
    PVIRTIOGPURESOURCE const pRes = pThisCC->pScanoutRes;
    AssertReturnVoid(pRes);
    AssertReturnVoid(x <= pThisCC->ScanoutRect.cx && cx <= pThisCC->ScanoutRect.cx - x);
    AssertReturnVoid(y <= pThisCC->ScanoutRect.cy && cy <= pThisCC->ScanoutRect.cy - y);
    Assert(cbDstLine >= (size_t)pThisCC->ScanoutRect.cx * VIRTIOGPU_BPP_BYTES);

    /* ScanoutRect was checked to lie inside the resource by SET_SCANOUT. */
    uint32_t const xSrc  = pThisCC->ScanoutRect.x + x;
    uint32_t const ySrc  = pThisCC->ScanoutRect.y + y;
    size_t const   cbRow = (size_t)cx * VIRTIOGPU_BPP_BYTES;
    for (uint32_t iRow = 0; iRow < cy; iRow++)
        memcpy(pbDst + (size_t)(y + iRow) * cbDstLine + (size_t)x * VIRTIOGPU_BPP_BYTES,
               pRes->pbShadow + (size_t)(ySrc + iRow) * pRes->cbPitch + (size_t)xSrc * VIRTIOGPU_BPP_BYTES,
               cbRow);
}


/**
 * Checks whether a resource format is supported.
 *
 * @returns true if supported.
 * @param   uFormat     The guest format (VIRTIOGPU_FORMAT_XXX).
 */
static bool vgpuR3IsFormatSupported(uint32_t uFormat)
{
    switch (uFormat)
    {
        case VIRTIOGPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIOGPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIOGPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIOGPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIOGPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIOGPU_FORMAT_X8B8G8R8_UNORM:
        case VIRTIOGPU_FORMAT_A8B8G8R8_UNORM:
        case VIRTIOGPU_FORMAT_R8G8B8X8_UNORM:
            return true;
        default:
            return false;
    }
}


/**
 * Converts one row of pixels in place from a guest format to B8G8R8X8.
 *
 * The format names give the byte order in memory, e.g. X8R8G8B8 is the bytes
 * X, R, G, B.  Little-endian host assumed.
 *
 * @param   uFormat     The guest format (VIRTIOGPU_FORMAT_XXX), supported.
 * @param   pbRow       The row, 4 byte aligned.
 * @param   cPixels     Number of pixels in the row.
 */
static void vgpuR3ConvertRow(uint32_t uFormat, uint8_t *pbRow, uint32_t cPixels)
{
    uint32_t *pu32 = (uint32_t *)pbRow;
    switch (uFormat)
    {
        case VIRTIOGPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIOGPU_FORMAT_B8G8R8X8_UNORM:
            break; /* Already what Main wants. */

        case VIRTIOGPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIOGPU_FORMAT_R8G8B8X8_UNORM:
            /* R,G,B,A -> B,G,R,A: swap the red and blue bytes. */
            for (uint32_t i = 0; i < cPixels; i++)
            {
                uint32_t const u = pu32[i];
                pu32[i] = (u & UINT32_C(0xff00ff00)) | ((u & UINT32_C(0x000000ff)) << 16) | ((u >> 16) & UINT32_C(0x000000ff));
            }
            break;

        case VIRTIOGPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIOGPU_FORMAT_X8R8G8B8_UNORM:
            /* A,R,G,B -> B,G,R,A: reverse the bytes. */
            for (uint32_t i = 0; i < cPixels; i++)
                pu32[i] = ASMByteSwapU32(pu32[i]);
            break;

        case VIRTIOGPU_FORMAT_A8B8G8R8_UNORM:
        case VIRTIOGPU_FORMAT_X8B8G8R8_UNORM:
            /* A,B,G,R -> B,G,R,A: move the first byte to the end. */
            for (uint32_t i = 0; i < cPixels; i++)
                pu32[i] = ASMRotateRightU32(pu32[i], 8);
            break;

        default:
            AssertFailed(); /* Rejected by RESOURCE_CREATE_2D. */
            break;
    }
}


/**
 * Frees a resource that is no longer reachable by any other thread.
 *
 * @param   pRes        The resource.
 */
static void vgpuR3ResourceFree(PVIRTIOGPURESOURCE pRes)
{
    RTMemFree(pRes->paEntries);
    RTMemFree(pRes->pbShadow);
    RTMemFree(pRes);
}


/**
 * Looks up a resource.
 *
 * Only called by the control worker, which is the only thread changing the
 * list, so no lock is needed.
 *
 * @returns The resource or NULL if not found.
 * @param   pThisCC     The ring-3 device state.
 * @param   idResource  The resource ID (guest supplied).
 */
static PVIRTIOGPURESOURCE vgpuR3ResourceFind(PVIRTIOGPUCC pThisCC, uint32_t idResource)
{
    PVIRTIOGPURESOURCE pRes;
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        if (pRes->idResource == idResource)
            return pRes;
    }
    return NULL;
}


/**
 * Frees all resources and disables the scanout.
 *
 * Called by the control worker after a device reset and by the destructor
 * (when no worker is left).  Tolerates a half constructed instance.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 */
static void vgpuR3ResourcesDestroyAll(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC)
{
    RTLISTANCHOR ListFree;
    RTListInit(&ListFree);

    bool const fLock = PDMDevHlpCritSectIsInitialized(pDevIns, &pThis->CritSectState);
    if (fLock)
        vgpuR3StateLock(pDevIns, pThis);
    vgpuR3ScanoutDisableLocked(pThisCC);
    RTListMove(&ListFree, &pThisCC->ListResources);
    pThisCC->cResources           = 0;
    pThisCC->cTotalBackingEntries = 0;
    pThisCC->cbShadowTotal        = 0;
    if (fLock)
        vgpuR3StateUnlock(pDevIns, pThis);

    PVIRTIOGPURESOURCE pRes, pResNext;
    RTListForEachSafe(&ListFree, pRes, pResNext, VIRTIOGPURESOURCE, Node)
    {
        RTListNodeRemove(&pRes->Node);
        vgpuR3ResourceFree(pRes);
    }
}


/**
 * Reads bytes from the guest backing of a resource.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pRes        The resource; the caller checked off + cb <= cbBacking.
 * @param   pCursor     Position cache, start with zeros.
 * @param   off         The backing offset to read from.
 * @param   pbDst       Where to store the bytes.
 * @param   cb          Number of bytes to read.
 */
static int vgpuR3BackingRead(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPURESOURCE pRes, PVIRTIOGPUBACKINGCURSOR pCursor,
                             uint64_t off, uint8_t *pbDst, size_t cb)
{
    if (off < pCursor->offEntry)
    {
        pCursor->iEntry   = 0;
        pCursor->offEntry = 0;
    }

    while (cb > 0)
    {
        /* Advance to the entry containing 'off' (this also skips empty entries). */
        while (   pCursor->iEntry < pRes->cEntries
               && off - pCursor->offEntry >= pRes->paEntries[pCursor->iEntry].cb)
        {
            pCursor->offEntry += pRes->paEntries[pCursor->iEntry].cb;
            pCursor->iEntry++;
        }
        AssertReturn(pCursor->iEntry < pRes->cEntries, VERR_INTERNAL_ERROR_3);

        VIRTIOGPUMEMENTRY const *pEntry = &pRes->paEntries[pCursor->iEntry];
        uint64_t const offInEntry = off - pCursor->offEntry;
        size_t const   cbThis     = (size_t)RT_MIN((uint64_t)cb, pEntry->cb - offInEntry);
        /* GCPhysAddr + cb was checked for wrap-around when the backing was attached. */
        int rc = virtioCoreGCPhysRead(&pThis->Virtio, pDevIns, pEntry->GCPhysAddr + offInEntry, pbDst, cbThis);
        if (RT_FAILURE(rc))
            return rc;

        off   += cbThis;
        pbDst += cbThis;
        cb    -= cbThis;
    }
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Command processing                                                                                                           *
*********************************************************************************************************************************/

/**
 * Returns the size of a fixed size control request.
 *
 * @returns Request size in bytes, 0 if the command is not implemented.
 * @param   uType       The command type (guest supplied).
 */
static size_t vgpuR3CtrlCmdSize(uint32_t uType)
{
    switch (uType)
    {
        case VIRTIOGPU_CMD_GET_DISPLAY_INFO:        return sizeof(VIRTIOGPUCTRLHDR);
        case VIRTIOGPU_CMD_RESOURCE_CREATE_2D:      return sizeof(VIRTIOGPURESOURCECREATE2D);
        case VIRTIOGPU_CMD_RESOURCE_UNREF:          return sizeof(VIRTIOGPURESOURCEUNREF);
        case VIRTIOGPU_CMD_SET_SCANOUT:             return sizeof(VIRTIOGPUSETSCANOUT);
        case VIRTIOGPU_CMD_RESOURCE_FLUSH:          return sizeof(VIRTIOGPURESOURCEFLUSH);
        case VIRTIOGPU_CMD_TRANSFER_TO_HOST_2D:     return sizeof(VIRTIOGPUTRANSFERTOHOST2D);
        case VIRTIOGPU_CMD_RESOURCE_ATTACH_BACKING: return sizeof(VIRTIOGPURESOURCEATTACHBACKING);
        case VIRTIOGPU_CMD_RESOURCE_DETACH_BACKING: return sizeof(VIRTIOGPURESOURCEDETACHBACKING);
        default:                                    return 0;
    }
}


/**
 * VIRTIOGPU_CMD_GET_DISPLAY_INFO.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pResp       The response to fill in (zeroed, header filled in by the caller).
 */
static uint32_t vgpuR3CmdGetDisplayInfo(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                        PVIRTIOGPURESPDISPLAYINFO pResp)
{
    vgpuR3StateLock(pDevIns, pThis);
    for (uint32_t i = 0; i < pThis->Config.cScanouts && i < RT_ELEMENTS(pResp->aPModes); i++)
    {
        pResp->aPModes[i].r.x      = 0;
        pResp->aPModes[i].r.y      = 0;
        pResp->aPModes[i].r.cx     = pThisCC->cxHint;
        pResp->aPModes[i].r.cy     = pThisCC->cyHint;
        pResp->aPModes[i].fEnabled = 1;
        pResp->aPModes[i].fFlags   = 0;
    }
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_DISPLAY_INFO;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_CREATE_2D.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdResourceCreate2d(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                          PCVIRTIOGPURESOURCECREATE2D pReq)
{
    if (!pReq->idResource || vgpuR3ResourceFind(pThisCC, pReq->idResource))
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!vgpuR3IsFormatSupported(pReq->uFormat))
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    if (   !pReq->cx || pReq->cx > VIRTIOGPU_MAX_DIMENSION
        || !pReq->cy || pReq->cy > VIRTIOGPU_MAX_DIMENSION)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    if (pThisCC->cResources >= VIRTIOGPU_MAX_RESOURCES)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;

    uint32_t const cbPitch  = pReq->cx * VIRTIOGPU_BPP_BYTES;  /* <= 32 KB */
    size_t const   cbShadow = (size_t)cbPitch * pReq->cy;      /* <= 256 MB */
    if (cbShadow > VIRTIOGPU_MAX_SHADOW_MEMORY - pThisCC->cbShadowTotal)
    {
        LogRelMax(16, ("%s: Resource %RU32 (%RU32x%RU32) exceeds the shadow memory limit\n",
                       pThis->szInstance, pReq->idResource, pReq->cx, pReq->cy));
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    }

    PVIRTIOGPURESOURCE pRes = (PVIRTIOGPURESOURCE)RTMemAllocZ(sizeof(*pRes));
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    pRes->pbShadow = (uint8_t *)RTMemAllocZ(cbShadow);
    if (!pRes->pbShadow)
    {
        RTMemFree(pRes);
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    }
    pRes->idResource = pReq->idResource;
    pRes->uFormat    = pReq->uFormat;
    pRes->cx         = pReq->cx;
    pRes->cy         = pReq->cy;
    pRes->cbPitch    = cbPitch;
    pRes->cbShadow   = cbShadow;

    vgpuR3StateLock(pDevIns, pThis);
    RTListAppend(&pThisCC->ListResources, &pRes->Node);
    pThisCC->cResources++;
    pThisCC->cbShadowTotal += cbShadow;
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_UNREF.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdResourceUnref(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                       PCVIRTIOGPURESOURCEUNREF pReq)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;

    vgpuR3StateLock(pDevIns, pThis);
    if (pThisCC->pScanoutRes == pRes)
        vgpuR3ScanoutDisableLocked(pThisCC);
    RTListNodeRemove(&pRes->Node);
    pThisCC->cResources--;
    pThisCC->cbShadowTotal        -= pRes->cbShadow;
    pThisCC->cTotalBackingEntries -= pRes->cEntries;
    vgpuR3StateUnlock(pDevIns, pThis);

    vgpuR3ResourceFree(pRes);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_SET_SCANOUT.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdSetScanout(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PCVIRTIOGPUSETSCANOUT pReq)
{
    if (pReq->idScanout >= pThis->Config.cScanouts)
        return VIRTIOGPU_RESP_ERR_INVALID_SCANOUT_ID;

    /* Resource 0 or an empty rectangle disables the scanout. */
    if (!pReq->idResource || !pReq->r.cx || !pReq->r.cy)
    {
        vgpuR3StateLock(pDevIns, pThis);
        vgpuR3ScanoutDisableLocked(pThisCC);
        vgpuR3StateUnlock(pDevIns, pThis);
        return VIRTIOGPU_RESP_OK_NODATA;
    }

    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!vgpuR3RectIsInside(&pReq->r, pRes->cx, pRes->cy))
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    vgpuR3StateLock(pDevIns, pThis);
    /* Main only needs a resize when the displayed size changes, not when the
       guest flips between equally sized resources. */
    bool const fSizeChanged = !pThisCC->pScanoutRes
                           || pThisCC->ScanoutRect.cx != pReq->r.cx
                           || pThisCC->ScanoutRect.cy != pReq->r.cy;
    pThisCC->pScanoutRes = pRes;
    pThisCC->ScanoutRect = pReq->r;
    if (fSizeChanged)
        pThisCC->uScanoutGen++;
    vgpuR3DirtyAll(pThisCC);
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_FLUSH.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdResourceFlush(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                       PCVIRTIOGPURESOURCEFLUSH pReq)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!vgpuR3RectIsInside(&pReq->r, pRes->cx, pRes->cy))
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    vgpuR3StateLock(pDevIns, pThis);
    if (pThisCC->pScanoutRes == pRes)
    {
        /* Intersect with the displayed part; all values are <= VIRTIOGPU_MAX_DIMENSION * 2. */
        PCVIRTIOGPURECT const pScan = &pThisCC->ScanoutRect;
        uint32_t const x0 = RT_MAX(pReq->r.x, pScan->x);
        uint32_t const y0 = RT_MAX(pReq->r.y, pScan->y);
        uint32_t const x1 = RT_MIN(pReq->r.x + pReq->r.cx, pScan->x + pScan->cx);
        uint32_t const y1 = RT_MIN(pReq->r.y + pReq->r.cy, pScan->y + pScan->cy);
        if (x0 < x1 && y0 < y1)
            vgpuR3DirtyAdd(pThisCC, x0 - pScan->x, y0 - pScan->y, x1 - x0, y1 - y0);
    }
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_TRANSFER_TO_HOST_2D.
 *
 * Copies a rectangle from the guest backing into the shadow.  The backing is
 * laid out like the resource (pitch cx * 4) and offBacking is the backing
 * offset of the first pixel of the rectangle.
 *
 * Runs without the state lock while touching guest memory, see the NOTES.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdTransferToHost2d(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                          PCVIRTIOGPUTRANSFERTOHOST2D pReq)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!vgpuR3RectIsInside(&pReq->r, pRes->cx, pRes->cy))
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    if (!pReq->r.cx || !pReq->r.cy)
        return VIRTIOGPU_RESP_OK_NODATA;
    if (!pRes->cEntries)
        return VIRTIOGPU_RESP_ERR_UNSPEC;

    /* The bytes touched: (cy - 1) full rows plus one rectangle row.  At most ~256 MB, no overflow. */
    uint64_t const cbSpan = (uint64_t)(pReq->r.cy - 1) * pRes->cbPitch + (uint64_t)pReq->r.cx * VIRTIOGPU_BPP_BYTES;
    if (   pReq->offBacking > pRes->cbBacking
        || cbSpan > pRes->cbBacking - pReq->offBacking)
    {
        LogRelMax(16, ("%s: TRANSFER_TO_HOST_2D outside the backing of resource %RU32 (offset %#RX64, %RU64 bytes, backing %RU64)\n",
                       pThis->szInstance, pRes->idResource, pReq->offBacking, cbSpan, pRes->cbBacking));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    RT_UNTRUSTED_VALIDATED_FENCE();

    VIRTIOGPUBACKINGCURSOR Cursor = { 0, 0 };
    size_t const cbRow = (size_t)pReq->r.cx * VIRTIOGPU_BPP_BYTES;
    for (uint32_t iRow = 0; iRow < pReq->r.cy; iRow++)
    {
        uint8_t *pbDst = pRes->pbShadow + (size_t)(pReq->r.y + iRow) * pRes->cbPitch + (size_t)pReq->r.x * VIRTIOGPU_BPP_BYTES;
        int rc = vgpuR3BackingRead(pDevIns, pThis, pRes, &Cursor, pReq->offBacking + (uint64_t)iRow * pRes->cbPitch,
                                   pbDst, cbRow);
        if (RT_FAILURE(rc))
        {
            LogRelMax(16, ("%s: Reading the backing of resource %RU32 failed: %Rrc\n", pThis->szInstance, pRes->idResource, rc));
            return VIRTIOGPU_RESP_ERR_UNSPEC;
        }
        vgpuR3ConvertRow(pRes->uFormat, pbDst, pReq->r.cx);
    }
    STAM_REL_COUNTER_ADD(&pThis->StatTransferBytes, cbRow * pReq->r.cy);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_ATTACH_BACKING.
 *
 * The request is followed by cEntries VIRTIOGPUMEMENTRY structures in the
 * rest of the descriptor chain.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pVirtqBuf   The descriptor chain, positioned after the request.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdAttachBacking(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTQBUF pVirtqBuf,
                                       PCVIRTIOGPURESOURCEATTACHBACKING pReq)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (pRes->cEntries)
        return VIRTIOGPU_RESP_ERR_UNSPEC; /* Already has a backing. */

    uint32_t const cEntries = pReq->cEntries;
    if (!cEntries || cEntries > VIRTIOGPU_MAX_BACKING_ENTRIES)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    if (cEntries > VIRTIOGPU_MAX_TOTAL_BACKING_ENTRIES - pThisCC->cTotalBackingEntries)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;

    size_t const cbEntries = (size_t)cEntries * sizeof(VIRTIOGPUMEMENTRY); /* <= 256 KB */
    if (pVirtqBuf->cbPhysSend < cbEntries)
    {
        LogRelMax(16, ("%s: ATTACH_BACKING with %RU32 entries but only %zu bytes left in the request\n",
                       pThis->szInstance, cEntries, pVirtqBuf->cbPhysSend));
        return VIRTIOGPU_RESP_ERR_UNSPEC;
    }

    VIRTIOGPUMEMENTRY *paEntries = (VIRTIOGPUMEMENTRY *)RTMemAlloc(cbEntries);
    if (!paEntries)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, paEntries, cbEntries);

    uint64_t cbBacking = 0;
    for (uint32_t i = 0; i < cEntries; i++)
    {
        uint64_t const GCPhys = paEntries[i].GCPhysAddr;
        uint32_t const cb     = paEntries[i].cb;
        if (   cb > UINT64_MAX - GCPhys
            || cb > VIRTIOGPU_MAX_BACKING_SIZE - cbBacking)
        {
            LogRelMax(16, ("%s: Invalid backing entry %RU32 for resource %RU32 (%RGp LB %#RX32)\n",
                           pThis->szInstance, i, pRes->idResource, (RTGCPHYS)GCPhys, cb));
            RTMemFree(paEntries);
            return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
        }
        cbBacking += cb;
    }
    RT_UNTRUSTED_VALIDATED_FENCE();

    vgpuR3StateLock(pDevIns, pThis);
    pRes->paEntries  = paEntries;
    pRes->cEntries   = cEntries;
    pRes->cbBacking  = cbBacking;
    pThisCC->cTotalBackingEntries += cEntries;
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_DETACH_BACKING.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdDetachBacking(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                       PCVIRTIOGPURESOURCEDETACHBACKING pReq)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!pRes->cEntries)
        return VIRTIOGPU_RESP_ERR_UNSPEC;

    vgpuR3StateLock(pDevIns, pThis);
    VIRTIOGPUMEMENTRY *paEntries = pRes->paEntries;
    pThisCC->cTotalBackingEntries -= pRes->cEntries;
    pRes->paEntries = NULL;
    pRes->cEntries  = 0;
    pRes->cbBacking = 0;
    vgpuR3StateUnlock(pDevIns, pThis);

    RTMemFree(paEntries);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * Writes a response into the device writable segments of a descriptor chain.
 *
 * The segments are walked here instead of using the VirtioCore copy helpers,
 * so that any segment layout the guest hands us (including empty segments)
 * is handled gracefully.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pSgBuf      The return segments; their total size is at least cb.
 * @param   pv          The response.
 * @param   cb          The response size.
 */
static int vgpuR3ReturnBufWrite(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PCVIRTIOSGBUF pSgBuf, void *pv, size_t cb)
{
    uint8_t *pb = (uint8_t *)pv;
    for (unsigned iSeg = 0; iSeg < pSgBuf->cSegs && cb > 0; iSeg++)
    {
        size_t const cbThis = RT_MIN(pSgBuf->paSegs[iSeg].cbSeg, cb);
        if (cbThis)
        {
            int rc = virtioCoreGCPhysWrite(&pThis->Virtio, pDevIns, pSgBuf->paSegs[iSeg].GCPhys, pb, cbThis);
            if (RT_FAILURE(rc))
                return rc;
            pb += cbThis;
            cb -= cbThis;
        }
    }
    return cb == 0 ? VINF_SUCCESS : VERR_BUFFER_OVERFLOW;
}


/**
 * Completes a descriptor chain: writes the response (if any) and puts the
 * chain on the used ring.
 *
 * The response is only written when the return buffer can hold all of it;
 * otherwise the chain is completed with a used length of zero.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   uVirtq      The queue the chain came from.
 * @param   pVirtqBuf   The descriptor chain.
 * @param   pvResp      The response, NULL if none.
 * @param   cbResp      The response size, 0 if none.
 */
static void vgpuR3CmdComplete(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, uint16_t uVirtq,
                              PVIRTQBUF pVirtqBuf, void *pvResp, size_t cbResp)
{
    size_t cbUsed = 0;
    if (cbResp)
    {
        if (   pVirtqBuf->pSgPhysReturn
            && pVirtqBuf->cbPhysReturn >= cbResp)
        {
            int rc = vgpuR3ReturnBufWrite(pDevIns, pThis, pVirtqBuf->pSgPhysReturn, pvResp, cbResp);
            if (RT_SUCCESS(rc))
                cbUsed = cbResp;
            else
                LogRelMax(16, ("%s: Writing the response failed: %Rrc\n", pThis->szInstance, rc));
        }
        else
            LogRelMax(16, ("%s: Response buffer too small (%zu bytes, %zu needed)\n",
                           pThis->szInstance, pVirtqBuf->cbPhysReturn, cbResp));
    }

    PRTCRITSECT pCritSect = &pThisCC->aWorkers[uVirtq].CritSectVirtq;
    RTCritSectEnter(pCritSect);
    /* The driver may have reset the device while the command ran; its rings are gone then. */
    if (pThis->Virtio.fDeviceStatus & VIRTIO_STATUS_DRIVER_OK)
    {
        int rc;
        if (cbUsed)
            rc = virtioCoreR3VirtqUsedBufPut(pDevIns, &pThis->Virtio, uVirtq, 0 /*cb*/, pvResp, pVirtqBuf,
                                             cbUsed /*cbEnqueue*/, true /*fFence*/);
        else
            rc = virtioCoreR3VirtqUsedBufPut(pDevIns, &pThis->Virtio, uVirtq, (PRTSGBUF)NULL, pVirtqBuf, true /*fFence*/);
        if (RT_SUCCESS(rc))
            virtioCoreVirtqUsedRingSync(pDevIns, &pThis->Virtio, uVirtq);
    }
    RTCritSectLeave(pCritSect);
}


/**
 * Processes one control queue descriptor chain.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pVirtqBuf   The descriptor chain.
 */
static void vgpuR3CtrlCmdProcess(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTQBUF pVirtqBuf)
{
    VIRTIOGPUCTRLREQ uReq;
    RT_ZERO(uReq);
    VIRTIOGPURESPDISPLAYINFO Resp; /* The largest response; all of them start with the header. */
    RT_ZERO(Resp);
    size_t   cbResp    = sizeof(VIRTIOGPUCTRLHDR);
    uint32_t uRespType = VIRTIOGPU_RESP_ERR_UNSPEC;

    STAM_REL_COUNTER_INC(&pThis->StatCtrlCmds);

    /* The VirtioCore drain helper never reads more than the chain holds
       (pVirtqBuf->cbPhysSend), but the sizes must be checked before draining. */
    if (pVirtqBuf->cbPhysSend >= sizeof(uReq.Hdr))
    {
        virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, &uReq.Hdr, sizeof(uReq.Hdr));

        size_t const cbReq = vgpuR3CtrlCmdSize(uReq.Hdr.uType);
        Assert(cbReq <= sizeof(uReq));
        if (!cbReq)
            Log(("%s: Unsupported command %#RX32\n", pThis->szInstance, uReq.Hdr.uType));
        else if (pVirtqBuf->cbPhysSend < cbReq - sizeof(uReq.Hdr))
            LogRelMax(16, ("%s: Truncated command %#RX32 (%zu bytes left, %zu needed)\n",
                           pThis->szInstance, uReq.Hdr.uType, pVirtqBuf->cbPhysSend, cbReq - sizeof(uReq.Hdr)));
        else
        {
            if (cbReq > sizeof(uReq.Hdr))
                virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, (uint8_t *)&uReq + sizeof(uReq.Hdr),
                                          cbReq - sizeof(uReq.Hdr));

            switch (uReq.Hdr.uType)
            {
                case VIRTIOGPU_CMD_GET_DISPLAY_INFO:
                    uRespType = vgpuR3CmdGetDisplayInfo(pDevIns, pThis, pThisCC, &Resp);
                    cbResp    = sizeof(Resp);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_CREATE_2D:
                    uRespType = vgpuR3CmdResourceCreate2d(pDevIns, pThis, pThisCC, &uReq.Create2d);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_UNREF:
                    uRespType = vgpuR3CmdResourceUnref(pDevIns, pThis, pThisCC, &uReq.Unref);
                    break;
                case VIRTIOGPU_CMD_SET_SCANOUT:
                    uRespType = vgpuR3CmdSetScanout(pDevIns, pThis, pThisCC, &uReq.SetScanout);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_FLUSH:
                    uRespType = vgpuR3CmdResourceFlush(pDevIns, pThis, pThisCC, &uReq.Flush);
                    break;
                case VIRTIOGPU_CMD_TRANSFER_TO_HOST_2D:
                    uRespType = vgpuR3CmdTransferToHost2d(pDevIns, pThis, pThisCC, &uReq.Transfer2d);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_ATTACH_BACKING:
                    uRespType = vgpuR3CmdAttachBacking(pDevIns, pThis, pThisCC, pVirtqBuf, &uReq.AttachBacking);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_DETACH_BACKING:
                    uRespType = vgpuR3CmdDetachBacking(pDevIns, pThis, pThisCC, &uReq.DetachBacking);
                    break;
                default:
                    AssertFailed(); /* vgpuR3CtrlCmdSize and this switch disagree. */
                    break;
            }
        }
    }
    else
        LogRelMax(16, ("%s: Control request without a header (%zu bytes)\n", pThis->szInstance, pVirtqBuf->cbPhysSend));

    if (uRespType >= VIRTIOGPU_RESP_ERR_UNSPEC)
    {
        STAM_REL_COUNTER_INC(&pThis->StatCmdErrors);
        cbResp = sizeof(VIRTIOGPUCTRLHDR);
        Log(("%s: Command %#RX32 failed: %#RX32\n", pThis->szInstance, uReq.Hdr.uType, uRespType));
    }

    /* Fenced commands must echo the fence (the EDK2 driver relies on it); we
       always complete synchronously, so the fence is signalled with the response. */
    Resp.Hdr.uType = uRespType;
    if (uReq.Hdr.fFlags & VIRTIOGPU_FLAG_FENCE)
    {
        Resp.Hdr.fFlags   = uReq.Hdr.fFlags & (VIRTIOGPU_FLAG_FENCE | VIRTIOGPU_FLAG_INFO_RING_IDX);
        Resp.Hdr.uFenceId = uReq.Hdr.uFenceId;
        Resp.Hdr.uCtxId   = uReq.Hdr.uCtxId;
        Resp.Hdr.uRingIdx = uReq.Hdr.uRingIdx;
    }

    vgpuR3CmdComplete(pDevIns, pThis, pThisCC, VIRTIOGPU_VIRTQ_CONTROL, pVirtqBuf, &Resp, cbResp);
}


/**
 * Processes one cursor queue descriptor chain.
 *
 * There is no hardware cursor, so the command is ignored.  Cursor commands
 * have no response; the chain is just handed back.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pVirtqBuf   The descriptor chain.
 */
static void vgpuR3CursorCmdProcess(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTQBUF pVirtqBuf)
{
    STAM_REL_COUNTER_INC(&pThis->StatCursorCmds);
    vgpuR3CmdComplete(pDevIns, pThis, pThisCC, VIRTIOGPU_VIRTQ_CURSOR, pVirtqBuf, NULL, 0);
}


/*********************************************************************************************************************************
*   Worker threads                                                                                                               *
*********************************************************************************************************************************/

/**
 * Wakes up the worker of a queue if it sleeps.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   uVirtq      The queue number, < VIRTIOGPU_VIRTQ_COUNT.
 */
static void vgpuR3WakeWorker(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, uint16_t uVirtq)
{
    PVIRTIOGPUWORKER pWorker = &pThis->aWorkers[uVirtq];
    if (   !ASMAtomicXchgBool(&pWorker->fNotified, true)
        && ASMAtomicReadBool(&pWorker->fSleeping)
        && pWorker->hEvtProcess != NIL_SUPSEMEVENT)
    {
        int rc = PDMDevHlpSUPSemEventSignal(pDevIns, pWorker->hEvtProcess);
        AssertRC(rc);
    }
}


/**
 * Checks whether a worker may take buffers from its queue.
 *
 * @returns true if so.
 * @param   pThis       The shared device state.
 * @param   uVirtq      The queue number.
 */
static bool vgpuR3IsVirtqReady(PVIRTIOGPU pThis, uint16_t uVirtq)
{
    return ASMAtomicReadBool(&pThis->fVirtioReady)
        && !ASMAtomicReadBool(&pThis->fQuiescing)
        && (pThis->Virtio.fDeviceStatus & VIRTIO_STATUS_DRIVER_OK)
        && virtioCoreIsVirtqEnabled(&pThis->Virtio, uVirtq);
}


/**
 * @callback_method_impl{FNPDMTHREADWAKEUPDEV}
 */
static DECLCALLBACK(int) vgpuR3WorkerWakeUp(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PVIRTIOGPU      pThis  = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    uintptr_t const uVirtq = (uintptr_t)pThread->pvUser;
    AssertReturn(uVirtq < RT_ELEMENTS(pThis->aWorkers), VERR_INTERNAL_ERROR);
    if (pThis->aWorkers[uVirtq].hEvtProcess == NIL_SUPSEMEVENT)
        return VINF_SUCCESS;
    return PDMDevHlpSUPSemEventSignal(pDevIns, pThis->aWorkers[uVirtq].hEvtProcess);
}


/**
 * @callback_method_impl{FNPDMTHREADDEV}
 */
static DECLCALLBACK(int) vgpuR3WorkerThread(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    uint16_t const   uVirtq  = (uint16_t)(uintptr_t)pThread->pvUser;
    PVIRTIOGPU       pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC     pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    AssertReturn(uVirtq < VIRTIOGPU_VIRTQ_COUNT, VERR_INTERNAL_ERROR);
    PVIRTIOGPUWORKER pWorker = &pThis->aWorkers[uVirtq];

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        /* After a device reset the control worker, which owns the resources, frees them. */
        if (   uVirtq == VIRTIOGPU_VIRTQ_CONTROL
            && ASMAtomicXchgBool(&pThis->fDropResources, false))
            vgpuR3ResourcesDestroyAll(pDevIns, pThis, pThisCC);

        /* Count ourselves as active before checking fQuiescing, so the quiesce
           code either sees us or we see it. */
        bool fProcessed = false;
        ASMAtomicIncU32(&pThis->cActiveCmds);
        if (vgpuR3IsVirtqReady(pThis, uVirtq))
        {
            PVIRTQBUF pVirtqBuf = virtioCoreR3VirtqBufAlloc();
            if (pVirtqBuf)
            {
                int rc = virtioCoreR3VirtqAvailBufGet(pDevIns, &pThis->Virtio, uVirtq, pVirtqBuf, true /*fRemove*/);
                if (RT_SUCCESS(rc))
                {
                    if (uVirtq == VIRTIOGPU_VIRTQ_CONTROL)
                        vgpuR3CtrlCmdProcess(pDevIns, pThis, pThisCC, pVirtqBuf);
                    else
                        vgpuR3CursorCmdProcess(pDevIns, pThis, pThisCC, pVirtqBuf);
                    fProcessed = true;
                }
                else if (rc != VERR_NOT_AVAILABLE) /* VERR_NOT_AVAILABLE: queue empty. */
                    LogRelMax(16, ("%s: Getting a buffer from queue %u failed: %Rrc\n", pThis->szInstance, uVirtq, rc));
                virtioCoreR3VirtqBufRelease(&pThis->Virtio, pVirtqBuf);
            }
            else
                LogRelMax(16, ("%s: Out of memory for a queue buffer\n", pThis->szInstance));
        }
        if (   ASMAtomicDecU32(&pThis->cActiveCmds) == 0
            && ASMAtomicReadBool(&pThis->fQuiescing))
            PDMDevHlpAsyncNotificationCompleted(pDevIns);
        if (fProcessed)
            continue;

        /* Nothing (more) to do, or something failed: sleep until notified.  The
           flags make sure a notification racing with going to sleep is not lost. */
        ASMAtomicWriteBool(&pWorker->fSleeping, true);
        if (!ASMAtomicXchgBool(&pWorker->fNotified, false))
        {
            int rc = PDMDevHlpSUPSemEventWaitNoResume(pDevIns, pWorker->hEvtProcess, RT_INDEFINITE_WAIT);
            AssertLogRelMsgReturn(RT_SUCCESS(rc) || rc == VERR_INTERRUPTED, ("%Rrc\n", rc), rc);
            ASMAtomicWriteBool(&pWorker->fNotified, false);
        }
        ASMAtomicWriteBool(&pWorker->fSleeping, false);
    }
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   VirtioCore callbacks                                                                                                         *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{VIRTIOCORER3,pfnVirtqNotified}
 */
static DECLCALLBACK(void) vgpuR3VirtqNotified(PPDMDEVINS pDevIns, PVIRTIOCORE pVirtio, uint16_t uVirtq)
{
    PVIRTIOGPU pThis = RT_FROM_MEMBER(pVirtio, VIRTIOGPU, Virtio);
    if (uVirtq < VIRTIOGPU_VIRTQ_COUNT)
        vgpuR3WakeWorker(pDevIns, pThis, uVirtq);
    else
        LogFunc(("Ignoring a notification for unused queue %u\n", uVirtq));
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnStatusChanged}
 */
static DECLCALLBACK(void) vgpuR3StatusChanged(PVIRTIOCORE pVirtio, PVIRTIOCORECC pVirtioCC, uint32_t fDriverOk)
{
    PVIRTIOGPU   pThis   = RT_FROM_MEMBER(pVirtio, VIRTIOGPU, Virtio);
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pVirtioCC, VIRTIOGPUCC, Virtio);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;

    if (fDriverOk)
    {
        /* VirtioCore does not mask the driver features with the offered ones. */
        pVirtio->uDriverFeatures   &= pVirtio->uDeviceFeatures;
        pThis->fNegotiatedFeatures  = pVirtio->uDriverFeatures;
        ASMAtomicWriteBool(&pThis->fVirtioReady, true);
        LogRelMax(16, ("%s: Driver ready, features %#RX64\n", pThis->szInstance, pThis->fNegotiatedFeatures));

        for (uint16_t uVirtq = 0; uVirtq < VIRTIOGPU_VIRTQ_COUNT; uVirtq++)
            vgpuR3WakeWorker(pDevIns, pThis, uVirtq);
    }
    else
    {
        ASMAtomicWriteBool(&pThis->fVirtioReady, false);
        pThis->fNegotiatedFeatures = 0;
        pThis->Config.fEventsRead  = 0;

        /* Blank the display now; the control worker owns the resources and frees them. */
        vgpuR3StateLock(pDevIns, pThis);
        vgpuR3ScanoutDisableLocked(pThisCC);
        vgpuR3StateUnlock(pDevIns, pThis);
        ASMAtomicWriteBool(&pThis->fDropResources, true);
        vgpuR3WakeWorker(pDevIns, pThis, VIRTIOGPU_VIRTQ_CONTROL);
    }
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnDevCapRead}
 */
static DECLCALLBACK(int) vgpuR3DevCapRead(PPDMDEVINS pDevIns, uint32_t offCap, void *pvBuf, uint32_t cbToRead)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    if (   (cbToRead == 1 || cbToRead == 2 || cbToRead == 4)
        && offCap < sizeof(pThis->Config)
        && cbToRead <= sizeof(pThis->Config) - offCap)
        memcpy(pvBuf, (uint8_t const *)&pThis->Config + offCap, cbToRead);
    else
    {
        LogFunc(("Bad config read: off=%#RX32 cb=%RU32\n", offCap, cbToRead));
        memset(pvBuf, 0, cbToRead);
    }
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{VIRTIOCORER3,pfnDevCapWrite}
 *
 * Only events_clear is writable; writing 1 bits clears them in events_read.
 */
static DECLCALLBACK(int) vgpuR3DevCapWrite(PPDMDEVINS pDevIns, uint32_t offCap, const void *pvBuf, uint32_t cbWrite)
{
    PVIRTIOGPU     pThis    = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    uint32_t const offClear = (uint32_t)RT_UOFFSETOF(VIRTIOGPUCONFIG, fEventsClear);
    if (   (cbWrite == 1 || cbWrite == 2 || cbWrite == 4)
        && offCap >= offClear
        && offCap - offClear < sizeof(uint32_t)
        && cbWrite <= sizeof(uint32_t) - (offCap - offClear))
    {
        uint32_t fClear = 0;
        memcpy((uint8_t *)&fClear + (offCap - offClear), pvBuf, cbWrite);
        pThis->Config.fEventsRead &= ~fClear;
    }
    else
        LogFunc(("Ignoring config write: off=%#RX32 cb=%RU32\n", offCap, cbWrite));
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Display                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Brings Main's framebuffer up to date.
 *
 * Resizes Main's framebuffer if the scanout size changed, then copies the
 * dirty part of the scanout into it.  The connector is never called while
 * holding the state lock (Main calls pfnSetRenderVRAM from pfnResize).
 *
 * @returns VBox status code.
 * @retval  VERR_TRY_AGAIN if a resize is pending and fFailOnResize is set.
 * @param   pDevIns         The device instance.
 * @param   pThis           The shared device state.
 * @param   pThisCC         The ring-3 device state.
 * @param   fFull           Copy the whole scanout, not only the dirty part.
 * @param   fFailOnResize   Fail instead of resizing.
 */
static int vgpuR3DisplayUpdate(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, bool fFull, bool fFailOnResize)
{
    vgpuR3StateLock(pDevIns, pThis);
    PPDMIDISPLAYCONNECTOR const pDrv = pThisCC->pDrv;
    if (!pDrv)
    {
        vgpuR3StateUnlock(pDevIns, pThis);
        return VINF_SUCCESS;
    }

    if (pThisCC->uScanoutGenMain != pThisCC->uScanoutGen)
    {
        if (fFailOnResize)
        {
            vgpuR3StateUnlock(pDevIns, pThis);
            return VERR_TRY_AGAIN;
        }
        uint32_t const uGen   = pThisCC->uScanoutGen;
        uint32_t const cx     = pThisCC->pScanoutRes ? pThisCC->ScanoutRect.cx : 0;
        uint32_t const cy     = pThisCC->pScanoutRes ? pThisCC->ScanoutRect.cy : 0;
        bool const     fBlank = !cx || !cy;
        /* Like DevVGA, only blank after a real mode was shown. */
        bool const     fCall  = !fBlank || pThisCC->fMainHasMode;
        vgpuR3StateUnlock(pDevIns, pThis);

        if (fCall)
        {
            int rc = fBlank
                   ? pDrv->pfnResize(pDrv, 0, NULL, 0, 0, 0) /* The VGA blanking convention. */
                   : pDrv->pfnResize(pDrv, 32, NULL, cx * VIRTIOGPU_BPP_BYTES, cx, cy);
            if (rc == VINF_VGA_RESIZE_IN_PROGRESS)
                return VINF_SUCCESS; /* Must not touch the connector now; retried on the next refresh. */
            if (RT_FAILURE(rc))
                LogRelMax(16, ("%s: Resizing the display to %RU32x%RU32 failed: %Rrc\n", pThis->szInstance, cx, cy, rc));
        }

        vgpuR3StateLock(pDevIns, pThis);
        pThisCC->uScanoutGenMain = uGen;
        pThisCC->fMainHasMode    = !fBlank;
        fFull = true;
    }

    /* Main only changes pbData and the geometry while rendering is disabled,
       and disabling it takes our lock, so they are stable here. */
    bool     fUpdate = false;
    uint32_t xUpd = 0, yUpd = 0, cxUpd = 0, cyUpd = 0;
    if (   pThisCC->pScanoutRes
        && pThisCC->uScanoutGenMain == pThisCC->uScanoutGen
        && pThisCC->fRenderVRAM
        && pDrv->pbData
        && pDrv->cBits == 32
        && pDrv->cx == pThisCC->ScanoutRect.cx
        && pDrv->cy == pThisCC->ScanoutRect.cy
        && pDrv->cbScanline >= pThisCC->ScanoutRect.cx * VIRTIOGPU_BPP_BYTES)
    {
        if (fFull)
            vgpuR3DirtyAll(pThisCC);
        uint32_t const xEnd = RT_MIN(pThisCC->xDirtyEnd, pThisCC->ScanoutRect.cx);
        uint32_t const yEnd = RT_MIN(pThisCC->yDirtyEnd, pThisCC->ScanoutRect.cy);
        if (pThisCC->xDirty < xEnd && pThisCC->yDirty < yEnd)
        {
            xUpd  = pThisCC->xDirty;
            yUpd  = pThisCC->yDirty;
            cxUpd = xEnd - xUpd;
            cyUpd = yEnd - yUpd;
            vgpuR3ScanoutCopyLocked(pThisCC, xUpd, yUpd, cxUpd, cyUpd, pDrv->pbData, pDrv->cbScanline);
            fUpdate = true;
        }
        vgpuR3DirtyClear(pThisCC);
    }
    vgpuR3StateUnlock(pDevIns, pThis);

    if (fUpdate)
        pDrv->pfnUpdateRect(pDrv, xUpd, yUpd, cxUpd, cyUpd);
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNTMTIMERDEV, Display refresh timer}
 */
static DECLCALLBACK(void) vgpuR3TimerRefresh(PPDMDEVINS pDevIns, TMTIMERHANDLE hTimer, void *pvUser)
{
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    RT_NOREF(pvUser);

    /* Main calls back into pfnUpdateDisplay from here. */
    PPDMIDISPLAYCONNECTOR const pDrv = pThisCC->pDrv;
    if (pDrv)
        pDrv->pfnRefresh(pDrv);

    uint32_t const cMillies = ASMAtomicReadU32(&pThis->cMilliesRefreshInterval);
    if (cMillies)
        PDMDevHlpTimerSetMillies(pDevIns, hTimer, cMillies);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnUpdateDisplay}
 */
static DECLCALLBACK(int) vgpuR3PortUpdateDisplay(PPDMIDISPLAYPORT pInterface)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    return vgpuR3DisplayUpdate(pDevIns, pThis, pThisCC, false /*fFull*/, false /*fFailOnResize*/);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnUpdateDisplayAll}
 */
static DECLCALLBACK(int) vgpuR3PortUpdateDisplayAll(PPDMIDISPLAYPORT pInterface, bool fFailOnResize)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    return vgpuR3DisplayUpdate(pDevIns, pThis, pThisCC, true /*fFull*/, fFailOnResize);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnQueryVideoMode}
 */
static DECLCALLBACK(int) vgpuR3PortQueryVideoMode(PPDMIDISPLAYPORT pInterface, uint32_t *pcBits, uint32_t *pcx, uint32_t *pcy)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    AssertPtrReturn(pcBits, VERR_INVALID_POINTER);

    vgpuR3StateLock(pDevIns, pThis);
    *pcBits = 32;
    if (pcx)
        *pcx = pThisCC->pScanoutRes ? pThisCC->ScanoutRect.cx : 0;
    if (pcy)
        *pcy = pThisCC->pScanoutRes ? pThisCC->ScanoutRect.cy : 0;
    vgpuR3StateUnlock(pDevIns, pThis);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnSetRefreshRate}
 */
static DECLCALLBACK(int) vgpuR3PortSetRefreshRate(PPDMIDISPLAYPORT pInterface, uint32_t cMilliesInterval)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);

    ASMAtomicWriteU32(&pThis->cMilliesRefreshInterval, cMilliesInterval);
    if (cMilliesInterval)
        return PDMDevHlpTimerSetMillies(pDevIns, pThis->hRefreshTimer, cMilliesInterval);
    return PDMDevHlpTimerStop(pDevIns, pThis->hRefreshTimer);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnTakeScreenshot}
 */
static DECLCALLBACK(int) vgpuR3PortTakeScreenshot(PPDMIDISPLAYPORT pInterface, uint8_t **ppbData, size_t *pcbData,
                                                  uint32_t *pcx, uint32_t *pcy)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    AssertPtrReturn(ppbData, VERR_INVALID_POINTER);
    AssertPtrReturn(pcbData, VERR_INVALID_POINTER);
    AssertPtrReturn(pcx, VERR_INVALID_POINTER);
    AssertPtrReturn(pcy, VERR_INVALID_POINTER);

    int rc = VERR_NOT_SUPPORTED; /* No scanout, nothing to show. */
    vgpuR3StateLock(pDevIns, pThis);
    if (pThisCC->pScanoutRes)
    {
        uint32_t const cx     = pThisCC->ScanoutRect.cx;
        uint32_t const cy     = pThisCC->ScanoutRect.cy;
        size_t const   cbLine = (size_t)cx * VIRTIOGPU_BPP_BYTES;
        size_t const   cbData = cbLine * cy;
        uint8_t       *pbData = (uint8_t *)RTMemAlloc(cbData);
        if (pbData)
        {
            vgpuR3ScanoutCopyLocked(pThisCC, 0, 0, cx, cy, pbData, cbLine);
            *ppbData = pbData;
            *pcbData = cbData;
            *pcx     = cx;
            *pcy     = cy;
            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_NO_MEMORY;
    }
    vgpuR3StateUnlock(pDevIns, pThis);
    return rc;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnFreeScreenshot}
 */
static DECLCALLBACK(void) vgpuR3PortFreeScreenshot(PPDMIDISPLAYPORT pInterface, uint8_t *pbData)
{
    RT_NOREF(pInterface);
    RTMemFree(pbData);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnDisplayBlt}
 */
static DECLCALLBACK(int) vgpuR3PortDisplayBlt(PPDMIDISPLAYPORT pInterface, const void *pvData,
                                              uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    RT_NOREF(pInterface, pvData, x, y, cx, cy);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnUpdateDisplayRect}
 */
static DECLCALLBACK(void) vgpuR3PortUpdateDisplayRect(PPDMIDISPLAYPORT pInterface, int32_t x, int32_t y,
                                                      uint32_t cx, uint32_t cy)
{
    RT_NOREF(pInterface, x, y, cx, cy);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnCopyRect}
 */
static DECLCALLBACK(int) vgpuR3PortCopyRect(PPDMIDISPLAYPORT pInterface, uint32_t cx, uint32_t cy,
                                            const uint8_t *pbSrc, int32_t xSrc, int32_t ySrc, uint32_t cxSrc, uint32_t cySrc,
                                            uint32_t cbSrcLine, uint32_t cSrcBitsPerPixel,
                                            uint8_t *pbDst, int32_t xDst, int32_t yDst, uint32_t cxDst, uint32_t cyDst,
                                            uint32_t cbDstLine, uint32_t cDstBitsPerPixel)
{
    RT_NOREF(pInterface, cx, cy, pbSrc, xSrc, ySrc, cxSrc, cySrc, cbSrcLine, cSrcBitsPerPixel,
             pbDst, xDst, yDst, cxDst, cyDst, cbDstLine, cDstBitsPerPixel);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnSetRenderVRAM}
 */
static DECLCALLBACK(void) vgpuR3PortSetRenderVRAM(PPDMIDISPLAYPORT pInterface, bool fRender)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);

    vgpuR3StateLock(pDevIns, pThis);
    pThisCC->fRenderVRAM = fRender;
    if (fRender)
        vgpuR3DirtyAll(pThisCC); /* A new framebuffer starts out empty. */
    vgpuR3StateUnlock(pDevIns, pThis);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnSendModeHint}
 */
static DECLCALLBACK(int) vgpuR3PortSendModeHint(PPDMIDISPLAYPORT pInterface, uint32_t cx, uint32_t cy, uint32_t cBPP,
                                                uint32_t iDisplay, uint32_t dx, uint32_t dy, uint32_t fEnabled,
                                                uint32_t fNotifyGuest)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IPort);
    PPDMDEVINS   pDevIns = pThisCC->pDevIns;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    /* fNotifyGuest reflects VBVA capabilities which virtio GPU drivers never
       report; the guest is always told about a changed preferred mode. */
    RT_NOREF(cBPP, dx, dy, fEnabled, fNotifyGuest);

    if (iDisplay >= pThis->Config.cScanouts)
        return VERR_OUT_OF_RANGE;
    if (   !cx || cx > VIRTIOGPU_MAX_DIMENSION
        || !cy || cy > VIRTIOGPU_MAX_DIMENSION)
        return VERR_INVALID_PARAMETER;

    /* Lock order: device critical section (config space, interrupt) first, then the state lock. */
    int const rcLock = PDMDevHlpCritSectEnter(pDevIns, pDevIns->pCritSectRoR3, VERR_IGNORED);
    PDM_CRITSECT_RELEASE_ASSERT_RC_DEV(pDevIns, pDevIns->pCritSectRoR3, rcLock);

    vgpuR3StateLock(pDevIns, pThis);
    bool const fChanged = pThisCC->cxHint != cx || pThisCC->cyHint != cy;
    pThisCC->cxHint = cx;
    pThisCC->cyHint = cy;
    vgpuR3StateUnlock(pDevIns, pThis);

    if (fChanged)
    {
        pThis->Config.fEventsRead |= VIRTIOGPU_EVENT_DISPLAY;
        if (ASMAtomicReadBool(&pThis->fVirtioReady))
            virtioCoreNotifyConfigChanged(&pThis->Virtio);
    }

    PDMDevHlpCritSectLeave(pDevIns, pDevIns->pCritSectRoR3);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnReportHostCursorCapabilities}
 */
static DECLCALLBACK(void) vgpuR3PortReportHostCursorCapabilities(PPDMIDISPLAYPORT pInterface, bool fSupportsRenderCursor,
                                                                 bool fSupportsMoveCursor)
{
    RT_NOREF(pInterface, fSupportsRenderCursor, fSupportsMoveCursor);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnReportHostCursorPosition}
 */
static DECLCALLBACK(void) vgpuR3PortReportHostCursorPosition(PPDMIDISPLAYPORT pInterface, uint32_t x, uint32_t y,
                                                             bool fOutOfRange)
{
    RT_NOREF(pInterface, x, y, fOutOfRange);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnGetUniqueOutputTargetToken}
 */
static DECLCALLBACK(int) vgpuR3PortGetUniqueOutputTargetToken(PPDMIDISPLAYPORT pInterface, uint64_t *pu64OutputTargetToken)
{
    RT_NOREF(pInterface, pu64OutputTargetToken);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnQueryDefaultOutputTargetToken}
 */
static DECLCALLBACK(int) vgpuR3PortQueryDefaultOutputTargetToken(PPDMIDISPLAYPORT pInterface, uint32_t idScreen,
                                                                 uint64_t *pu64OutputTargetToken)
{
    RT_NOREF(pInterface, idScreen, pu64OutputTargetToken);
    return VERR_NOT_IMPLEMENTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnCreateOutputTarget}
 */
static DECLCALLBACK(int) vgpuR3PortCreateOutputTarget(PPDMIDISPLAYPORT pInterface, uint32_t idScreen,
                                                      PDMDISPLAYOUTPUTTARGETFORMAT enmFormat, uint32_t cWidth, uint32_t cHeight,
                                                      uint32_t uFlags, uint64_t u64OutputTargetToken)
{
    RT_NOREF(pInterface, idScreen, enmFormat, cWidth, cHeight, uFlags, u64OutputTargetToken);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnCreateOutputTargetAsync}
 */
static DECLCALLBACK(int) vgpuR3PortCreateOutputTargetAsync(PPDMIDISPLAYPORT pInterface, uint32_t idScreen,
                                                           PDMDISPLAYOUTPUTTARGETFORMAT enmFormat, uint32_t cWidth,
                                                           uint32_t cHeight, uint32_t uFlags, uint64_t u64OutputTargetToken)
{
    RT_NOREF(pInterface, idScreen, enmFormat, cWidth, cHeight, uFlags, u64OutputTargetToken);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnOutputTargetDesc}
 */
static DECLCALLBACK(int) vgpuR3PortOutputTargetDesc(PPDMIDISPLAYPORT pInterface, uint64_t u64OutputTargetToken,
                                                    PDMDISPLAYOUTPUTTARGETDESC *pDescOut)
{
    RT_NOREF(pInterface, u64OutputTargetToken, pDescOut);
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnRetainOutputTarget}
 */
static DECLCALLBACK(void) vgpuR3PortRetainOutputTarget(PPDMIDISPLAYPORT pInterface, uint64_t u64OutputTargetToken)
{
    RT_NOREF(pInterface, u64OutputTargetToken);
}


/**
 * @interface_method_impl{PDMIDISPLAYPORT,pfnReleaseOutputTarget}
 */
static DECLCALLBACK(void) vgpuR3PortReleaseOutputTarget(PPDMIDISPLAYPORT pInterface, uint64_t u64OutputTargetToken)
{
    RT_NOREF(pInterface, u64OutputTargetToken);
}


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) vgpuR3QueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PVIRTIOGPUCC pThisCC = RT_FROM_MEMBER(pInterface, VIRTIOGPUCC, IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pThisCC->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIDISPLAYPORT, &pThisCC->IPort);
    return NULL;
}


/*********************************************************************************************************************************
*   Saved state                                                                                                                  *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{FNSSMDEVSAVEPREP}
 *
 * Refuses to save: the resources and the guest backing are not saved yet.
 */
static DECLCALLBACK(int) vgpuR3SavePrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    RT_NOREF(pSSM);
    /* Note! pfnSSMSetCfgError only works while loading, so report it as a VM error. */
    return PDMDevHlpVMSetError(pDevIns, VERR_NOT_SUPPORTED, RT_SRC_POS,
                               N_("virtio-gpu does not support saving the VM state yet"));
}


/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 *
 * Not reached because vgpuR3SavePrep refuses to save; it only keeps the
 * saved state unit well formed.
 */
static DECLCALLBACK(int) vgpuR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    return virtioCoreR3SaveExec(&pThis->Virtio, pDevIns->pHlpR3, pSSM, VIRTIOGPU_SAVED_STATE_VERSION, VIRTIOGPU_VIRTQ_COUNT);
}


/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) vgpuR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    RT_NOREF(pSSM, uPass);
    LogRel(("%s: Saved states are not supported (unit version %RU32)\n", pThis->szInstance, uVersion));
    return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
}


/*********************************************************************************************************************************
*   Device interface                                                                                                             *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{FNDBGFHANDLERDEV, virtiogpu}
 */
static DECLCALLBACK(void) vgpuR3Info(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);

    pHlp->pfnPrintf(pHlp, "%s: status=%#04x ready=%RTbool quiescing=%RTbool features=%#RX64 active=%RU32\n",
                    pThis->szInstance, pThis->Virtio.fDeviceStatus, pThis->fVirtioReady, pThis->fQuiescing,
                    pThis->fNegotiatedFeatures, pThis->cActiveCmds);
    pHlp->pfnPrintf(pHlp, "Config: events_read=%#RX32 num_scanouts=%RU32 num_capsets=%RU32\n",
                    pThis->Config.fEventsRead, pThis->Config.cScanouts, pThis->Config.cCapsets);

    vgpuR3StateLock(pDevIns, pThis);
    pHlp->pfnPrintf(pHlp, "Preferred mode: %RU32x%RU32\n", pThisCC->cxHint, pThisCC->cyHint);
    if (pThisCC->pScanoutRes)
        pHlp->pfnPrintf(pHlp, "Scanout 0: resource %RU32, rect %RU32,%RU32 %RU32x%RU32\n",
                        pThisCC->pScanoutRes->idResource, pThisCC->ScanoutRect.x, pThisCC->ScanoutRect.y,
                        pThisCC->ScanoutRect.cx, pThisCC->ScanoutRect.cy);
    else
        pHlp->pfnPrintf(pHlp, "Scanout 0: disabled\n");
    pHlp->pfnPrintf(pHlp, "Display: %s, render=%RTbool, mode shown=%RTbool, generation %RU32 (Main %RU32)\n",
                    pThisCC->pDrv ? "attached" : "headless", pThisCC->fRenderVRAM, pThisCC->fMainHasMode,
                    pThisCC->uScanoutGen, pThisCC->uScanoutGenMain);
    pHlp->pfnPrintf(pHlp, "Resources: %RU32 (shadow memory %zu bytes, %RU32 backing entries)\n",
                    pThisCC->cResources, pThisCC->cbShadowTotal, pThisCC->cTotalBackingEntries);
    PVIRTIOGPURESOURCE pRes;
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        pHlp->pfnPrintf(pHlp, "  #%RU32: format %RU32, %RU32x%RU32, backing %RU32 entries / %RU64 bytes\n",
                        pRes->idResource, pRes->uFormat, pRes->cx, pRes->cy, pRes->cEntries, pRes->cbBacking);
    }
    vgpuR3StateUnlock(pDevIns, pThis);

    /* The queue dump reads guest memory, so do it without the state lock. */
    for (uint16_t uVirtq = 0; uVirtq < VIRTIOGPU_VIRTQ_COUNT; uVirtq++)
    {
        pHlp->pfnPrintf(pHlp, "Queue %u (%s):\n", uVirtq, virtioCoreVirtqGetName(&pThis->Virtio, uVirtq));
        if (   (pThis->Virtio.fDeviceStatus & VIRTIO_STATUS_DRIVER_OK)
            && virtioCoreIsVirtqEnabled(&pThis->Virtio, uVirtq))
            virtioCoreR3VirtqInfo(pDevIns, pHlp, &pThis->Virtio, pszArgs, uVirtq);
        else
            pHlp->pfnPrintf(pHlp, "       not enabled\n");
    }
}


/**
 * @callback_method_impl{FNPDMDEVASYNCNOTIFY}
 */
static DECLCALLBACK(bool) vgpuR3DeviceQuiesced(PPDMDEVINS pDevIns)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    if (ASMAtomicReadU32(&pThis->cActiveCmds))
        return false;
    virtioCoreR3VmStateChanged(&pThis->Virtio, pThis->enmQuiescingFor);
    return true;
}


/**
 * Stops the workers from taking new buffers and waits (asynchronously) for
 * the commands in progress.  fQuiescing stays set until pfnResume.
 *
 * @param   pDevIns         The device instance.
 * @param   enmQuiescingFor The reason.
 */
static void vgpuR3QuiesceDevice(PPDMDEVINS pDevIns, VIRTIOVMSTATECHANGED enmQuiescingFor)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);

    pThis->enmQuiescingFor = enmQuiescingFor;
    ASMAtomicWriteBool(&pThis->fQuiescing, true);
    PDMDevHlpSetAsyncNotification(pDevIns, vgpuR3DeviceQuiesced);
    if (!ASMAtomicReadU32(&pThis->cActiveCmds))
        PDMDevHlpAsyncNotificationCompleted(pDevIns);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnSuspend}
 */
static DECLCALLBACK(void) vgpuR3Suspend(PPDMDEVINS pDevIns)
{
    LogFunc(("\n"));
    vgpuR3QuiesceDevice(pDevIns, kvirtIoVmStateChangedSuspend);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnPowerOff}
 */
static DECLCALLBACK(void) vgpuR3PowerOff(PPDMDEVINS pDevIns)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    LogFunc(("\n"));

    /* Main does not stop the refresh, and its display driver goes away before we do. */
    ASMAtomicWriteU32(&pThis->cMilliesRefreshInterval, 0);
    if (pThis->hRefreshTimer != NIL_TMTIMERHANDLE)
        PDMDevHlpTimerStop(pDevIns, pThis->hRefreshTimer);

    vgpuR3QuiesceDevice(pDevIns, kvirtIoVmStateChangedPowerOff);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnResume}
 */
static DECLCALLBACK(void) vgpuR3Resume(PPDMDEVINS pDevIns)
{
    PVIRTIOGPU pThis = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    LogFunc(("\n"));

    ASMAtomicWriteBool(&pThis->fQuiescing, false);
    for (uint16_t uVirtq = 0; uVirtq < VIRTIOGPU_VIRTQ_COUNT; uVirtq++)
        vgpuR3WakeWorker(pDevIns, pThis, uVirtq);

    /* Make the guest look at the queues again too. */
    virtioCoreR3VmStateChanged(&pThis->Virtio, kvirtIoVmStateChangedResume);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnReset}
 */
static DECLCALLBACK(void) vgpuR3Reset(PPDMDEVINS pDevIns)
{
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    LogFunc(("\n"));

    /* A full virtio reset: vgpuR3StatusChanged(0) blanks the display and makes the
       control worker free the resources, then the queues are disabled.  No need to
       wait for the workers: a command in flight finishes against the old state and
       its completion is dropped because the driver is no longer ready. */
    virtioCoreR3ResetDevice(pDevIns, &pThis->Virtio, &pThisCC->Virtio);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnAttach}
 */
static DECLCALLBACK(int) vgpuR3Attach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);

    AssertMsgReturn(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG, ("virtio-gpu does not support hot-plugging\n"),
                    VERR_INVALID_PARAMETER);
    AssertMsgReturn(iLUN == 0, ("Invalid LUN #%u\n", iLUN), VERR_PDM_NO_SUCH_LUN);

    PPDMIBASE pDrvBase = NULL;
    int rc = PDMDevHlpDriverAttach(pDevIns, iLUN, &pThisCC->IBase, &pDrvBase, "Display Port");
    if (RT_SUCCESS(rc))
    {
        PPDMIDISPLAYCONNECTOR pDrv = PDMIBASE_QUERY_INTERFACE(pDrvBase, PDMIDISPLAYCONNECTOR);
        AssertLogRelMsgReturn(pDrv, ("virtio-gpu: LUN#0 has no display connector interface\n"), VERR_PDM_MISSING_INTERFACE);
        /* pbData may be NULL while there is no framebuffer. */
        AssertLogRelMsgReturn(pDrv->pfnRefresh && pDrv->pfnResize && pDrv->pfnUpdateRect,
                              ("virtio-gpu: Incomplete display connector\n"), VERR_INTERNAL_ERROR);
        pThisCC->pDrvBase = pDrvBase;
        pThisCC->pDrv     = pDrv;
        return VINF_SUCCESS;
    }
    if (rc == VERR_PDM_NO_ATTACHED_DRIVER)
    {
        LogRel(("virtio-gpu#%u: No display driver attached, running headless\n", pDevIns->iInstance));
        return VINF_SUCCESS;
    }
    AssertLogRelMsgFailed(("virtio-gpu: Failed to attach LUN#0: %Rrc\n", rc));
    return rc;
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnDetach}
 */
static DECLCALLBACK(void) vgpuR3Detach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    RT_NOREF(fFlags);
    AssertMsgReturnVoid(iLUN == 0, ("Invalid LUN #%u\n", iLUN));

    vgpuR3StateLock(pDevIns, pThis);
    pThisCC->pDrv        = NULL;
    pThisCC->pDrvBase    = NULL;
    pThisCC->fRenderVRAM = false;
    vgpuR3StateUnlock(pDevIns, pThis);
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnDestruct}
 */
static DECLCALLBACK(int) vgpuR3Destruct(PPDMDEVINS pDevIns)
{
    PDMDEV_CHECK_VERSIONS_RETURN_QUIET(pDevIns);
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);

    /* PDM has already destroyed the display driver. */
    pThisCC->pDrv     = NULL;
    pThisCC->pDrvBase = NULL;

    /* Stop the workers before closing the semaphores their wake-up callback signals. */
    for (unsigned i = 0; i < RT_ELEMENTS(pThisCC->aWorkers); i++)
    {
        if (pThisCC->aWorkers[i].pThread)
        {
            int rcThread = VINF_SUCCESS;
            int rc = PDMDevHlpThreadDestroy(pDevIns, pThisCC->aWorkers[i].pThread, &rcThread);
            if (RT_FAILURE(rc) || RT_FAILURE(rcThread))
                LogRel(("%s: Destroying worker %u failed: rc=%Rrc rcThread=%Rrc\n", pThis->szInstance, i, rc, rcThread));
            pThisCC->aWorkers[i].pThread = NULL;
        }
        if (pThis->aWorkers[i].hEvtProcess != NIL_SUPSEMEVENT)
        {
            PDMDevHlpSUPSemEventClose(pDevIns, pThis->aWorkers[i].hEvtProcess);
            pThis->aWorkers[i].hEvtProcess = NIL_SUPSEMEVENT;
        }
        if (RTCritSectIsInitialized(&pThisCC->aWorkers[i].CritSectVirtq))
            RTCritSectDelete(&pThisCC->aWorkers[i].CritSectVirtq);
    }

    /* No other thread can touch the resources any more. */
    vgpuR3ResourcesDestroyAll(pDevIns, pThis, pThisCC);

    virtioCoreR3Term(pDevIns, &pThis->Virtio, &pThisCC->Virtio);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREGR3,pfnConstruct}
 */
static DECLCALLBACK(int) vgpuR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PVIRTIOGPU      pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC    pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    PCPDMDEVHLPR3   pHlp    = pDevIns->pHlpR3;

    /*
     * Initialize enough for the destructor to work whatever fails below.
     */
    pThisCC->pDevIns = pDevIns;
    RTListInit(&pThisCC->ListResources);
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aWorkers); i++)
        pThis->aWorkers[i].hEvtProcess = NIL_SUPSEMEVENT;
    pThis->hRefreshTimer = NIL_TMTIMERHANDLE;
    RTStrPrintf(pThis->szInstance, sizeof(pThis->szInstance), "VIRTIOGPU%d", iInstance);

    pThisCC->cxHint      = VIRTIOGPU_DEFAULT_WIDTH;
    pThisCC->cyHint      = VIRTIOGPU_DEFAULT_HEIGHT;
    pThisCC->uScanoutGen = 1; /* Different from uScanoutGenMain, so the first refresh syncs Main. */
    vgpuR3DirtyClear(pThisCC);

    pThisCC->IBase.pfnQueryInterface                = vgpuR3QueryInterface;
    pThisCC->IPort.pfnUpdateDisplay                 = vgpuR3PortUpdateDisplay;
    pThisCC->IPort.pfnUpdateDisplayAll              = vgpuR3PortUpdateDisplayAll;
    pThisCC->IPort.pfnQueryVideoMode                = vgpuR3PortQueryVideoMode;
    pThisCC->IPort.pfnSetRefreshRate                = vgpuR3PortSetRefreshRate;
    pThisCC->IPort.pfnTakeScreenshot                = vgpuR3PortTakeScreenshot;
    pThisCC->IPort.pfnFreeScreenshot                = vgpuR3PortFreeScreenshot;
    pThisCC->IPort.pfnDisplayBlt                    = vgpuR3PortDisplayBlt;
    pThisCC->IPort.pfnUpdateDisplayRect             = vgpuR3PortUpdateDisplayRect;
    pThisCC->IPort.pfnSetRenderVRAM                 = vgpuR3PortSetRenderVRAM;
    pThisCC->IPort.pfnCopyRect                      = vgpuR3PortCopyRect;
    pThisCC->IPort.pfnSetViewport                   = NULL;
    pThisCC->IPort.pfnSendModeHint                  = vgpuR3PortSendModeHint;
    pThisCC->IPort.pfnReportHostCursorCapabilities  = vgpuR3PortReportHostCursorCapabilities;
    pThisCC->IPort.pfnReportHostCursorPosition      = vgpuR3PortReportHostCursorPosition;
    pThisCC->IPort.pfnReportMonitorPositions        = NULL;
    pThisCC->IPort.pfnGetUniqueOutputTargetToken    = vgpuR3PortGetUniqueOutputTargetToken;
    pThisCC->IPort.pfnQueryDefaultOutputTargetToken = vgpuR3PortQueryDefaultOutputTargetToken;
    pThisCC->IPort.pfnCreateOutputTarget            = vgpuR3PortCreateOutputTarget;
    pThisCC->IPort.pfnCreateOutputTargetAsync       = vgpuR3PortCreateOutputTargetAsync;
    pThisCC->IPort.pfnOutputTargetDesc              = vgpuR3PortOutputTargetDesc;
    pThisCC->IPort.pfnRetainOutputTarget            = vgpuR3PortRetainOutputTarget;
    pThisCC->IPort.pfnReleaseOutputTarget           = vgpuR3PortReleaseOutputTarget;

    /*
     * Validate and read the configuration.  MmioBase and Irq are read by VirtioCore.
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "MonitorCount|MmioBase|Irq", "");

    uint32_t cMonitors = 1;
    int rc = pHlp->pfnCFGMQueryU32Def(pCfg, "MonitorCount", &cMonitors, 1);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"MonitorCount\""));
    if (cMonitors < 1 || cMonitors > VIRTIOGPU_MAX_MONITORS)
        return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                   N_("virtio-gpu: \"MonitorCount\" is %RU32, but only %d is supported"),
                                   cMonitors, VIRTIOGPU_MAX_MONITORS);

    pThis->Config.fEventsRead  = 0;
    pThis->Config.fEventsClear = 0;
    pThis->Config.cScanouts    = cMonitors;
    pThis->Config.cCapsets     = 0;

    /* The state lock and the refresh timer must exist before the display driver
       is attached: its constructor calls pfnSetRenderVRAM and pfnSetRefreshRate. */
    rc = PDMDevHlpCritSectInit(pDevIns, &pThis->CritSectState, RT_SRC_POS, "VirtioGpu#%d", iInstance);
    AssertRCReturn(rc, rc);

    rc = PDMDevHlpTimerCreate(pDevIns, TMCLOCK_REAL, vgpuR3TimerRefresh, NULL /*pvUser*/,
                              TMTIMER_FLAGS_NO_CRIT_SECT | TMTIMER_FLAGS_NO_RING0, "virtio-gpu Refresh",
                              &pThis->hRefreshTimer);
    AssertRCReturn(rc, rc);

    /*
     * The virtio core.  The callbacks must be set before the init call.
     */
    pThisCC->Virtio.pfnVirtqNotified = vgpuR3VirtqNotified;
    pThisCC->Virtio.pfnStatusChanged = vgpuR3StatusChanged;
    pThisCC->Virtio.pfnDevCapRead    = vgpuR3DevCapRead;
    pThisCC->Virtio.pfnDevCapWrite   = vgpuR3DevCapWrite;

    VIRTIOPCIPARAMS VirtioPciParams;
    RT_ZERO(VirtioPciParams);
    VirtioPciParams.uDeviceId      = VIRTIOGPU_PCI_DEVICE_ID;
    VirtioPciParams.uClassBase     = VIRTIOGPU_PCI_CLASS_BASE;
    VirtioPciParams.uClassSub      = VIRTIOGPU_PCI_CLASS_SUB;
    VirtioPciParams.uClassProg     = VIRTIOGPU_PCI_CLASS_PROG;
    VirtioPciParams.uSubsystemId   = VIRTIOGPU_PCI_DEVICE_ID;
    VirtioPciParams.uInterruptLine = 0x00;
    VirtioPciParams.uInterruptPin  = 0x01;
    VirtioPciParams.uDeviceType    = VIRTIO_DEVICE_TYPE_GPU;

    rc = virtioCoreR3Init(pDevIns, &pThis->Virtio, &pThisCC->Virtio, &VirtioPciParams, pThis->szInstance,
                          VIRTIOGPU_FEATURES_OFFERED, 0 /*fOfferLegacy*/, &pThis->Config, sizeof(pThis->Config));
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to initialize the virtio core"));

    /*
     * The queues and their workers.
     */
    static const char * const s_apszVirtqNames[VIRTIOGPU_VIRTQ_COUNT]  = { "controlq", "cursorq" };
    static const char * const s_apszThreadNames[VIRTIOGPU_VIRTQ_COUNT] = { "VGpuCtrl", "VGpuCursor" };
    for (uint16_t uVirtq = 0; uVirtq < VIRTIOGPU_VIRTQ_COUNT; uVirtq++)
    {
        rc = virtioCoreR3VirtqAttach(&pThis->Virtio, uVirtq, s_apszVirtqNames[uVirtq]);
        if (RT_FAILURE(rc))
            return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to attach a virtqueue"));

        rc = RTCritSectInit(&pThisCC->aWorkers[uVirtq].CritSectVirtq);
        if (RT_FAILURE(rc))
            return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to create a queue critical section"));

        rc = PDMDevHlpSUPSemEventCreate(pDevIns, &pThis->aWorkers[uVirtq].hEvtProcess);
        if (RT_FAILURE(rc))
            return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to create a worker semaphore"));

        rc = PDMDevHlpThreadCreate(pDevIns, &pThisCC->aWorkers[uVirtq].pThread, (void *)(uintptr_t)uVirtq,
                                   vgpuR3WorkerThread, vgpuR3WorkerWakeUp, 0 /*cbStack*/, RTTHREADTYPE_IO,
                                   s_apszThreadNames[uVirtq]);
        if (RT_FAILURE(rc))
            return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to create a worker thread"));
    }

    /*
     * The display driver.
     */
    rc = vgpuR3Attach(pDevIns, 0 /*iLUN*/, PDM_TACH_FLAGS_NOT_HOT_PLUG);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to attach the display driver"));

    /*
     * Saved state (refused for now, see vgpuR3SavePrep), info and statistics.
     */
    rc = PDMDevHlpSSMRegisterEx(pDevIns, VIRTIOGPU_SAVED_STATE_VERSION, sizeof(*pThis), NULL /*pszBefore*/,
                                NULL /*pfnLivePrep*/, NULL /*pfnLiveExec*/, NULL /*pfnLiveVote*/,
                                vgpuR3SavePrep, vgpuR3SaveExec, NULL /*pfnSaveDone*/,
                                NULL /*pfnLoadPrep*/, vgpuR3LoadExec, NULL /*pfnLoadDone*/);
    AssertRCReturn(rc, rc);

    PDMDevHlpDBGFInfoRegister(pDevIns, "virtiogpu", "virtio-gpu state: resources, scanout and queues.", vgpuR3Info);

    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCtrlCmds,      STAMTYPE_COUNTER, "CtrlCmds",      STAMUNIT_OCCURENCES,
                          "Control queue commands processed.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCursorCmds,    STAMTYPE_COUNTER, "CursorCmds",    STAMUNIT_OCCURENCES,
                          "Cursor queue commands processed (and ignored).");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCmdErrors,     STAMTYPE_COUNTER, "CmdErrors",     STAMUNIT_OCCURENCES,
                          "Control queue commands answered with an error.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatTransferBytes, STAMTYPE_COUNTER, "TransferBytes", STAMUNIT_BYTES,
                          "Bytes copied from guest backing by TRANSFER_TO_HOST_2D.");

    LogRel(("%s: 2D virtio GPU with %RU32 scanout(s)\n", pThis->szInstance, cMonitors));
    return VINF_SUCCESS;
}


/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceVirtioGpu =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "virtio-gpu",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_GRAPHICS,
    /* .cMaxInstances = */          1,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(VIRTIOGPU),
    /* .cbInstanceCC = */           sizeof(VIRTIOGPUCC),
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         1,
    /* .cMaxMsixVectors = */        VBOX_MSIX_MAX_ENTRIES,
    /* .pszDescription = */         "Virtio GPU (2D).\n",
#if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           vgpuR3Construct,
    /* .pfnDestruct = */            vgpuR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               vgpuR3Reset,
    /* .pfnSuspend = */             vgpuR3Suspend,
    /* .pfnResume = */              vgpuR3Resume,
    /* .pfnAttach = */              vgpuR3Attach,
    /* .pfnDetach = */              vgpuR3Detach,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            vgpuR3PowerOff,
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

