/* $Id$ */
/** @file
 * VBox graphics devices - Virtio GPU, host 3D renderer (virglrenderer) wrapper interface.
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

#ifndef VBOX_INCLUDED_SRC_Graphics_DevVirtioGpuRenderer_h
#define VBOX_INCLUDED_SRC_Graphics_DevVirtioGpuRenderer_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/types.h>


/*
 * This is the only interface the virtio GPU device uses to reach the host
 * renderer library.  The device never sees virglrenderer types: the library is
 * loaded at run time and every call is marshalled to one dedicated renderer
 * thread inside DevVirtioGpuRenderer.cpp, so all functions below may be called
 * from any thread (they are serialized and block until the renderer thread
 * finished the call).  They must not be called from the fence callback.
 */

/** Opaque renderer instance. */
typedef struct VGPURENDERER *PVGPURENDERER;

/**
 * Fence callback, called when a context fence created with
 * vgpuR3RendererContextFence() (or a later one on the same ring) retired.
 *
 * @param   pvUser      The user argument given to vgpuR3RendererCreate().
 * @param   idCtx       The context id.
 * @param   uRingIdx    The ring index (comes from the library, validate it).
 * @param   uFenceId    The fence id.
 *
 * @note    Called on threads owned by the renderer library, possibly while
 *          a renderer call is in progress on the renderer thread.  It must
 *          only queue work and wake somebody up: no renderer calls, no
 *          guest memory access, no blocking on locks held across renderer
 *          calls.
 */
typedef DECLCALLBACKTYPE(void, FNVGPURENDERERFENCE,(void *pvUser, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId));
/** Pointer to a fence callback. */
typedef FNVGPURENDERERFENCE *PFNVGPURENDERERFENCE;

DECLHIDDEN(int)  vgpuR3RendererCreate(const char *pszLibrary, PFNVGPURENDERERFENCE pfnFence, void *pvFenceUser,
                                      PVGPURENDERER *ppRenderer, PRTERRINFO pErrInfo);
DECLHIDDEN(void) vgpuR3RendererDestroy(PVGPURENDERER pRenderer, bool fCleanup);
DECLHIDDEN(int)  vgpuR3RendererQueryCapset(PVGPURENDERER pRenderer, uint32_t idCapset, uint32_t *puMaxVersion,
                                           uint32_t *pcbCapset, uint8_t const **ppbCapset);
DECLHIDDEN(int)  vgpuR3RendererContextCreate(PVGPURENDERER pRenderer, uint32_t idCtx, uint32_t fContextInit,
                                             const char *pachName, uint32_t cchName);
DECLHIDDEN(void) vgpuR3RendererContextDestroy(PVGPURENDERER pRenderer, uint32_t idCtx);
DECLHIDDEN(int)  vgpuR3RendererSubmit(PVGPURENDERER pRenderer, uint32_t idCtx, void *pvCmds, uint32_t cbCmds);
DECLHIDDEN(int)  vgpuR3RendererContextFence(PVGPURENDERER pRenderer, uint32_t idCtx, uint32_t uRingIdx, uint64_t uFenceId);
DECLHIDDEN(int)  vgpuR3RendererBlobCreate(PVGPURENDERER pRenderer, uint32_t idCtx, uint32_t idResource, uint32_t uBlobMem,
                                          uint32_t fBlobFlags, uint64_t idBlob, uint64_t cbBlob);
DECLHIDDEN(void) vgpuR3RendererResourceUnref(PVGPURENDERER pRenderer, uint32_t idResource);
DECLHIDDEN(void) vgpuR3RendererContextAttach(PVGPURENDERER pRenderer, uint32_t idCtx, uint32_t idResource);
DECLHIDDEN(void) vgpuR3RendererContextDetach(PVGPURENDERER pRenderer, uint32_t idCtx, uint32_t idResource);
DECLHIDDEN(int)  vgpuR3RendererResourceMap(PVGPURENDERER pRenderer, uint32_t idResource, void **ppvHost, uint64_t *pcbHost,
                                           uint32_t *pfMapInfo);
DECLHIDDEN(int)  vgpuR3RendererResourceUnmap(PVGPURENDERER pRenderer, uint32_t idResource);

#endif /* !VBOX_INCLUDED_SRC_Graphics_DevVirtioGpuRenderer_h */

