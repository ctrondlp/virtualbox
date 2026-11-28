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
 *    briefly after virgl_renderer_cleanup().  VBoxDD, on the other hand, is
 *    unloaded when the VM is destroyed, so the log callback (which lives
 *    there) is removed again when the renderer is destroyed.
 *
 *  - OpenGL (virgl) is optional.  The library renders the guest's OpenGL
 *    with the host's OpenGL and asks its user for the contexts.  On Windows
 *    hosts they are WGL contexts on a hidden message-only window owned by the
 *    renderer thread.  All of them share objects with one root context that is
 *    never current and lives as long as the library is initialized: the
 *    library destroys the first context it creates again, so that one cannot
 *    be the share partner.  The library makes one of the contexts current on
 *    its own fence thread; WGL allows that for a device context of another
 *    thread.  There is no implementation for other hosts yet.
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
#include <iprt/ctype.h>
#include <iprt/env.h>
#include <iprt/errcore.h>
#include <iprt/ldr.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/time.h>
#ifdef RT_OS_WINDOWS
# include <iprt/win/windows.h>
#endif

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
/** VIRTGPU_DRM_CAPSET_VIRGL and VIRTGPU_DRM_CAPSET_VIRGL2: OpenGL. */
#define VGPU_VIRGL_CAPSET_VIRGL                 1
#define VGPU_VIRGL_CAPSET_VIRGL2                2
/** The number of capsets kept (virgl, virgl2, Venus). */
#define VGPU_VIRGL_CAPSET_COUNT                 3

/** @name errno values the library returns (positive or negated).
 * The values are the same in the MinGW and the Microsoft C runtimes.
 * @{ */
#define VGPU_VIRGL_ENOMEM                       12
#define VGPU_VIRGL_EINVAL                       22
/** @} */

AssertCompile(VGPU_VIRGL_CAPSET_VENUS == VIRTIOGPU_CAPSET_VENUS);
AssertCompile(VGPU_VIRGL_CAPSET_VIRGL == VIRTIOGPU_CAPSET_VIRGL);
AssertCompile(VGPU_VIRGL_CAPSET_VIRGL2 == VIRTIOGPU_CAPSET_VIRGL2);
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
 * struct virgl_renderer_gl_ctx_param (virglrenderer.h:43-49): what kind of
 * OpenGL context the library wants.
 */
typedef struct VGPUVIRGLGLCTXPARAM
{
    /** version */
    int         iVersion;
    /** shared: share objects with the other contexts. */
    bool        fShared;
    /** major_ver */
    int         iMajor;
    /** minor_ver */
    int         iMinor;
    /** compat_ctx: compatibility instead of core profile. */
    int         fCompat;
} VGPUVIRGLGLCTXPARAM;
AssertCompileSize(VGPUVIRGLGLCTXPARAM, 20);
AssertCompileMemberOffset(VGPUVIRGLGLCTXPARAM, iMajor, 8);

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
    /** write_fence: context 0 (virgl) fences, never created here. */
    DECLCALLBACKMEMBER(void,   pfnWriteFence,(void *pvCookie, uint32_t uFence));
    /** create_gl_context: NULL without OpenGL. */
    DECLCALLBACKMEMBER(void *, pfnCreateGlContext,(void *pvCookie, int iScanout, struct VGPUVIRGLGLCTXPARAM *pParam));
    /** destroy_gl_context: NULL without OpenGL. */
    DECLCALLBACKMEMBER(void,   pfnDestroyGlContext,(void *pvCookie, void *pvGlCtx));
    /** make_current: NULL without OpenGL. */
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
    /** @name Optional, NULL in a library without them (see g_aVirglOptionalSymbols).
     * @{ */
    /** virgl_renderer_set_vk_memory_limit: limit for the memory all contexts can allocate on the host. */
    DECLCALLBACKMEMBER(void,     pfnSetVkMemoryLimit,(uint64_t cbLimit));
    /** virgl_renderer_get_vk_memory_used: what counts against that limit. */
    DECLCALLBACKMEMBER(uint64_t, pfnGetVkMemoryUsed,(void));
    /** virgl_set_log_callback: where the library sends its messages (it writes to stderr otherwise).
     * The first callback argument is enum virgl_log_level_flags. */
    DECLCALLBACKMEMBER(void,     pfnSetLogCallback,(void (*pfnLog)(int iLevel, const char *pszMsg, void *pvUser), void *pvUser,
                                                    void (*pfnFreeUser)(void *pvUser)));
    /** @} */
    /** @name OpenGL (virgl), all of them are needed for it (see g_aVirglGlSymbols).
     * @{ */
    /** virgl_renderer_force_ctx_0: forget which OpenGL context is current and make the library's own
     * (context 0) current, see vgpuR3RendererExec. */
    DECLCALLBACKMEMBER(void, pfnForceCtx0,(void));
    /** virgl_renderer_resource_create (:278), without a backing. */
    DECLCALLBACKMEMBER(int,  pfnResourceCreate,(const VGPURENDERERCREATE3D *pArgs, VGPURENDERERIOV *paIov, uint32_t cIov));
    /** virgl_renderer_resource_attach_iov (:331): the library keeps the pointer to the array. */
    DECLCALLBACKMEMBER(int,  pfnResourceAttachIov,(int idResource, VGPURENDERERIOV *paIov, int cIov));
    /** virgl_renderer_resource_detach_iov (:333) */
    DECLCALLBACKMEMBER(void, pfnResourceDetachIov,(int idResource, VGPURENDERERIOV **ppaIov, int *pcIov));
    /** virgl_renderer_transfer_read_iov (:308): host to guest. */
    DECLCALLBACKMEMBER(int,  pfnTransferReadIov,(uint32_t idResource, uint32_t idCtx, uint32_t uLevel, uint32_t cbStride,
                                                 uint32_t cbLayerStride, VGPURENDERERBOX *pBox, uint64_t off,
                                                 VGPURENDERERIOV *paIov, int cIov));
    /** virgl_renderer_transfer_write_iov (:315): guest to host. */
    DECLCALLBACKMEMBER(int,  pfnTransferWriteIov,(uint32_t idResource, uint32_t idCtx, int iLevel, uint32_t cbStride,
                                                  uint32_t cbLayerStride, VGPURENDERERBOX *pBox, uint64_t off,
                                                  VGPURENDERERIOV *paIov, unsigned cIov));
    /** virgl_renderer_poll (:173): lets the library check its OpenGL queries, which its fence thread
     * waits for before it retires a fence. */
    DECLCALLBACKMEMBER(void, pfnPoll,(void));
    /** @} */
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

/** Symbols a library may lack (an older build of it); the members stay NULL then. */
static const struct
{
    const char *pszName;
    uint32_t    offMember;
} g_aVirglOptionalSymbols[] =
{
    { "virgl_renderer_set_vk_memory_limit",         RT_UOFFSETOF(VGPUVIRGLFNS, pfnSetVkMemoryLimit) },
    { "virgl_renderer_get_vk_memory_used",          RT_UOFFSETOF(VGPUVIRGLFNS, pfnGetVkMemoryUsed) },
    { "virgl_set_log_callback",                     RT_UOFFSETOF(VGPUVIRGLFNS, pfnSetLogCallback) },
};

/** Symbols needed for OpenGL (virgl); without them only Venus is offered. */
static const struct
{
    const char *pszName;
    uint32_t    offMember;
} g_aVirglGlSymbols[] =
{
    { "virgl_renderer_resource_create",             RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceCreate) },
    { "virgl_renderer_resource_attach_iov",         RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceAttachIov) },
    { "virgl_renderer_resource_detach_iov",         RT_UOFFSETOF(VGPUVIRGLFNS, pfnResourceDetachIov) },
    { "virgl_renderer_transfer_read_iov",           RT_UOFFSETOF(VGPUVIRGLFNS, pfnTransferReadIov) },
    { "virgl_renderer_transfer_write_iov",          RT_UOFFSETOF(VGPUVIRGLFNS, pfnTransferWriteIov) },
    { "virgl_renderer_poll",                        RT_UOFFSETOF(VGPUVIRGLFNS, pfnPoll) },
    { "virgl_renderer_force_ctx_0",                 RT_UOFFSETOF(VGPUVIRGLFNS, pfnForceCtx0) },
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
/** With OpenGL: how often the renderer thread lets the library check its queries while the guest
 * is rendering.  The library's fence thread holds a fence back until that happened when a query is
 * pending, so this bounds how late such a fence is signalled. */
#define VGPURENDERER_GL_POLL_BUSY_MS            1
/** With OpenGL: the same while nothing was asked of the renderer for VGPURENDERER_GL_POLL_BUSY_WINDOW_MS. */
#define VGPURENDERER_GL_POLL_IDLE_MS            50
/** With OpenGL: how long after the last request the renderer counts as busy. */
#define VGPURENDERER_GL_POLL_BUSY_WINDOW_MS     500
/** How long a caller waits for one call into the library to finish.  A call
 * that takes longer (a hung or lost GPU) declares the renderer dead, so that
 * suspend and power off do not wait for ever. */
#define VGPURENDERER_CALL_TIMEOUT_MS            RT_MS_1MIN


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
    kVGpuRenderReq_ResourceCreate3d,
    kVGpuRenderReq_ResourceAttachIov,
    kVGpuRenderReq_ResourceDetachIov,
    kVGpuRenderReq_Transfer,
    kVGpuRenderReq_ResourceRead,
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
        VGPURENDERERCREATE3D Create3d;
        struct
        {
            uint32_t        idResource;
            uint32_t        cIov;
            PVGPURENDERERIOV paIov;
        } AttachIov;
        struct
        {
            bool            fToHost;
            uint32_t        idResource;
            uint32_t        idCtx;
            uint32_t        uLevel;
            uint32_t        cbStride;
            uint32_t        cbLayerStride;
            VGPURENDERERBOX Box;
            uint64_t        off;
        } Transfer;
        struct
        {
            uint32_t        idResource;
            uint32_t        cbStride;
            VGPURENDERERBOX Box;
            /** The destination, a heap block owned by the request (see vgpuR3RendererCall). */
            void           *pvDst;
            size_t          cbDst;
        } Read;
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
    /** @name The capsets, fetched once at init and read only afterwards.
     * Unused entries have a NULL pbCapset.
     * @{ */
    struct
    {
        uint32_t                idCapset;
        uint32_t                uMaxVersion;
        uint32_t                cbCapset;
        uint8_t                *pbCapset;
    } aCapsets[VGPU_VIRGL_CAPSET_COUNT];
    /** @} */
    /** Whether the device wants OpenGL (virgl) too. */
    bool                        fWantVirgl;
    /** Whether the library has all the entry points OpenGL needs. */
    bool                        fHaveGlSymbols;
    /** Set when the library was initialized with OpenGL (written once by the
     * renderer thread during init, read only afterwards). */
    bool                        fVirgl;
#ifdef RT_OS_WINDOWS
    /** @name WGL state for OpenGL, owned by the renderer thread.
     * @{ */
    RTLDRMOD                    hLdrOpenGL;
    HWND                        hwndGl;
    HDC                         hdcGl;
    /** The root of the share group: never current, destroyed with the window. */
    HGLRC                       hrcRoot;
    HGLRC                       (WINAPI *pfnWglCreateContext)(HDC);
    BOOL                        (WINAPI *pfnWglDeleteContext)(HGLRC);
    BOOL                        (WINAPI *pfnWglMakeCurrent)(HDC, HGLRC);
    PROC                        (WINAPI *pfnWglGetProcAddress)(LPCSTR);
    const unsigned char *       (WINAPI *pfnGlGetString)(unsigned);
    HGLRC                       (WINAPI *pfnWglCreateContextAttribs)(HDC, HGLRC, const int *);
    /** @} */
#endif
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


#ifdef RT_OS_WINDOWS
/** @name WGL_ARB_create_context(_profile).
 * @{ */
# define VGPU_WGL_CONTEXT_MAJOR_VERSION_ARB             0x2091
# define VGPU_WGL_CONTEXT_MINOR_VERSION_ARB             0x2092
# define VGPU_WGL_CONTEXT_PROFILE_MASK_ARB              0x9126
# define VGPU_WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x0001
# define VGPU_WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x0002
/** @} */
/** GL_RENDERER and GL_VERSION for glGetString. */
# define VGPU_GL_RENDERER                               0x1F01
# define VGPU_GL_VERSION                                0x1F02
/** The window class of the hidden window the contexts are created on. */
# define VGPU_WGL_WINDOW_CLASS                          L"VBoxVirtioGpuGL"

/**
 * create_gl_context callback: creates an OpenGL context of the requested
 * version and profile, sharing objects with all the others.
 *
 * Called on the renderer thread, from inside a library call.
 */
static DECLCALLBACK(void *) vgpuR3VirglCreateGlContext(void *pvCookie, int iScanout, VGPUVIRGLGLCTXPARAM *pParam)
{
    PVGPURENDERER pThis = (PVGPURENDERER)pvCookie;
    RT_NOREF(iScanout);
    AssertReturn(RT_VALID_PTR(pThis) && pThis->u32Magic == VGPURENDERER_MAGIC && pThis->pfnWglCreateContextAttribs, NULL);
    AssertPtrReturn(pParam, NULL);
    int const aiAttribs[] =
    {
        VGPU_WGL_CONTEXT_MAJOR_VERSION_ARB, pParam->iMajor,
        VGPU_WGL_CONTEXT_MINOR_VERSION_ARB, pParam->iMinor,
        VGPU_WGL_CONTEXT_PROFILE_MASK_ARB,  pParam->fCompat ? VGPU_WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
                                                            : VGPU_WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    HGLRC hrc = pThis->pfnWglCreateContextAttribs(pThis->hdcGl, pThis->hrcRoot, aiAttribs);
    if (!hrc)
        Log(("VirtioGpu: No OpenGL %d.%d %s context (%u)\n", pParam->iMajor, pParam->iMinor,
             pParam->fCompat ? "compatibility" : "core", GetLastError()));
    return hrc;
}


/**
 * destroy_gl_context callback.
 */
static DECLCALLBACK(void) vgpuR3VirglDestroyGlContext(void *pvCookie, void *pvGlCtx)
{
    PVGPURENDERER pThis = (PVGPURENDERER)pvCookie;
    AssertReturnVoid(RT_VALID_PTR(pThis) && pThis->u32Magic == VGPURENDERER_MAGIC && pThis->pfnWglDeleteContext);
    if (pvGlCtx)
        pThis->pfnWglDeleteContext((HGLRC)pvGlCtx);
}


/**
 * make_current callback: called on the renderer thread and on the library's
 * fence thread (with the context created for it).
 *
 * @returns 0 on success, -1 on failure.
 */
static DECLCALLBACK(int) vgpuR3VirglMakeCurrent(void *pvCookie, int iScanout, void *pvGlCtx)
{
    PVGPURENDERER pThis = (PVGPURENDERER)pvCookie;
    RT_NOREF(iScanout);
    AssertReturn(RT_VALID_PTR(pThis) && pThis->u32Magic == VGPURENDERER_MAGIC && pThis->pfnWglMakeCurrent, -1);
    return pThis->pfnWglMakeCurrent(pvGlCtx ? pThis->hdcGl : NULL, (HGLRC)pvGlCtx) ? 0 : -1;
}


/**
 * Releases what vgpuR3RendererWglInit set up.  Renderer thread, no context
 * of the library may exist any more.
 *
 * @param   pThis       The renderer instance.
 */
static void vgpuR3RendererWglTerm(PVGPURENDERER pThis)
{
    if (pThis->pfnWglMakeCurrent)
        pThis->pfnWglMakeCurrent(NULL, NULL);
    if (pThis->hrcRoot)
        pThis->pfnWglDeleteContext(pThis->hrcRoot);
    pThis->hrcRoot = NULL;
    if (pThis->hdcGl)
        ReleaseDC(pThis->hwndGl, pThis->hdcGl);
    pThis->hdcGl = NULL;
    if (pThis->hwndGl)
        DestroyWindow(pThis->hwndGl);
    pThis->hwndGl = NULL;
    pThis->pfnWglCreateContextAttribs = NULL;
    /* opengl32.dll stays loaded (the driver does not like being unloaded). */
}


/**
 * Prepares WGL: a hidden window with a pixel format and the root context.
 * Renderer thread.
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 */
static int vgpuR3RendererWglInit(PVGPURENDERER pThis)
{
    static const struct
    {
        const char *pszName;
        uint32_t    offMember;
    } s_aSymbols[] =
    {
        { "wglCreateContext",   RT_UOFFSETOF(VGPURENDERER, pfnWglCreateContext) },
        { "wglDeleteContext",   RT_UOFFSETOF(VGPURENDERER, pfnWglDeleteContext) },
        { "wglMakeCurrent",     RT_UOFFSETOF(VGPURENDERER, pfnWglMakeCurrent) },
        { "wglGetProcAddress",  RT_UOFFSETOF(VGPURENDERER, pfnWglGetProcAddress) },
        { "glGetString",        RT_UOFFSETOF(VGPURENDERER, pfnGlGetString) },
    };
    if (pThis->hLdrOpenGL == NIL_RTLDRMOD)
    {
        int rc = RTLdrLoadSystem("opengl32", true /*fNoUnload*/, &pThis->hLdrOpenGL);
        if (RT_FAILURE(rc))
        {
            pThis->hLdrOpenGL = NIL_RTLDRMOD;
            LogRel(("VirtioGpu: Loading opengl32.dll failed: %Rrc\n", rc));
            return rc;
        }
    }
    for (unsigned i = 0; i < RT_ELEMENTS(s_aSymbols); i++)
    {
        int rc = RTLdrGetSymbol(pThis->hLdrOpenGL, s_aSymbols[i].pszName, (void **)((uint8_t *)pThis + s_aSymbols[i].offMember));
        if (RT_FAILURE(rc))
        {
            LogRel(("VirtioGpu: opengl32.dll lacks %s: %Rrc\n", s_aSymbols[i].pszName, rc));
            return rc;
        }
    }

    /* A message-only window: it is never shown and gets no broadcast messages, so nobody waits for a
       thread that does not pump messages. */
    WNDCLASSW WndClass;
    RT_ZERO(WndClass);
    WndClass.style         = CS_OWNDC;
    WndClass.lpfnWndProc   = DefWindowProcW;
    WndClass.hInstance     = GetModuleHandleW(NULL);
    WndClass.lpszClassName = VGPU_WGL_WINDOW_CLASS;
    if (!RegisterClassW(&WndClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogRel(("VirtioGpu: RegisterClassW failed: %u\n", GetLastError()));
        return VERR_NOT_SUPPORTED;
    }
    pThis->hwndGl = CreateWindowExW(0, VGPU_WGL_WINDOW_CLASS, L"", WS_POPUP, 0, 0, 1, 1, HWND_MESSAGE, NULL,
                                    WndClass.hInstance, NULL);
    if (pThis->hwndGl)
        pThis->hdcGl = GetDC(pThis->hwndGl);
    if (!pThis->hdcGl)
    {
        LogRel(("VirtioGpu: Creating the OpenGL window failed: %u\n", GetLastError()));
        vgpuR3RendererWglTerm(pThis);
        return VERR_NOT_SUPPORTED;
    }

    PIXELFORMATDESCRIPTOR Pfd;
    RT_ZERO(Pfd);
    Pfd.nSize      = sizeof(Pfd);
    Pfd.nVersion   = 1;
    Pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    Pfd.iPixelType = PFD_TYPE_RGBA;
    Pfd.cColorBits = 32;
    int const iFormat = ChoosePixelFormat(pThis->hdcGl, &Pfd);
    if (!iFormat || !SetPixelFormat(pThis->hdcGl, iFormat, &Pfd))
    {
        LogRel(("VirtioGpu: No OpenGL pixel format: %u\n", GetLastError()));
        vgpuR3RendererWglTerm(pThis);
        return VERR_NOT_SUPPORTED;
    }

    /* wglCreateContextAttribsARB can only be looked up with a context current. */
    char szRenderer[128] = "";
    char szVersion[128]  = "";
    HGLRC hrcBoot = pThis->pfnWglCreateContext(pThis->hdcGl);
    if (hrcBoot && pThis->pfnWglMakeCurrent(pThis->hdcGl, hrcBoot))
    {
        *(PROC *)&pThis->pfnWglCreateContextAttribs = pThis->pfnWglGetProcAddress("wglCreateContextAttribsARB");
        const char *psz = (const char *)pThis->pfnGlGetString(VGPU_GL_RENDERER);
        RTStrCopy(szRenderer, sizeof(szRenderer), psz ? psz : "?");
        psz = (const char *)pThis->pfnGlGetString(VGPU_GL_VERSION);
        RTStrCopy(szVersion, sizeof(szVersion), psz ? psz : "?");
        pThis->pfnWglMakeCurrent(NULL, NULL);
    }
    if (hrcBoot)
        pThis->pfnWglDeleteContext(hrcBoot);
    if (!pThis->pfnWglCreateContextAttribs)
    {
        LogRel(("VirtioGpu: The host OpenGL has no wglCreateContextAttribsARB (%u)\n", GetLastError()));
        vgpuR3RendererWglTerm(pThis);
        return VERR_NOT_SUPPORTED;
    }

    /* The root of the share group.  The driver returns the newest version compatible with 3.2 core. */
    int const aiAttribs[] =
    {
        VGPU_WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        VGPU_WGL_CONTEXT_MINOR_VERSION_ARB, 2,
        VGPU_WGL_CONTEXT_PROFILE_MASK_ARB,  VGPU_WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    pThis->hrcRoot = pThis->pfnWglCreateContextAttribs(pThis->hdcGl, NULL, aiAttribs);
    if (!pThis->hrcRoot)
    {
        LogRel(("VirtioGpu: The host OpenGL (%s, %s) has no 3.2 core profile (%u)\n", szRenderer, szVersion, GetLastError()));
        vgpuR3RendererWglTerm(pThis);
        return VERR_NOT_SUPPORTED;
    }
    LogRel(("VirtioGpu: Host OpenGL: %s, %s\n", szRenderer, szVersion));
    return VINF_SUCCESS;
}
#endif /* RT_OS_WINDOWS */


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
 * Fetches a capset and keeps a copy of it.  Renderer thread, after init.
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   iSlot       The index into VGPURENDERER::aCapsets.
 * @param   idCapset    The capset id.
 * @param   cbMin       The smallest plausible size.
 */
static int vgpuR3RendererFetchCapset(PVGPURENDERER pThis, unsigned iSlot, uint32_t idCapset, uint32_t cbMin)
{
    AssertReturn(iSlot < RT_ELEMENTS(pThis->aCapsets) && !pThis->aCapsets[iSlot].pbCapset, VERR_INTERNAL_ERROR_4);
    uint32_t uMaxVersion = UINT32_MAX;
    uint32_t cbCapset    = UINT32_MAX;
    pThis->Fns.pfnGetCapSet(idCapset, &uMaxVersion, &cbCapset);
    if (   cbCapset < cbMin
        || cbCapset > VGPURENDERER_MAX_CAPSET_SIZE
        || uMaxVersion > VGPURENDERER_MAX_CAPSET_VERSION)
    {
        LogRel(("VirtioGpu: Implausible capset %RU32 from the renderer: version %RU32, %RU32 bytes\n",
                idCapset, uMaxVersion, cbCapset));
        return VERR_NOT_SUPPORTED;
    }

    /* The buffer is the maximum size, not cbCapset: the library writes its own
       structure and its idea of the size is all we have to go on. */
    uint8_t *pbCapset = (uint8_t *)RTMemAllocZ(VGPURENDERER_MAX_CAPSET_SIZE);
    if (!pbCapset)
        return VERR_NO_MEMORY;
    pThis->Fns.pfnFillCaps(idCapset, uMaxVersion, pbCapset);
    pThis->aCapsets[iSlot].idCapset    = idCapset;
    pThis->aCapsets[iSlot].uMaxVersion = uMaxVersion;
    pThis->aCapsets[iSlot].cbCapset    = cbCapset;
    pThis->aCapsets[iSlot].pbCapset    = pbCapset;
    return VINF_SUCCESS;
}


/** VIRGL_LOG_LEVEL_INFO and VIRGL_LOG_LEVEL_WARNING of enum
 * virgl_log_level_flags; debug is below, errors are above. */
#define VGPU_VIRGL_LOG_LEVEL_INFO       1
#define VGPU_VIRGL_LOG_LEVEL_WARNING    2

/** How many kinds of renderer messages vgpuR3RendererLibLog tells apart; a kind
 * is the level and the text of a message without its digits. */
#define VGPU_RENDERER_LOG_KINDS         64
/** How many messages of each kind are logged. */
#define VGPU_RENDERER_LOG_PER_KIND      4

/** The kinds of renderer messages seen (hashes, 0 for a free slot) and how many
 * of each.  The library is loaded once per VM process and its log callback has
 * no device instance, hence global; a race between two threads costs at most a
 * line too many or too few. */
static uint32_t volatile g_auVgpuRendererLogKinds[VGPU_RENDERER_LOG_KINDS];
/** Messages seen per kind, see g_auVgpuRendererLogKinds. */
static uint32_t volatile g_acVgpuRendererLogKinds[VGPU_RENDERER_LOG_KINDS];

/**
 * Counts a renderer message by its kind.
 *
 * @returns The number of messages of this kind seen so far, this one included;
 *          1 when the table of kinds is full.
 * @param   iLevel      enum virgl_log_level_flags.
 * @param   pszMsg      The message.
 * @param   cch         The length of the message.
 */
static uint32_t vgpuR3RendererLogCountKind(int iLevel, const char *pszMsg, size_t cch)
{
    /* FNV-1a over the level and the text without digits, so that messages that
       only differ in ids and sizes are of one kind. */
    uint32_t uHash = (UINT32_C(2166136261) ^ (uint32_t)iLevel) * UINT32_C(16777619);
    for (size_t off = 0; off < cch; off++)
        if (!RT_C_IS_DIGIT(pszMsg[off]))
            uHash = (uHash ^ (uint8_t)pszMsg[off]) * UINT32_C(16777619);
    if (!uHash)
        uHash = 1;

    for (uint32_t i = 0; i < RT_ELEMENTS(g_auVgpuRendererLogKinds); i++)
    {
        uint32_t const uKind = ASMAtomicReadU32(&g_auVgpuRendererLogKinds[i]);
        if (   uKind == uHash
            || (   !uKind
                && (   ASMAtomicCmpXchgU32(&g_auVgpuRendererLogKinds[i], uHash, 0)
                    || ASMAtomicReadU32(&g_auVgpuRendererLogKinds[i]) == uHash)))
            return ASMAtomicIncU32(&g_acVgpuRendererLogKinds[i]);
    }
    return 1;
}

/**
 * Log callback of the renderer library (virgl_log_callback_type), called on
 * any of its threads.
 *
 * @param   iLevel      enum virgl_log_level_flags.
 * @param   pszMsg      The message; what it says about guest objects comes from
 *                      the guest.
 * @param   pvUser      Unused.
 */
static void vgpuR3RendererLibLog(int iLevel, const char *pszMsg, void *pvUser)
{
    RT_NOREF(pvUser);
    if (iLevel < VGPU_VIRGL_LOG_LEVEL_INFO || !pszMsg)
        return;
    size_t cch = strlen(pszMsg);
    while (cch > 0 && (pszMsg[cch - 1] == '\n' || pszMsg[cch - 1] == '\r'))
        cch--;

    /* A guest can make the library repeat itself as often as it likes: every
       context a program closes and every device it leaks gets a line.  So only
       the first few messages of each kind are logged, and routine ones cannot
       use up the budget before the one that explains a failure (the Venus part
       says why it gives up on a guest context at the information level). */
    uint32_t const cSeen = vgpuR3RendererLogCountKind(iLevel, pszMsg, cch);
    if (cSeen > VGPU_RENDERER_LOG_PER_KIND)
        return;
    const char *pszMore = cSeen == VGPU_RENDERER_LOG_PER_KIND ? " (further messages like this are not logged)" : "";
    if (iLevel == VGPU_VIRGL_LOG_LEVEL_INFO)
        LogRelMax(128, ("VirtioGpu: renderer info: %.*s%s\n", (int)RT_MIN(cch, 400), pszMsg, pszMore));
    else
        LogRelMax(256, ("VirtioGpu: renderer %s: %.*s%s\n", iLevel == VGPU_VIRGL_LOG_LEVEL_WARNING ? "warning" : "error",
                        (int)RT_MIN(cch, 400), pszMsg, pszMore));
}


/**
 * Initializes the library and fetches the capsets.  Renderer thread.
 *
 * @returns VBox status code.  On failure after a successful
 *          virgl_renderer_init(), fInitialized stays set and the caller
 *          has to clean up.
 * @param   pThis       The renderer instance.
 */
static int vgpuR3RendererExecInit(PVGPURENDERER pThis)
{
    uint32_t fFlags = VGPU_VIRGL_RENDERER_VENUS
                    | VGPU_VIRGL_RENDERER_RENDER_SERVER
                    | VGPU_VIRGL_RENDERER_THREAD_SYNC
                    | VGPU_VIRGL_RENDERER_ASYNC_FENCE_CB;

    /* OpenGL needs contexts from us.  Without them (or without the entry points) only Venus is offered. */
    bool fVirgl = false;
#ifdef RT_OS_WINDOWS
    if (pThis->fWantVirgl && pThis->fHaveGlSymbols)
    {
        int rc = vgpuR3RendererWglInit(pThis);
        if (RT_SUCCESS(rc))
        {
            pThis->Callbacks.pfnCreateGlContext  = vgpuR3VirglCreateGlContext;
            pThis->Callbacks.pfnDestroyGlContext = vgpuR3VirglDestroyGlContext;
            pThis->Callbacks.pfnMakeCurrent      = vgpuR3VirglMakeCurrent;
            fVirgl = true;
        }
        else
            LogRel(("VirtioGpu: No host OpenGL contexts (%Rrc), offering Vulkan only\n", rc));
    }
    else if (pThis->fWantVirgl)
        LogRel(("VirtioGpu: The renderer library has no OpenGL entry points, offering Vulkan only\n"));
#else
    if (pThis->fWantVirgl)
        LogRel(("VirtioGpu: OpenGL (virgl) is not implemented for this host, offering Vulkan only\n"));
#endif
    if (!fVirgl)
        fFlags |= VGPU_VIRGL_RENDERER_NO_VIRGL;

    /* The library's warnings and errors go to the release log: they say why it rejects a command
       stream or gives up on a context, which otherwise only shows as a failed submit or a black guest. */
    if (pThis->Fns.pfnSetLogCallback)
        pThis->Fns.pfnSetLogCallback(vgpuR3RendererLibLog, NULL /*pvUser*/, NULL /*pfnFreeUser*/);

    int iRc = pThis->Fns.pfnInit(pThis, (int)fFlags, &pThis->Callbacks);
    if (iRc != 0)
    {
        LogRel(("VirtioGpu: virgl_renderer_init(%#RX32) failed: %d\n", fFlags, iRc));
#ifdef RT_OS_WINDOWS
        if (fVirgl)
            vgpuR3RendererWglTerm(pThis);
#endif
        return VERR_NOT_SUPPORTED;
    }
    pThis->fInitialized = true;

    /* Queried after init (the library wants proxy_initialized to be set for this). */
    int rc = vgpuR3RendererFetchCapset(pThis, 0, VGPU_VIRGL_CAPSET_VENUS, sizeof(VGPUVENUSCAPSETHDR));
    if (RT_FAILURE(rc))
        return rc;
    VGPUVENUSCAPSETHDR const *pHdr = (VGPUVENUSCAPSETHDR const *)pThis->aCapsets[0].pbCapset;
    if (!pHdr->uWireFormatVersion || !pHdr->uVkXmlVersion)
    {
        LogRel(("VirtioGpu: The Venus capset is empty (wire format %RU32, vk.xml %#RX32); is this the right library?\n",
                pHdr->uWireFormatVersion, pHdr->uVkXmlVersion));
        RTMemFree(pThis->aCapsets[0].pbCapset);
        pThis->aCapsets[0].pbCapset = NULL;
        return VERR_NOT_SUPPORTED;
    }
    LogRel(("VirtioGpu: Venus capset v%RU32, %RU32 bytes: wire format %RU32, vk.xml %#RX32, serialization spec %RU32,"
            " protocol spec %RU32, blob id 0 %RU32\n", pThis->aCapsets[0].uMaxVersion, pThis->aCapsets[0].cbCapset,
            pHdr->uWireFormatVersion, pHdr->uVkXmlVersion, pHdr->uVkExtCmdSerializationSpecVersion,
            pHdr->uVkMesaVenusProtocolSpecVersion, pHdr->fSupportsBlobId0));

    /* The OpenGL capsets.  Both or none: guests pick virgl2 when it is there and fall back to virgl. */
    if (fVirgl)
    {
        rc = vgpuR3RendererFetchCapset(pThis, 1, VGPU_VIRGL_CAPSET_VIRGL, sizeof(uint32_t));
        if (RT_SUCCESS(rc))
            rc = vgpuR3RendererFetchCapset(pThis, 2, VGPU_VIRGL_CAPSET_VIRGL2, sizeof(uint32_t));
        if (RT_SUCCESS(rc))
        {
            pThis->fVirgl = true;
            LogRel(("VirtioGpu: OpenGL (virgl) capsets: v%RU32 %RU32 bytes, v%RU32 %RU32 bytes\n",
                    pThis->aCapsets[1].uMaxVersion, pThis->aCapsets[1].cbCapset,
                    pThis->aCapsets[2].uMaxVersion, pThis->aCapsets[2].cbCapset));
        }
        else
        {
            /* The library is initialized with OpenGL but the device will not offer it. */
            LogRel(("VirtioGpu: No usable OpenGL capsets (%Rrc), offering Vulkan only\n", rc));
            for (unsigned i = 1; i < RT_ELEMENTS(pThis->aCapsets); i++)
            {
                RTMemFree(pThis->aCapsets[i].pbCapset);
                pThis->aCapsets[i].pbCapset = NULL;
            }
        }
    }
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

    /* The library remembers which of its OpenGL contexts is current, but some of its calls make
       another one current without updating that: creating a context leaves the new one current.  The
       commands of the context it believes to be current then run in the wrong OpenGL context, where
       its vertex arrays and framebuffers do not exist (GL_INVALID_OPERATION), and the library
       refuses everything the guest sends for it from then on.  Embedders are expected to reset this
       before every call, as QEMU does: then the next call for a context really makes it current. */
    if (pThis->fVirgl)
        pThis->Fns.pfnForceCtx0();

    int iRc = 0;
    switch (pReq->enmType)
    {
        case kVGpuRenderReq_Cleanup:
            pThis->Fns.pfnCleanup(pThis);
            pThis->fInitialized = false;
#ifdef RT_OS_WINDOWS
            /* The library destroyed its contexts (and joined its fence thread). */
            vgpuR3RendererWglTerm(pThis);
#endif
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

        case kVGpuRenderReq_ResourceCreate3d:
            iRc = pThis->fVirgl ? pThis->Fns.pfnResourceCreate(&pReq->u.Create3d, NULL, 0) : -VGPU_VIRGL_EINVAL;
            break;

        case kVGpuRenderReq_ResourceAttachIov:
            iRc = pThis->fVirgl
                ? pThis->Fns.pfnResourceAttachIov((int)pReq->u.AttachIov.idResource, pReq->u.AttachIov.paIov,
                                                  (int)pReq->u.AttachIov.cIov)
                : -VGPU_VIRGL_EINVAL;
            break;

        case kVGpuRenderReq_ResourceDetachIov:
            if (pThis->fVirgl)
            {
                PVGPURENDERERIOV paIov = NULL;
                int              cIov  = 0;
                pThis->Fns.pfnResourceDetachIov((int)pReq->u.Ids.idResource, &paIov, &cIov);
            }
            break;

        case kVGpuRenderReq_Transfer:
            if (!pThis->fVirgl)
                iRc = -VGPU_VIRGL_EINVAL;
            else if (pReq->u.Transfer.fToHost)
                iRc = pThis->Fns.pfnTransferWriteIov(pReq->u.Transfer.idResource, pReq->u.Transfer.idCtx,
                                                     (int)pReq->u.Transfer.uLevel, pReq->u.Transfer.cbStride,
                                                     pReq->u.Transfer.cbLayerStride, &pReq->u.Transfer.Box,
                                                     pReq->u.Transfer.off, NULL, 0);
            else
                iRc = pThis->Fns.pfnTransferReadIov(pReq->u.Transfer.idResource, pReq->u.Transfer.idCtx,
                                                    pReq->u.Transfer.uLevel, pReq->u.Transfer.cbStride,
                                                    pReq->u.Transfer.cbLayerStride, &pReq->u.Transfer.Box,
                                                    pReq->u.Transfer.off, NULL, 0);
            break;

        case kVGpuRenderReq_ResourceRead:
            if (!pThis->fVirgl)
                iRc = -VGPU_VIRGL_EINVAL;
            else
            {
                VGPURENDERERIOV Iov = { pReq->u.Read.pvDst, pReq->u.Read.cbDst };
                iRc = pThis->Fns.pfnTransferReadIov(pReq->u.Read.idResource, 0 /*idCtx*/, 0 /*uLevel*/, pReq->u.Read.cbStride,
                                                    0 /*cbLayerStride*/, &pReq->u.Read.Box, 0 /*off*/, &Iov, 1);
            }
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

    uint64_t msLastReq = 0;
    for (;;)
    {
        PVGPURENDERREQ pReq = ASMAtomicXchgPtrT(&pThis->pReqPending, NULL, PVGPURENDERREQ);
        if (pReq)
        {
            vgpuR3RendererExec(pThis, pReq);
            int rc = RTSemEventSignal(pThis->hEvtDone);
            AssertLogRelRC(rc);
            msLastReq = RTTimeMilliTS();
            continue;
        }
        if (ASMAtomicReadBool(&pThis->fTerminate))
            break;

        /* With OpenGL the library wants to be polled, see VGPURENDERER_GL_POLL_BUSY_MS. */
        RTMSINTERVAL cMsWait = RT_INDEFINITE_WAIT;
        if (pThis->fVirgl && pThis->fInitialized)
            cMsWait = RTTimeMilliTS() - msLastReq < VGPURENDERER_GL_POLL_BUSY_WINDOW_MS
                    ? VGPURENDERER_GL_POLL_BUSY_MS : VGPURENDERER_GL_POLL_IDLE_MS;
        int rc = RTSemEventWait(pThis->hEvtReq, cMsWait);
        if (rc == VERR_TIMEOUT)
        {
            if (pThis->fVirgl && pThis->fInitialized)
                pThis->Fns.pfnPoll();
            continue;
        }
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

    /* The thread works on a heap copy, so that a call stuck in the library can be
       abandoned without the thread writing to our stack when it returns (or never).
       The data the request points to (command stream, name) is copied as well, as
       the caller frees or reuses its own copy once we give up on the call.  After a
       timeout both copies are leaked, as the library may still be reading them. */
    PVGPURENDERREQ pReqHeap = (PVGPURENDERREQ)RTMemDup(pReq, sizeof(*pReq));
    if (!pReqHeap)
    {
        RTCritSectLeave(&pThis->CritSectCall);
        return VERR_NO_MEMORY;
    }
    void *pvPayload = NULL;
    if (pReq->enmType == kVGpuRenderReq_Submit && pReq->u.Submit.cDWords)
    {
        pvPayload = RTMemDup(pReq->u.Submit.pvCmds, (size_t)pReq->u.Submit.cDWords * 4); /* 8 byte aligned, the library wants 4. */
        pReqHeap->u.Submit.pvCmds = pvPayload;
    }
    else if (pReq->enmType == kVGpuRenderReq_ContextCreate)
    {
        pvPayload = RTMemAlloc((size_t)pReq->u.ContextCreate.cchName + 1);
        if (pvPayload)
        {
            memcpy(pvPayload, pReq->u.ContextCreate.pachName, pReq->u.ContextCreate.cchName);
            ((char *)pvPayload)[pReq->u.ContextCreate.cchName] = '\0';
        }
        pReqHeap->u.ContextCreate.pachName = (const char *)pvPayload;
    }
    else if (pReq->enmType == kVGpuRenderReq_ResourceRead)
    {
        /* The library writes into a heap block, copied to the caller's buffer when the call is done. */
        pvPayload = RTMemAllocZ(pReq->u.Read.cbDst);
        pReqHeap->u.Read.pvDst = pvPayload;
    }
    if (   !pvPayload
        && (   (pReq->enmType == kVGpuRenderReq_Submit && pReq->u.Submit.cDWords)
            || pReq->enmType == kVGpuRenderReq_ContextCreate
            || pReq->enmType == kVGpuRenderReq_ResourceRead))
    {
        RTMemFree(pReqHeap);
        RTCritSectLeave(&pThis->CritSectCall);
        return VERR_NO_MEMORY;
    }
    ASMAtomicWritePtr(&pThis->pReqPending, pReqHeap);
    int rc = RTSemEventSignal(pThis->hEvtReq);
    if (RT_FAILURE(rc) && ASMAtomicCmpXchgPtr(&pThis->pReqPending, (PVGPURENDERREQ)NULL, pReqHeap))
    {
        /* The thread never saw the request. */
        AssertLogRelMsgFailed(("%Rrc\n", rc));
        RTMemFree(pvPayload);
        RTMemFree(pReqHeap);
        RTCritSectLeave(&pThis->CritSectCall);
        return rc;
    }

    uint64_t const msDoneDeadline = RTTimeMilliTS() + VGPURENDERER_CALL_TIMEOUT_MS;
    for (;;)
    {
        uint64_t const msNow = RTTimeMilliTS();
        if (msNow >= msDoneDeadline)
        {
            /* Leak the request and its data: the thread may still use them. */
            ASMAtomicWriteBool(&pThis->fDead, true);
            LogRel(("VirtioGpu: A call into the renderer library (request %d) did not finish in %RU32 ms, giving up on it\n",
                    (int)pReqHeap->enmType, (uint32_t)VGPURENDERER_CALL_TIMEOUT_MS));
            RTCritSectLeave(&pThis->CritSectCall);
            return VERR_TIMEOUT;
        }
        rc = RTSemEventWait(pThis->hEvtDone, (RTMSINTERVAL)RT_MIN(msDoneDeadline - msNow, 1000));
        if (RT_SUCCESS(rc))
            break;
        if (rc != VERR_TIMEOUT)
        {
            AssertLogRelMsgFailed(("%Rrc\n", rc));
            RTThreadSleep(10);
        }
    }
    /* Hand the results back, but keep the caller's own pointers. */
    void const * const pvOrgCmds = pReq->enmType == kVGpuRenderReq_Submit ? pReq->u.Submit.pvCmds : NULL;
    const char * const pachOrgName = pReq->enmType == kVGpuRenderReq_ContextCreate ? pReq->u.ContextCreate.pachName : NULL;
    void * const       pvOrgDst    = pReq->enmType == kVGpuRenderReq_ResourceRead ? pReq->u.Read.pvDst : NULL;
    *pReq = *pReqHeap;
    if (pReq->enmType == kVGpuRenderReq_ResourceRead)
    {
        if (RT_SUCCESS(pReq->rc))
            memcpy(pvOrgDst, pvPayload, pReq->u.Read.cbDst);
        pReq->u.Read.pvDst = pvOrgDst;
    }
    if (pReq->enmType == kVGpuRenderReq_Submit)
        pReq->u.Submit.pvCmds = (void *)pvOrgCmds;
    else if (pReq->enmType == kVGpuRenderReq_ContextCreate)
        pReq->u.ContextCreate.pachName = pachOrgName;
    RTMemFree(pvPayload);
    RTMemFree(pReqHeap);
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
 * @param   fVirgl          Whether to offer OpenGL (virgl) as well as Vulkan (Venus).  Not getting
 *                          it is no failure, see vgpuR3RendererHasVirgl.
 * @param   pfnFence        The fence callback.
 * @param   pvFenceUser     User argument for pfnFence.
 * @param   ppRenderer      Where to return the renderer instance.
 * @param   pErrInfo        Where to return extended error information, optional.
 */
DECLHIDDEN(int) vgpuR3RendererCreate(const char *pszLibrary, bool fVirgl, PFNVGPURENDERERFENCE pfnFence, void *pvFenceUser,
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
    pThis->fWantVirgl                     = fVirgl;
#ifdef RT_OS_WINDOWS
    pThis->hLdrOpenGL                     = NIL_RTLDRMOD;
#endif
    pThis->Callbacks.iVersion             = VGPU_VIRGL_CALLBACKS_VERSION;
    pThis->Callbacks.pfnWriteFence        = vgpuR3VirglWriteFence;
    pThis->Callbacks.pfnWriteContextFence = vgpuR3VirglWriteContextFence;
    /* The DRM, server fd and EGL callbacks stay NULL (zeroed); the GL ones are set by init when OpenGL is used. */

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
        for (unsigned i = 0; i < RT_ELEMENTS(g_aVirglOptionalSymbols) && RT_SUCCESS(rc); i++)
            if (RT_FAILURE(RTLdrGetSymbol(pThis->hLdrMod, g_aVirglOptionalSymbols[i].pszName,
                                          (void **)((uint8_t *)&pThis->Fns + g_aVirglOptionalSymbols[i].offMember))))
                *(void **)((uint8_t *)&pThis->Fns + g_aVirglOptionalSymbols[i].offMember) = NULL;
        pThis->fHaveGlSymbols = RT_SUCCESS(rc);
        for (unsigned i = 0; i < RT_ELEMENTS(g_aVirglGlSymbols) && RT_SUCCESS(rc); i++)
            if (RT_FAILURE(RTLdrGetSymbol(pThis->hLdrMod, g_aVirglGlSymbols[i].pszName,
                                          (void **)((uint8_t *)&pThis->Fns + g_aVirglGlSymbols[i].offMember))))
                pThis->fHaveGlSymbols = false;
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
 * Limits the host memory all Venus contexts together can allocate.
 *
 * An allocation that would go over the limit fails in the guest with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY, as it does when the card is full, instead of
 * the guest taking the memory the host's own programs need.
 *
 * @returns VINF_SUCCESS, VERR_NOT_SUPPORTED if the library has no such limit.
 * @param   pRenderer   The renderer instance.
 * @param   cbLimit     The limit in bytes, 0 for none.
 * @note    The library only reads and writes atomic counters for this, so it
 *          needs no call through the renderer thread.
 */
DECLHIDDEN(int) vgpuR3RendererSetMemoryLimit(PVGPURENDERER pRenderer, uint64_t cbLimit)
{
    AssertPtrReturn(pRenderer, VERR_INVALID_POINTER);
    if (!pRenderer->Fns.pfnSetVkMemoryLimit)
        return VERR_NOT_SUPPORTED;
    pRenderer->Fns.pfnSetVkMemoryLimit(cbLimit);
    return VINF_SUCCESS;
}


/**
 * Gets the host memory the Venus contexts have allocated that counts against the limit.
 *
 * @returns Bytes, UINT64_MAX if the library cannot tell.
 * @param   pRenderer   The renderer instance.
 */
DECLHIDDEN(uint64_t) vgpuR3RendererGetMemoryUsed(PVGPURENDERER pRenderer)
{
    AssertPtrReturn(pRenderer, UINT64_MAX);
    return pRenderer->Fns.pfnGetVkMemoryUsed ? pRenderer->Fns.pfnGetVkMemoryUsed() : UINT64_MAX;
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

        /* The library keeps the log callback and stays loaded, and its threads may log after
           VBoxDD is gone.  Remove it after the cleanup, so that the cleanup is still logged,
           and before any of the returns below that leave the library running. */
        if (pThis->Fns.pfnSetLogCallback)
            pThis->Fns.pfnSetLogCallback(NULL /*pfnLog*/, NULL /*pvUser*/, NULL /*pfnFreeUser*/);

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
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aCapsets); i++)
        RTMemFree(pThis->aCapsets[i].pbCapset);
    pThis->u32Magic = ~VGPURENDERER_MAGIC;
    RTMemFree(pThis);
}


/**
 * Returns the cached capset.
 *
 * @returns VBox status code, VERR_NOT_FOUND for an unsupported capset id.
 * @param   pThis           The renderer instance.
 * @param   idCapset        The capset id: VIRTIOGPU_CAPSET_VENUS and, with OpenGL,
 *                          VIRTIOGPU_CAPSET_VIRGL and VIRTIOGPU_CAPSET_VIRGL2.
 * @param   puMaxVersion    Where to return the highest version.
 * @param   pcbCapset       Where to return the capset size.
 * @param   ppbCapset       Where to return the capset data (read only, valid
 *                          until the renderer is destroyed).  Optional.
 */
DECLHIDDEN(int) vgpuR3RendererQueryCapset(PVGPURENDERER pThis, uint32_t idCapset, uint32_t *puMaxVersion,
                                          uint32_t *pcbCapset, uint8_t const **ppbCapset)
{
    AssertReturn(pThis && pThis->u32Magic == VGPURENDERER_MAGIC, VERR_INVALID_HANDLE);
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aCapsets); i++)
        if (pThis->aCapsets[i].pbCapset && pThis->aCapsets[i].idCapset == idCapset)
        {
            *puMaxVersion = pThis->aCapsets[i].uMaxVersion;
            *pcbCapset    = pThis->aCapsets[i].cbCapset;
            if (ppbCapset)
                *ppbCapset = pThis->aCapsets[i].pbCapset;
            return VINF_SUCCESS;
        }
    return VERR_NOT_FOUND;
}


/**
 * Whether OpenGL (virgl) contexts and resources can be used.
 *
 * @returns true if the library was initialized with OpenGL.
 * @param   pThis       The renderer instance.
 */
DECLHIDDEN(bool) vgpuR3RendererHasVirgl(PVGPURENDERER pThis)
{
    AssertReturn(pThis && pThis->u32Magic == VGPURENDERER_MAGIC, false);
    return pThis->fVirgl;
}


/**
 * Checks whether the renderer stopped answering (a call timed out).
 *
 * The library may then still be working with memory it was given, so the caller
 * must not release such memory.
 *
 * @returns true if the renderer is dead.
 * @param   pThis       The renderer instance.
 */
DECLHIDDEN(bool) vgpuR3RendererIsDead(PVGPURENDERER pThis)
{
    AssertReturn(pThis && pThis->u32Magic == VGPURENDERER_MAGIC, true);
    return ASMAtomicReadBool(&pThis->fDead);
}


/**
 * Creates an OpenGL resource without a backing (virgl_renderer_resource_create).
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   pArgs       What to create.  Guest values, checked by the library.
 */
DECLHIDDEN(int) vgpuR3RendererResourceCreate3d(PVGPURENDERER pThis, PCVGPURENDERERCREATE3D pArgs)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType    = kVGpuRenderReq_ResourceCreate3d;
    Req.u.Create3d = *pArgs;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Creating OpenGL resource %RU32 (target %RU32, format %RU32, bind %#RX32, %RU32x%RU32x%RU32) failed: %d (%Rrc)\n",
                       pArgs->idResource, pArgs->uTarget, pArgs->uFormat, pArgs->fBind, pArgs->cx, pArgs->cy, pArgs->cDepth,
                       Req.iVirglRc, rc));
    return rc;
}


/**
 * Gives an OpenGL resource its guest memory (virgl_renderer_resource_attach_iov).
 *
 * The library reads and writes that memory whenever the guest asks for a
 * transfer, also from commands inside a command stream.
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 * @param   paIov       The pieces.  The library keeps this pointer: the array
 *                      and the memory it describes must stay valid until
 *                      vgpuR3RendererResourceDetachIov() or
 *                      vgpuR3RendererResourceUnref() returned.  If the renderer
 *                      died (a call timed out) they must never be freed.
 * @param   cIov        The number of pieces, at most INT32_MAX.
 */
DECLHIDDEN(int) vgpuR3RendererResourceAttachIov(PVGPURENDERER pThis, uint32_t idResource, PVGPURENDERERIOV paIov, uint32_t cIov)
{
    AssertReturn(cIov && cIov <= INT32_MAX, VERR_INVALID_PARAMETER);
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType                = kVGpuRenderReq_ResourceAttachIov;
    Req.u.AttachIov.idResource = idResource;
    Req.u.AttachIov.cIov       = cIov;
    Req.u.AttachIov.paIov      = paIov;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Attaching a backing to resource %RU32 failed: %d (%Rrc)\n", idResource, Req.iVirglRc, rc));
    return rc;
}


/**
 * Takes the guest memory away from an OpenGL resource again
 * (virgl_renderer_resource_detach_iov).
 *
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 */
DECLHIDDEN(void) vgpuR3RendererResourceDetachIov(PVGPURENDERER pThis, uint32_t idResource)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType          = kVGpuRenderReq_ResourceDetachIov;
    Req.u.Ids.idResource = idResource;
    vgpuR3RendererCall(pThis, &Req);
}


/**
 * Copies between an OpenGL resource and its guest memory
 * (virgl_renderer_transfer_write_iov / virgl_renderer_transfer_read_iov).
 *
 * @returns VBox status code.
 * @param   pThis           The renderer instance.
 * @param   fToHost         true: guest memory to host resource, false: the other way.
 * @param   idResource      The resource id.
 * @param   idCtx           The context id, 0 for none.
 * @param   uLevel          The mipmap level.
 * @param   cbStride        The row stride in the guest memory, 0 for the default.
 * @param   cbLayerStride   The layer stride in the guest memory, 0 for the default.
 * @param   pBox            The part of the resource.
 * @param   off             Where the data starts in the guest memory.
 * @note    All values come from the guest, the library checks them against
 *          the resource and the attached memory.
 */
DECLHIDDEN(int) vgpuR3RendererTransfer(PVGPURENDERER pThis, bool fToHost, uint32_t idResource, uint32_t idCtx, uint32_t uLevel,
                                       uint32_t cbStride, uint32_t cbLayerStride, PCVGPURENDERERBOX pBox, uint64_t off)
{
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType                  = kVGpuRenderReq_Transfer;
    Req.u.Transfer.fToHost       = fToHost;
    Req.u.Transfer.idResource    = idResource;
    Req.u.Transfer.idCtx         = idCtx;
    Req.u.Transfer.uLevel        = uLevel;
    Req.u.Transfer.cbStride      = cbStride;
    Req.u.Transfer.cbLayerStride = cbLayerStride;
    Req.u.Transfer.Box           = *pBox;
    Req.u.Transfer.off           = off;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        Log(("VirtioGpu: Transfer %s resource %RU32 failed: %d (%Rrc)\n", fToHost ? "to" : "from", idResource, Req.iVirglRc, rc));
    return rc;
}


/**
 * Reads pixels of an OpenGL resource into host memory, for the display.
 *
 * @returns VBox status code.
 * @param   pThis       The renderer instance.
 * @param   idResource  The resource id.
 * @param   x           The left edge.
 * @param   y           The top edge.
 * @param   cx          The width.
 * @param   cy          The height.
 * @param   cbStride    The row stride of the destination.
 * @param   pvDst       Where to store the pixels (in the format of the resource).
 * @param   cbDst       The size of the destination, at least cbStride * cy.
 */
DECLHIDDEN(int) vgpuR3RendererResourceRead(PVGPURENDERER pThis, uint32_t idResource, uint32_t x, uint32_t y,
                                           uint32_t cx, uint32_t cy, uint32_t cbStride, void *pvDst, size_t cbDst)
{
    AssertReturn(cx && cy && cbDst && (uint64_t)cbStride * cy <= cbDst, VERR_INVALID_PARAMETER);
    VGPURENDERREQ Req;
    RT_ZERO(Req);
    Req.enmType           = kVGpuRenderReq_ResourceRead;
    Req.u.Read.idResource = idResource;
    Req.u.Read.cbStride   = cbStride;
    Req.u.Read.Box.x      = x;
    Req.u.Read.Box.y      = y;
    Req.u.Read.Box.cx     = cx;
    Req.u.Read.Box.cy     = cy;
    Req.u.Read.Box.cDepth = 1;
    Req.u.Read.pvDst      = pvDst;
    Req.u.Read.cbDst      = cbDst;
    int rc = vgpuR3RendererCall(pThis, &Req);
    if (RT_FAILURE(rc))
        LogRelMax(16, ("VirtioGpu: Reading resource %RU32 (%RU32,%RU32 %RU32x%RU32) failed: %d (%Rrc)\n",
                       idResource, x, y, cx, cy, Req.iVirglRc, rc));
    return rc;
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

