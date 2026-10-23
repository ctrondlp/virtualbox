/* $Id: Display.cpp 113251 2026-03-04 14:03:34Z alexander.eichner@oracle.com $ */
/** @file
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2026 Oracle and/or its affiliates.
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

#define LOG_GROUP LOG_GROUP_MAIN_DISPLAY
#include "Display.h"

#include <iprt/semaphore.h>
#include <iprt/thread.h>
#include <iprt/asm.h>
#include <iprt/time.h>
#include <iprt/cpp/utils.h>
#include <iprt/alloca.h>
#include <iprt/mem.h>
#include <iprt/uuid.h>

#include <VBox/vmm/vmmr3vtable.h>
#include <VBox/vmm/pdmdrv.h>

#include <VBoxVideo.h>

/**
 * Display driver instance data.
 *
 * @implements PDMIDISPLAYCONNECTOR
 */
typedef struct DRVMAINDISPLAY
{
    /** Pointer to the display object. */
    Display                    *pDisplay;
    /** Pointer to the driver instance structure. */
    PPDMDRVINS                  pDrvIns;
    /** Pointer to the display port interface of the driver/device above us. */
    PPDMIDISPLAYPORT            pUpPort;
    /** Our display connector interface. */
    PDMIDISPLAYCONNECTOR        IConnector;
} DRVMAINDISPLAY, *PDRVMAINDISPLAY;

/**
 * A 32bpp buffer the VGA device renders the primary screen into, see
 * Display::i_handleDisplayResize.
 */
typedef struct DISPLAYSHADOWBUF
{
    /** Next retired buffer. */
    struct DISPLAYSHADOWBUF    *pNext;
    /** The pixels (in the same allocation). */
    uint8_t                    *pbPixels;
} DISPLAYSHADOWBUF;

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

Display::Display()
{
    mpShadow         = NULL;
    mpRetiredShadows = NULL;
    int vrc = RTCritSectInit(&mShadowLock);
    AssertRC(vrc);
    fVGAResizing = false;
    mxVBVAOrigin = 0;
    myVBVAOrigin = 0;
    mfVBVAScreenValid = false;
    mpvVBVAScreen     = NULL;
    mcxVBVAScreen     = 0;
    mcyVBVAScreen     = 0;
    mcbVBVAScreenLine = 0;
    mcBitsVBVAScreen  = 0;
    mfVBVAScreenFlags = 0;
}

Display::~Display()
{
    if (mpDrv)
        mpDrv->pDisplay = NULL;

    mpDrv = NULL;
}


// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

int Display::SetFramebuffer(unsigned iScreenID, Framebuffer *pFramebuffer)
{
    RT_NOREF(iScreenID);
    m_pFramebuffer = pFramebuffer;
    return VINF_SUCCESS;
}


/**
 *  Handles display resize event.
 *
 * @param uScreenId Screen ID
 * @param bpp       New bits per pixel.
 * @param pvVRAM    VRAM pointer.
 * @param cbLine    New bytes per line.
 * @param w         New display width.
 * @param h         New display height.
 * @param flags     Flags of the new video mode.
 * @param xOrigin   New display origin X.
 * @param yOrigin   New display origin Y.
 * @param fVGAResize Whether the resize is originated from the VGA device (DevVGA).
 */
int Display::i_handleDisplayResize(unsigned uScreenId, uint32_t bpp, void *pvVRAM,
                                   uint32_t cbLine, uint32_t w, uint32_t h, uint16_t flags,
                                   int32_t xOrigin, int32_t yOrigin, bool fVGAResize)
{
    RT_NOREF(fVGAResize);

    LogRel2(("Display::i_handleDisplayResize: uScreenId=%d pvVRAM=%p w=%d h=%d bpp=%d cbLine=0x%X flags=0x%X\n", uScreenId,
             pvVRAM, w, h, bpp, cbLine, flags));

    if (uScreenId == 0 && mpDrv)
    {
        mpDrv->pUpPort->pfnSetRenderVRAM(mpDrv->pUpPort, false);

        mpDrv->IConnector.pbData     = NULL;
        mpDrv->IConnector.cbScanline = 0;
        mpDrv->IConnector.cBits      = 32;
        mpDrv->IConnector.cx         = 0;
        mpDrv->IConnector.cy         = 0;
    }

    /* Log changes. */
    LogRel(("Display::i_handleDisplayResize: uScreenId=%d pvVRAM=%p w=%d h=%d bpp=%d cbLine=0x%X flags=0x%X origin=%d,%d\n",
            uScreenId, pvVRAM, w, h, bpp, cbLine, flags, xOrigin, yOrigin));

    /* VBVA and VMSVGA screens in the 32bpp layout of the SDL surface are shown straight
       from VRAM (like the GUI does).  Everything else the VGA device renders into a
       32bpp buffer of ours.  That buffer is set up right here, on the EMT and while the
       VGA device holds its lock, so it always matches the mode the device renders:
       the SDL thread only gets to the new mode later, when the guest may have
       changed it again (the device would then draw the new mode into a buffer made
       for the old one). */
    void    *pvScreen     = NULL;
    uint32_t cbScreenLine = 0;
    if (!fVGAResize && pvVRAM && bpp == 32 && cbLine >= w * 4)
    {
        pvScreen     = pvVRAM;
        cbScreenLine = cbLine;
    }
    else if (uScreenId == 0 && mpDrv && w && h)
    {
        uint8_t *pbShadow = i_allocShadow(w, h);
        if (pbShadow)
        {
            mpDrv->IConnector.pbData     = pbShadow;
            mpDrv->IConnector.cbScanline = w * 4;
            mpDrv->IConnector.cBits      = 32;
            mpDrv->IConnector.cx         = w;
            mpDrv->IConnector.cy         = h;
            mpDrv->pUpPort->pfnSetRenderVRAM(mpDrv->pUpPort, true);
            pvScreen     = pbShadow;
            cbScreenLine = w * 4;
        }
    }

    int vrc = m_pFramebuffer->notifyChange(uScreenId, 0, 0, w, h, pvScreen, 32, cbScreenLine);
    LogFunc(("NotifyChange vrc=%Rrc\n", vrc)); RT_NOREF(vrc);

    /* The VGA device redraws its own modes completely after a resize, but with VBVA
       the guest only reports what changes from now on: fill the new buffer. */
    if (!fVGAResize && pvScreen && pvScreen != pvVRAM)
        mpDrv->pUpPort->pfnUpdateDisplayRect(mpDrv->pUpPort, 0, 0, w, h);

    return VINF_SUCCESS;
}


/**
 * Allocates a new buffer for the VGA device to render the primary screen into,
 * retiring the current one (the SDL thread may still be showing it).
 *
 * @returns Pointer to the pixels (32bpp, @a cx * 4 bytes per line), NULL on failure.
 * @param   cx      The width.
 * @param   cy      The height.
 * @thread  EMT
 */
uint8_t *Display::i_allocShadow(uint32_t cx, uint32_t cy)
{
    /* The VGA device has checked the mode against the VRAM size, this is just sanity. */
    size_t const cbPixels = (size_t)cx * cy * 4;
    AssertReturn(cx <= _16K && cy <= _16K, NULL);

    size_t const      cbHdr = RT_ALIGN_Z(sizeof(DISPLAYSHADOWBUF), 64);
    DISPLAYSHADOWBUF *pBuf  = (DISPLAYSHADOWBUF *)RTMemAllocZ(cbHdr + cbPixels);
    if (!pBuf)
    {
        LogRel(("Display: Failed to allocate a %ux%u framebuffer\n", cx, cy));
        return NULL;
    }
    pBuf->pbPixels = (uint8_t *)pBuf + cbHdr;

    RTCritSectEnter(&mShadowLock);
    if (mpShadow)
    {
        mpShadow->pNext  = mpRetiredShadows;
        mpRetiredShadows = mpShadow;
    }
    mpShadow = pBuf;
    RTCritSectLeave(&mShadowLock);

    return pBuf->pbPixels;
}


/**
 * Frees the retired buffers the SDL thread no longer shows.
 *
 * @param   pvInUse     The pixels the framebuffer shows now (can be a retired
 *                      buffer when the guest changed the mode again meanwhile).
 * @thread  SDL
 */
void Display::i_freeRetiredShadows(void *pvInUse)
{
    RTCritSectEnter(&mShadowLock);
    DISPLAYSHADOWBUF **ppBuf = &mpRetiredShadows;
    while (*ppBuf)
    {
        DISPLAYSHADOWBUF *pBuf = *ppBuf;
        if (pBuf->pbPixels != pvInUse)
        {
            *ppBuf = pBuf->pNext;
            RTMemFree(pBuf);
        }
        else
            ppBuf = &pBuf->pNext;
    }
    RTCritSectLeave(&mShadowLock);
}


/**
 * Handle display update
 *
 * @returns COM status code
 * @param w New display width
 * @param h New display height
 */
void Display::i_handleDisplayUpdate (int x, int y, int w, int h)
{
    // if there is no Framebuffer, this call is not interesting
    if (m_pFramebuffer == NULL)
        return;

    //m_pFramebuffer->Lock();
    m_pFramebuffer->notifyUpdate(x, y, w, h);
    //m_pFramebuffer->Unlock();
}


void Display::i_invalidateAndUpdateScreen(uint32_t aScreenId)
{
    RT_NOREF(aScreenId);

    /* If the framebuffer shows the guest VRAM directly there is nothing for the VGA
       device to render (and it must not write into the guest VRAM). */
    if (m_pFramebuffer->usesGuestVRAM())
        return;

    mpDrv->IConnector.pbData     = m_pFramebuffer->getPixelData();
    mpDrv->IConnector.cbScanline = m_pFramebuffer->getBytesPerLine();
    mpDrv->IConnector.cBits      = m_pFramebuffer->getBitsPerPixel();
    mpDrv->IConnector.cx         = m_pFramebuffer->getWidth();
    mpDrv->IConnector.cy         = m_pFramebuffer->getHeight();

    mpDrv->pUpPort->pfnSetRenderVRAM(mpDrv->pUpPort, true);
}


// private methods
/////////////////////////////////////////////////////////////////////////////

/**
 * Handle display resize event issued by the VGA device for the primary screen.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnResize
 */
DECLCALLBACK(int) Display::i_displayResizeCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                   uint32_t bpp, void *pvVRAM, uint32_t cbLine, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    Display *pThis = pDrv->pDisplay;

    LogRelFlowFunc(("bpp %d, pvVRAM %p, cbLine %d, cx %d, cy %d\n",
                  bpp, pvVRAM, cbLine, cx, cy));

    bool f = ASMAtomicCmpXchgBool(&pThis->fVGAResizing, true, false);
    if (!f)
    {
        /* This is a result of recursive call when the source bitmap is being updated
         * during a VGA resize. Tell the VGA device to ignore the call.
         *
         * @todo It is a workaround, actually pfnUpdateDisplayAll must
         * fail on resize.
         */
        LogRel(("displayResizeCallback: already processing\n"));
        return VINF_VGA_RESIZE_IN_PROGRESS;
    }

    /* The VGA device owns the screen layout now. */
    pThis->mfVBVAScreenValid = false;

    int vrc = pThis->i_handleDisplayResize(0, bpp, pvVRAM, cbLine, cx, cy, 0, 0, 0, true);

    /* Restore the flag.  */
    f = ASMAtomicCmpXchgBool(&pThis->fVGAResizing, false, true);
    AssertRelease(f);

    return vrc;
}

/**
 * Handle display update.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnUpdateRect
 */
DECLCALLBACK(void) Display::i_displayUpdateCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                    uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);

    pDrv->pDisplay->i_handleDisplayUpdate(x, y, cx, cy);
}

/**
 * Periodic display refresh callback.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnRefresh
 * @thread EMT
 */
/*static*/ DECLCALLBACK(void) Display::i_displayRefreshCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);

    pDrv->pUpPort->pfnUpdateDisplay(pDrv->pUpPort);
}

/**
 * Reset notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnReset
 */
DECLCALLBACK(void) Display::i_displayResetCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    RT_NOREF(pInterface);
    LogRelFlowFunc(("\n"));
}

/**
 * LFBModeChange notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnLFBModeChange
 */
DECLCALLBACK(void) Display::i_displayLFBModeChangeCallback(PPDMIDISPLAYCONNECTOR pInterface, bool fEnabled)
{
    RT_NOREF(pInterface, fEnabled);
}

/**
 * Adapter information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessAdapterData
 */
DECLCALLBACK(void) Display::i_displayProcessAdapterDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM,
                                                                uint32_t u32VRAMSize)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    //pDrv->pDisplay->processAdapterData(pvVRAM, u32VRAMSize);
    RT_NOREF(pDrv, pvVRAM, u32VRAMSize);
}

/**
 * Display information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessDisplayData
 */
DECLCALLBACK(void) Display::i_displayProcessDisplayDataCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                                void *pvVRAM, unsigned uScreenId)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    //pDrv->pDisplay->processDisplayData(pvVRAM, uScreenId);
    RT_NOREF(pDrv, pvVRAM, uScreenId);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAEnable}
 */
DECLCALLBACK(int) Display::i_displayVBVAEnable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId,
                                               VBVAHOSTFLAGS RT_UNTRUSTED_VOLATILE_GUEST *pHostFlags)
{
    RT_NOREF(pInterface);
    LogRelFlowFunc(("uScreenId=%u\n", uScreenId));

    /* Tell the guest that VBVA is active so that it reports what it draws.  Once VBVA is
       enabled the VGA device stops refreshing the screen by itself.  (VMSVGA passes no
       host flags.) */
    if (pHostFlags)
    {
        ASMAtomicWriteU32(&pHostFlags->u32HostEvents, VBVA_F_MODE_ENABLED);
        ASMAtomicWriteU32(&pHostFlags->u32SupportedOrders, 0);
    }
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVADisable}
 */
DECLCALLBACK(void) Display::i_displayVBVADisable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    LogRelFlowFunc(("uScreenId=%u\n", uScreenId));

    if (uScreenId == 0)
    {
        /* The VGA device takes over the primary screen again, so redraw all of it. */
        pDrv->pDisplay->mxVBVAOrigin = 0;
        pDrv->pDisplay->myVBVAOrigin = 0;
        pDrv->pDisplay->mfVBVAScreenValid = false;
        pDrv->pUpPort->pfnUpdateDisplayAll(pDrv->pUpPort, false /*fFailOnResize*/);
    }
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAUpdateBegin}
 */
DECLCALLBACK(void) Display::i_displayVBVAUpdateBegin(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    RT_NOREF(pInterface, uScreenId);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAUpdateProcess}
 */
DECLCALLBACK(void) Display::i_displayVBVAUpdateProcess(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId,
                                                       struct VBVACMDHDR const RT_UNTRUSTED_VOLATILE_GUEST *pCmd, size_t cbCmd)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    AssertReturnVoid(cbCmd >= sizeof(VBVACMDHDR));

    if (uScreenId != 0)
        return;

    VBVACMDHDR Hdr;
    RT_COPY_VOLATILE(Hdr, *pCmd);
    RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();

    /* Copy the area the guest drew to from VRAM to our framebuffer.  The VGA device
       clips the rectangle to the screen. */
    pDrv->pUpPort->pfnUpdateDisplayRect(pDrv->pUpPort, Hdr.x, Hdr.y, Hdr.w, Hdr.h);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAUpdateEnd}
 */
DECLCALLBACK(void) Display::i_displayVBVAUpdateEnd(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, int32_t x, int32_t y,
                                                   uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    Display *pThis = pDrv->pDisplay;

    if (uScreenId == 0 && cx && cy)
        pThis->i_handleDisplayUpdate(x - pThis->mxVBVAOrigin, y - pThis->myVBVAOrigin, (int)cx, (int)cy);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAResize}
 */
DECLCALLBACK(int) Display::i_displayVBVAResize(PPDMIDISPLAYCONNECTOR pInterface, PCVBVAINFOVIEW pView,
                                               PCVBVAINFOSCREEN pScreen, void *pvVRAM, bool fResetInputMapping)
{
    PDRVMAINDISPLAY pDrv = RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector);
    Display *pThis = pDrv->pDisplay;
    RT_NOREF(pView, fResetInputMapping);
    LogRelFlowFunc(("uScreenId=%u %ux%u bpp=%u flags=%#x\n", pScreen->u32ViewIndex, pScreen->u32Width,
                    pScreen->u32Height, pScreen->u16BitsPerPixel, pScreen->u16Flags));

    /* Only the primary screen is shown.  Keep the current window when the guest
       disables or blanks the screen. */
    if (   pScreen->u32ViewIndex != 0
        || (pScreen->u16Flags & (VBVA_SCREEN_F_DISABLED | VBVA_SCREEN_F_BLANK2)))
        return VINF_SUCCESS;

    /* Linux guests send the same layout again with every display update.  Only
       resize the window when something changed. */
    void * const pvScreen = (uint8_t *)pvVRAM + pScreen->u32StartOffset;
    if (   pThis->mfVBVAScreenValid
        && pThis->mcxVBVAScreen     == pScreen->u32Width
        && pThis->mcyVBVAScreen     == pScreen->u32Height
        && pThis->mcbVBVAScreenLine == pScreen->u32LineSize
        && pThis->mcBitsVBVAScreen  == pScreen->u16BitsPerPixel
        && pThis->mfVBVAScreenFlags == pScreen->u16Flags
        && pThis->mxVBVAOrigin      == pScreen->i32OriginX
        && pThis->myVBVAOrigin      == pScreen->i32OriginY)
    {
        if (pThis->mpvVBVAScreen != pvScreen)
        {
            /* Page flip (e.g. GNOME): same layout, the screen now starts elsewhere in
               VRAM.  The guest only reports changes relative to the previous frame,
               so copy the whole new frame (unless shown straight from VRAM). */
            pThis->mpvVBVAScreen = pvScreen;
            if (pScreen->u16BitsPerPixel == 32 && pScreen->u32LineSize >= pScreen->u32Width * 4)
                pThis->m_pFramebuffer->setVRAMPointer(pvScreen);
            pDrv->pUpPort->pfnUpdateDisplayRect(pDrv->pUpPort, 0, 0, pScreen->u32Width, pScreen->u32Height);
            pThis->i_handleDisplayUpdate(0, 0, (int)pScreen->u32Width, (int)pScreen->u32Height);
        }
        return VINF_SUCCESS;
    }

    pThis->mfVBVAScreenValid = true;
    pThis->mpvVBVAScreen     = pvScreen;
    pThis->mcxVBVAScreen     = pScreen->u32Width;
    pThis->mcyVBVAScreen     = pScreen->u32Height;
    pThis->mcbVBVAScreenLine = pScreen->u32LineSize;
    pThis->mcBitsVBVAScreen  = pScreen->u16BitsPerPixel;
    pThis->mfVBVAScreenFlags = pScreen->u16Flags;
    pThis->mxVBVAOrigin      = pScreen->i32OriginX;
    pThis->myVBVAOrigin      = pScreen->i32OriginY;
    return pThis->i_handleDisplayResize(0, pScreen->u16BitsPerPixel, pvScreen,
                                        pScreen->u32LineSize, pScreen->u32Width, pScreen->u32Height, pScreen->u16Flags,
                                        pScreen->i32OriginX, pScreen->i32OriginY, false /*fVGAResize*/);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAMousePointerShape}
 */
DECLCALLBACK(int) Display::i_displayVBVAMousePointerShape(PPDMIDISPLAYCONNECTOR pInterface, bool fVisible, bool fAlpha,
                                                          uint32_t xHot, uint32_t yHot, uint32_t cx, uint32_t cy,
                                                          const void *pvShape)
{
    RT_NOREF(pInterface, fVisible, fAlpha, xHot, yHot, cx, cy, pvShape);
    /* There is no host mouse pointer, the guest has to draw its own. */
    return VERR_NOT_SUPPORTED;
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAGuestCapabilityUpdate}
 */
DECLCALLBACK(void) Display::i_displayVBVAGuestCapabilityUpdate(PPDMIDISPLAYCONNECTOR pInterface, uint32_t fCapabilities)
{
    RT_NOREF(pInterface);
    LogRelFlowFunc(("fCapabilities=%#x\n", fCapabilities));
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAInputMappingUpdate}
 */
DECLCALLBACK(void) Display::i_displayVBVAInputMappingUpdate(PPDMIDISPLAYCONNECTOR pInterface, int32_t xOrigin, int32_t yOrigin,
                                                            uint32_t cx, uint32_t cy)
{
    RT_NOREF(pInterface, xOrigin, yOrigin, cx, cy);
}


/**
 * @interface_method_impl{PDMIDISPLAYCONNECTOR,pfnVBVAReportCursorPosition}
 */
DECLCALLBACK(void) Display::i_displayVBVAReportCursorPosition(PPDMIDISPLAYCONNECTOR pInterface, uint32_t fFlags,
                                                              uint32_t uScreen, uint32_t x, uint32_t y)
{
    RT_NOREF(pInterface, fFlags, uScreen, x, y);
}


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
DECLCALLBACK(void *)  Display::i_drvQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVMAINDISPLAY pDrv = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIDISPLAYCONNECTOR, &pDrv->IConnector);
    return NULL;
}


/**
 * @interface_method_impl{PDMDRVREG,pfnPowerOff,
 *  Tries to ensure no client calls gets to HGCM or the VGA device from here on.}
 */
DECLCALLBACK(void) Display::i_drvPowerOff(PPDMDRVINS pDrvIns)
{
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    /*
     * Do much of the work that i_drvDestruct does.
     */
    if (pThis->pUpPort)
        pThis->pUpPort->pfnSetRenderVRAM(pThis->pUpPort, false);

    pThis->IConnector.pbData     = NULL;
    pThis->IConnector.cbScanline = 0;
    pThis->IConnector.cBits      = 32;
    pThis->IConnector.cx         = 0;
    pThis->IConnector.cy         = 0;
}


/**
 * Destruct a display driver instance.
 *
 * @returns VBox status code.
 * @param   pDrvIns     The driver instance data.
 */
DECLCALLBACK(void) Display::i_drvDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    /*
     * We repeat much of what i_drvPowerOff does in case it wasn't called.
     * In addition we sever the connection between us and the display.
     */
    if (pThis->pUpPort)
        pThis->pUpPort->pfnSetRenderVRAM(pThis->pUpPort, false);

    pThis->IConnector.pbData     = NULL;
    pThis->IConnector.cbScanline = 0;
    pThis->IConnector.cBits      = 32;
    pThis->IConnector.cx         = 0;
    pThis->IConnector.cy         = 0;

    if (pThis->pDisplay)
    {
        pThis->pDisplay->mpDrv = NULL;
        pThis->pDisplay = NULL;
    }
}


/**
 * Construct a display driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
DECLCALLBACK(int) Display::i_drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    RT_NOREF(fFlags, pCfg);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    /*
     * Validate configuration.
     */
    PDMDRV_VALIDATE_CONFIG_RETURN(pDrvIns, "", "");
    AssertMsgReturn(PDMDrvHlpNoAttach(pDrvIns) == VERR_PDM_NO_ATTACHED_DRIVER,
                    ("Configuration error: Not possible to attach anything to this driver!\n"),
                    VERR_PDM_DRVINS_NO_ATTACH);

    /*
     * Init Interfaces.
     */
    pDrvIns->IBase.pfnQueryInterface           = Display::i_drvQueryInterface;

    pThis->IConnector.pfnResize                = Display::i_displayResizeCallback;
    pThis->IConnector.pfnUpdateRect            = Display::i_displayUpdateCallback;
    pThis->IConnector.pfnRefresh               = Display::i_displayRefreshCallback;
    pThis->IConnector.pfnReset                 = Display::i_displayResetCallback;
    pThis->IConnector.pfnLFBModeChange         = Display::i_displayLFBModeChangeCallback;
    pThis->IConnector.pfnProcessAdapterData    = Display::i_displayProcessAdapterDataCallback;
    pThis->IConnector.pfnProcessDisplayData    = Display::i_displayProcessDisplayDataCallback;
    pThis->IConnector.pfnVBVAEnable            = Display::i_displayVBVAEnable;
    /* The VGA device calls these without checking for NULL. */
    pThis->IConnector.pfnVBVADisable               = Display::i_displayVBVADisable;
    pThis->IConnector.pfnVBVAUpdateBegin           = Display::i_displayVBVAUpdateBegin;
    pThis->IConnector.pfnVBVAUpdateProcess         = Display::i_displayVBVAUpdateProcess;
    pThis->IConnector.pfnVBVAUpdateEnd             = Display::i_displayVBVAUpdateEnd;
    pThis->IConnector.pfnVBVAResize                = Display::i_displayVBVAResize;
    pThis->IConnector.pfnVBVAMousePointerShape     = Display::i_displayVBVAMousePointerShape;
    pThis->IConnector.pfnVBVAGuestCapabilityUpdate = Display::i_displayVBVAGuestCapabilityUpdate;
    pThis->IConnector.pfnVBVAInputMappingUpdate    = Display::i_displayVBVAInputMappingUpdate;
    pThis->IConnector.pfnVBVAReportCursorPosition  = Display::i_displayVBVAReportCursorPosition;

    /*
     * Get the IDisplayPort interface of the above driver/device.
     */
    pThis->pUpPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYPORT);
    if (!pThis->pUpPort)
    {
        AssertMsgFailed(("Configuration error: No display port interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }

    /*
     * Get the Display object pointer and update the mpDrv member.
     */
    RTUUID UuidDisp;
    RTUuidFromStr(&UuidDisp, DISPLAY_OID);
    Display *pDisplay = (Display *)PDMDrvHlpQueryGenericUserObject(pDrvIns, &UuidDisp);
    if (!pDisplay)
    {
        AssertMsgFailed(("Configuration error: No/bad Display object!\n"));
        return VERR_NOT_FOUND;
    }
    pThis->pDisplay = pDisplay;
    pThis->pDisplay->mpDrv = pThis;

    /* Disable VRAM to a buffer copy initially. */
    pThis->pUpPort->pfnSetRenderVRAM(pThis->pUpPort, false);
    pThis->IConnector.cBits = 32; /* DevVGA does nothing otherwise. */

    /*
     * Start periodic screen refreshes
     */
    pThis->pUpPort->pfnSetRefreshRate(pThis->pUpPort, 20);

    return VINF_SUCCESS;
}


/**
 * Display driver registration record.
 */
const PDMDRVREG Display::DrvReg =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "MainDisplay",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Main display driver (Main as in the API).",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_DISPLAY,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVMAINDISPLAY),
    /* pfnConstruct */
    Display::i_drvConstruct,
    /* pfnDestruct */
    Display::i_drvDestruct,
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
    Display::i_drvPowerOff,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

/* vi: set tabstop=4 shiftwidth=4 expandtab: */
