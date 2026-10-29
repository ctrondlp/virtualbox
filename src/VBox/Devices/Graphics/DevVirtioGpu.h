/* $Id$ */
/** @file
 * VBox graphics devices - Virtio GPU, wire protocol definitions (virtio 1.x, device id 0x1050).
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

#ifndef VBOX_INCLUDED_SRC_Graphics_DevVirtioGpu_h
#define VBOX_INCLUDED_SRC_Graphics_DevVirtioGpu_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/types.h>
#include <iprt/assertcompile.h>


/** @name Virtio GPU PCI identification.
 * @{ */
/** The virtio PCI device id of the GPU device (0x1040 + device type 16). */
#define VIRTIOGPU_PCI_DEVICE_ID                 UINT16_C(0x1050)
/** The virtio device type of the GPU device. */
#define VIRTIOGPU_DEVICE_TYPE                   16
/** @} */

/** @name Virtio GPU feature bits (device specific, bits 0..23).
 * @{ */
/** Virgl 3D mode is supported. */
#define VIRTIOGPU_F_VIRGL                       RT_BIT_64(0)
/** EDID is supported. */
#define VIRTIOGPU_F_EDID                        RT_BIT_64(1)
/** Resource UUID assignment is supported. */
#define VIRTIOGPU_F_RESOURCE_UUID               RT_BIT_64(2)
/** Size based blob resources are supported. */
#define VIRTIOGPU_F_RESOURCE_BLOB               RT_BIT_64(3)
/** Multiple context types and synchronization timelines are supported. */
#define VIRTIOGPU_F_CONTEXT_INIT                RT_BIT_64(4)
/** @} */

/** The number of scanouts the device can have, the specification limit. */
#define VIRTIOGPU_MAX_SCANOUTS                  16

/** @name Virtqueue numbers.
 * @{ */
#define VIRTIOGPU_VIRTQ_CONTROL                 0
#define VIRTIOGPU_VIRTQ_CURSOR                  1
#define VIRTIOGPU_VIRTQ_COUNT                   2
/** @} */

/** @name Device configuration space events (VIRTIOGPUCONFIG::fEventsRead).
 * @{ */
/** The display configuration changed, the driver should fetch it with VIRTIOGPU_CMD_GET_DISPLAY_INFO. */
#define VIRTIOGPU_EVENT_DISPLAY                 RT_BIT_32(0)
/** @} */

/**
 * Device specific configuration (struct virtio_gpu_config).
 */
typedef struct VIRTIOGPUCONFIG
{
    /** Pending events, read only for the driver (VIRTIOGPU_EVENT_XXX). */
    uint32_t    fEventsRead;
    /** Write 1 bits to clear the corresponding events. */
    uint32_t    fEventsClear;
    /** The number of scanouts, 1..16. */
    uint32_t    cScanouts;
    /** The number of capability sets (0 without virgl or blob support). */
    uint32_t    cCapsets;
} VIRTIOGPUCONFIG;
AssertCompileSize(VIRTIOGPUCONFIG, 16);
typedef VIRTIOGPUCONFIG *PVIRTIOGPUCONFIG;


/** @name Command and response types (VIRTIOGPUCTRLHDR::uType).
 * @{ */
/* 2D commands. */
#define VIRTIOGPU_CMD_GET_DISPLAY_INFO          UINT32_C(0x0100)
#define VIRTIOGPU_CMD_RESOURCE_CREATE_2D        UINT32_C(0x0101)
#define VIRTIOGPU_CMD_RESOURCE_UNREF            UINT32_C(0x0102)
#define VIRTIOGPU_CMD_SET_SCANOUT               UINT32_C(0x0103)
#define VIRTIOGPU_CMD_RESOURCE_FLUSH            UINT32_C(0x0104)
#define VIRTIOGPU_CMD_TRANSFER_TO_HOST_2D       UINT32_C(0x0105)
#define VIRTIOGPU_CMD_RESOURCE_ATTACH_BACKING   UINT32_C(0x0106)
#define VIRTIOGPU_CMD_RESOURCE_DETACH_BACKING   UINT32_C(0x0107)
#define VIRTIOGPU_CMD_GET_CAPSET_INFO           UINT32_C(0x0108)
#define VIRTIOGPU_CMD_GET_CAPSET                UINT32_C(0x0109)
#define VIRTIOGPU_CMD_GET_EDID                  UINT32_C(0x010a)
#define VIRTIOGPU_CMD_RESOURCE_ASSIGN_UUID      UINT32_C(0x010b)
#define VIRTIOGPU_CMD_RESOURCE_CREATE_BLOB      UINT32_C(0x010c)
#define VIRTIOGPU_CMD_SET_SCANOUT_BLOB          UINT32_C(0x010d)
/* 3D commands. */
#define VIRTIOGPU_CMD_CTX_CREATE                UINT32_C(0x0200)
#define VIRTIOGPU_CMD_CTX_DESTROY               UINT32_C(0x0201)
#define VIRTIOGPU_CMD_CTX_ATTACH_RESOURCE       UINT32_C(0x0202)
#define VIRTIOGPU_CMD_CTX_DETACH_RESOURCE       UINT32_C(0x0203)
#define VIRTIOGPU_CMD_RESOURCE_CREATE_3D        UINT32_C(0x0204)
#define VIRTIOGPU_CMD_TRANSFER_TO_HOST_3D       UINT32_C(0x0205)
#define VIRTIOGPU_CMD_TRANSFER_FROM_HOST_3D     UINT32_C(0x0206)
#define VIRTIOGPU_CMD_SUBMIT_3D                 UINT32_C(0x0207)
#define VIRTIOGPU_CMD_RESOURCE_MAP_BLOB         UINT32_C(0x0208)
#define VIRTIOGPU_CMD_RESOURCE_UNMAP_BLOB       UINT32_C(0x0209)
/* Cursor commands (cursor queue). */
#define VIRTIOGPU_CMD_UPDATE_CURSOR             UINT32_C(0x0300)
#define VIRTIOGPU_CMD_MOVE_CURSOR               UINT32_C(0x0301)
/* Success responses. */
#define VIRTIOGPU_RESP_OK_NODATA                UINT32_C(0x1100)
#define VIRTIOGPU_RESP_OK_DISPLAY_INFO          UINT32_C(0x1101)
#define VIRTIOGPU_RESP_OK_CAPSET_INFO           UINT32_C(0x1102)
#define VIRTIOGPU_RESP_OK_CAPSET                UINT32_C(0x1103)
#define VIRTIOGPU_RESP_OK_EDID                  UINT32_C(0x1104)
#define VIRTIOGPU_RESP_OK_RESOURCE_UUID         UINT32_C(0x1105)
#define VIRTIOGPU_RESP_OK_MAP_INFO              UINT32_C(0x1106)
/* Error responses. */
#define VIRTIOGPU_RESP_ERR_UNSPEC               UINT32_C(0x1200)
#define VIRTIOGPU_RESP_ERR_OUT_OF_MEMORY        UINT32_C(0x1201)
#define VIRTIOGPU_RESP_ERR_INVALID_SCANOUT_ID   UINT32_C(0x1202)
#define VIRTIOGPU_RESP_ERR_INVALID_RESOURCE_ID  UINT32_C(0x1203)
#define VIRTIOGPU_RESP_ERR_INVALID_CONTEXT_ID   UINT32_C(0x1204)
#define VIRTIOGPU_RESP_ERR_INVALID_PARAMETER    UINT32_C(0x1205)
/** @} */

/** @name VIRTIOGPUCTRLHDR::fFlags
 * @{ */
/** The command carries a fence: the response must not be sent before the command completed
 * and must echo uFenceId. */
#define VIRTIOGPU_FLAG_FENCE                    RT_BIT_32(0)
/** uRingIdx is valid (VIRTIOGPU_F_CONTEXT_INIT only). */
#define VIRTIOGPU_FLAG_INFO_RING_IDX            RT_BIT_32(1)
/** @} */

/**
 * Header of every command and response (struct virtio_gpu_ctrl_hdr).
 */
typedef struct VIRTIOGPUCTRLHDR
{
    /** Command or response type (VIRTIOGPU_CMD_XXX / VIRTIOGPU_RESP_XXX). */
    uint32_t    uType;
    /** VIRTIOGPU_FLAG_XXX. */
    uint32_t    fFlags;
    /** Fence id, valid with VIRTIOGPU_FLAG_FENCE. */
    uint64_t    uFenceId;
    /** 3D context id (0 for 2D commands). */
    uint32_t    uCtxId;
    /** Ring index, valid with VIRTIOGPU_FLAG_INFO_RING_IDX. */
    uint8_t     uRingIdx;
    uint8_t     abPadding[3];
} VIRTIOGPUCTRLHDR;
AssertCompileSize(VIRTIOGPUCTRLHDR, 24);
typedef VIRTIOGPUCTRLHDR *PVIRTIOGPUCTRLHDR;
typedef const VIRTIOGPUCTRLHDR *PCVIRTIOGPUCTRLHDR;

/** A rectangle (struct virtio_gpu_rect). */
typedef struct VIRTIOGPURECT
{
    uint32_t    x;
    uint32_t    y;
    uint32_t    cx;
    uint32_t    cy;
} VIRTIOGPURECT;
AssertCompileSize(VIRTIOGPURECT, 16);
typedef VIRTIOGPURECT *PVIRTIOGPURECT;
typedef const VIRTIOGPURECT *PCVIRTIOGPURECT;

/** VIRTIOGPU_CMD_GET_DISPLAY_INFO response (struct virtio_gpu_resp_display_info). */
typedef struct VIRTIOGPURESPDISPLAYINFO
{
    VIRTIOGPUCTRLHDR    Hdr;
    struct
    {
        VIRTIOGPURECT   r;
        uint32_t        fEnabled;
        uint32_t        fFlags;
    } aPModes[VIRTIOGPU_MAX_SCANOUTS];
} VIRTIOGPURESPDISPLAYINFO;
AssertCompileSize(VIRTIOGPURESPDISPLAYINFO, 24 + 16 * 24);
typedef VIRTIOGPURESPDISPLAYINFO *PVIRTIOGPURESPDISPLAYINFO;

/** @name Resource formats (enum virtio_gpu_formats).
 * @{ */
#define VIRTIOGPU_FORMAT_B8G8R8A8_UNORM         1
#define VIRTIOGPU_FORMAT_B8G8R8X8_UNORM         2
#define VIRTIOGPU_FORMAT_A8R8G8B8_UNORM         3
#define VIRTIOGPU_FORMAT_X8R8G8B8_UNORM         4
#define VIRTIOGPU_FORMAT_R8G8B8A8_UNORM         67
#define VIRTIOGPU_FORMAT_X8B8G8R8_UNORM         68
#define VIRTIOGPU_FORMAT_A8B8G8R8_UNORM         121
#define VIRTIOGPU_FORMAT_R8G8B8X8_UNORM         134
/** @} */

/** VIRTIOGPU_CMD_RESOURCE_CREATE_2D (struct virtio_gpu_resource_create_2d). */
typedef struct VIRTIOGPURESOURCECREATE2D
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            idResource;
    uint32_t            uFormat;
    uint32_t            cx;
    uint32_t            cy;
} VIRTIOGPURESOURCECREATE2D;
AssertCompileSize(VIRTIOGPURESOURCECREATE2D, 40);
typedef const VIRTIOGPURESOURCECREATE2D *PCVIRTIOGPURESOURCECREATE2D;

/** VIRTIOGPU_CMD_RESOURCE_UNREF (struct virtio_gpu_resource_unref). */
typedef struct VIRTIOGPURESOURCEUNREF
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            idResource;
    uint32_t            uPadding;
} VIRTIOGPURESOURCEUNREF;
AssertCompileSize(VIRTIOGPURESOURCEUNREF, 32);
typedef const VIRTIOGPURESOURCEUNREF *PCVIRTIOGPURESOURCEUNREF;

/** VIRTIOGPU_CMD_SET_SCANOUT (struct virtio_gpu_set_scanout). */
typedef struct VIRTIOGPUSETSCANOUT
{
    VIRTIOGPUCTRLHDR    Hdr;
    VIRTIOGPURECT       r;
    uint32_t            idScanout;
    uint32_t            idResource;
} VIRTIOGPUSETSCANOUT;
AssertCompileSize(VIRTIOGPUSETSCANOUT, 48);
typedef const VIRTIOGPUSETSCANOUT *PCVIRTIOGPUSETSCANOUT;

/** VIRTIOGPU_CMD_RESOURCE_FLUSH (struct virtio_gpu_resource_flush). */
typedef struct VIRTIOGPURESOURCEFLUSH
{
    VIRTIOGPUCTRLHDR    Hdr;
    VIRTIOGPURECT       r;
    uint32_t            idResource;
    uint32_t            uPadding;
} VIRTIOGPURESOURCEFLUSH;
AssertCompileSize(VIRTIOGPURESOURCEFLUSH, 48);
typedef const VIRTIOGPURESOURCEFLUSH *PCVIRTIOGPURESOURCEFLUSH;

/** VIRTIOGPU_CMD_TRANSFER_TO_HOST_2D (struct virtio_gpu_transfer_to_host_2d). */
typedef struct VIRTIOGPUTRANSFERTOHOST2D
{
    VIRTIOGPUCTRLHDR    Hdr;
    VIRTIOGPURECT       r;
    /** Byte offset into the resource backing storage where the rectangle's data starts. */
    uint64_t            offBacking;
    uint32_t            idResource;
    uint32_t            uPadding;
} VIRTIOGPUTRANSFERTOHOST2D;
AssertCompileSize(VIRTIOGPUTRANSFERTOHOST2D, 56);
typedef const VIRTIOGPUTRANSFERTOHOST2D *PCVIRTIOGPUTRANSFERTOHOST2D;

/** VIRTIOGPU_CMD_RESOURCE_ATTACH_BACKING (struct virtio_gpu_resource_attach_backing), followed by cEntries VIRTIOGPUMEMENTRY. */
typedef struct VIRTIOGPURESOURCEATTACHBACKING
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            idResource;
    uint32_t            cEntries;
} VIRTIOGPURESOURCEATTACHBACKING;
AssertCompileSize(VIRTIOGPURESOURCEATTACHBACKING, 32);
typedef const VIRTIOGPURESOURCEATTACHBACKING *PCVIRTIOGPURESOURCEATTACHBACKING;

/** One guest memory range of a resource backing store (struct virtio_gpu_mem_entry). */
typedef struct VIRTIOGPUMEMENTRY
{
    uint64_t            GCPhysAddr;
    uint32_t            cb;
    uint32_t            uPadding;
} VIRTIOGPUMEMENTRY;
AssertCompileSize(VIRTIOGPUMEMENTRY, 16);
typedef const VIRTIOGPUMEMENTRY *PCVIRTIOGPUMEMENTRY;

/** VIRTIOGPU_CMD_RESOURCE_DETACH_BACKING (struct virtio_gpu_resource_detach_backing). */
typedef struct VIRTIOGPURESOURCEDETACHBACKING
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            idResource;
    uint32_t            uPadding;
} VIRTIOGPURESOURCEDETACHBACKING;
AssertCompileSize(VIRTIOGPURESOURCEDETACHBACKING, 32);
typedef const VIRTIOGPURESOURCEDETACHBACKING *PCVIRTIOGPURESOURCEDETACHBACKING;

/** VIRTIOGPU_CMD_GET_EDID (struct virtio_gpu_get_edid). */
typedef struct VIRTIOGPUGETEDID
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            idScanout;
    uint32_t            uPadding;
} VIRTIOGPUGETEDID;
AssertCompileSize(VIRTIOGPUGETEDID, 32);
typedef const VIRTIOGPUGETEDID *PCVIRTIOGPUGETEDID;

/** VIRTIOGPU_CMD_GET_EDID response (struct virtio_gpu_resp_edid). */
typedef struct VIRTIOGPURESPEDID
{
    VIRTIOGPUCTRLHDR    Hdr;
    uint32_t            cbEdid;
    uint32_t            uPadding;
    uint8_t             abEdid[1024];
} VIRTIOGPURESPEDID;
AssertCompileSize(VIRTIOGPURESPEDID, 24 + 8 + 1024);
typedef VIRTIOGPURESPEDID *PVIRTIOGPURESPEDID;

/** VIRTIOGPU_CMD_UPDATE_CURSOR / VIRTIOGPU_CMD_MOVE_CURSOR (struct virtio_gpu_update_cursor). */
typedef struct VIRTIOGPUUPDATECURSOR
{
    VIRTIOGPUCTRLHDR    Hdr;
    /** Scanout id and position. */
    uint32_t            idScanout;
    uint32_t            x;
    uint32_t            y;
    uint32_t            uPadding0;
    /** The cursor resource (VIRTIOGPU_CMD_UPDATE_CURSOR) and its hotspot. */
    uint32_t            idResource;
    uint32_t            xHot;
    uint32_t            yHot;
    uint32_t            uPadding1;
} VIRTIOGPUUPDATECURSOR;
AssertCompileSize(VIRTIOGPUUPDATECURSOR, 56);
typedef const VIRTIOGPUUPDATECURSOR *PCVIRTIOGPUUPDATECURSOR;

#endif /* !VBOX_INCLUDED_SRC_Graphics_DevVirtioGpu_h */

