/* $Id$ */
/** @file
 * VBox graphics devices - Virtio GPU, host 3D renderer (virglrenderer) wrapper.
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

/*
 * Design notes (see also the @page in DevVirtioGpu.cpp):
 *
 *  - The library (a virglrenderer build, MIT licensed, built with MinGW on
 *    Windows) is loaded with RTLdrLoadEx and its C entry points are resolved
 *    with RTLdrGetSymbol.  Nothing is linked and no library header is used:
 *    the few constants and structure layouts needed are reproduced below.
 *    Only plain C values cross the boundary.  No file descriptors, CRT
 *    objects or heap blocks are passed between the two runtimes.
 *
 *  - virglrenderer keeps global state without locking, so every
 *    virgl_renderer_* call is made from one dedicated thread created here
 *    ("VGpuRender").  The public functions post a request to that thread and
 *    wait for it.  Callers are serialized by CritSectCall, so at most one
 *    request is outstanding.
 *
 *  - Fences retire on threads owned by the library (one per context, created
 *    and joined by the library on context create/destroy).  They are passed
 *    straight to the device's fence callback, which only queues work.
 *
 *  - The library is never unloaded (RTLDRLOAD_FLAGS_NO_UNLOAD): on Windows its
 *    render server is an in-process thread that is detached and may still run
 *    briefly after virgl_renderer_cleanup().  For the same reason no log
 *    callback is installed: it would point into VBoxDD, which is unloaded
 *    when the VM is destroyed.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_VIRTIO
#include <VBox/log.h>
#include <VBox/err.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/env.h>
#include <iprt/errcore.h>
#include <iprt/ldr.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/time.h>

#include "DevVirtioGpu.h"
#include "DevVirtioGpuRenderer.h"


/*********************************************************************************************************************************
*   virglrenderer ABI                                                                                                            *
*********************************************************************************************************************************/
/*
 * Mirrors of the virglrenderer C interface, copied from src/virglrenderer.h,
 * src/venus_hw.h and src/drm/drm-uapi/virtgpu_drm.h of the Windows fork
 * (the line numbers refer to that tree).
 *
 * @todo verify against src/virglrenderer.h of the built fork whenever the
 *       library is rebuilt: these were copied from the source tree, the
 *       binary is not checked.  vgpuR3RendererExecInit() sanity checks the
 *       Venus capset at run time and falls back to 2D if it looks wrong.  That
 *       check cannot tell whether the render server or the host Vulkan driver
 *       work: the capset is built from tables compiled into the library, the
 *       server only initializes Vulkan when the first context is created.
 */

/** @name virgl_renderer_init() flags (virglrenderer.h:110-155).
 * @{ */
/** VIRGL_RENDERER_THREAD_SYNC (2): fences are signalled by a thread instead of polling. */
#define VGPU_VIRGL_RENDERER_THREAD_SYNC         UINT32_C(0x0002)
/** VIRGL_RENDERER_VENUS (1 << 6). */
#define VGPU_VIRGL_RENDERER_VENUS               RT_BIT_32(6)
/** VIRGL_RENDERER_NO_VIRGL (1 << 7): no OpenGL (virgl) renderer. */
#define VGPU_VIRGL_RENDERER_NO_VIRGL            RT_BIT_32(7)
/** VIRGL_RENDERER_ASYNC_FENCE_CB (1 << 8): write_context_fence is called from the sync thread. */
#define VGPU_VIRGL_RENDERER_ASYNC_FENCE_CB      RT_BIT_32(8)
/** VIRGL_RENDERER_RENDER_SERVER (1 << 9): Venus runs in the render server (an in-process thread on Windows). */
#define VGPU_VIRGL_RENDERER_RENDER_SERVER       RT_BIT_32(9)
/** @} */

/** The virgl_renderer_callbacks version implemented here.  Version 3 adds
 * write_context_fence and get_server_fd (virglrenderer.h:51, 80-100); the
 * library accepts 1..VIRGL_RENDERER_CALLBACKS_VERSION (4). */
#define VGPU_VIRGL_CALLBACKS_VERSION            3

/** VIRGL_RENDERER_FENCE_FLAG_MERGEABLE (virglrenderer.h:454). */
#define VGPU_VIRGL_FENCE_FLAG_MERGEABLE         RT_BIT_32(0)
/** VIRGL_RENDERER_MAP_CACHE_MASK (virglrenderer.h:427), same values as VIRTIOGPU_MAP_CACHE_XXX. */
#define VGPU_VIRGL_MAP_CACHE_MASK               UINT32_C(0x0f)
/** VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK (virglrenderer.h:392). */
#define VGPU_VIRGL_CONTEXT_FLAG_CAPSET_ID_MASK  UINT32_C(0xff)
/** VIRTGPU_DRM_CAPSET_VENUS (virtgpu_drm.h:169). */
#define VGPU_VIRGL_CAPSET_VENUS                 4

/** @name errno values the library returns (positive or negated).
 * The values are the same in the MinGW and the Microsoft C runtimes.
 * @{ */
#define VGPU_VIRGL_ENOMEM                       12
#define VGPU_VIRGL_EINVAL                       22
/** @} */

AssertCompile(VGPU_VIRGL_CAPSET_VENUS == VIRTIOGPU_CAPSET_VENUS);
AssertCompile(VGPU_VIRGL_CONTEXT_FLAG_CAPSET_ID_MASK == VIRTIOGPU_CONTEXT_INIT_CAPSET_ID_MASK);
AssertCompile(VGPU_VIRGL_MAP_CACHE_MASK == VIRTIOGPU_MAP_CACHE_MASK);
/* The library takes the virtio blob memory types and flags unchanged (virglrenderer.h:399-406). */
AssertCompile(VIRTIOGPU_BLOB_MEM_HOST3D == 0x0002);
AssertCompile(VIRTIOGPU_BLOB_FLAG_VALID_MASK == 0x0007);

/**
 * The start of struct virgl_renderer_capset_venus (venus_hw.h:29-72), enough
 * for the sanity check.  The full structure is 160 bytes in the fork.
 */
typedef struct VGPUVENUSCAPSETHDR
{
    /** wire_format_version */
    uint32_t    uWireFormatVersion;
    /** vk_xml_version */
    uint32_t    uVkXmlVersion;
    /** vk_ext_command_serialization_spec_version */
    uint32_t    uVkExtCmdSerializationSpecVersion;
    /** vk_mesa_venus_protocol_spec_version */
    uint32_t    uVkMesaVenusProtocolSpecVersion;
    /** supports_blob_id_0 */
    uint32_t    fSupportsBlobId0;
} VGPUVENUSCAPSETHDR;
AssertCompileSize(VGPUVENUSCAPSETHDR, 20);

/**
 * struct virgl_renderer_callbacks (virglrenderer.h:53-107).
 *
 * The library keeps a pointer to this structure (and to the cookie) until
 * virgl_renderer_cleanup(), so it lives in VGPURENDERER.
 */
typedef struct VGPUVIRGLCALLBACKS
{
    /** version */
    int         iVersion;
    /** write_fence: context 0 (virgl) fences, unused with VIRGL_RENDERER_NO_VIRGL. */
    DECLCALLBACKMEMBER(void,   pfnWriteFence,(void *pvCookie, uint32_t uFence));
    /** create_gl_context: unused (NULL). */
    DECLCALLBACKMEMBER(void *, pfnCreateGlContext,(void *pvCookie, int iScanout, void *pvParam));
    /** destroy_gl_context: unused (NULL). */
    DECLCALLBACKMEMBER(void,   pfnDestroyGlContext,(void *pvCookie, void *pvGlCtx));
    /** make_current: unused (NULL). */
    DECLCALLBACKMEMBER(int,    pfnMakeCurrent,(void *pvCookie, int iScanout, void *pvGlCtx));
    /** get_drm_fd (v2): unused (NULL). */
    DECLCALLBACKMEMBER(int,    pfnGetDrmFd,(void *pvCookie));
    /** write_context_fence (v3). */
    DECLCALLBACKMEMBER(void,   pfnWriteContextFence,(void *pvCookie, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId));
    /** get_server_fd (v3): must stay NULL, so the library starts its own render server. */
    DECLCALLBACKMEMBER(int,    pfnGetServerFd,(void *pvCookie, uint32_t uVersion));
    /** get_egl_display (v4): not read with version 3, NULL. */
    DECLCALLBACKMEMBER(void *, pfnGetEglDisplay,(void *pvCookie));
} VGPUVIRGLCALLBACKS;
#if ARCH_BITS == 64
AssertCompileMemberOffset(VGPUVIRGLCALLBACKS, pfnWriteFence, 8);
AssertCompileMemberOffset(VGPUVIRGLCALLBACKS, pfnWriteContextFence, 48);
AssertCompileMemberOffset(VGPUVIRGLCALLBACKS, pfnGetServerFd, 56);
AssertCompileSize(VGPUVIRGLCALLBACKS, 72);
#endif

/**
 * struct virgl_renderer_resource_create_blob_args (virglrenderer.h:408-418).
 */
typedef struct VGPUVIRGLBLOBARGS
{
    /** res_handle */
    uint32_t        idResource;
    /** ctx_id */
    uint32_t        idCtx;
    /** blob_mem */
    uint32_t        uBlobMem;
    /** blob_flags */
    uint32_t        fBlobFlags;
    /** blob_id */
    uint64_t        idBlob;
    /** size */
    uint64_t        cbBlob;
    /** iovecs (const struct iovec *): always NULL here, only host blobs are created. */
    const void     *paIoVecs;
    /** num_iovs */
    uint32_t        cIoVecs;
} VGPUVIRGLBLOBARGS;
AssertCompileMemberOffset(VGPUVIRGLBLOBARGS, idBlob, 16);
AssertCompileMemberOffset(VGPUVIRGLBLOBARGS, cbBlob, 24);
AssertCompileMemberOffset(VGPUVIRGLBLOBARGS, paIoVecs, 32);
#if ARCH_BITS == 64
AssertCompileSize(VGPUVIRGLBLOBARGS, 48);
#endif

/**
 * The library entry points used (signatures from virglrenderer.h, see the
 * line numbers).  'int' is kept where the library uses it.
 */
typedef struct VGPUVIRGLFNS
{
    /** virgl_renderer_init (:172) */
    DECLCALLBACKMEMBER(int,  pfnInit,(void *pvCookie, int fFlags, VGPUVIRGLCALLBACKS *pCallbacks));
    /** virgl_renderer_cleanup (:383) */
    DECLCALLBACKMEMBER(void, pfnCleanup,(void *pvCookie));
    /** virgl_renderer_get_cap_set (:325) */
    DECLCALLBACKMEMBER(void, pfnGetCapSet,(uint32_t idSet, uint32_t *puMaxVersion, uint32_t *pcbMaxSize));
    /** virgl_renderer_fill_caps (:328) */
    DECLCALLBACKMEMBER(void, pfnFillCaps,(uint32_t idSet, uint32_t uVersion, void *pvCaps));
    /** virgl_renderer_context_create_with_flags (:394) */
    DECLCALLBACKMEMBER(int,  pfnContextCreateWithFlags,(uint32_t idCtx, uint32_t fCtxFlags, uint32_t cchName,
                                                        const char *pachName));
    /** virgl_renderer_context_destroy (:286) */
    DECLCALLBACKMEMBER(void, pfnContextDestroy,(uint32_t idCtx));
    /** virgl_renderer_submit_cmd (:304), the size is in dwords. */
    DECLCALLBACKMEMBER(int,  pfnSubmitCmd,(void *pvBuffer, int idCtx, int cDWords));
    /** virgl_renderer_context_create_fence (:455) */
    DECLCALLBACKMEMBER(int,  pfnContextCreateFence,(uint32_t idCtx, uint32_t fFlags, uint32_t uRingIdx, uint64_t uFenceId));
    /** virgl_renderer_resource_create_blob (:420) */
    DECLCALLBACKMEMBER(int,  pfnResourceCreateBlob,(const VGPUVIRGLBLOBARGS *pArgs));
    /** virgl_renderer_resource_unref (:280) */
    DECLCALLBACKMEMBER(void, pfnResourceUnref,(uint32_t idResource));
    /** virgl_renderer_ctx_attach_resource (:339) */
    DECLCALLBACKMEMBER(void, pfnCtxAttachResource,(int idCtx, int idResource));
    /** virgl_renderer_ctx_detach_resource (:340) */
    DECLCALLBACKMEMBER(void, pfnCtxDetachResource,(int idCtx, int idResource));
    /** virgl_renderer_resource_map (:423) */
    DECLCALLBACKMEMBER(int,  pfnResourceMap,(uint32_t idResource, void **ppvMap, uint64_t *pcbMap));
    /** virgl_renderer_resource_unmap (:425) */
    DECLCALLBACKMEMBER(int,  pfnResourceUnmap,(uint32_t idResource));
    /** virgl_renderer_resource_get_map_info (:433) */
    DECLCALLBACKMEMBER(int,  pfnResourceGetMapInfo,(uint32_t idResource, uint32_t *pfMapInfo));
} VGPUVIRGLFNS;

/** Symbol name to VGPUVIRGLFNS member mapping. */
static const struct
{
    const char *pszName;
    uint32_t    offMember;
} g_aVirglSymbols[] =
{
    { "virgl_renderer_init",                        RT_UOFFSETOF(VGPUVIRGLFNS, pfnInit) },
    { "virgl_renderer_cleanup",                     RT_UOFFSETOF(VGPUVIRGLFNS, pfnCleanup) },
    { "virgl_renderer_get_cap_set",                 RT_UOFFSETOF(VGPUVIRGLFNS, pfnGetCapSet) },
    { "virgl_renderer_fill_caps",                   RT_UOFFSETOF(VGPUVIRGLFNS, pfnFillCaps) },
    { "virgl_renderer_context_create_with_flags",   RT_UOFFSETOF(VGPUVIRGLFNS, pfnContextCreateWithFlags) },
    { "virgl_renderer_context_destroy",             RT_UOFFSETOF(VGPUVIRGLFNS, pfnContextDestroy) },
    { "virgl_renderer_submit_cmd",                  RT_UOFFSETOF(VGPUVIRGLFNS, pfnSubmitCmd) },
    { "virgl_renderer_context_create_fence",        RT_UOFFSETOF(VGPUVIRGLFNS, pfnContextCreateFence) },
    { "virgl_renderer_resource_create_blob",        RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceCreateBlob) },
    { "virgl_renderer_resource_unref",              RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceUnref) },
    { "virgl_renderer_ctx_attach_resource",         RT_UOFFSETOF(VGPUVIRGLFNS, pfnCtxAttachResource) },
    { "virgl_renderer_ctx_detach_resource",         RT_UOFFSETOF(VGPUVIRGLFNS, pfnCtxDetachResource) },
    { "virgl_renderer_resource_map",                RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceMap) },
    { "virgl_renderer_resource_unmap",              RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceUnmap) },
    { "virgl_renderer_resource_get_map_info",       RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceGetMapInfo) },
};


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** VGPURENDERER::u32Magic (Richard Feynman). */
#define VGPURENDERER_MAGIC                      UINT32_C(0x19180511)
/** The largest capset accepted from the library. */
#define VGPURENDERER_MAX_CAPSET_SIZE            _4K
/** The highest capset version accepted from the library (Venus has 0). */
#define VGPURENDERER_MAX_CAPSET_VERSION         16
/** Stack size of the renderer thread.  Mapping the first Vulkan backed blob
 * initializes a Vulkan instance and device on this thread. */
#define VGPURENDERER_THREAD_STACK_SIZE          (4 * _1M)
/** How long the destructor waits for the renderer thread. */
#define VGPURENDERER_TERM_TIMEOUT_MS            RT_MS_30SEC
/** How long a caller waits for the call lock.  Only one thread calls into the
 * renderer at a time, so waiting longer than this means a call into the library
 * is stuck: the renderer is then declared dead and the library is leaked. */
#define VGPURENDERER_CALL_LOCK_TIMEOUT_MS       RT_MS_30SEC


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/** Renderer thread request types. */
typedef enum VGPURENDERREQTYPE
{
    kVGpuRenderReq_Invalid = 0,
    kVGpuRenderReq_Init,
    kVGpuRenderReq_Cleanup,
    kVGpuRenderReq_ContextCreate,
    kVGpuRenderReq_ContextDestroy,
    kVGpuRenderReq_Submit,
    kVGpuRenderReq_ContextFence,
    kVGpuRenderReq_BlobCreate,
    kVGpuRenderReq_ResourceUnref,
    kVGpuRenderReq_ContextAttach,
    kVGpuRenderReq_ContextDetach,
    kVGpuRenderReq_ResourceMap,
    kVGpuRenderReq_ResourceUnmap,
    kVGpuRenderReq_End
} VGPURENDERREQTYPE;

/**
 * A request to the renderer thread, lives on the caller's stack.
 */
typedef struct VGPURENDERREQ
{
    /** The request type. */
    VGPURENDERREQTYPE   enmType;
    /** The VBox status code of the call. */
    int                 rc;
    /** The raw library return value, for logging. */
    int                 iVirglRc;
    /** Arguments and results. */
    union
    {
        struct
        {
            uint32_t        idCtx;
            uint32_t        fCtxFlags;
            uint32_t        cchName;
            const char     *pachName;
        } ContextCreate;
        struct
        {
            uint32_t        idCtx;
            uint32_t        cDWords;
            void           *pvCmds;
        } Submit;
        struct
        {
            uint32_t        idCtx;
            uint32_t        uRingIdx;
            uint64_t        uFenceId;
        } Fence;
        VGPUVIRGLBLOBARGS   BlobCreate;
        struct
        {
            uint32_t        idCtx;
            uint32_t        idResource;
        } Ids;
        struct
        {
            uint32_t        idResource;
            uint32_t        fMapInfo;
            void           *pvHost;
            uint64_t        cbHost;
        } Map;
    } u;
} VGPURENDERREQ;
/** Pointer to a renderer thread request. */
typedef VGPURENDERREQ *PVGPURENDERREQ;

/**
 * The renderer instance.
 */
typedef struct VGPURENDERER
{
    /** VGPURENDERER_MAGIC. */
    uint32_t                    u32Magic;
    /** Set by the destructor to stop the renderer thread. */
    bool volatile               fTerminate;
    /** Set by the destructor before it tears anything down: the fence callback
     * stops calling the device, which may be gone while library threads of
     * contexts we could not destroy still run. */
    bool volatile               fNoFence;
    /** Set when a call into the library is stuck: no further calls are made. */
    bool volatile               fDead;
    /** Set while virgl_renderer_init() succeeded and no cleanup happened
     * (renderer thread only, read by others after joining it). */
    bool                        fInitialized;
    /** The library handle. */
    RTLDRMOD                    hLdrMod;
    /** The library entry points. */
    VGPUVIRGLFNS                Fns;
    /** The callbacks given to the library, referenced by it while initialized. */
    VGPUVIRGLCALLBACKS          Callbacks;
    /** The device's fence callback. */
    PFNVGPURENDERERFENCE        pfnFence;
    /** User argument for pfnFence. */
    void                       *pvFenceUser;
    /** The renderer thread. */
    RTTHREAD                    hThread;
    /** Signalled to make the renderer thread look at pReqPending / fTerminate. */
    RTSEMEVENT                  hEvtReq;
    /** Signalled by the renderer thread when a request is done. */
    RTSEMEVENT                  hEvtDone;
    /** Serializes the callers. */
    RTCRITSECT                  CritSectCall;
    /** The request for the renderer thread, NULL if none. */
    PVGPURENDERREQ volatile     pReqPending;
    /** @name The Venus capset, fetched once at init and read only afterwards.
     * @{ */
    uint32_t                    uCapsetMaxVersion;
    uint32_t                    cbCapset;
    uint8_t                    *pbCapset;
    /** @} */
} VGPURENDERER;


/*********************************************************************************************************************************
*   Library callbacks                                                                                                            *
*********************************************************************************************************************************/

/**
 * write_fence callback: context 0 (OpenGL virgl) fences, which are never
 * created because the virgl renderer is disabled.
 */
static DECLCALLBACK(void) vgpuR3VirglWriteFence(void *pvCookie, uint32_t uFence)
{
    RT_NOREF(pvCookie, uFence);
    Log(("VirtioGpu: Unexpected context 0 fence %#RX32\n", uFence));
}


/**
 * write_context_fence callback, see FNVGPURENDERERFENCE for the rules.
 */
static DECLCALLBACK(void) vgpuR3VirglWriteContextFence(void *pvCookie, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId)
{
    PVGPURENDERER pThis = (PVGPURENDERER)pvCookie;
    AssertReturnVoid(RT_VALID_PTR(pThis) && pThis->u32Magic == VGPURENDERER_MAGIC);
    if (ASMAtomicReadBool(&pThis->fNoFence))
        return;
    pThis->pfnFence(pThis->pvFenceUser, idCtx, uRingIdx, uFenceId);
}


/*********************************************************************************************************************************
*   Renderer thread                                                                                                              *
*********************************************************************************************************************************/

/**
 * Converts a library return value to a VBox status code.
 *
 * @returns VBox status code.
 * @param   iVirglRc    0 on success, otherwise a positive or negated errno
 *                      value or -1.
 */
static int vgpuR3VirglRcToVBox(int iVirglRc)
{
    if (iVirglRc == 0)
        return VINF_SUCCESS;
    if (iVirglRc == VGPU_VIRGL_EINVAL || iVirglRc == -VGPU_VIRGL_EINVAL)
        return VERR_INVALID_PARAMETER;
    if (iVirglRc == VGPU_VIRGL_ENOMEM || iVirglRc == -VGPU_VIRGL_ENOMEM)
        return VERR_NO_MEMORY;
    return VERR_GENERAL_FAILURE;
}


/**
 * Initializes the library and fetches the Venus capset.  Renderer thread.
 *
 * @returns VBox status code.  On failure after a successful
 *          virgl_renderer_init(), fInitialized stays set and the caller
 *          has to clean up.
 * @param   pThis       The renderer instance.
 */
static int vgpuR3RendererExecInit(PVGPURENDERER pThis)
{
    uint32_t const fFlags = VGPU_VIRGL_RENDERER_VENUS
                          | VGPU_VIRGL_RENDERER_RENDER_SERVER
                          | VGPU_VIRGL_RENDERER_THREAD_SYNC
                          | VGPU_VIRGL_RENDERER_ASYNC_FENCE_CB
                          | VGPU_VIRGL_RENDERER_NO_VIRGL;
    int iRc = pThis->Fns.pfnInit(pThis, (int)fFlags, &pThis->Callbacks);
    if (iRc != 0)
    {
        LogRel(("VirtioGpu: virgl_renderer_init(%#RX32) failed: %d\n", fFlags, iRc));
        return VERR_NOT_SUPPORTED;
    }
    pThis->fInitialized = true;

    /* Queried after init (the library wants proxy_initialized to be set for this). */
    uint32_t uMaxVersion = UINT32_MAX;
    uint32_t cbCapset    = UINT32_MAX;
    pThis->Fns.pfnGetCapSet(VGPU_VIRGL_CAPSET_VENUS, &uMaxVersion, &cbCapset);
    if (   cbCapset < sizeof(VGPUVENUSCAPSETHDR)
        || cbCapset > VGPURENDERER_MAX_CAPSET_SIZE
        || uMaxVersion > VGPURENDERER_MAX_CAPSET_VERSION)
    {
        LogRel(("VirtioGpu: Implausible Venus capset from the renderer: version %RU32, %RU32 bytes\n", uMaxVersion, cbCapset));
        return VERR_NOT_SUPPORTED;
    }

    /* The buffer is the maximum size, not cbCapset: the library writes its own
       structure and its idea of the size is all we have to go on. */
    uint8_t *pbCapset = (uint8_t *)RTMemAllocZ(VGPURENDERER_MAX_CAPSET_SIZE);
    if (!pbCapset)
        return VERR_NO_MEMORY;
    pThis->Fns.pfnFillCaps(VGPU_VIRGL_CAPSET_VENUS, uMaxVersion, pbCapset);

    VGPUVENUSCAPSETHDR const *pHdr = (VGPUVENUSCAPSETHDR const *)pbCapset;
    if (!pHdr->uWireFormatVersion || !pHdr->uVkXmlVersion)
    {
        LogRel(("VirtioGpu: The Venus capset is empty (wire format %RU32, vk.xml %#RX32); is this the right library?\n",
                pHdr->uWireFormatVersion, pHdr->uVkXmlVersion));
        RTMemFree(pbCapset);
        return VERR_NOT_SUPPORTED;
    }
    LogRel(("VirtioGpu: Venus capset v%RU32, %RU32 bytes: wire format %RU32, vk.xml %#RX32, serialization spec %RU32,"
            " protocol spec %RU32, blob id 0 %RU32\n", uMaxVersion, cbCapset, pHdr->uWireFormatVersion, pHdr->uVkXmlVersion,
            pHdr->uVkExtCmdSerializationSpecVersion, pHdr->uVkMesaVenusProtocolSpecVersion, pHdr->fSupportsBlobId0));

    pThis->uCapsetMaxVersion = uMaxVersion;
    pThis->cbCapset          = cbCapset;
    pThis->pbCapset          = pbCapset;
    return VINF_SUCCESS;
}


/**
 * Executes one request.  Renderer thread.
 *
 * @param   pThis       The renderer instance.
 * @param   pReq        The request.
 */
static void vgpuR3RendererExec(PVGPURENDERER pThis, PVGPURENDERREQ pReq)
{
    if (pReq->enmType == kVGpuRenderReq_Init)
    {
        pReq->rc = vgpuR3RendererExecInit(pThis);
        return;
    }
    if (!pThis->fInitialized)
    {
        pReq->rc = VERR_INVALID_STATE;
        return;
    }

    int iRc = 0;
    switch (pReq->enmType)
    {
        case kVGpuRenderReq_Cleanup:
            pThis->Fns.pfnCleanup(pThis);
            pThis->fInitialized = false;
            break;

        case kVGpuRenderReq_ContextCreate:
            iRc = pThis->Fns.pfnContextCreateWithFlags(pReq->u.ContextCreate.idCtx, pReq->u.ContextCreate.fCtxFlags,
                                                       pReq->u.ContextCreate.cchName, pReq->u.ContextCreate.pachName);
            break;

        case kVGpuRenderReq_ContextDestroy:
            pThis->Fns.pfnContextDestroy(pReq->u.Ids.idCtx);
            break;

        case kVGpuRenderReq_Submit:
            iRc = pThis->Fns.pfnSubmitCmd(pReq->u.Submit.pvCmds, (int)pReq->u.Submit.idCtx, (int)pReq->u.Submit.cDWords);
            break;

        case kVGpuRenderReq_ContextFence:
            iRc = pThis->Fns.pfnContextCreateFence(pReq->u.Fence.idCtx, VGPU_VIRGL_FENCE_FLAG_MERGEABLE,
                                                   pReq->u.Fence.uRingIdx, pReq->u.Fence.uFenceId);
            break;

        case kVGpuRenderReq_BlobCreate:
            iRc = pThis->Fns.pfnResourceCreateBlob(&pReq->u.BlobCreate);
            break;

        case kVGpuRenderReq_ResourceUnref:
            pThis->Fns.pfnResourceUnref(pReq->u.Ids.idResource);
            break;

        case kVGpuRenderReq_ContextAttach:
            pThis->Fns.pfnCtxAttachResource((int)pReq->u.Ids.idCtx, (int)pReq->u.Ids.idResource);
            break;

        case kVGpuRenderReq_ContextDetach:
            pThis->Fns.pfnCtxDetachResource((int)pReq->u.Ids.idCtx, (int)pReq->u.Ids.idResource);
            break;

        case kVGpuRenderReq_ResourceMap:
        {
            void    *pvHost = NULL;
            uint64_t cbHost = 0;
            iRc = pThis->Fns.pfnResourceMap(pReq->u.Map.idResource, &pvHost, &cbHost);
            if (iRc == 0)
            {
                uint32_t fMapInfo = 0;
                iRc = pThis->Fns.pfnResourceGetMapInfo(pReq->u.Map.idResource, &fMapInfo);
                if (iRc == 0 && pvHost && cbHost)
                {
                    pReq->u.Map.pvHost   = pvHost;
                    pReq->u.Map.cbHost   = cbHost;
                    pReq->u.Map.fMapInfo = fMapInfo & VGPU_VIRGL_MAP_CACHE_MASK;
                }
                else
                {
                    LogRelMax(16, ("VirtioGpu: Mapping resource %RU32 gave %p LB %#RX64, map info rc %d\n",
                                   pReq->u.Map.idResource, pvHost, cbHost, iRc));
                    pThis->Fns.pfnResourceUnmap(pReq->u.Map.idResource);
                    if (iRc == 0)
                        iRc = -1;
                }
            }
            break;
        }

        case kVGpuRenderReq_ResourceUnmap:
            iRc = pThis->Fns.pfnResourceUnmap(pReq->u.Ids.idResource);
            break;

        default:
            AssertMsgFailed(("%d\n", pReq->enmType));
            pReq->rc = VERR_INTERNAL_ERROR_3;
            return;
    }
    pReq->iVirglRc = iRc;
    pReq->rc       = vgpuR3VirglRcToVBox(iRc);
}


/**
 * @callback_method_impl{FNRTTHREAD, The renderer thread: the only thread calling into the library.}
 */
static DECLCALLBACK(int) vgpuR3RendererThread(RTTHREAD hThreadSelf, void *pvUser)
{
    PVGPURENDERER pThis = (PVGPURENDERER)pvUser;
    RT_NOREF(hThreadSelf);

    for (;;)
    {
        PVGPURENDERREQ pReq = ASMAtomicXchgPtrT(&pThis->pReqPending, NULL, PVGPURENDERREQ);
        if (pReq)
        {
            vgpuR3RendererExec(pThis, pReq);
            int rc = RTSemEventSignal(pThis->hEvtDone);
            AssertLogRelRC(rc);
            continue;
        }
        if (ASMAtomicReadBool(&pThis->fTerminate))
            break;
        int rc = RTSemEventWait(pThis->hEvtReq, RT_INDEFINITE_WAIT);
        AssertLogRelMsgReturn(RT_SUCCESS(rc), ("%Rrc\n", rc), rc);
    }
    return VINF_SUCCESS;
}


/**
 * Executes a request on the renderer thread and waits for it.
 *
 * @returns The request's status code.
 * @param   pThis       The renderer instance.
 * @param   pReq        The request (rc and results are updated).
 */
static int vgpuR3RendererCall(PVGPURENDERER pThis, PVGPURENDERREQ pReq)
{
    AssertReturn(pThis->hThread != NIL_RTTHREAD, VERR_INVALID_STATE);
    AssertReturn(RTThreadSelf() != pThis->hThread, VERR_DEADLOCK); /* The renderer thread must not wait for itself. */
    pReq->rc       = VERR_INTERNAL_ERROR_2;
    pReq->iVirglRc = 0;
    if (ASMAtomicReadBool(&pThis->fDead))
        return VERR_INVALID_STATE;

    /* Only one thread calls into the renderer at a time, so not getting the lock
       in time means that a call into the library is stuck.  Give up on the library
       rather than hanging the caller (and with it power off) for ever.  Our own
       request is not posted yet, so nothing refers to the caller's stack. */
    uint64_t const msDeadline = RTTimeMilliTS() + VGPURENDERER_CALL_LOCK_TIMEOUT_MS;
    while (RTCritSectTryEnter(&pThis->CritSectCall) != VINF_SUCCESS)
    {
        if (RTTimeMilliTS() >= msDeadline || ASMAtomicReadBool(&pThis->fDead))
        {
            ASMAtomicWriteBool(&pThis->fDead, true);
            LogRel(("VirtioGpu: A call into the renderer library is stuck, giving up on it\n"));
            return VERR_TIMEOUT;
        }
        RTThreadSleep(10);
    }
    Assert(!pThis->pReqPending);
    ASMAtomicWritePtr(&pThis->pReqPending, pReq);
    int rc = RTSemEventSignal(pThis->hEvtReq);
    if (RT_FAILURE(rc) && ASMAtomicCmpXchgPtr(&pThis->pReqPending, (PVGPURENDERREQ)NULL, pReq))
    {
        /* The thread never saw the request. */
        AssertLogRelMsgFailed(("%Rrc\n", rc));
        RTCritSectLeave(&pThis->CritSectCall);
        return rc;
    }

    /* The request lives on our stack, so do not return before the thread is done with it. */
    for (;;)
    {
        rc = RTSemEventWait(pThis->hEvtDone, RT_INDEFINITE_WAIT);
        if (RT_SUCCESS(rc))
            break;
        AssertLogRelMsgFailed(("%Rrc\n", rc));
        RTThreadSleep(10);
    }
    RTCritSectLeave(&pThis->CritSectCall);
    return pReq->rc;
}


/*********************************************************************************************************************************
*   Interface                                                                                                                    *
*********************************************************************************************************************************/

/**
 * Loads and initializes the renderer library.
 *
 * @returns VBox status code.  On failure the device has to run 2D only; the
 *          reason is in the release log and in pErrInfo.
 * @param   pszLibrary      The library path.
 * @param   pfnFence        The fence callback.
 * @param   pvFenceUser     User argument for pfnFence.
 * @param   ppRenderer      Where to return the renderer instance.
 * @param   pErrInfo        Where to return extended error information, optional.
 */
DECLHIDDEN(int) vgpuR3RendererCreate(const char *pszLibrary, PFNVGPURENDERERFENCE pfnFence, void *pvFenceUser,
                                     PVGPURENDERER *ppRenderer, PRTERRINFO pErrInfo)
{
    AssertPtrReturn(ppRenderer, VERR_INVALID_POINTER);
    *ppRenderer = NULL;
    AssertPtrReturn(pszLibrary, VERR_INVALID_POINTER);
    AssertPtrReturn(pfnFence, VERR_INVALID_POINTER);

    /* The library silently drops THREAD_SYNC when this is set and init still succeeds,
       but without it nothing ever signals a fence (async fence callbacks are in use and
       there is no polling), so every fenced command would hang the guest GPU.  Refuse. */
    if (RTEnvExist("VIRGL_DISABLE_MT"))
    {
        LogRel(("VirtioGpu: VIRGL_DISABLE_MT is set in the environment, which breaks fence signalling; not using the renderer\n"));
        RTErrInfoSet(pErrInfo, VERR_NOT_SUPPORTED, "VIRGL_DISABLE_MT is set in the environment of the VM process");
        return VERR_NOT_SUPPORTED;
    }

    PVGPURENDERER pThis = (PVGPURENDERER)RTMemAllocZ(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;
    pThis->u32Magic                       = VGPURENDERER_MAGIC;
    pThis->hLdrMod                        = NIL_RTLDRMOD;
    pThis->hThread                        = NIL_RTTHREAD;
    pThis->hEvtReq                        = NIL_RTSEMEVENT;
    pThis->hEvtDone                       = NIL_RTSEMEVENT;
    pThis->pfnFence                       = pfnFence;
    pThis->pvFenceUser                    = pvFenceUser;
    pThis->Callbacks.iVersion             = VGPU_VIRGL_CALLBACKS_VERSION;
    pThis->Callbacks.pfnWriteFence        = vgpuR3VirglWriteFence;
    pThis->Callbacks.pfnWriteContextFence = vgpuR3VirglWriteContextFence;
    /* The GL, DRM, server fd and EGL callbacks stay NULL (zeroed). */

    /* Imported DLLs (the MinGW runtime, the Vulkan loader) are looked for next to the library. */
    int rc = RTLdrLoadEx(pszLibrary, &pThis->hLdrMod, RTLDRLOAD_FLAGS_NO_UNLOAD | RTLDRLOAD_FLAGS_NT_SEARCH_DLL_LOAD_DIR,
                         pErrInfo);
    if (RT_SUCCESS(rc))
    {
        for (unsigned i = 0; i < RT_ELEMENTS(g_aVirglSymbols) && RT_SUCCESS(rc); i++)
        {
            rc = RTLdrGetSymbol(pThis->hLdrMod, g_aVirglSymbols[i].pszName,
                                (void **)((uint8_t *)&pThis->Fns + g_aVirglSymbols[i].offMember));
            if (RT_FAILURE(rc))
                RTErrInfoSetF(pErrInfo, rc, "Symbol '%s' not found in '%s'", g_aVirglSymbols[i].pszName, pszLibrary);
        }
    }
    else
        RTErrInfoAddF(pErrInfo, rc, " (loading '%s')", pszLibrary);

    if (RT_SUCCESS(rc))
        rc = RTCritSectInit(&pThis->CritSectCall);
    if (RT_SUCCESS(rc))
        rc = RTSemEventCreate(&pThis->hEvtReq);
    if (RT_SUCCESS(rc))
        rc = RTSemEventCreate(&pThis->hEvtDone);
    if (RT_SUCCESS(rc))
    {
        rc = RTThreadCreate(&pThis->hThread, vgpuR3RendererThread, pThis, VGPURENDERER_THREAD_STACK_SIZE, RTTHREADTYPE_IO,
                            RTTHREADFLAGS_WAITABLE, "VGpuRender");
        if (RT_FAILURE(rc))
            pThis->hThread = NIL_RTTHREAD;
    }
    if (RT_SUCCESS(rc))
    {
        VGPURENDERREQ Req;
        RT_ZERO(Req);
        Req.enmType = kVGpuRenderReq_Init;
        rc = vgpuR3RendererCall(pThis, &Req);
        if (RT_FAILURE(rc))
            RTErrInfoSetF(pErrInfo, rc, "Initializing the Venus renderer from '%s' failed (see the log)", pszLibrary);
    }

    if (RT_SUCCESS(rc))
    {
        *ppRenderer = pThis;
        return VINF_SUCCESS;
    }
    vgpuR3RendererDestroy(pThis, true /*fCleanup*/);
    return rc;
}


/**
 * Shuts the renderer down and frees the instance.
 *
 * All contexts must have been destroyed and all blob mappings released
 * before, so the library calls no fence callback any more.
 *
 * @param   pThis       The renderer instance, NULL is ignored.
 * @param   fCleanup    Whether to call virgl_renderer_cleanup().  Pass false
 *                      when the library memory may still be visible to the
 *                      guest (a failed guest unmap); the library and this
 *                      instance are then left alone until the process exits.
 */
DECLHIDDEN(void) vgpuR3RendererDestroy(PVGPURENDERER pThis, bool fCleanup)
{
    if (!pThis)
        return;
    AssertReturnVoid(pThis->u32Magic == VGPURENDERER_MAGIC);

    /* From here on the device may disappear at any time, so the library's fence
       threads must not call it any more (they may outlive us if we have to leak
       a stuck library). */
    ASMAtomicWriteBool(&pThis->fNoFence, true);

    if (pThis->hThread != NIL_RTTHREAD)
    {
        if (fCleanup && !ASMAtomicReadBool(&pThis->fDead))
        {
            VGPURENDERREQ Req;
            RT_ZERO(Req);
            Req.enmType = kVGpuRenderReq_Cleanup;
            vgpuR3RendererCall(pThis, &Req);
        }
        if (ASMAtomicReadBool(&pThis->fDead))
        {
            /* A call is stuck inside the library and the thread still references the instance. */
            LogRel(("VirtioGpu: Leaving the stuck renderer behind\n"));
            return;
        }

        ASMAtomicWriteBool(&pThis->fTerminate, true);
        RTSemEventSignal(pThis->hEvtReq);
        int rcThread = VINF_SUCCESS;
        int rc = RTThreadWait(pThis->hThread, VGPURENDERER_TERM_TIMEOUT_MS, &rcThread);
        if (RT_FAILURE(rc))
        {
            /* The thread is stuck in the library and still references the instance. */
            LogRel(("VirtioGpu: The renderer thread did not stop (%Rrc), leaking it\n", rc));
            return;
        }
        pThis->hThread = NIL_RTTHREAD;
    }

    if (pThis->fInitialized)
    {
        /* The library still points at our callbacks and cookie, keep them. */
        LogRel(("VirtioGpu: Leaving the renderer initialized until the process exits\n"));
        return;
    }

    if (pThis->hEvtDone != NIL_RTSEMEVENT)
        RTSemEventDestroy(pThis->hEvtDone);
    if (pThis->hEvtReq != NIL_RTSEMEVENT)
        RTSemEventDestroy(pThis->hEvtReq);
    if (RTCritSectIsInitialized(&pThis->CritSectCall))
        RTCritSectDelete(&pThis->CritSectCall);
    if (pThis->hLdrMod != NIL_RTLDRMOD)
        RTLdrClose(pThis->hLdrMod); /* RTLDRLOAD_FLAGS_NO_UNLOAD: the image stays loaded. */
    RTMemFree(pThis->pbCapset);
    pThis->u32Magic = ~VGPURENDERER_MAGIC;
    RTMemFree(pThis);
}


/**
 * Returns the cached capset.
 *
 * @returns VBox status code, VERR_NOT_FOUND for an unsupported capset id.
 * @param   pThis           The renderer instance.
 * @param   idCapset        The capset id, only VIRTIOGPU_CAPSET_VENUS is known.
 * @param   puMaxVersion    Where to return the highest version.
 * @param   pcbCapset       Where to return the capset size.
 * @param   ppbCapset       Where to return the capset data (read only, valid
 *                          until the renderer is destroyed).  Optional.
 */
DECLHIDDEN(int) vgpuR3RendererQueryCapset(PVGPURENDERER pThis, uint32_t idCapset, uint32_t *puMaxVersion,
                                          uint32_t *pcbCapset, uint8_t const **ppbCapset)
{
    AssertReturn(pThis && pThis->u32Magic == VGPURENDERER_MAGIC, VERR_INVALID_HANDLE);
    if (idCapset != VIRTIOGPU_CAPSET_VENUS || !pThis->pbCapset)
        return VERR_NOT_FOUND;
    *puMaxVersion = pThis->uCapsetMaxVersion;
    *pcbCapset    = pThis->cbCapset;
    if (ppbCapset)
        *ppbCapset = pThis->pbCapset;
    return VINF_SUCCESS;
}


/**
 * Creates a context (virgl_renderer_context_create_with_flags).
 *
 * @returns VBox status code.
 * @param   pThis           The renderer instance.
 * @param   idCtx           The context id, 1..INT32_MAX, not in use.
 * @param   fContextInit    The context flags (capset id).
 * @param   pachName        The debug name, need not be terminated.
 * @param   cchName         The length of the debug name.
 */
DECLHIDDEN(int) vgpuR3RendererContextCreate(PVGPURENDERER pThis, uint32_t idCtx, uint32_t fContextInit,
                                            const char *pachName, uint32_t cchName)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType                     = kVGpuRenderReq_ContextCreate;
    Req.u.ContextCreate.idCtx       = idCtx;
    Req.u.ContextCreate.fCtxFlags   = fContextInit;
    Req.u.ContextCreate.cchName     = cchName;
    Req.u.ContextCreate.pachName    = pachName;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Creating context %RU32 failed: %d (%Rrc)\n", idCtx, Req.iVirglRc, rc));
    return rc;
}


/**
 * Destroys a context (virgl_renderer_context_destroy).  No fence callback
 * for the context happens after this returns.
 *
 * @param   pThis       The renderer instance.
 * @param   idCtx       The context id.
 */
DECLHIDDEN(void) vgpuR3RendererContextDestroy(PVGPURENDERER pThis, uint32_t idCtx)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType     = kVGpuRenderReq_ContextDestroy;
    Req.u.Ids.idCtx = idCtx;
    vgpuR3RendererCall(pThis, &Req);
}


/**
 * Submits a command buffer to a context (virgl_renderer_submit_cmd).
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idCtx       The context id.
 * @param   pvCmds      The commands, 4 byte aligned, not changed while the
 *                      call runs.  The library does not modify them.
 * @param   cbCmds      The size in bytes, a multiple of 4.
 */
DECLHIDDEN(int) vgpuR3RendererSubmit(PVGPURENDERER pThis, uint32_t idCtx, void *pvCmds, uint32_t cbCmds)
{
    AssertReturn(!(cbCmds & 3) && cbCmds / 4 <= INT32_MAX, VERR_INVALID_PARAMETER);
    AssertReturn(!((uintptr_t)pvCmds & 3), VERR_INVALID_POINTER);

    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_Submit;
    Req.u.Submit.idCtx   = idCtx;
    Req.u.Submit.cDWords = cbCmds / 4;
    Req.u.Submit.pvCmds  = pvCmds;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Submitting %RU32 bytes to context %RU32 failed: %d (%Rrc)\n", cbCmds, idCtx, Req.iVirglRc, rc));
    return rc;
}


/**
 * Creates a mergeable context fence (virgl_renderer_context_create_fence).
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idCtx       The context id.
 * @param   uRingIdx    The ring (timeline) index, below 64.
 * @param   uFenceId    The fence id passed back to the fence callback.
 */
DECLHIDDEN(int) vgpuR3RendererContextFence(PVGPURENDERER pThis, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ContextFence;
    Req.u.Fence.idCtx    = idCtx;
    Req.u.Fence.uRingIdx = uRingIdx;
    Req.u.Fence.uFenceId = uFenceId;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Creating fence %#RX64 on context %RU32 ring %RU32 failed: %d (%Rrc)\n",
                       uFenceId, idCtx, uRingIdx, Req.iVirglRc, rc));
    return rc;
}


/**
 * Creates a host blob resource without guest backing
 * (virgl_renderer_resource_create_blob).
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idCtx       The creating context.
 * @param   idResource  The resource id, 1..INT32_MAX, not in use.
 * @param   uBlobMem    VIRTIOGPU_BLOB_MEM_HOST3D.
 * @param   fBlobFlags  VIRTIOGPU_BLOB_FLAG_XXX.
 * @param   idBlob      The context specific blob id.
 * @param   cbBlob      The size in bytes, not zero.
 */
DECLHIDDEN(int) vgpuR3RendererBlobCreate(PVGPURENDERER pThis, uint32_t idCtx, uint32_t idResource, uint32_t uBlobMem,
                                         uint32_t fBlobFlags, uint64_t idBlob, uint64_t cbBlob)
{
    AssertReturn(uBlobMem == VIRTIOGPU_BLOB_MEM_HOST3D, VERR_INVALID_PARAMETER);

    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType                 = kVGpuRenderReq_BlobCreate;
    Req.u.BlobCreate.idResource = idResource;
    Req.u.BlobCreate.idCtx      = idCtx;
    Req.u.BlobCreate.uBlobMem   = uBlobMem;
    Req.u.BlobCreate.fBlobFlags = fBlobFlags;
    Req.u.BlobCreate.idBlob     = idBlob;
    Req.u.BlobCreate.cbBlob     = cbBlob;
    Req.u.BlobCreate.paIoVecs   = NULL;
    Req.u.BlobCreate.cIoVecs    = 0;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Creating blob resource %RU32 (context %RU32, blob %#RX64, %#RX64 bytes, flags %#RX32)"
                       " failed: %d (%Rrc)\n", idResource, idCtx, idBlob, cbBlob, fBlobFlags, Req.iVirglRc, rc));
    return rc;
}


/**
 * Releases a resource (virgl_renderer_resource_unref); it is detached from
 * all contexts by the library.
 *
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 */
DECLHIDDEN(void) vgpuR3RendererResourceUnref(PVGPURENDERER pThis, uint32_t idResource)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ResourceUnref;
    Req.u.Ids.idResource = idResource;
    vgpuR3RendererCall(pThis, &Req);
}


/**
 * Attaches a resource to a context (virgl_renderer_ctx_attach_resource).
 *
 * @param   pThis       The renderer instance.
 * @param   idCtx       The context id, 1..INT32_MAX.
 * @param   idResource  The resource id, 1..INT32_MAX.
 */
DECLHIDDEN(void) vgpuR3RendererContextAttach(PVGPURENDERER pThis, uint32_t idCtx, uint32_t idResource)
{
    AssertReturnVoid(idCtx <= INT32_MAX && idResource <= INT32_MAX);
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ContextAttach;
    Req.u.Ids.idCtx      = idCtx;
    Req.u.Ids.idResource = idResource;
    vgpuR3RendererCall(pThis, &Req);
}


/**
 * Detaches a resource from a context (virgl_renderer_ctx_detach_resource).
 *
 * @param   pThis       The renderer instance.
 * @param   idCtx       The context id, 1..INT32_MAX.
 * @param   idResource  The resource id, 1..INT32_MAX.
 */
DECLHIDDEN(void) vgpuR3RendererContextDetach(PVGPURENDERER pThis, uint32_t idCtx, uint32_t idResource)
{
    AssertReturnVoid(idCtx <= INT32_MAX && idResource <= INT32_MAX);
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ContextDetach;
    Req.u.Ids.idCtx      = idCtx;
    Req.u.Ids.idResource = idResource;
    vgpuR3RendererCall(pThis, &Req);
}


/**
 * Maps a blob resource into the host address space
 * (virgl_renderer_resource_map + virgl_renderer_resource_get_map_info).
 *
 * @returns VBox status code.  On failure nothing is mapped.
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 * @param   ppvHost     Where to return the host address.  Not validated here.
 * @param   pcbHost     Where to return the size of the mapping.  Not validated here.
 * @param   pfMapInfo   Where to return the caching type (VIRTIOGPU_MAP_CACHE_XXX).
 */
DECLHIDDEN(int) vgpuR3RendererResourceMap(PVGPURENDERER pThis, uint32_t idResource, void **ppvHost, uint64_t *pcbHost,
                                          uint32_t *pfMapInfo)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ResourceMap;
    Req.u.Map.idResource = idResource;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_SUCCESS(rc))
    {
        *ppvHost   = Req.u.Map.pvHost;
        *pcbHost   = Req.u.Map.cbHost;
        *pfMapInfo = Req.u.Map.fMapInfo;
    }
    else
        LogRelMax(16, ("VirtioGpu: Mapping resource %RU32 failed: %d (%Rrc)\n", idResource, Req.iVirglRc, rc));
    return rc;
}


/**
 * Releases the host mapping of a blob resource (virgl_renderer_resource_unmap).
 * The guest must no longer be able to reach the memory.
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 */
DECLHIDDEN(int) vgpuR3RendererResourceUnmap(PVGPURENDERER pThis, uint32_t idResource)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ResourceUnmap;
    Req.u.Ids.idResource = idResource;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Unmapping resource %RU32 failed: %d (%Rrc)\n", idResource, Req.iVirglRc, rc));
    return rc;
}

