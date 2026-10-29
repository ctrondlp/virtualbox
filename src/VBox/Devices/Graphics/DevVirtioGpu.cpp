/* $Id$ */
/** @file
 * VBox graphics devices - Virtio GPU, 2D and optional Venus 3D (virtio 1.x, device id 0x1050).
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

/** @page pg_dev_virtio_gpu    Virtio GPU Device
 *
 * @section sec_dev_virtio_gpu_notes    NOTES
 *
 * Design:
 *  - A modern-only (virtio 1.x) PCI device with the PCI device id 0x1050, built
 *    on the VirtioCore transport and running in ring-3 only.  The PCI class is
 *    "display controller, other" (0x0380) and not VGA (0x0300), because the
 *    OVMF VirtioGpuDxe driver does not bind to a virtio GPU claiming to be VGA.
 *  - Without Venus no device specific feature bits are offered (no virgl,
 *    EDID, blob resources or context init), so only the 2D command set is
 *    implemented: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_UNREF,
 *    SET_SCANOUT, RESOURCE_FLUSH, TRANSFER_TO_HOST_2D, RESOURCE_ATTACH_BACKING
 *    and RESOURCE_DETACH_BACKING.  Everything else gets
 *    VIRTIO_GPU_RESP_ERR_UNSPEC.  The Venus additions are described in
 *    @ref sec_dev_virtio_gpu_venus.
 *  - Every 2D resource has a host shadow copy, 32 bpp and always stored as
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
 *    display bookkeeping, and the context table as seen by the info handler.
 *    It is only held for short host memory operations, never across a
 *    renderer call or a host window map/unmap.
 *  - VIRTIOGPUR3::CritSectFence protects the fence progress of the contexts
 *    (VIRTIOGPUCONTEXT::auFenceSignaled) and the context ids as seen by the
 *    fence callback.  It may be followed by CritSectState, never the other
 *    way around, and is never held across a renderer call: destroying a
 *    context joins the library thread that may be waiting for it.
 *  - VIRTIOGPUWORKERR3::CritSectVirtq serializes the used ring updates of a
 *    queue (the VirtioCore used ring bookkeeping is not thread safe).
 *
 * Limitations:
 *  - One scanout only.  No hardware cursor: cursor queue commands are
 *    completed without effect, so the guest has to draw its own cursor.
 *  - No OpenGL (virgl) 3D, no EDID, no guest memory blobs, no blob scanout.
 *  - Saving the VM state (and thus teleporting) is refused.
 *  - VirtioCore only uses INTx interrupts, not MSI-X.
 *  - Resources are limited to 8192x8192 pixels, 512 resources, 256 MB of host
 *    shadow memory per VM and 16384 backing entries per resource.
 *  - Little-endian hosts only: the wire structures are used as they are.
 *
 * @section sec_dev_virtio_gpu_venus    Venus (Vulkan)
 *
 * With "Venus" enabled the device offers VIRTIO_GPU_F_VIRGL,
 * VIRTIO_GPU_F_RESOURCE_BLOB and VIRTIO_GPU_F_CONTEXT_INIT with one capset
 * (VIRTIOGPU_CAPSET_VENUS) and forwards the guest's Vulkan command streams to
 * a host renderer library (a virglrenderer build, loaded at run time, see
 * DevVirtioGpuRenderer.cpp).  Any failure to load or initialize it, or to
 * create the host visible window, falls back to 2D with a release log entry.
 *
 * Commands: GET_CAPSET_INFO / GET_CAPSET (the capset is fetched once at
 * init), CTX_CREATE / CTX_DESTROY (Venus contexts only, at most
 * VIRTIOGPU_MAX_CONTEXTS), CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE,
 * SUBMIT_3D (copied into host memory, at most VIRTIOGPU_MAX_SUBMIT_SIZE),
 * RESOURCE_CREATE_BLOB (VIRTIOGPU_BLOB_MEM_HOST3D only, no guest backing),
 * RESOURCE_MAP_BLOB / RESOURCE_UNMAP_BLOB.  RESOURCE_UNREF handles both
 * resource kinds; the 2D commands reject blob resources.
 *
 * Host visible window: a 64-bit prefetchable BAR (VIRTIOGPU_HOST_VISIBLE_BAR)
 * registered as an MMIO2 host window (PGMPHYS_MMIO2_FLAGS_HOST_WINDOW), so no
 * memory backs it up front, and announced to the driver as virtio shared
 * memory region VIRTIOGPU_SHM_ID_HOST_VISIBLE.  MAP_BLOB has the library map
 * the blob into the VM process and plugs those host pages into the window at
 * the offset the guest chose (PDMDevHlpMmio2MapHostRange).  Unmapping always
 * takes the pages out of the guest's reach first and only then releases the
 * library mapping, so the guest never sees freed host memory.  If taking the
 * pages out fails, the library mapping (and the resource in the library) is
 * kept for the rest of the VM's life and the library is not cleaned up.
 *
 * Fences: a successful command with VIRTIO_GPU_FLAG_FENCE for a live context
 * is not completed right away.  A mergeable context fence is created on the
 * (context, ring) the header names (ring 0 without
 * VIRTIO_GPU_FLAG_INFO_RING_IDX) and the descriptor chain is parked in
 * VIRTIOGPUR3::ListPending with a copy of the response.  The library's fence
 * threads report retired fences through vgpuR3FenceCallback(), which only
 * records the highest fence id per (context, ring) under CritSectFence, sets
 * VIRTIOGPU::fFencesSignaled and wakes the control worker.  The control
 * worker then completes every parked command whose fence id is not above
 * the recorded value.  Commands without a context, failed commands and
 * commands for an unknown context complete right away.  The number of parked
 * commands is limited (VIRTIOGPU_MAX_PENDING_FENCES): when the limit is
 * reached the control worker stops taking new buffers until fences retire.
 *
 * Threads:
 *  - EMTs: VirtioCore MMIO, PDM callbacks, the display port.  They never call
 *    the renderer and only ask the control worker to drop the 3D state.
 *  - The control queue worker owns resources, contexts and parked commands:
 *    it is the only thread changing them (besides the destructor after the
 *    workers stopped).  It calls the renderer and the host window helpers
 *    without holding any lock (the helpers may rendezvous with the EMTs).
 *  - The renderer thread (DevVirtioGpuRenderer.cpp) is the only thread
 *    calling into the library; everybody else posts a request to it and
 *    waits.  The constructor and destructor use it too.
 *  - Library threads: per context fence threads (created and joined by the
 *    library on context create / destroy) calling vgpuR3FenceCallback(), and
 *    the in-process render server thread running the host Vulkan driver.
 *
 * Teardown (device reset, driver writing status 0, destruction; done by the
 * control worker, or by the destructor once the workers stopped), in this
 * order: (1) take every mapped blob out of the host window, then release its
 * library mapping; (2) destroy the contexts, after which no fence callback
 * can happen, and drop their parked commands without writing a response (the
 * guest's queues are gone); (3) release the blob resources in the library;
 * (4) free the resource structures.  On destruction the library is then
 * cleaned up and the renderer thread stopped before the worker semaphores
 * the fence callback signals are closed.
 *
 * Security: everything the guest sends is validated before it reaches the
 * library (ids, sizes, alignments, window ranges, flags, ring indexes), and
 * the library's answers (mapping address, size and caching type) are checked
 * before the guest gets to see host memory.  Note that the render server runs
 * the host Vulkan driver inside the VM process with guest supplied command
 * streams; virglrenderer's own validation is what protects the host there.
 *
 * @section sec_dev_virtio_gpu_config   Configuration
 *
 *  - "MonitorCount": number of scanouts, must be 1 for now (default 1).
 *  - "MmioBase", "Irq": use the virtio-over-MMIO transport instead of PCI
 *    (read by VirtioCore).  Venus needs PCI and is disabled then.
 *  - "Venus": offer Venus 3D (default false).
 *  - "RendererPath": absolute path of the renderer library (default
 *    VIRTIOGPU_RENDERER_DEFAULT_NAME in the VirtualBox architecture
 *    directory).
 *  - "HostVisibleSize": size of the host visible window in bytes, a power of
 *    two between 64 MB and 16 GB (default 1 GB).
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
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/uuid.h>

#include "../VirtIO/VirtioCore.h"
#include "DevVirtioGpu.h"
#include "DevVirtioGpuRenderer.h"
#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** The saved state version (saving is refused, see vgpuR3SavePrep). */
#define VIRTIOGPU_SAVED_STATE_VERSION           UINT32_C(1)
/** The device specific features offered to the driver without Venus: none, 2D only. */
#define VIRTIOGPU_FEATURES_OFFERED_2D           UINT64_C(0)
/** The device specific features offered to the driver with Venus.  RESOURCE_BLOB
 * must never be offered without VIRGL: a Linux driver would then put its dumb
 * buffers into guest memory blobs, which this device does not implement. */
#define VIRTIOGPU_FEATURES_OFFERED_VENUS        (VIRTIOGPU_F_VIRGL | VIRTIOGPU_F_RESOURCE_BLOB | VIRTIOGPU_F_CONTEXT_INIT)

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

/** @name Venus limits.
 * @{ */
/** The maximum number of 3D contexts. */
#define VIRTIOGPU_MAX_CONTEXTS                  64
/** The number of fence rings (timelines) per context: the virtio limit, and
 * the library's PROXY_CONTEXT_TIMELINE_COUNT. */
#define VIRTIOGPU_MAX_RINGS                     64
/** The maximum size of one SUBMIT_3D command stream. */
#define VIRTIOGPU_MAX_SUBMIT_SIZE               (8 * _1M)
/** The maximum number of blob resources. */
#define VIRTIOGPU_MAX_BLOB_RESOURCES            8192
/** The maximum size of one blob resource. */
#define VIRTIOGPU_MAX_BLOB_SIZE                 _4G
/** The maximum host shared memory of all blobs with blob id 0 together (for
 * Venus these are plain host memory the library allocates on request). */
#define VIRTIOGPU_MAX_SHM_BLOB_MEMORY           _1G
/** The maximum number of fenced commands waiting for their fence. */
#define VIRTIOGPU_MAX_PENDING_FENCES            512
/** The page size blob mappings are aligned to (the guest's and the host's). */
#define VIRTIOGPU_BLOB_PAGE_SIZE                _4K
/** How often taking a blob out of the window is retried while busy (DMA). */
#define VIRTIOGPU_UNMAP_BUSY_RETRIES            50
/** How long to wait between those retries, in milliseconds. */
#define VIRTIOGPU_UNMAP_BUSY_SLEEP_MS           2
/** The PCI region (BAR4+5, 64-bit) of the host visible window. */
#define VIRTIOGPU_HOST_VISIBLE_BAR              4
/** The default size of the host visible window. */
#define VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT     _1G
/** The minimum size of the host visible window. */
#define VIRTIOGPU_HOST_VISIBLE_SIZE_MIN         _64M
/** The maximum size of the host visible window (one PGM MMIO2 chunk). */
#define VIRTIOGPU_HOST_VISIBLE_SIZE_MAX         (UINT64_C(16) * _1G)
/** The default renderer library name, looked for in the VirtualBox
 * architecture directory. */
#if defined(RT_OS_WINDOWS)
# define VIRTIOGPU_RENDERER_DEFAULT_NAME        "libvirglrenderer-1.dll"
#elif defined(RT_OS_DARWIN)
# define VIRTIOGPU_RENDERER_DEFAULT_NAME        "libvirglrenderer.1.dylib"
#else
# define VIRTIOGPU_RENDERER_DEFAULT_NAME        "libvirglrenderer.so.1"
#endif
/** @} */

AssertCompile((uint64_t)VIRTIOGPU_MAX_DIMENSION * VIRTIOGPU_MAX_DIMENSION * VIRTIOGPU_BPP_BYTES <= VIRTIOGPU_MAX_SHADOW_MEMORY);
AssertCompile(VIRTIOGPU_MAX_RINGS <= 256); /* VIRTIOGPUCTRLHDR::uRingIdx is a byte. */
AssertCompile(VIRTIOGPU_MAX_SUBMIT_SIZE % 4 == 0 && VIRTIOGPU_MAX_SUBMIT_SIZE <= _16M);
AssertCompile(RT_IS_POWER_OF_TWO(VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT));
AssertCompile(VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT >= VIRTIOGPU_HOST_VISIBLE_SIZE_MIN);
AssertCompile(VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT <= VIRTIOGPU_HOST_VISIBLE_SIZE_MAX);
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
 * A 2D or blob resource.
 *
 * Resources are created, changed and freed only by the control queue worker
 * (and by the destructor).  The list membership, the backing description,
 * the blob mapping state and the scanout pointer are changed while holding
 * VIRTIOGPU::CritSectState, so the display and info code can safely look at
 * them under that lock.
 *
 * Blob resources (fBlob) live in the renderer library: they have no shadow,
 * no guest backing and no dimensions, and only the blob members are used.
 */
typedef struct VIRTIOGPURESOURCE
{
    /** Node in VIRTIOGPUR3::ListResources. */
    RTLISTNODE              Node;
    /** The resource ID chosen by the guest, never 0 (at most INT32_MAX for blobs). */
    uint32_t                idResource;
    /** Set for a Venus blob resource, clear for a 2D resource. */
    bool                    fBlob;
    /** Blob: set while the blob is mapped into the host visible window. */
    bool                    fMapped;
    /** Blob: taking the mapping out of the window failed, so the library
     * memory stays mapped (and the resource alive in the library) for good. */
    bool                    fMapLeaked;
    /** Blob: VIRTIOGPU_BLOB_MEM_XXX. */
    uint32_t                uBlobMem;
    /** Blob: VIRTIOGPU_BLOB_FLAG_XXX. */
    uint32_t                fBlobFlags;
    /** Blob: the context that created it. */
    uint32_t                idCtxCreator;
    /** Blob: the caching type of the mapping (VIRTIOGPU_MAP_CACHE_XXX). */
    uint32_t                fMapInfo;
    /** Blob: the context specific blob id. */
    uint64_t                idBlob;
    /** Blob: the size in bytes as given by the guest. */
    uint64_t                cbBlob;
    /** Blob: offset of the mapping in the host visible window. */
    uint64_t                offMap;
    /** Blob: size of the mapping in the host visible window (whole pages). */
    uint64_t                cbMap;
    /** Blob: the library's host mapping while mapped. */
    void                   *pvMap;
    /** 2D: the guest format (VIRTIOGPU_FORMAT_XXX). */
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
/** Pointer to a resource. */
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
    VIRTIOGPUGETCAPSETINFO          GetCapsetInfo;
    VIRTIOGPUGETCAPSET              GetCapset;
    VIRTIOGPUCTXCREATE              CtxCreate;
    VIRTIOGPUCTXRESOURCE            CtxResource;
    VIRTIOGPUCMDSUBMIT              Submit;
    VIRTIOGPURESOURCECREATEBLOB     CreateBlob;
    VIRTIOGPURESOURCEMAPBLOB        MapBlob;
    VIRTIOGPURESOURCEUNMAPBLOB      UnmapBlob;
} VIRTIOGPUCTRLREQ;

/**
 * The fixed size control queue responses.  All variants start with the
 * common header.  (The GET_CAPSET response is allocated separately.)
 */
typedef union VIRTIOGPUCTRLRESP
{
    VIRTIOGPUCTRLHDR                Hdr;
    VIRTIOGPURESPDISPLAYINFO        DisplayInfo;
    VIRTIOGPURESPCAPSETINFO         CapsetInfo;
    VIRTIOGPURESPMAPINFO            MapInfo;
} VIRTIOGPUCTRLRESP;

/**
 * A Venus context slot.
 *
 * Owned by the control worker.  idCtx changes while holding both
 * VIRTIOGPUR3::CritSectFence and VIRTIOGPU::CritSectState; auFenceSignaled
 * is written by the fence callback under CritSectFence.
 */
typedef struct VIRTIOGPUCONTEXT
{
    /** The context id chosen by the guest (1..INT32_MAX), 0 if the slot is free. */
    uint32_t                idCtx;
    /** The context init flags (the capset id). */
    uint32_t                fContextInit;
    /** The highest fence id retired per ring. */
    uint64_t                auFenceSignaled[VIRTIOGPU_MAX_RINGS];
    /** The debug name, printable ASCII only. */
    char                    szName[RT_SIZEOFMEMB(VIRTIOGPUCTXCREATE, achDebugName) + 1];
} VIRTIOGPUCONTEXT;
/** Pointer to a Venus context slot. */
typedef VIRTIOGPUCONTEXT *PVIRTIOGPUCONTEXT;

/**
 * A fenced control command waiting for its fence, owned by the control worker.
 */
typedef struct VIRTIOGPUPENDING
{
    /** Node in VIRTIOGPUR3::ListPending. */
    RTLISTNODE              Node;
    /** The descriptor chain (one reference held). */
    PVIRTQBUF               pVirtqBuf;
    /** Index of the context slot in VIRTIOGPUR3::paContexts. */
    uint32_t                iCtx;
    /** The ring index, below VIRTIOGPU_MAX_RINGS. */
    uint32_t                uRingIdx;
    /** The fence id. */
    uint64_t                uFenceId;
    /** Size of the response in abResp. */
    size_t                  cbResp;
    /** The response to write when the fence retired. */
    RT_FLEXIBLE_ARRAY_EXTENSION
    uint8_t                 abResp[RT_FLEXIBLE_ARRAY];
} VIRTIOGPUPENDING;
/** Pointer to a fenced command waiting for its fence. */
typedef VIRTIOGPUPENDING *PVIRTIOGPUPENDING;

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
    /** Set by a device reset: the control worker must free all resources (and the 3D state). */
    bool volatile           fDropResources;
    /** Set by the fence callback: the control worker must look at the fenced commands. */
    bool volatile           fFencesSignaled;
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
    /** The host visible window (MMIO2 host window on VIRTIOGPU_HOST_VISIBLE_BAR),
     * NIL_PGMMMIO2HANDLE without Venus. */
    PGMMMIO2HANDLE          hMmio2HostVisible;
    /** The size of the host visible window, 0 without Venus. */
    uint64_t                cbHostVisible;
    /** Protects the resources, the scanout and the display state, see the NOTES. */
    PDMCRITSECT             CritSectState;

    /** @name Statistics.
     * @{ */
    STAMCOUNTER             StatCtrlCmds;
    STAMCOUNTER             StatCursorCmds;
    STAMCOUNTER             StatCmdErrors;
    STAMCOUNTER             StatTransferBytes;
    STAMCOUNTER             StatCtxCreates;
    STAMCOUNTER             StatSubmits;
    STAMCOUNTER             StatSubmitBytes;
    STAMCOUNTER             StatBlobMaps;
    STAMCOUNTER             StatFencesDeferred;
    STAMCOUNTER             StatFencesSignaled;
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
    /** Number of 2D resources in the list. */
    uint32_t                cResources;
    /** Number of backing entries of all resources. */
    uint32_t                cTotalBackingEntries;
    /** Host memory used by all resource shadows. */
    size_t                  cbShadowTotal;
    /** Number of blob resources in the list. */
    uint32_t                cBlobs;
    /** Number of blob resources mapped into the host visible window. */
    uint32_t                cMappedBlobs;
    /** Bytes of the host visible window in use. */
    uint64_t                cbMapped;
    /** Size of all blobs with blob id 0 (host shared memory). */
    uint64_t                cbShmBlobs;
    /** @} */

    /** @name Venus: owned by the control worker, see the NOTES.
     * @{ */
    /** The renderer, NULL without Venus. */
    PVGPURENDERER           pRenderer;
    /** The context slots (VIRTIOGPU_MAX_CONTEXTS entries), NULL without Venus. */
    PVIRTIOGPUCONTEXT       paContexts;
    /** Number of contexts in use. */
    uint32_t                cContexts;
    /** Number of fenced commands in ListPending (atomic, read by the info handler). */
    uint32_t volatile       cPending;
    /** The fenced commands waiting for their fence (VIRTIOGPUPENDING). */
    RTLISTANCHOR            ListPending;
    /** Protects the fence progress, see the NOTES. */
    RTCRITSECT              CritSectFence;
    /** Set when taking a blob out of the host visible window failed: the
     * library must not be cleaned up any more. */
    bool                    fWindowLeaked;
    /** Why Venus is not active, for the info handler. */
    const char             *pszVenusState;
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
 * Looks up a 2D resource, see vgpuR3ResourceFind.
 *
 * @returns The resource or NULL if not found or a blob resource.
 * @param   pThisCC     The ring-3 device state.
 * @param   idResource  The resource ID (guest supplied).
 */
static PVIRTIOGPURESOURCE vgpuR3Resource2dFind(PVIRTIOGPUCC pThisCC, uint32_t idResource)
{
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, idResource);
    return pRes && !pRes->fBlob ? pRes : NULL;
}


/**
 * Looks up a Venus context.
 *
 * Only called by the control worker (the only thread changing the context
 * ids), so no lock is needed.
 *
 * @returns The index of the context slot, UINT32_MAX if not found.
 * @param   pThisCC     The ring-3 device state.
 * @param   idCtx       The context ID (guest supplied).
 */
static uint32_t vgpuR3ContextLookup(PVIRTIOGPUCC pThisCC, uint32_t idCtx)
{
    if (idCtx && pThisCC->paContexts)
        for (uint32_t i = 0; i < VIRTIOGPU_MAX_CONTEXTS; i++)
            if (pThisCC->paContexts[i].idCtx == idCtx)
                return i;
    return UINT32_MAX;
}


/**
 * Frees all resources and disables the scanout.
 *
 * Called by the control worker after a device reset and by the destructor
 * (when no worker is left), after vgpuR3VenusTeardown released the blob
 * resources in the renderer.  Tolerates a half constructed instance.
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
    pThisCC->cBlobs               = 0;
    pThisCC->cMappedBlobs         = 0;
    pThisCC->cbMapped             = 0;
    pThisCC->cbShmBlobs           = 0;
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


/**
 * Takes a blob out of the host visible window, then releases the library's
 * host mapping of it.
 *
 * Control worker or destructor, no locks held: the window helper may
 * rendezvous with the EMTs.
 *
 * @returns VBox status code.  On failure the guest may still reach the
 *          memory, so the library mapping is kept and the resource stays
 *          mapped.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pRes        The blob resource, mapped.
 */
static int vgpuR3BlobUnmap(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTIOGPURESOURCE pRes)
{
    Assert(pRes->fBlob && pRes->fMapped);
    int rc = PDMDevHlpMmio2UnmapHostRange(pDevIns, pThis->hMmio2HostVisible, pRes->offMap, pRes->cbMap);
    /* VERR_RESOURCE_BUSY: another device has part of the range mapped for DMA right now. */
    for (unsigned cTries = 0; rc == VERR_RESOURCE_BUSY && cTries < VIRTIOGPU_UNMAP_BUSY_RETRIES; cTries++)
    {
        RTThreadSleep(VIRTIOGPU_UNMAP_BUSY_SLEEP_MS);
        rc = PDMDevHlpMmio2UnmapHostRange(pDevIns, pThis->hMmio2HostVisible, pRes->offMap, pRes->cbMap);
    }
    if (RT_FAILURE(rc))
    {
        LogRelMax(16, ("%s: Taking blob %RU32 out of the host visible window (%#RX64 LB %#RX64) failed: %Rrc;"
                       " keeping its memory mapped\n", pThis->szInstance, pRes->idResource, pRes->offMap, pRes->cbMap, rc));
        return rc;
    }

    /* The guest cannot reach the memory any more; a failure here only leaks it. */
    vgpuR3RendererResourceUnmap(pThisCC->pRenderer, pRes->idResource);

    vgpuR3StateLock(pDevIns, pThis);
    pThisCC->cMappedBlobs--;
    pThisCC->cbMapped -= pRes->cbMap;
    pRes->fMapped  = false;
    pRes->offMap   = 0;
    pRes->cbMap    = 0;
    pRes->pvMap    = NULL;
    pRes->fMapInfo = 0;
    vgpuR3StateUnlock(pDevIns, pThis);
    return VINF_SUCCESS;
}


/**
 * Releases a blob resource (RESOURCE_UNREF of a blob).
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pRes        The blob resource.
 */
static uint32_t vgpuR3BlobUnref(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTIOGPURESOURCE pRes)
{
    Assert(pRes->fBlob);
    /* Out of the guest's reach first, then the library may free the memory. */
    if (pRes->fMapped && RT_FAILURE(vgpuR3BlobUnmap(pDevIns, pThis, pThisCC, pRes)))
        return VIRTIOGPU_RESP_ERR_UNSPEC;
    vgpuR3RendererResourceUnref(pThisCC->pRenderer, pRes->idResource);

    vgpuR3StateLock(pDevIns, pThis);
    RTListNodeRemove(&pRes->Node);
    pThisCC->cBlobs--;
    if (!pRes->idBlob)
        pThisCC->cbShmBlobs -= pRes->cbBlob;
    vgpuR3StateUnlock(pDevIns, pThis);

    vgpuR3ResourceFree(pRes);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/*********************************************************************************************************************************
*   Command processing                                                                                                           *
*********************************************************************************************************************************/

/**
 * Returns the size of a fixed size control request.
 *
 * @returns Request size in bytes, 0 if the command is not implemented or
 *          needs a feature the driver did not negotiate.
 * @param   uType       The command type (guest supplied).
 * @param   fFeatures   The negotiated features.  The 3D bits are only ever
 *                      set when Venus is active.
 */
static size_t vgpuR3CtrlCmdSize(uint32_t uType, uint64_t fFeatures)
{
    bool const f3d   = RT_BOOL(fFeatures & VIRTIOGPU_F_VIRGL);
    bool const fBlob = RT_BOOL(fFeatures & VIRTIOGPU_F_RESOURCE_BLOB);
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
        case VIRTIOGPU_CMD_GET_CAPSET_INFO:         return f3d   ? sizeof(VIRTIOGPUGETCAPSETINFO) : 0;
        case VIRTIOGPU_CMD_GET_CAPSET:              return f3d   ? sizeof(VIRTIOGPUGETCAPSET) : 0;
        case VIRTIOGPU_CMD_CTX_CREATE:              return f3d   ? sizeof(VIRTIOGPUCTXCREATE) : 0;
        case VIRTIOGPU_CMD_CTX_DESTROY:             return f3d   ? sizeof(VIRTIOGPUCTRLHDR) : 0;
        case VIRTIOGPU_CMD_CTX_ATTACH_RESOURCE:
        case VIRTIOGPU_CMD_CTX_DETACH_RESOURCE:     return f3d   ? sizeof(VIRTIOGPUCTXRESOURCE) : 0;
        case VIRTIOGPU_CMD_SUBMIT_3D:               return f3d   ? sizeof(VIRTIOGPUCMDSUBMIT) : 0;
        case VIRTIOGPU_CMD_RESOURCE_CREATE_BLOB:    return fBlob ? sizeof(VIRTIOGPURESOURCECREATEBLOB) : 0;
        case VIRTIOGPU_CMD_RESOURCE_MAP_BLOB:       return fBlob ? sizeof(VIRTIOGPURESOURCEMAPBLOB) : 0;
        case VIRTIOGPU_CMD_RESOURCE_UNMAP_BLOB:     return fBlob ? sizeof(VIRTIOGPURESOURCEUNMAPBLOB) : 0;
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
    if (pRes->fBlob)
        return vgpuR3BlobUnref(pDevIns, pThis, pThisCC, pRes);

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

    PVIRTIOGPURESOURCE pRes = vgpuR3Resource2dFind(pThisCC, pReq->idResource);
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
    PVIRTIOGPURESOURCE pRes = vgpuR3Resource2dFind(pThisCC, pReq->idResource);
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
    PVIRTIOGPURESOURCE pRes = vgpuR3Resource2dFind(pThisCC, pReq->idResource);
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
    PVIRTIOGPURESOURCE pRes = vgpuR3Resource2dFind(pThisCC, pReq->idResource);
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
    PVIRTIOGPURESOURCE pRes = vgpuR3Resource2dFind(pThisCC, pReq->idResource);
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


/*********************************************************************************************************************************
*   Venus (3D)                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Converts a renderer status code to a response type.
 *
 * @returns Response type.
 * @param   rc          The status code of a failed renderer call.
 */
static uint32_t vgpuR3RespFromRendererRc(int rc)
{
    switch (rc)
    {
        case VERR_NO_MEMORY:            return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
        case VERR_INVALID_PARAMETER:    return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
        default:                        return VIRTIOGPU_RESP_ERR_UNSPEC;
    }
}


/**
 * Writes the response of a parked fenced command and frees it.  Control
 * worker, no locks held.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pPend       The command, already off the list.
 */
static void vgpuR3PendingComplete(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTIOGPUPENDING pPend)
{
    vgpuR3CmdComplete(pDevIns, pThis, pThisCC, VIRTIOGPU_VIRTQ_CONTROL, pPend->pVirtqBuf, pPend->abResp, pPend->cbResp);
    virtioCoreR3VirtqBufRelease(&pThis->Virtio, pPend->pVirtqBuf);
    ASMAtomicDecU32(&pThisCC->cPending);
    RTMemFree(pPend);
}


/**
 * Completes or drops parked fenced commands without looking at their fence.
 *
 * Control worker or destructor.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   iCtx        Only the commands of this context slot, UINT32_MAX
 *                      for all.
 * @param   fDrop       Drop the commands without writing a response or
 *                      touching the queue (the guest's queues are gone).
 */
static void vgpuR3PendingFlush(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, uint32_t iCtx, bool fDrop)
{
    PVIRTIOGPUPENDING pPend, pPendNext;
    RTListForEachSafe(&pThisCC->ListPending, pPend, pPendNext, VIRTIOGPUPENDING, Node)
    {
        if (iCtx == UINT32_MAX || pPend->iCtx == iCtx)
        {
            RTListNodeRemove(&pPend->Node);
            if (!fDrop)
                vgpuR3PendingComplete(pDevIns, pThis, pThisCC, pPend);
            else
            {
                virtioCoreR3VirtqBufRelease(&pThis->Virtio, pPend->pVirtqBuf);
                ASMAtomicDecU32(&pThisCC->cPending);
                RTMemFree(pPend);
            }
        }
    }
}


/**
 * Completes the parked fenced commands whose fence retired.  Control worker.
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 */
static void vgpuR3FencesProcess(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC)
{
    RTLISTANCHOR ListDone;
    RTListInit(&ListDone);
    PVIRTIOGPUPENDING pPend, pPendNext;

    /* The list is ours; the lock is only for the progress the fence callback records. */
    RTCritSectEnter(&pThisCC->CritSectFence);
    RTListForEachSafe(&pThisCC->ListPending, pPend, pPendNext, VIRTIOGPUPENDING, Node)
    {
        if (pThisCC->paContexts[pPend->iCtx].auFenceSignaled[pPend->uRingIdx] >= pPend->uFenceId)
        {
            RTListNodeRemove(&pPend->Node);
            RTListAppend(&ListDone, &pPend->Node);
        }
    }
    RTCritSectLeave(&pThisCC->CritSectFence);

    /* Guest memory is written without any lock held. */
    RTListForEachSafe(&ListDone, pPend, pPendNext, VIRTIOGPUPENDING, Node)
    {
        RTListNodeRemove(&pPend->Node);
        vgpuR3PendingComplete(pDevIns, pThis, pThisCC, pPend);
    }
}


/**
 * Parks a successfully executed fenced command until its fence retired.
 * Control worker.
 *
 * @returns true if parked, false if the caller has to complete the command
 *          now (no such context, out of memory, the fence failed).
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pVirtqBuf   The descriptor chain; a reference is taken.
 * @param   pReqHdr     The request header (host copy).
 * @param   pvResp      The response, header filled in.
 * @param   cbResp      The response size.
 */
static bool vgpuR3FenceDefer(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTQBUF pVirtqBuf,
                             PCVIRTIOGPUCTRLHDR pReqHdr, void const *pvResp, size_t cbResp)
{
    RT_NOREF(pDevIns);
    uint32_t const iCtx = vgpuR3ContextLookup(pThisCC, pReqHdr->uCtxId);
    if (iCtx == UINT32_MAX)
        return false; /* No context (any more), e.g. CTX_DESTROY: nothing to wait for. */
    uint32_t const uRingIdx = pReqHdr->fFlags & VIRTIOGPU_FLAG_INFO_RING_IDX ? pReqHdr->uRingIdx : 0;
    AssertReturn(uRingIdx < VIRTIOGPU_MAX_RINGS, false); /* Checked by vgpuR3CtrlCmdProcess. */

    PVIRTIOGPUPENDING pPend = (PVIRTIOGPUPENDING)RTMemAlloc(RT_UOFFSETOF(VIRTIOGPUPENDING, abResp) + cbResp);
    if (!pPend)
    {
        LogRelMax(16, ("%s: Out of memory for a fenced command, signalling fence %#RX64 early\n",
                       pThis->szInstance, pReqHdr->uFenceId));
        return false;
    }

    int rc = vgpuR3RendererContextFence(pThisCC->pRenderer, pReqHdr->uCtxId, uRingIdx, pReqHdr->uFenceId);
    if (RT_FAILURE(rc))
    {
        RTMemFree(pPend);
        return false;
    }

    /* The fence may have retired already: the callback then set fFencesSignaled
       and this worker finds the command on its next round. */
    pPend->pVirtqBuf = pVirtqBuf;
    pPend->iCtx      = iCtx;
    pPend->uRingIdx  = uRingIdx;
    pPend->uFenceId  = pReqHdr->uFenceId;
    pPend->cbResp    = cbResp;
    memcpy(pPend->abResp, pvResp, cbResp);
    virtioCoreR3VirtqBufRetain(pVirtqBuf);
    RTListAppend(&pThisCC->ListPending, &pPend->Node);
    ASMAtomicIncU32(&pThisCC->cPending);
    STAM_REL_COUNTER_INC(&pThis->StatFencesDeferred);
    return true;
}


/**
 * Destroys a context: in the library, then its parked commands, then the
 * slot.  Control worker or destructor.
 *
 * @param   pDevIns         The device instance.
 * @param   pThis           The shared device state.
 * @param   pThisCC         The ring-3 device state.
 * @param   iCtx            The context slot, in use.
 * @param   fDropPending    Drop the parked commands instead of completing
 *                          them (the guest's queues are gone).
 */
static void vgpuR3ContextDestroy(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, uint32_t iCtx, bool fDropPending)
{
    PVIRTIOGPUCONTEXT pCtx = &pThisCC->paContexts[iCtx];
    Assert(pCtx->idCtx);

    /* The library joins the context's fence thread: no callback for it after this. */
    vgpuR3RendererContextDestroy(pThisCC->pRenderer, pCtx->idCtx);

    /* Its fences will never retire now; the guest does not wait for them any more. */
    vgpuR3PendingFlush(pDevIns, pThis, pThisCC, iCtx, fDropPending);

    RTCritSectEnter(&pThisCC->CritSectFence);
    vgpuR3StateLock(pDevIns, pThis);
    pCtx->idCtx        = 0;
    pCtx->fContextInit = 0;
    RT_ZERO(pCtx->auFenceSignaled);
    pCtx->szName[0]    = '\0';
    pThisCC->cContexts--;
    vgpuR3StateUnlock(pDevIns, pThis);
    RTCritSectLeave(&pThisCC->CritSectFence);
}


/**
 * Drops all Venus state, in the order given in the NOTES.
 *
 * Called by the control worker after a device reset and by the destructor
 * (when no worker is left).  The resource structures are freed afterwards by
 * vgpuR3ResourcesDestroyAll().
 *
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 */
static void vgpuR3VenusTeardown(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC)
{
    if (!pThisCC->pRenderer)
        return;

    /* 1. Take the blobs out of the host visible window, then release the library mappings. */
    PVIRTIOGPURESOURCE pRes;
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        if (   pRes->fBlob
            && pRes->fMapped
            && RT_FAILURE(vgpuR3BlobUnmap(pDevIns, pThis, pThisCC, pRes)))
        {
            pRes->fMapLeaked       = true;
            pThisCC->fWindowLeaked = true;
        }
    }

    /* 2. The contexts, dropping their parked commands: the queues are gone. */
    for (uint32_t iCtx = 0; iCtx < VIRTIOGPU_MAX_CONTEXTS; iCtx++)
        if (pThisCC->paContexts[iCtx].idCtx)
            vgpuR3ContextDestroy(pDevIns, pThis, pThisCC, iCtx, true /*fDropPending*/);

    /* 3. The blob resources, except those whose memory the guest may still reach. */
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        if (pRes->fBlob && !pRes->fMapLeaked)
            vgpuR3RendererResourceUnref(pThisCC->pRenderer, pRes->idResource);
    }

    /* 4. Nothing should be parked any more, but make sure. */
    vgpuR3PendingFlush(pDevIns, pThis, pThisCC, UINT32_MAX, true /*fDrop*/);
}


/**
 * VIRTIOGPU_CMD_GET_CAPSET_INFO.
 *
 * @returns Response type.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 * @param   pResp       The response to fill in (zeroed, header filled in by the caller).
 */
static uint32_t vgpuR3CmdGetCapsetInfo(PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PCVIRTIOGPUGETCAPSETINFO pReq,
                                       PVIRTIOGPURESPCAPSETINFO pResp)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    if (pReq->iCapset >= pThis->Config.cCapsets)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    /* There is one capset, Venus. */
    uint32_t uMaxVersion = 0;
    uint32_t cbCapset    = 0;
    int rc = vgpuR3RendererQueryCapset(pThisCC->pRenderer, VIRTIOGPU_CAPSET_VENUS, &uMaxVersion, &cbCapset, NULL);
    AssertRCReturn(rc, VIRTIOGPU_RESP_ERR_UNSPEC);
    pResp->idCapset          = VIRTIOGPU_CAPSET_VENUS;
    pResp->uCapsetMaxVersion = uMaxVersion;
    pResp->cbCapsetMax       = cbCapset;
    return VIRTIOGPU_RESP_OK_CAPSET_INFO;
}


/**
 * VIRTIOGPU_CMD_GET_CAPSET.
 *
 * @returns Response type.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 * @param   ppbResp     Where to return the response (header zeroed, capset
 *                      data filled in), to be freed with RTMemFree.
 * @param   pcbResp     Where to return the response size.
 */
static uint32_t vgpuR3CmdGetCapset(PVIRTIOGPUCC pThisCC, PCVIRTIOGPUGETCAPSET pReq, uint8_t **ppbResp, size_t *pcbResp)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    uint32_t       uMaxVersion = 0;
    uint32_t       cbCapset    = 0;
    uint8_t const *pbCapset    = NULL;
    int rc = vgpuR3RendererQueryCapset(pThisCC->pRenderer, pReq->idCapset, &uMaxVersion, &cbCapset, &pbCapset);
    if (RT_FAILURE(rc) || pReq->uCapsetVersion > uMaxVersion)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    /* cbCapset is at most 4 KB, checked when the renderer was created. */
    size_t const cbResp = sizeof(VIRTIOGPUCTRLHDR) + cbCapset;
    uint8_t     *pbResp = (uint8_t *)RTMemAllocZ(cbResp);
    if (!pbResp)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    memcpy(pbResp + sizeof(VIRTIOGPUCTRLHDR), pbCapset, cbCapset);
    *ppbResp = pbResp;
    *pcbResp = cbResp;
    return VIRTIOGPU_RESP_OK_CAPSET;
}


/**
 * VIRTIOGPU_CMD_CTX_CREATE.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdCtxCreate(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PCVIRTIOGPUCTXCREATE pReq)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    uint32_t const idCtx = pReq->Hdr.uCtxId;
    /* The library takes the id as an int in places. */
    if (!idCtx || idCtx > INT32_MAX || vgpuR3ContextLookup(pThisCC, idCtx) != UINT32_MAX)
        return VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID;
    /* Venus contexts only: capset id 4 and no other flag, which needs CONTEXT_INIT. */
    if (   !(pThis->fNegotiatedFeatures & VIRTIOGPU_F_CONTEXT_INIT)
        || pReq->fContextInit != VIRTIOGPU_CAPSET_VENUS)
    {
        LogRelMax(16, ("%s: Refusing context %RU32 with context_init %#RX32 (only Venus contexts are supported)\n",
                       pThis->szInstance, idCtx, pReq->fContextInit));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    uint32_t const cchName = pReq->cchDebugName;
    if (cchName > sizeof(pReq->achDebugName))
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    uint32_t iCtx = 0;
    while (iCtx < VIRTIOGPU_MAX_CONTEXTS && pThisCC->paContexts[iCtx].idCtx)
        iCtx++;
    if (iCtx >= VIRTIOGPU_MAX_CONTEXTS)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    RT_UNTRUSTED_VALIDATED_FENCE();

    /* Only printable ASCII of the debug name goes to the library and the log. */
    char szName[RT_SIZEOFMEMB(VIRTIOGPUCONTEXT, szName)];
    AssertCompile(sizeof(szName) > sizeof(pReq->achDebugName));
    for (uint32_t i = 0; i < cchName; i++)
    {
        char const ch = pReq->achDebugName[i];
        szName[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
    }
    szName[cchName] = '\0';

    /* Claim the slot before the library knows the context, so no fence of it is missed. */
    PVIRTIOGPUCONTEXT pCtx = &pThisCC->paContexts[iCtx];
    RTCritSectEnter(&pThisCC->CritSectFence);
    vgpuR3StateLock(pDevIns, pThis);
    pCtx->idCtx        = idCtx;
    pCtx->fContextInit = pReq->fContextInit;
    RT_ZERO(pCtx->auFenceSignaled);
    memcpy(pCtx->szName, szName, cchName + 1);
    pThisCC->cContexts++;
    vgpuR3StateUnlock(pDevIns, pThis);
    RTCritSectLeave(&pThisCC->CritSectFence);

    int rc = vgpuR3RendererContextCreate(pThisCC->pRenderer, idCtx, pReq->fContextInit, szName, cchName);
    if (RT_FAILURE(rc))
    {
        RTCritSectEnter(&pThisCC->CritSectFence);
        vgpuR3StateLock(pDevIns, pThis);
        pCtx->idCtx        = 0;
        pCtx->fContextInit = 0;
        pCtx->szName[0]    = '\0';
        pThisCC->cContexts--;
        vgpuR3StateUnlock(pDevIns, pThis);
        RTCritSectLeave(&pThisCC->CritSectFence);
        return vgpuR3RespFromRendererRc(rc);
    }

    STAM_REL_COUNTER_INC(&pThis->StatCtxCreates);
    LogRelMax(64, ("%s: Venus context %RU32 '%s' created\n", pThis->szInstance, idCtx, szName));
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_CTX_DESTROY.
 *
 * Parked commands of the context are completed: their fences never retire
 * now, and the guest stopped waiting for them when it destroyed the context.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReqHdr     The request (host copy), just a header.
 */
static uint32_t vgpuR3CmdCtxDestroy(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PCVIRTIOGPUCTRLHDR pReqHdr)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    uint32_t const iCtx = vgpuR3ContextLookup(pThisCC, pReqHdr->uCtxId);
    if (iCtx == UINT32_MAX)
        return VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID;
    vgpuR3ContextDestroy(pDevIns, pThis, pThisCC, iCtx, false /*fDropPending*/);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_CTX_ATTACH_RESOURCE and VIRTIOGPU_CMD_CTX_DETACH_RESOURCE.
 *
 * 2D resources are not known to the library; attaching them (Linux attaches
 * every buffer object a context's file opens) is accepted without effect.
 *
 * @returns Response type.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 * @param   fAttach     Attach (true) or detach (false).
 */
static uint32_t vgpuR3CmdCtxResource(PVIRTIOGPUCC pThisCC, PCVIRTIOGPUCTXRESOURCE pReq, bool fAttach)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    if (vgpuR3ContextLookup(pThisCC, pReq->Hdr.uCtxId) == UINT32_MAX)
        return VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID;
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (pRes->fBlob)
    {
        /* Both ids were checked to be at most INT32_MAX when created. */
        if (fAttach)
            vgpuR3RendererContextAttach(pThisCC->pRenderer, pReq->Hdr.uCtxId, pRes->idResource);
        else
            vgpuR3RendererContextDetach(pThisCC->pRenderer, pReq->Hdr.uCtxId, pRes->idResource);
    }
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * VIRTIOGPU_CMD_SUBMIT_3D.
 *
 * The command stream follows the request in the descriptor chain.  It is
 * copied into host memory, so the guest cannot change it while the library
 * parses it.
 *
 * @returns Response type.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pVirtqBuf   The descriptor chain, positioned after the request.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdSubmit3d(PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC, PVIRTQBUF pVirtqBuf, PCVIRTIOGPUCMDSUBMIT pReq)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    uint32_t const idCtx = pReq->Hdr.uCtxId;
    if (vgpuR3ContextLookup(pThisCC, idCtx) == UINT32_MAX)
        return VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID;
    uint32_t const cbData = pReq->cbData;
    if ((cbData & 3) || cbData > VIRTIOGPU_MAX_SUBMIT_SIZE)
    {
        LogRelMax(16, ("%s: SUBMIT_3D of %#RX32 bytes refused (not a multiple of 4 or above %#x)\n",
                       pThis->szInstance, cbData, VIRTIOGPU_MAX_SUBMIT_SIZE));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    if (pVirtqBuf->cbPhysSend < cbData)
    {
        LogRelMax(16, ("%s: SUBMIT_3D of %#RX32 bytes but only %zu bytes left in the request\n",
                       pThis->szInstance, cbData, pVirtqBuf->cbPhysSend));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    RT_UNTRUSTED_VALIDATED_FENCE();
    if (!cbData)
        return VIRTIOGPU_RESP_OK_NODATA;

    void *pvCmds = RTMemAlloc(cbData); /* At least 8 byte aligned, the library wants 4. */
    if (!pvCmds)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, pvCmds, cbData);

    int rc = vgpuR3RendererSubmit(pThisCC->pRenderer, idCtx, pvCmds, cbData);
    RTMemFree(pvCmds);
    STAM_REL_COUNTER_INC(&pThis->StatSubmits);
    STAM_REL_COUNTER_ADD(&pThis->StatSubmitBytes, cbData);
    return RT_SUCCESS(rc) ? VIRTIOGPU_RESP_OK_NODATA : VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_CREATE_BLOB.
 *
 * Only host memory blobs (VIRTIOGPU_BLOB_MEM_HOST3D) of a Venus context are
 * supported; guest memory blobs are refused.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdResourceCreateBlob(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                            PCVIRTIOGPURESOURCECREATEBLOB pReq)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    uint32_t const idResource = pReq->idResource;
    /* The library takes the id as an int in places. */
    if (!idResource || idResource > INT32_MAX || vgpuR3ResourceFind(pThisCC, idResource))
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (   pReq->uBlobMem != VIRTIOGPU_BLOB_MEM_HOST3D
        || pReq->cEntries != 0
        || (pReq->fBlobFlags & ~VIRTIOGPU_BLOB_FLAG_VALID_MASK)
        || !pReq->cbBlob
        || pReq->cbBlob > VIRTIOGPU_MAX_BLOB_SIZE)
    {
        LogRelMax(16, ("%s: Refusing blob %RU32: memory type %RU32, %RU32 entries, flags %#RX32, %#RX64 bytes\n",
                       pThis->szInstance, idResource, pReq->uBlobMem, pReq->cEntries, pReq->fBlobFlags, pReq->cbBlob));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    if (vgpuR3ContextLookup(pThisCC, pReq->Hdr.uCtxId) == UINT32_MAX)
        return VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID;
    if (pThisCC->cBlobs >= VIRTIOGPU_MAX_BLOB_RESOURCES)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    if (   !pReq->idBlob
        && pReq->cbBlob > VIRTIOGPU_MAX_SHM_BLOB_MEMORY - pThisCC->cbShmBlobs)
    {
        LogRelMax(16, ("%s: Blob %RU32 (%#RX64 bytes) exceeds the shared memory limit\n",
                       pThis->szInstance, idResource, pReq->cbBlob));
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    }
    RT_UNTRUSTED_VALIDATED_FENCE();

    PVIRTIOGPURESOURCE pRes = (PVIRTIOGPURESOURCE)RTMemAllocZ(sizeof(*pRes));
    if (!pRes)
        return VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY;
    int rc = vgpuR3RendererBlobCreate(pThisCC->pRenderer, pReq->Hdr.uCtxId, idResource, pReq->uBlobMem, pReq->fBlobFlags,
                                      pReq->idBlob, pReq->cbBlob);
    if (RT_FAILURE(rc))
    {
        RTMemFree(pRes);
        return vgpuR3RespFromRendererRc(rc);
    }
    pRes->idResource   = idResource;
    pRes->fBlob        = true;
    pRes->uBlobMem     = pReq->uBlobMem;
    pRes->fBlobFlags   = pReq->fBlobFlags;
    pRes->idCtxCreator = pReq->Hdr.uCtxId;
    pRes->idBlob       = pReq->idBlob;
    pRes->cbBlob       = pReq->cbBlob;

    vgpuR3StateLock(pDevIns, pThis);
    RTListAppend(&pThisCC->ListResources, &pRes->Node);
    pThisCC->cBlobs++;
    if (!pRes->idBlob)
        pThisCC->cbShmBlobs += pRes->cbBlob;
    vgpuR3StateUnlock(pDevIns, pThis);
    return VIRTIOGPU_RESP_OK_NODATA;
}


/**
 * Checks whether a part of the host visible window is in use.
 *
 * @returns true if any page of it is backed by a blob.
 * @param   pThisCC     The ring-3 device state.
 * @param   off         The offset, inside the window.
 * @param   cb          The size, inside the window.
 */
static bool vgpuR3WindowRangeInUse(PVIRTIOGPUCC pThisCC, uint64_t off, uint64_t cb)
{
    /* All values are below 16 GB (VIRTIOGPU_HOST_VISIBLE_SIZE_MAX), no overflow. */
    PVIRTIOGPURESOURCE pRes;
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        if (   pRes->fBlob
            && pRes->fMapped
            && off < pRes->offMap + pRes->cbMap
            && pRes->offMap < off + cb)
            return true;
    }
    return false;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_MAP_BLOB.
 *
 * The guest picks the offset in the host visible window; the blob's size
 * rounded up to whole pages is what it will access there.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 * @param   pResp       The response to fill in (zeroed, header filled in by the caller).
 */
static uint32_t vgpuR3CmdResourceMapBlob(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                         PCVIRTIOGPURESOURCEMAPBLOB pReq, PVIRTIOGPURESPMAPINFO pResp)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes || !pRes->fBlob)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!(pRes->fBlobFlags & VIRTIOGPU_BLOB_FLAG_USE_MAPPABLE) || pRes->fMapped)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;

    /* cbBlob is at most 4 GB, no overflow. */
    uint64_t const offRegion = pReq->offRegion;
    uint64_t const cbWant    = RT_ALIGN_64(pRes->cbBlob, VIRTIOGPU_BLOB_PAGE_SIZE);
    if (   (offRegion & (VIRTIOGPU_BLOB_PAGE_SIZE - 1))
        || offRegion >= pThis->cbHostVisible
        || cbWant > pThis->cbHostVisible - offRegion
        || vgpuR3WindowRangeInUse(pThisCC, offRegion, cbWant))
    {
        LogRelMax(16, ("%s: Refusing to map blob %RU32 (%#RX64 bytes) at %#RX64 of the %#RX64 byte window\n",
                       pThis->szInstance, pRes->idResource, pRes->cbBlob, offRegion, pThis->cbHostVisible));
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    }
    RT_UNTRUSTED_VALIDATED_FENCE();

    void    *pvHost   = NULL;
    uint64_t cbHost   = 0;
    uint32_t fMapInfo = 0;
    int rc = vgpuR3RendererResourceMap(pThisCC->pRenderer, pRes->idResource, &pvHost, &cbHost, &fMapInfo);
    if (RT_FAILURE(rc))
        return VIRTIOGPU_RESP_ERR_UNSPEC;

    /* Never hand out more than the library mapped, and only whole pages of it. */
    uint64_t const cbMap = RT_MIN(cbHost, cbWant);
    if (   ((uintptr_t)pvHost & (VIRTIOGPU_BLOB_PAGE_SIZE - 1))
        || (cbMap & (VIRTIOGPU_BLOB_PAGE_SIZE - 1))
        || !cbMap
        || (   fMapInfo != VIRTIOGPU_MAP_CACHE_CACHED
            && fMapInfo != VIRTIOGPU_MAP_CACHE_UNCACHED
            && fMapInfo != VIRTIOGPU_MAP_CACHE_WC))
    {
        LogRelMax(16, ("%s: Unusable mapping of blob %RU32: %p LB %#RX64 (want %#RX64), map info %#RX32\n",
                       pThis->szInstance, pRes->idResource, pvHost, cbHost, cbWant, fMapInfo));
        vgpuR3RendererResourceUnmap(pThisCC->pRenderer, pRes->idResource);
        return VIRTIOGPU_RESP_ERR_UNSPEC;
    }
    if (cbMap < cbWant)
        LogRelMax(16, ("%s: Blob %RU32 maps only %#RX64 of %#RX64 bytes, the rest of the range stays unbacked\n",
                       pThis->szInstance, pRes->idResource, cbMap, cbWant));

    /* No lock held: the helper may rendezvous with the EMTs. */
    rc = PDMDevHlpMmio2MapHostRange(pDevIns, pThis->hMmio2HostVisible, offRegion, pvHost, cbMap, 0 /*fFlags*/);
    if (RT_FAILURE(rc))
    {
        LogRelMax(16, ("%s: Mapping blob %RU32 at %#RX64 LB %#RX64 of the host visible window failed: %Rrc\n",
                       pThis->szInstance, pRes->idResource, offRegion, cbMap, rc));
        vgpuR3RendererResourceUnmap(pThisCC->pRenderer, pRes->idResource);
        return rc == VERR_RESOURCE_BUSY ? VIRTIOGPU_RESP_ERR_INVALID_PARAMETER : VIRTIOGPU_RESP_ERR_UNSPEC;
    }

    vgpuR3StateLock(pDevIns, pThis);
    pRes->fMapped  = true;
    pRes->offMap   = offRegion;
    pRes->cbMap    = cbMap;
    pRes->pvMap    = pvHost;
    pRes->fMapInfo = fMapInfo;
    pThisCC->cMappedBlobs++;
    pThisCC->cbMapped += cbMap;
    vgpuR3StateUnlock(pDevIns, pThis);

    STAM_REL_COUNTER_INC(&pThis->StatBlobMaps);
    pResp->fMapInfo = fMapInfo;
    return VIRTIOGPU_RESP_OK_MAP_INFO;
}


/**
 * VIRTIOGPU_CMD_RESOURCE_UNMAP_BLOB.
 *
 * @returns Response type.
 * @param   pDevIns     The device instance.
 * @param   pThis       The shared device state.
 * @param   pThisCC     The ring-3 device state.
 * @param   pReq        The request (host copy).
 */
static uint32_t vgpuR3CmdResourceUnmapBlob(PPDMDEVINS pDevIns, PVIRTIOGPU pThis, PVIRTIOGPUCC pThisCC,
                                           PCVIRTIOGPURESOURCEUNMAPBLOB pReq)
{
    AssertReturn(pThisCC->pRenderer, VIRTIOGPU_RESP_ERR_UNSPEC);
    PVIRTIOGPURESOURCE pRes = vgpuR3ResourceFind(pThisCC, pReq->idResource);
    if (!pRes || !pRes->fBlob)
        return VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!pRes->fMapped)
        return VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
    int rc = vgpuR3BlobUnmap(pDevIns, pThis, pThisCC, pRes);
    return RT_SUCCESS(rc) ? VIRTIOGPU_RESP_OK_NODATA : VIRTIOGPU_RESP_ERR_UNSPEC;
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
    VIRTIOGPUCTRLRESP uResp; /* The fixed size responses; all of them start with the header. */
    RT_ZERO(uResp);
    uint8_t *pbRespAlloc = NULL; /* GET_CAPSET has a variable size response. */
    size_t   cbResp      = sizeof(VIRTIOGPUCTRLHDR);
    uint32_t uRespType   = VIRTIOGPU_RESP_ERR_UNSPEC;
    uint64_t const fFeatures = pThis->fNegotiatedFeatures;

    STAM_REL_COUNTER_INC(&pThis->StatCtrlCmds);

    /* The VirtioCore drain helper never reads more than the chain holds
       (pVirtqBuf->cbPhysSend), but the sizes must be checked before draining. */
    if (pVirtqBuf->cbPhysSend >= sizeof(uReq.Hdr))
    {
        virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, &uReq.Hdr, sizeof(uReq.Hdr));

        size_t const cbReq = vgpuR3CtrlCmdSize(uReq.Hdr.uType, fFeatures);
        Assert(cbReq <= sizeof(uReq));
        if (!cbReq)
            Log(("%s: Unsupported command %#RX32\n", pThis->szInstance, uReq.Hdr.uType));
        else if (pVirtqBuf->cbPhysSend < cbReq - sizeof(uReq.Hdr))
            LogRelMax(16, ("%s: Truncated command %#RX32 (%zu bytes left, %zu needed)\n",
                           pThis->szInstance, uReq.Hdr.uType, pVirtqBuf->cbPhysSend, cbReq - sizeof(uReq.Hdr)));
        else if (   (fFeatures & VIRTIOGPU_F_VIRGL)
                 && (uReq.Hdr.fFlags & (VIRTIOGPU_FLAG_FENCE | VIRTIOGPU_FLAG_INFO_RING_IDX))
                    == (VIRTIOGPU_FLAG_FENCE | VIRTIOGPU_FLAG_INFO_RING_IDX)
                 && uReq.Hdr.uRingIdx >= VIRTIOGPU_MAX_RINGS)
        {
            LogRelMax(16, ("%s: Command %#RX32 with fence ring %u refused\n",
                           pThis->szInstance, uReq.Hdr.uType, uReq.Hdr.uRingIdx));
            uRespType = VIRTIOGPU_RESP_ERR_INVALID_PARAMETER;
        }
        else
        {
            if (cbReq > sizeof(uReq.Hdr))
                virtioCoreR3VirtqBufDrain(&pThis->Virtio, pVirtqBuf, (uint8_t *)&uReq + sizeof(uReq.Hdr),
                                          cbReq - sizeof(uReq.Hdr));

            switch (uReq.Hdr.uType)
            {
                case VIRTIOGPU_CMD_GET_DISPLAY_INFO:
                    uRespType = vgpuR3CmdGetDisplayInfo(pDevIns, pThis, pThisCC, &uResp.DisplayInfo);
                    cbResp    = sizeof(uResp.DisplayInfo);
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
                case VIRTIOGPU_CMD_GET_CAPSET_INFO:
                    uRespType = vgpuR3CmdGetCapsetInfo(pThis, pThisCC, &uReq.GetCapsetInfo, &uResp.CapsetInfo);
                    cbResp    = sizeof(uResp.CapsetInfo);
                    break;
                case VIRTIOGPU_CMD_GET_CAPSET:
                    uRespType = vgpuR3CmdGetCapset(pThisCC, &uReq.GetCapset, &pbRespAlloc, &cbResp);
                    break;
                case VIRTIOGPU_CMD_CTX_CREATE:
                    uRespType = vgpuR3CmdCtxCreate(pDevIns, pThis, pThisCC, &uReq.CtxCreate);
                    break;
                case VIRTIOGPU_CMD_CTX_DESTROY:
                    uRespType = vgpuR3CmdCtxDestroy(pDevIns, pThis, pThisCC, &uReq.Hdr);
                    break;
                case VIRTIOGPU_CMD_CTX_ATTACH_RESOURCE:
                    uRespType = vgpuR3CmdCtxResource(pThisCC, &uReq.CtxResource, true /*fAttach*/);
                    break;
                case VIRTIOGPU_CMD_CTX_DETACH_RESOURCE:
                    uRespType = vgpuR3CmdCtxResource(pThisCC, &uReq.CtxResource, false /*fAttach*/);
                    break;
                case VIRTIOGPU_CMD_SUBMIT_3D:
                    uRespType = vgpuR3CmdSubmit3d(pThis, pThisCC, pVirtqBuf, &uReq.Submit);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_CREATE_BLOB:
                    uRespType = vgpuR3CmdResourceCreateBlob(pDevIns, pThis, pThisCC, &uReq.CreateBlob);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_MAP_BLOB:
                    uRespType = vgpuR3CmdResourceMapBlob(pDevIns, pThis, pThisCC, &uReq.MapBlob, &uResp.MapInfo);
                    cbResp    = sizeof(uResp.MapInfo);
                    break;
                case VIRTIOGPU_CMD_RESOURCE_UNMAP_BLOB:
                    uRespType = vgpuR3CmdResourceUnmapBlob(pDevIns, pThis, pThisCC, &uReq.UnmapBlob);
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
        RTMemFree(pbRespAlloc);
        pbRespAlloc = NULL;
        cbResp      = sizeof(VIRTIOGPUCTRLHDR);
        Log(("%s: Command %#RX32 failed: %#RX32\n", pThis->szInstance, uReq.Hdr.uType, uRespType));
    }
    void * const      pvResp   = pbRespAlloc ? (void *)pbRespAlloc : (void *)&uResp;
    PVIRTIOGPUCTRLHDR pRespHdr = (PVIRTIOGPUCTRLHDR)pvResp;
    Assert(cbResp >= sizeof(*pRespHdr) && (pbRespAlloc || cbResp <= sizeof(uResp)));

    /* Fenced commands must echo the fence (the EDK2 driver relies on it). */
    pRespHdr->uType = uRespType;
    if (uReq.Hdr.fFlags & VIRTIOGPU_FLAG_FENCE)
    {
        pRespHdr->fFlags   = uReq.Hdr.fFlags & (VIRTIOGPU_FLAG_FENCE | VIRTIOGPU_FLAG_INFO_RING_IDX);
        pRespHdr->uFenceId = uReq.Hdr.uFenceId;
        pRespHdr->uCtxId   = uReq.Hdr.uCtxId;
        pRespHdr->uRingIdx = uReq.Hdr.uRingIdx;
    }

    /* 2D commands complete synchronously, so their fence is signalled with the
       response.  A successful fenced command of a Venus context waits for the
       renderer to retire a fence created behind it (see the NOTES). */
    bool fParked = false;
    if (   (uReq.Hdr.fFlags & VIRTIOGPU_FLAG_FENCE)
        && uRespType < VIRTIOGPU_RESP_ERR_UNSPEC
        && (fFeatures & VIRTIOGPU_F_VIRGL)
        && pThisCC->pRenderer
        && uReq.Hdr.uCtxId != 0)
        fParked = vgpuR3FenceDefer(pDevIns, pThis, pThisCC, pVirtqBuf, &uReq.Hdr, pvResp, cbResp);
    if (!fParked)
        vgpuR3CmdComplete(pDevIns, pThis, pThisCC, VIRTIOGPU_VIRTQ_CONTROL, pVirtqBuf, pvResp, cbResp);
    RTMemFree(pbRespAlloc);
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
 * @callback_method_impl{FNVGPURENDERERFENCE}
 *
 * Runs on a renderer library thread: records the progress of the ring and
 * wakes the control worker, which completes the commands (see the NOTES).
 */
static DECLCALLBACK(void) vgpuR3FenceCallback(void *pvUser, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId)
{
    PPDMDEVINS   pDevIns = (PPDMDEVINS)pvUser;
    PVIRTIOGPU   pThis   = PDMDEVINS_2_DATA(pDevIns, PVIRTIOGPU);
    PVIRTIOGPUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PVIRTIOGPUCC);
    AssertPtrReturnVoid(pThisCC->paContexts);
    /* The library echoes what we passed; don't trust it blindly (free slots have id 0). */
    if (uRingIdx >= VIRTIOGPU_MAX_RINGS || !idCtx)
    {
        LogRelMax(16, ("%s: Ignoring fence %#RX64 for context %RU32 ring %RU32\n", pThis->szInstance, uFenceId, idCtx, uRingIdx));
        return;
    }

    RTCritSectEnter(&pThisCC->CritSectFence);
    for (uint32_t i = 0; i < VIRTIOGPU_MAX_CONTEXTS; i++)
    {
        PVIRTIOGPUCONTEXT pCtx = &pThisCC->paContexts[i];
        if (pCtx->idCtx == idCtx)
        {
            /* Fences retire in order per ring; mergeable ones may not be reported. */
            if (uFenceId > pCtx->auFenceSignaled[uRingIdx])
                pCtx->auFenceSignaled[uRingIdx] = uFenceId;
            break;
        }
    }
    RTCritSectLeave(&pThisCC->CritSectFence);

    STAM_REL_COUNTER_INC(&pThis->StatFencesSignaled);
    ASMAtomicWriteBool(&pThis->fFencesSignaled, true);
    vgpuR3WakeWorker(pDevIns, pThis, VIRTIOGPU_VIRTQ_CONTROL);
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
        /* After a device reset the control worker, which owns the resources and
           the 3D state, frees them. */
        if (   uVirtq == VIRTIOGPU_VIRTQ_CONTROL
            && ASMAtomicXchgBool(&pThis->fDropResources, false))
        {
            vgpuR3VenusTeardown(pDevIns, pThis, pThisCC);
            vgpuR3ResourcesDestroyAll(pDevIns, pThis, pThisCC);
        }

        /* Count ourselves as active before checking fQuiescing, so the quiesce
           code either sees us or we see it. */
        bool fProcessed = false;
        ASMAtomicIncU32(&pThis->cActiveCmds);
        if (vgpuR3IsVirtqReady(pThis, uVirtq))
        {
            /* Complete the fenced commands whose fence retired.  The flag is only
               consumed while the queue is usable; vgpuR3Resume wakes us again. */
            if (   uVirtq == VIRTIOGPU_VIRTQ_CONTROL
                && ASMAtomicXchgBool(&pThis->fFencesSignaled, false))
                vgpuR3FencesProcess(pDevIns, pThis, pThisCC);

            /* With too many fenced commands parked, take no new buffers until
               fences retire (the fence callback wakes us up). */
            PVIRTQBUF pVirtqBuf = NULL;
            if (   uVirtq != VIRTIOGPU_VIRTQ_CONTROL
                || ASMAtomicReadU32(&pThisCC->cPending) < VIRTIOGPU_MAX_PENDING_FENCES)
            {
                pVirtqBuf = virtioCoreR3VirtqBufAlloc();
                if (!pVirtqBuf)
                    LogRelMax(16, ("%s: Out of memory for a queue buffer\n", pThis->szInstance));
            }
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
    pHlp->pfnPrintf(pHlp, "Resources: %RU32 (shadow memory %zu bytes, %RU32 backing entries), %RU32 blobs\n",
                    pThisCC->cResources, pThisCC->cbShadowTotal, pThisCC->cTotalBackingEntries, pThisCC->cBlobs);
    PVIRTIOGPURESOURCE pRes;
    RTListForEach(&pThisCC->ListResources, pRes, VIRTIOGPURESOURCE, Node)
    {
        if (!pRes->fBlob)
            pHlp->pfnPrintf(pHlp, "  #%RU32: format %RU32, %RU32x%RU32, backing %RU32 entries / %RU64 bytes\n",
                            pRes->idResource, pRes->uFormat, pRes->cx, pRes->cy, pRes->cEntries, pRes->cbBacking);
        else if (pRes->fMapped)
            pHlp->pfnPrintf(pHlp, "  #%RU32: blob %#RX64 of context %RU32, %#RX64 bytes, flags %#RX32, mapped at %#RX64 LB %#RX64"
                            " (cache %RU32)\n", pRes->idResource, pRes->idBlob, pRes->idCtxCreator, pRes->cbBlob, pRes->fBlobFlags,
                            pRes->offMap, pRes->cbMap, pRes->fMapInfo);
        else
            pHlp->pfnPrintf(pHlp, "  #%RU32: blob %#RX64 of context %RU32, %#RX64 bytes, flags %#RX32%s\n",
                            pRes->idResource, pRes->idBlob, pRes->idCtxCreator, pRes->cbBlob, pRes->fBlobFlags,
                            pRes->fMapLeaked ? ", mapping leaked" : "");
    }

    if (pThisCC->pRenderer)
    {
        pHlp->pfnPrintf(pHlp, "Venus: %RU32 context(s), %RU32 fenced command(s) waiting, %#RX64 shared memory blob bytes\n",
                        pThisCC->cContexts, ASMAtomicReadU32(&pThisCC->cPending), pThisCC->cbShmBlobs);
        pHlp->pfnPrintf(pHlp, "Host visible window: BAR%u, %#RX64 bytes, %RU32 blob(s) mapped using %#RX64 bytes%s\n",
                        VIRTIOGPU_HOST_VISIBLE_BAR, pThis->cbHostVisible, pThisCC->cMappedBlobs, pThisCC->cbMapped,
                        pThisCC->fWindowLeaked ? ", some mappings leaked" : "");
        for (uint32_t i = 0; i < VIRTIOGPU_MAX_CONTEXTS; i++)
            if (pThisCC->paContexts[i].idCtx)
                pHlp->pfnPrintf(pHlp, "  context %RU32: '%s', context_init %#RX32\n", pThisCC->paContexts[i].idCtx,
                                pThisCC->paContexts[i].szName, pThisCC->paContexts[i].fContextInit);
    }
    else
        pHlp->pfnPrintf(pHlp, "Venus: %s\n", pThisCC->pszVenusState ? pThisCC->pszVenusState : "off");
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
       control worker free the resources and the Venus state, then the queues are
       disabled.  No need to wait for the workers: a command in flight finishes
       against the old state and its completion is dropped because the driver is
       no longer ready; parked fenced commands are dropped without a response. */
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

    /* Stop the workers first: after that only this thread and the renderer's
       fence callbacks touch the device state. */
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
    }

    /* Venus: take the blobs out of the window and destroy the contexts (no fence
       callback after that), then shut the library down.  It is not cleaned up
       if the guest may still see library memory (the process exits anyway). */
    vgpuR3VenusTeardown(pDevIns, pThis, pThisCC);
    if (pThisCC->pRenderer)
    {
        vgpuR3RendererDestroy(pThisCC->pRenderer, !pThisCC->fWindowLeaked /*fCleanup*/);
        pThisCC->pRenderer = NULL;
    }

    /* Now the semaphores the fence callback and the wake-up callback signal can go. */
    for (unsigned i = 0; i < RT_ELEMENTS(pThisCC->aWorkers); i++)
    {
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

    if (RTCritSectIsInitialized(&pThisCC->CritSectFence))
        RTCritSectDelete(&pThisCC->CritSectFence);
    RTMemFree(pThisCC->paContexts);
    pThisCC->paContexts = NULL;

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
    RTListInit(&pThisCC->ListPending);
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aWorkers); i++)
        pThis->aWorkers[i].hEvtProcess = NIL_SUPSEMEVENT;
    pThis->hRefreshTimer     = NIL_TMTIMERHANDLE;
    pThis->hMmio2HostVisible = NIL_PGMMMIO2HANDLE;
    pThisCC->pszVenusState   = "disabled (\"Venus\" is false)";
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
     * Validate and read the configuration.  MmioBase and Irq are read by VirtioCore
     * (MmioBase also here, Venus needs the PCI transport).
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "MonitorCount|MmioBase|Irq|Venus|RendererPath|HostVisibleSize", "");

    uint32_t cMonitors = 1;
    int rc = pHlp->pfnCFGMQueryU32Def(pCfg, "MonitorCount", &cMonitors, 1);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"MonitorCount\""));
    if (cMonitors < 1 || cMonitors > VIRTIOGPU_MAX_MONITORS)
        return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                   N_("virtio-gpu: \"MonitorCount\" is %RU32, but only %d is supported"),
                                   cMonitors, VIRTIOGPU_MAX_MONITORS);

    bool fVenus = false;
    rc = pHlp->pfnCFGMQueryBoolDef(pCfg, "Venus", &fVenus, false);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"Venus\""));

    char szRendererPath[RTPATH_MAX];
    rc = pHlp->pfnCFGMQueryStringDef(pCfg, "RendererPath", szRendererPath, sizeof(szRendererPath), "");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"RendererPath\""));
    /* A relative name would be looked up along the DLL search path; insist on knowing what is loaded. */
    if (szRendererPath[0] && !RTPathStartsWithRoot(szRendererPath))
        return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                   N_("virtio-gpu: \"RendererPath\" must be an absolute path (%s)"), szRendererPath);

    uint64_t cbHostVisible = VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT;
    rc = pHlp->pfnCFGMQueryU64Def(pCfg, "HostVisibleSize", &cbHostVisible, VIRTIOGPU_HOST_VISIBLE_SIZE_DEFAULT);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"HostVisibleSize\""));
    if (   fVenus
        && (   !RT_IS_POWER_OF_TWO(cbHostVisible)
            || cbHostVisible < VIRTIOGPU_HOST_VISIBLE_SIZE_MIN
            || cbHostVisible > VIRTIOGPU_HOST_VISIBLE_SIZE_MAX))
        return PDMDevHlpVMSetError(pDevIns, VERR_INVALID_PARAMETER, RT_SRC_POS,
                                   N_("virtio-gpu: \"HostVisibleSize\" is %#RX64, it must be a power of two from 64 MB to 16 GB"),
                                   cbHostVisible);

    /* VirtioCore reads this too; Venus needs the PCI transport for its BAR. */
    RTGCPHYS GCPhysMmioBase = NIL_RTGCPHYS;
    rc = pHlp->pfnCFGMQueryU64Def(pCfg, "MmioBase", &GCPhysMmioBase, NIL_RTGCPHYS);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to read \"MmioBase\""));

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
     * Venus: load the renderer.  Any failure falls back to 2D.  The features and
     * the shared memory region must be known before the virtio core is set up.
     */
    uint64_t fFeatures = VIRTIOGPU_FEATURES_OFFERED_2D;
    if (fVenus)
    {
        if (GCPhysMmioBase != NIL_RTGCPHYS)
        {
            LogRel(("%s: Venus needs the PCI transport, staying 2D\n", pThis->szInstance));
            pThisCC->pszVenusState = "not available with the virtio-mmio transport";
        }
        else
        {
            char szPath[RTPATH_MAX];
            if (szRendererPath[0])
                rc = RTStrCopy(szPath, sizeof(szPath), szRendererPath);
            else
            {
                rc = RTPathAppPrivateArch(szPath, sizeof(szPath));
                if (RT_SUCCESS(rc))
                    rc = RTPathAppend(szPath, sizeof(szPath), VIRTIOGPU_RENDERER_DEFAULT_NAME);
            }
            AssertRCReturn(rc, rc);

            rc = RTCritSectInit(&pThisCC->CritSectFence);
            AssertRCReturn(rc, rc);
            pThisCC->paContexts = (PVIRTIOGPUCONTEXT)RTMemAllocZ(sizeof(VIRTIOGPUCONTEXT) * VIRTIOGPU_MAX_CONTEXTS);
            AssertReturn(pThisCC->paContexts, VERR_NO_MEMORY);

            RTERRINFOSTATIC ErrInfo;
            rc = vgpuR3RendererCreate(szPath, vgpuR3FenceCallback, pDevIns, &pThisCC->pRenderer, RTErrInfoInitStatic(&ErrInfo));
            if (RT_SUCCESS(rc))
            {
                pThis->cbHostVisible   = cbHostVisible;
                pThis->Config.cCapsets = 1;
                fFeatures              = VIRTIOGPU_FEATURES_OFFERED_VENUS;

                /* Announced to the driver as a virtio shared memory region (PCI capability). */
                pThisCC->Virtio.cShmRegions          = 1;
                pThisCC->Virtio.aShmRegions[0].idShm = VIRTIOGPU_SHM_ID_HOST_VISIBLE;
                pThisCC->Virtio.aShmRegions[0].iBar  = VIRTIOGPU_HOST_VISIBLE_BAR;
                pThisCC->Virtio.aShmRegions[0].off   = 0;
                pThisCC->Virtio.aShmRegions[0].cb    = cbHostVisible;
            }
            else
            {
                LogRel(("%s: Venus is not available, staying 2D: renderer '%s': %Rrc%s%s\n", pThis->szInstance, szPath, rc,
                        RTErrInfoIsSet(&ErrInfo.Core) ? " - " : "", RTErrInfoIsSet(&ErrInfo.Core) ? ErrInfo.Core.pszMsg : ""));
                pThisCC->pszVenusState = "not available, the renderer failed to load or initialize (see the release log)";
                pThisCC->pRenderer     = NULL;
            }
        }
    }

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
                          fFeatures, 0 /*fOfferLegacy*/, &pThis->Config, sizeof(pThis->Config));
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("virtio-gpu: Failed to initialize the virtio core"));

    /*
     * The host visible window: a 64-bit prefetchable BAR without backing memory
     * (the PCI device exists only now).  This needs a NEM backend that can map
     * host memory ranges; without one, withdraw the 3D features again.  The
     * guest has not seen anything yet, and the shared memory capability then
     * names an unimplemented BAR, which drivers ignore.
     */
    if (pThisCC->pRenderer)
    {
        void *pvDummy = NULL;
        rc = PDMDevHlpPCIIORegionCreateMmio2Ex(pDevIns, pDevIns->apPciDevs[0], VIRTIOGPU_HOST_VISIBLE_BAR, cbHostVisible,
                                               (PCIADDRESSSPACE)(PCI_ADDRESS_SPACE_MEM_PREFETCH | PCI_ADDRESS_SPACE_BAR64),
                                               PGMPHYS_MMIO2_FLAGS_HOST_WINDOW, NULL /*pfnMapUnmap*/, "VirtioGpu-HostVisible",
                                               &pvDummy, &pThis->hMmio2HostVisible);
        if (RT_FAILURE(rc))
        {
            LogRel(("%s: Creating the %#RX64 byte host visible window failed (%Rrc; it needs NEM without the support"
                    " driver), staying 2D\n", pThis->szInstance, cbHostVisible, rc));
            pThis->Virtio.uDeviceFeatures &= ~VIRTIOGPU_FEATURES_OFFERED_VENUS;
            pThis->Config.cCapsets         = 0;
            pThis->cbHostVisible           = 0;
            pThis->hMmio2HostVisible       = NIL_PGMMMIO2HANDLE;
            vgpuR3RendererDestroy(pThisCC->pRenderer, true /*fCleanup*/);
            pThisCC->pRenderer     = NULL;
            pThisCC->pszVenusState = "not available, the host visible window could not be created (see the release log)";
        }
    }

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

    PDMDevHlpDBGFInfoRegister(pDevIns, "virtiogpu", "virtio-gpu state: resources, scanout, Venus contexts and queues.", vgpuR3Info);

    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCtrlCmds,      STAMTYPE_COUNTER, "CtrlCmds",      STAMUNIT_OCCURENCES,
                          "Control queue commands processed.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCursorCmds,    STAMTYPE_COUNTER, "CursorCmds",    STAMUNIT_OCCURENCES,
                          "Cursor queue commands processed (and ignored).");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCmdErrors,     STAMTYPE_COUNTER, "CmdErrors",     STAMUNIT_OCCURENCES,
                          "Control queue commands answered with an error.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatTransferBytes, STAMTYPE_COUNTER, "TransferBytes", STAMUNIT_BYTES,
                          "Bytes copied from guest backing by TRANSFER_TO_HOST_2D.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatCtxCreates,     STAMTYPE_COUNTER, "Venus/CtxCreates",     STAMUNIT_OCCURENCES,
                          "Venus contexts created.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatSubmits,        STAMTYPE_COUNTER, "Venus/Submits",        STAMUNIT_OCCURENCES,
                          "SUBMIT_3D commands passed to the renderer.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatSubmitBytes,    STAMTYPE_COUNTER, "Venus/SubmitBytes",    STAMUNIT_BYTES,
                          "Command stream bytes passed to the renderer by SUBMIT_3D.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatBlobMaps,       STAMTYPE_COUNTER, "Venus/BlobMaps",       STAMUNIT_OCCURENCES,
                          "Blobs mapped into the host visible window.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatFencesDeferred, STAMTYPE_COUNTER, "Venus/FencesDeferred", STAMUNIT_OCCURENCES,
                          "Fenced commands parked until their renderer fence retired.");
    PDMDevHlpSTAMRegister(pDevIns, &pThis->StatFencesSignaled, STAMTYPE_COUNTER, "Venus/FencesSignaled", STAMUNIT_OCCURENCES,
                          "Fence retirements reported by the renderer.");

    if (pThisCC->pRenderer)
        LogRel(("%s: Virtio GPU with %RU32 scanout(s) and Venus 3D, host visible window %RU64 MB at BAR%u\n",
                pThis->szInstance, cMonitors, cbHostVisible / _1M, VIRTIOGPU_HOST_VISIBLE_BAR));
    else
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
    /* .pszDescription = */         "Virtio GPU (2D, optional Venus 3D).\n",
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

