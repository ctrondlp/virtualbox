/* $Id$ */
/** @file
 * NVMe host adapter driver to boot from disks.
 *
 * The controller is driven through the index/data I/O port pair in BAR2 so that
 * no MMIO access (and thus no switch out of real mode) is required, in the same
 * way as the AHCI driver does it. The driver presents the namespaces as SCSI
 * disks by translating the few SCSI commands scsi.c issues (INQUIRY, READ
 * CAPACITY(16), READ(16) and WRITE(16)) into NVMe commands, one outstanding
 * command at a time, polled.
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

#include <stdint.h>
#include <string.h>
#include "biosint.h"
#include "ebda.h"
#include "inlines.h"
#include "pciutil.h"
#include "scsi.h"

//#define DEBUG_NVME 1
#if DEBUG_NVME
# define DBG_NVME(...)        BX_INFO(__VA_ARGS__)
#else
# define DBG_NVME(...)
#endif

/** Register offsets. */
#define NVME_REG_CAP        0x00    /* 64-bit, accessed as two dwords. */
#define NVME_REG_CC         0x14
# define NVME_CC_EN             0x00000001UL
/** CSS=NVM, MPS=0 (4K), AMS=0, IOSQES=6 (64 bytes), IOCQES=4 (16 bytes). */
# define NVME_CC_CONFIG         0x00460000UL
#define NVME_REG_CSTS       0x1c
# define NVME_CSTS_RDY          0x00000001UL
# define NVME_CSTS_CFS          0x00000002UL
#define NVME_REG_AQA        0x24
#define NVME_REG_ASQ        0x28    /* 64-bit, accessed as two dwords. */
#define NVME_REG_ACQ        0x30    /* 64-bit, accessed as two dwords. */
/** Doorbell registers, valid for CAP.DSTRD = 0 which is checked on init. */
#define NVME_REG_SQ_TAIL(q) (0x1000 + (q) * 8)
#define NVME_REG_CQ_HEAD(q) (0x1000 + (q) * 8 + 4)

/** Index/data register pair offsets from the I/O base. */
#define NVME_REG_IDX        0
#define NVME_REG_DATA       4

#define NVME_CAP_HI_DSTRD_MASK  0x0000000fUL    /* CAP bits 35:32 */
#define NVME_CAP_HI_CSS_NVM     0x00000020UL    /* CAP bit 37 */

/** Admin command opcodes. */
#define NVME_ADM_DELETE_SQ  0x00
#define NVME_ADM_CREATE_SQ  0x01
#define NVME_ADM_CREATE_CQ  0x05
#define NVME_ADM_IDENTIFY   0x06
/** NVM command opcodes. */
#define NVME_NVM_WRITE      0x01
#define NVME_NVM_READ       0x02

/** Number of entries in each queue. */
#define NVME_QUEUE_ENTRIES  4
/** The I/O queue pair ID. */
#define NVME_IO_QID         1

/** The page size (CC.MPS = 0). */
#define NVME_PAGE_SIZE      4096
/** Pages used: Admin SQ, admin CQ, I/O SQ, I/O CQ and a scratch page. */
#define NVME_PAGES          5
/** Memory to allocate (KB), enough for NVME_PAGES pages after aligning to a page boundary. */
#define NVME_ALLOC_KB       ((NVME_PAGES + 1) * 4)

/** Sectors transferred by one command at most (PRP list fits into the scratch page). */
#define NVME_MAX_SECTORS    127

#define NVME_NO_DEVICE      0xffff

/** Offsets in the namespace identify data structure. */
#define NVME_NS_NSZE        0
#define NVME_NS_FLBAS       26
#define NVME_NS_LBAF        128

/** Offsets (in units of dwords) of the submission queue entry fields. */
#define NVME_SQE_DW0        0
#define NVME_SQE_NSID       1
#define NVME_SQE_PRP1_LO    6
#define NVME_SQE_PRP1_HI    7
#define NVME_SQE_PRP2_LO    8
#define NVME_SQE_PRP2_HI    9
#define NVME_SQE_CDW10      10
#define NVME_SQE_CDW11      11
#define NVME_SQE_CDW12      12
#define NVME_SQE_SIZE_DW    16
#define NVME_CQE_SIZE_DW    4

/**
 * NVMe controller data, lives in the 1KB block scsi.c allocates in the EBDA.
 */
typedef struct
{
    /** Base I/O port of the index/data register pair. */
    uint16_t        iobase;
    /** Segment of the first page of the 4K aligned queue memory. */
    uint16_t        seg_mem;
    /** Maximum time in BIOS ticks to wait for the controller to change its state. */
    uint16_t        cTicksTimeout;
    /** Next command ID. */
    uint16_t        cid;
    /** Admin queue tail. */
    uint8_t         admin_sq_tail;
    /** Admin completion queue head. */
    uint8_t         admin_cq_head;
    /** Admin completion queue phase tag. */
    uint8_t         admin_phase;
    /** I/O submission queue tail. */
    uint8_t         io_sq_tail;
    /** I/O completion queue head. */
    uint8_t         io_cq_head;
    /** I/O completion queue phase tag. */
    uint8_t         io_phase;
    /** Saved high bits of EAX. */
    uint16_t        saved_eax_hi;
} nvme_t;

ct_assert(sizeof(nvme_t) <= 1024);

/* Warning: Destroys high bits of EAX. */
uint32_t inpd(uint16_t port);
#pragma aux inpd =      \
    ".386"              \
    "in     eax, dx"    \
    "mov    dx, ax"     \
    "shr    eax, 16"    \
    "xchg   ax, dx"     \
    parm [dx] value [dx ax] modify nomemory;

/* Warning: Destroys high bits of EAX. */
void outpd(uint16_t port, uint32_t val);
#pragma aux outpd =     \
    ".386"              \
    "xchg   ax, cx"     \
    "shl    eax, 16"    \
    "mov    ax, cx"     \
    "out    dx, eax"    \
    parm [dx] [cx ax] modify nomemory;

/* Note: Reading high EAX bits destroys them - *must* be restored later. */
uint16_t eax_hi_rd(void);
#pragma aux eax_hi_rd = \
    ".386"              \
    "shr    eax, 16"    \
    value [ax] modify nomemory;

void eax_hi_wr(uint16_t);
#pragma aux eax_hi_wr = \
    ".386"              \
    "shl    eax, 16"    \
    parm [ax] modify nomemory;

#define NVME_WRITE_REG(iobase, reg, val)    \
    outpd((iobase) + NVME_REG_IDX, (reg));  \
    outpd((iobase) + NVME_REG_DATA, (val))

#define NVME_READ_REG(iobase, reg, val)     \
    outpd((iobase) + NVME_REG_IDX, (reg));  \
    (val) = inpd((iobase) + NVME_REG_DATA)

/**
 * Converts a segment:offset pair into a 32bit physical address.
 */
static uint32_t nvme_addr_to_phys(void __far *ptr)
{
    return ((uint32_t)FP_SEG(ptr) << 4) + FP_OFF(ptr);
}

/** Returns a pointer to the given page of the queue memory. */
#define NVME_PAGE(nvme, iPage)  (((nvme)->seg_mem + (iPage) * (NVME_PAGE_SIZE / 16)) :> 0)
#define NVME_PAGE_ADMIN_SQ  0
#define NVME_PAGE_ADMIN_CQ  1
#define NVME_PAGE_IO_SQ     2
#define NVME_PAGE_IO_CQ     3
#define NVME_PAGE_SCRATCH   4

/**
 * Waits for CSTS.RDY to get the given value.
 *
 * @returns 0 on success, 1 on timeout or fatal controller status.
 */
static int nvme_wait_ready(nvme_t __far *nvme, uint32_t fRdy)
{
    volatile uint32_t __far *ticks = MK_FP(0x40, 0x6C);
    uint32_t                 end_tick = *ticks + nvme->cTicksTimeout;
    uint32_t                 val;

    while (*ticks < end_tick)
    {
        NVME_READ_REG(nvme->iobase, NVME_REG_CSTS, val);
        if ((val & NVME_CSTS_RDY) == fRdy)
            return 0;
        if (val & NVME_CSTS_CFS)
            break;
    }
    return 1;
}

/**
 * Submits a command and waits for its completion, polled.
 *
 * @returns 0 on success, otherwise the NVMe status (non-zero) or 0xffff on timeout.
 * @param   nvme    The controller.
 * @param   fIo     Whether to use the I/O queue pair (otherwise the admin queue pair).
 * @param   opc     The opcode.
 * @param   nsid    Namespace ID.
 * @param   prp1    PRP entry 1 (32-bit physical address).
 * @param   prp2    PRP entry 2 (32-bit physical address).
 * @param   cdw10   Command dword 10.
 * @param   cdw11   Command dword 11.
 * @param   cdw12   Command dword 12.
 */
static uint16_t nvme_cmd(nvme_t __far *nvme, uint8_t fIo, uint8_t opc, uint32_t nsid, uint32_t prp1, uint32_t prp2,
                         uint32_t cdw10, uint32_t cdw11, uint32_t cdw12)
{
    volatile uint32_t __far *ticks = MK_FP(0x40, 0x6C);
    uint32_t                 end_tick;
    uint32_t __far          *sqe;
    volatile uint32_t __far *cqe;
    uint8_t                  qid     = fIo ? NVME_IO_QID : 0;
    uint8_t                  tail    = fIo ? nvme->io_sq_tail : nvme->admin_sq_tail;
    uint8_t                  head    = fIo ? nvme->io_cq_head : nvme->admin_cq_head;
    uint8_t                  phase   = fIo ? nvme->io_phase   : nvme->admin_phase;
    uint16_t                 status;

    sqe = (uint32_t __far *)(NVME_PAGE(nvme, fIo ? NVME_PAGE_IO_SQ : NVME_PAGE_ADMIN_SQ)) + tail * NVME_SQE_SIZE_DW;
    cqe = (volatile uint32_t __far *)(NVME_PAGE(nvme, fIo ? NVME_PAGE_IO_CQ : NVME_PAGE_ADMIN_CQ)) + head * NVME_CQE_SIZE_DW;

    _fmemset(sqe, 0, NVME_SQE_SIZE_DW * 4);
    sqe[NVME_SQE_DW0]     = opc | ((uint32_t)nvme->cid << 16);
    sqe[NVME_SQE_NSID]    = nsid;
    sqe[NVME_SQE_PRP1_LO] = prp1;
    sqe[NVME_SQE_PRP2_LO] = prp2;
    sqe[NVME_SQE_CDW10]   = cdw10;
    sqe[NVME_SQE_CDW11]   = cdw11;
    sqe[NVME_SQE_CDW12]   = cdw12;
    nvme->cid++;

    tail = (tail + 1) % NVME_QUEUE_ENTRIES;
    NVME_WRITE_REG(nvme->iobase, NVME_REG_SQ_TAIL(qid), tail);

    /* Wait for the completion queue entry to show up (phase tag is bit 16 of dword 3). */
    end_tick = *ticks + 5 * 19; /* About five seconds. */
    while (((cqe[3] >> 16) & 1) != phase)
    {
        if (*ticks >= end_tick)
        {
            DBG_NVME("NVMe: Timeout waiting for command %x completion\n", opc);
            return 0xffff;
        }
    }

    status = (uint16_t)(cqe[3] >> 17); /* Status field without the phase tag. */

    head = (head + 1) % NVME_QUEUE_ENTRIES;
    if (head == 0)
        phase ^= 1;
    NVME_WRITE_REG(nvme->iobase, NVME_REG_CQ_HEAD(qid), head);

    if (fIo)
    {
        nvme->io_sq_tail = tail;
        nvme->io_cq_head = head;
        nvme->io_phase   = phase;
    }
    else
    {
        nvme->admin_sq_tail = tail;
        nvme->admin_cq_head = head;
        nvme->admin_phase   = phase;
    }

    if (status)
        DBG_NVME("NVMe: Command %x failed with status %x\n", opc, status);
    return status;
}

/**
 * Identifies a namespace into the scratch page.
 *
 * @returns 0 on success, non-zero on failure or if the namespace isn't active.
 */
static int nvme_identify_ns(nvme_t __far *nvme, uint32_t nsid)
{
    uint8_t __far *scratch = NVME_PAGE(nvme, NVME_PAGE_SCRATCH);
    uint32_t       i;

    _fmemset(scratch, 0, NVME_PAGE_SIZE);
    if (nvme_cmd(nvme, 0 /*fIo*/, NVME_ADM_IDENTIFY, nsid, nvme_addr_to_phys(scratch), 0, 0 /*CNS=namespace*/, 0, 0) != 0)
        return 1;

    /* An unallocated namespace has a size of zero. */
    for (i = 0; i < 8; i++)
        if (scratch[NVME_NS_NSZE + i])
            return 0;
    return 1;
}

/**
 * Transfers sectors.
 *
 * @returns 0 on success, non-zero on failure.
 */
static int nvme_rw(nvme_t __far *nvme, uint8_t fWrite, uint32_t nsid, uint32_t lba_lo, uint32_t lba_hi,
                   uint32_t cSectors, uint8_t __far *buffer)
{
    uint32_t phys = nvme_addr_to_phys(buffer);

    while (cSectors)
    {
        uint32_t cChunk = cSectors > NVME_MAX_SECTORS ? NVME_MAX_SECTORS : cSectors;
        uint32_t cb     = cChunk * 512;
        uint32_t cbFirst = NVME_PAGE_SIZE - (phys & (NVME_PAGE_SIZE - 1));
        uint32_t prp2   = 0;
        uint16_t rc;

        if (cb > cbFirst)
        {
            uint32_t next = (phys & ~(uint32_t)(NVME_PAGE_SIZE - 1)) + NVME_PAGE_SIZE;
            uint32_t cbRem = cb - cbFirst;

            if (cbRem <= NVME_PAGE_SIZE)
                prp2 = next;
            else
            {
                /* Build a PRP list in the scratch page. */
                uint32_t __far *list = (uint32_t __far *)NVME_PAGE(nvme, NVME_PAGE_SCRATCH);
                uint16_t        cPages = (uint16_t)((cbRem + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE);
                uint16_t        i;

                for (i = 0; i < cPages; i++)
                {
                    list[i * 2]     = next + (uint32_t)i * NVME_PAGE_SIZE;
                    list[i * 2 + 1] = 0;
                }
                prp2 = nvme_addr_to_phys(list);
            }
        }

        rc = nvme_cmd(nvme, 1 /*fIo*/, fWrite ? NVME_NVM_WRITE : NVME_NVM_READ, nsid, phys, prp2,
                      lba_lo, lba_hi, cChunk - 1);
        if (rc)
            return 1;

        phys     += cb;
        cSectors -= cChunk;
        lba_lo   += cChunk;
        if (lba_lo < cChunk)
            lba_hi++;
    }
    return 0;
}

/**
 * Handles a SCSI command with data to the initiator, read direction.
 */
int nvme_scsi_cmd_data_in(void __far *pvHba, uint8_t idTgt, uint8_t __far *aCDB,
                          uint8_t cbCDB, uint8_t __far *buffer, uint32_t length)
{
    nvme_t __far   *nvme = (nvme_t __far *)pvHba;
    uint16_t        eax_hi = eax_hi_rd();
    uint32_t        nsid   = (uint32_t)idTgt + 1;
    int             rc     = 1;

    nvme->saved_eax_hi = eax_hi;
    switch (aCDB[0])
    {
        case SCSI_INQUIRY:
            if (nvme_identify_ns(nvme, nsid) == 0)
            {
                _fmemset(buffer, 0, length > 36 ? 36 : (uint16_t)length);
                if (length > 4)
                    buffer[4] = 31; /* Additional length. */
                rc = 0;     /* Peripheral device type 0 = disk. */
            }
            break;

        case SCSI_SERVICE_ACT:
            if ((aCDB[1] & 0x1f) == SCSI_READ_CAP_16)
            {
                if (nvme_identify_ns(nvme, nsid) == 0)
                {
                    uint8_t __far *id = NVME_PAGE(nvme, NVME_PAGE_SCRATCH);
                    uint8_t        iFmt = id[NVME_NS_FLBAS] & 0x0f;
                    uint8_t        cShift = id[NVME_NS_LBAF + iFmt * 4 + 2]; /* LBADS */
                    uint32_t       lo = *(uint32_t __far *)&id[NVME_NS_NSZE];
                    uint32_t       hi = *(uint32_t __far *)&id[NVME_NS_NSZE + 4];
                    uint32_t       cbSector = 1UL << cShift;
                    uint8_t        abRes[32];

                    /* Last LBA = NSZE - 1, big endian. */
                    if (lo-- == 0)
                        hi--;
                    _fmemset(abRes, 0, sizeof(abRes));
                    abRes[0] = (uint8_t)(hi >> 24);
                    abRes[1] = (uint8_t)(hi >> 16);
                    abRes[2] = (uint8_t)(hi >> 8);
                    abRes[3] = (uint8_t)hi;
                    abRes[4] = (uint8_t)(lo >> 24);
                    abRes[5] = (uint8_t)(lo >> 16);
                    abRes[6] = (uint8_t)(lo >> 8);
                    abRes[7] = (uint8_t)lo;
                    abRes[8]  = (uint8_t)(cbSector >> 24);
                    abRes[9]  = (uint8_t)(cbSector >> 16);
                    abRes[10] = (uint8_t)(cbSector >> 8);
                    abRes[11] = (uint8_t)cbSector;
                    _fmemcpy(buffer, abRes, length > sizeof(abRes) ? sizeof(abRes) : (uint16_t)length);
                    rc = 0;
                }
            }
            break;

        case SCSI_READ_16:
        {
            uint32_t lba_hi = ((uint32_t)aCDB[2] << 24) | ((uint32_t)aCDB[3] << 16) | ((uint32_t)aCDB[4] << 8) | aCDB[5];
            uint32_t lba_lo = ((uint32_t)aCDB[6] << 24) | ((uint32_t)aCDB[7] << 16) | ((uint32_t)aCDB[8] << 8) | aCDB[9];

            rc = nvme_rw(nvme, 0 /*fWrite*/, nsid, lba_lo, lba_hi, length / 512, buffer);
            break;
        }

        default:
            DBG_NVME("NVMe: Unsupported SCSI command %x\n", aCDB[0]);
    }

    eax_hi_wr(nvme->saved_eax_hi);
    return rc;
}

/**
 * Handles a SCSI command with data to the target, write direction.
 */
int nvme_scsi_cmd_data_out(void __far *pvHba, uint8_t idTgt, uint8_t __far *aCDB,
                           uint8_t cbCDB, uint8_t __far *buffer, uint32_t length)
{
    nvme_t __far   *nvme = (nvme_t __far *)pvHba;
    uint16_t        eax_hi = eax_hi_rd();
    int             rc     = 1;

    nvme->saved_eax_hi = eax_hi;
    if (aCDB[0] == SCSI_WRITE_16)
    {
        uint32_t lba_hi = ((uint32_t)aCDB[2] << 24) | ((uint32_t)aCDB[3] << 16) | ((uint32_t)aCDB[4] << 8) | aCDB[5];
        uint32_t lba_lo = ((uint32_t)aCDB[6] << 24) | ((uint32_t)aCDB[7] << 16) | ((uint32_t)aCDB[8] << 8) | aCDB[9];

        rc = nvme_rw(nvme, 1 /*fWrite*/, (uint32_t)idTgt + 1, lba_lo, lba_hi, length / 512, buffer);
    }
    else
        DBG_NVME("NVMe: Unsupported SCSI command %x\n", aCDB[0]);

    eax_hi_wr(nvme->saved_eax_hi);
    return rc;
}

/**
 * Resets and enables the controller and creates the I/O queue pair.
 *
 * @returns 0 on success, 1 on failure.
 */
static int nvme_hba_init(nvme_t __far *nvme)
{
    uint16_t iobase = nvme->iobase;
    uint32_t cap_lo, cap_hi, val;
    uint32_t phys;

    NVME_READ_REG(iobase, NVME_REG_CAP, cap_lo);
    NVME_READ_REG(iobase, NVME_REG_CAP + 4, cap_hi);
    DBG_NVME("NVMe: CAP=%08lx%08lx\n", cap_hi, cap_lo);

    if (   (cap_hi & NVME_CAP_HI_DSTRD_MASK) != 0       /* Doorbells must be packed. */
        || !(cap_hi & NVME_CAP_HI_CSS_NVM)
        || (uint16_t)cap_lo < NVME_QUEUE_ENTRIES - 1)   /* MQES */
        return 1;

    /* Timeout in 500ms units, 18.2 ticks per second, add one for good measure. */
    nvme->cTicksTimeout = (uint16_t)(((cap_lo >> 24) & 0xff) + 1) * 10;

    /* Disable the controller first. */
    NVME_READ_REG(iobase, NVME_REG_CC, val);
    if (val & NVME_CC_EN)
    {
        NVME_WRITE_REG(iobase, NVME_REG_CC, val & ~NVME_CC_EN);
        if (nvme_wait_ready(nvme, 0))
            return 1;
    }

    _fmemset(NVME_PAGE(nvme, NVME_PAGE_ADMIN_SQ), 0, NVME_PAGE_SIZE);
    _fmemset(NVME_PAGE(nvme, NVME_PAGE_ADMIN_CQ), 0, NVME_PAGE_SIZE);
    _fmemset(NVME_PAGE(nvme, NVME_PAGE_IO_SQ), 0, NVME_PAGE_SIZE);
    _fmemset(NVME_PAGE(nvme, NVME_PAGE_IO_CQ), 0, NVME_PAGE_SIZE);

    nvme->cid           = 0;
    nvme->admin_sq_tail = 0;
    nvme->admin_cq_head = 0;
    nvme->admin_phase   = 1;
    nvme->io_sq_tail    = 0;
    nvme->io_cq_head    = 0;
    nvme->io_phase      = 1;

    /* Set up the admin queues and enable the controller. */
    NVME_WRITE_REG(iobase, NVME_REG_AQA, ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16) | (NVME_QUEUE_ENTRIES - 1));
    phys = nvme_addr_to_phys(NVME_PAGE(nvme, NVME_PAGE_ADMIN_SQ));
    NVME_WRITE_REG(iobase, NVME_REG_ASQ, phys);
    NVME_WRITE_REG(iobase, NVME_REG_ASQ + 4, 0);
    phys = nvme_addr_to_phys(NVME_PAGE(nvme, NVME_PAGE_ADMIN_CQ));
    NVME_WRITE_REG(iobase, NVME_REG_ACQ, phys);
    NVME_WRITE_REG(iobase, NVME_REG_ACQ + 4, 0);

    NVME_WRITE_REG(iobase, NVME_REG_CC, NVME_CC_CONFIG | NVME_CC_EN);
    if (nvme_wait_ready(nvme, NVME_CSTS_RDY))
        return 1;

    /* Create the I/O completion queue (physically contiguous, interrupts disabled) and the submission queue. */
    if (nvme_cmd(nvme, 0 /*fIo*/, NVME_ADM_CREATE_CQ, 0, nvme_addr_to_phys(NVME_PAGE(nvme, NVME_PAGE_IO_CQ)), 0,
                 ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16) | NVME_IO_QID, 0x1 /*PC*/, 0) != 0)
        return 1;
    if (nvme_cmd(nvme, 0 /*fIo*/, NVME_ADM_CREATE_SQ, 0, nvme_addr_to_phys(NVME_PAGE(nvme, NVME_PAGE_IO_SQ)), 0,
                 ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16) | NVME_IO_QID, ((uint32_t)NVME_IO_QID << 16) | 0x1 /*PC*/, 0) != 0)
        return 1;

    return 0;
}

/**
 * Initializes the NVMe controller found at the given PCI location.
 */
int nvme_scsi_init(void __far *pvHba, uint8_t u8Bus, uint8_t u8DevFn)
{
    nvme_t __far *nvme = (nvme_t __far *)pvHba;
    uint32_t      u32Bar;
    uint16_t      seg;
    uint16_t      ebda_seg_old;
    uint16_t      eax_hi;
    int           rc;

    /* BAR2 holds the index/data register pair. */
    u32Bar = pci_read_config_dword(u8Bus, u8DevFn, 0x18);
    if (!(u32Bar & 0x01))
    {
        DBG_NVME("NVMe: BAR2 is not an I/O BAR\n");
        return 1;
    }

    /* The queues must be page aligned, allocate outside of the EBDA (which may move). */
    ebda_seg_old = read_word(0x0040, 0x000E);
    seg = conv_mem_alloc(NVME_ALLOC_KB, 0 /*in_ebda*/);
    if (seg == 0)
    {
        DBG_NVME("NVMe: Could not allocate memory\n");
        return 1;
    }

    /* The allocation moved the EBDA (and with it our HBA block) down. */
    nvme = (nvme_t __far *)MK_FP(FP_SEG(pvHba) - (ebda_seg_old - read_word(0x0040, 0x000E)), FP_OFF(pvHba));

    _fmemset(nvme, 0, sizeof(*nvme));
    nvme->iobase  = (uint16_t)(u32Bar & 0xfffc);
    nvme->seg_mem = (seg + (NVME_PAGE_SIZE / 16 - 1)) & ~(NVME_PAGE_SIZE / 16 - 1);

    /* Enable PCI memory, I/O, bus mastering access in command register. */
    pci_write_config_word(u8Bus, u8DevFn, 4, 0x7);

    eax_hi = eax_hi_rd();
    rc = nvme_hba_init(nvme);
    eax_hi_wr(eax_hi);
    return rc;
}
