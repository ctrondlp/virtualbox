/* $Id: Display.h 113251 2026-03-04 14:03:34Z alexander.eichner@oracle.com $ */
/** @file
 * VBox frontends: Basic Frontend (BFE):
 * Declaration of Display class
 */

/*
 * Copyright (C) 2023-2026 Oracle and/or its affiliates.
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

#ifndef VBOX_INCLUDED_SRC_VBoxBFE_Display_h
#define VBOX_INCLUDED_SRC_VBoxBFE_Display_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/critsect.h>
#include <iprt/semaphore.h>
#include <VBox/vmm/pdm.h>

#include <VBox/com/defs.h>
#include "Framebuffer.h"

class Display
{

public:

    Display();
    ~Display();

    // public methods only for internal purposes
    static const PDMDRVREG  DrvReg;

    uint32_t getWidth();
    uint32_t getHeight();
    uint32_t getBitsPerPixel();

    int SetFramebuffer(unsigned iScreenID, Framebuffer *pFramebuffer);
    void getFramebufferDimensions(int32_t *px1, int32_t *py1, int32_t *px2,
                                  int32_t *py2);

    void resetFramebuffer();

    void setRunning(void) { mfMachineRunning = true; };

    int i_handleDisplayResize(unsigned uScreenId, uint32_t bpp, void *pvVRAM,
                              uint32_t cbLine, uint32_t w, uint32_t h, uint16_t flags,
                              int32_t xOrigin, int32_t yOrigin, bool fVGAResize);
    void i_handleDisplayUpdate (int x, int y, int w, int h);

    void i_invalidateAndUpdateScreen(uint32_t aScreenId);
    void i_freeRetiredShadows(void *pvInUse);


private:

    void updateDisplayData();

    static DECLCALLBACK(void*) i_drvQueryInterface(PPDMIBASE pInterface, const char *pszIID);
    static DECLCALLBACK(int)   i_drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags);
    static DECLCALLBACK(void)  i_drvDestruct(PPDMDRVINS pDrvIns);
    static DECLCALLBACK(void)  i_drvPowerOff(PPDMDRVINS pDrvIns);
    static DECLCALLBACK(int)   i_displayResizeCallback(PPDMIDISPLAYCONNECTOR pInterface, uint32_t bpp, void *pvVRAM, uint32_t cbLine, uint32_t cx, uint32_t cy);
    static DECLCALLBACK(void)  i_displayUpdateCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                       uint32_t x, uint32_t y, uint32_t cx, uint32_t cy);
    static DECLCALLBACK(void)  i_displayRefreshCallback(PPDMIDISPLAYCONNECTOR pInterface);
    static DECLCALLBACK(void)  i_displayResetCallback(PPDMIDISPLAYCONNECTOR pInterface);
    static DECLCALLBACK(void)  i_displayLFBModeChangeCallback(PPDMIDISPLAYCONNECTOR pInterface, bool fEnabled);
    static DECLCALLBACK(void)  i_displayProcessAdapterDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM, uint32_t u32VRAMSize);
    static DECLCALLBACK(void)  i_displayProcessDisplayDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM, unsigned uScreenId);
    static DECLCALLBACK(int)   i_displayVBVAEnable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId,
                                                   VBVAHOSTFLAGS RT_UNTRUSTED_VOLATILE_GUEST *pHostFlags);
    static DECLCALLBACK(void)  i_displayVBVADisable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId);
    static DECLCALLBACK(void)  i_displayVBVAUpdateBegin(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId);
    static DECLCALLBACK(void)  i_displayVBVAUpdateProcess(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId,
                                                          struct VBVACMDHDR const RT_UNTRUSTED_VOLATILE_GUEST *pCmd, size_t cbCmd);
    static DECLCALLBACK(void)  i_displayVBVAUpdateEnd(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, int32_t x, int32_t y,
                                                      uint32_t cx, uint32_t cy);
    static DECLCALLBACK(int)   i_displayVBVAResize(PPDMIDISPLAYCONNECTOR pInterface, PCVBVAINFOVIEW pView,
                                                   PCVBVAINFOSCREEN pScreen, void *pvVRAM, bool fResetInputMapping);
    static DECLCALLBACK(int)   i_displayVBVAMousePointerShape(PPDMIDISPLAYCONNECTOR pInterface, bool fVisible, bool fAlpha,
                                                              uint32_t xHot, uint32_t yHot, uint32_t cx, uint32_t cy,
                                                              const void *pvShape);
    static DECLCALLBACK(void)  i_displayVBVAGuestCapabilityUpdate(PPDMIDISPLAYCONNECTOR pInterface, uint32_t fCapabilities);
    static DECLCALLBACK(void)  i_displayVBVAInputMappingUpdate(PPDMIDISPLAYCONNECTOR pInterface, int32_t xOrigin, int32_t yOrigin,
                                                               uint32_t cx, uint32_t cy);
    static DECLCALLBACK(void)  i_displayVBVAReportCursorPosition(PPDMIDISPLAYCONNECTOR pInterface, uint32_t fFlags,
                                                                 uint32_t uScreen, uint32_t x, uint32_t y);

    static DECLCALLBACK(void) i_doInvalidateAndUpdate(struct DRVMAINDISPLAY  *mpDrv);
    uint8_t *i_allocShadow(uint32_t cx, uint32_t cy);

    /** Pointer to the associated display driver. */
    struct DRVMAINDISPLAY  *mpDrv;

    /** The 32bpp buffer the VGA device currently renders the primary screen into
     * (see i_handleDisplayResize), NULL if none. */
    struct DISPLAYSHADOWBUF *mpShadow;
    /** Buffers replaced by a newer one, freed by the SDL thread once it no
     * longer shows them (i_freeRetiredShadows). */
    struct DISPLAYSHADOWBUF *mpRetiredShadows;
    /** Protects mpShadow and mpRetiredShadows. */
    RTCRITSECT              mShadowLock;

    Framebuffer *m_pFramebuffer;
    bool mFramebufferOpened;

    bool        mfMachineRunning;
    volatile bool fVGAResizing;

    /** Origin of the primary screen as last set by the guest via VBVA.  VBVA
     * reports dirty rectangles in coordinates relative to this. */
    int32_t     mxVBVAOrigin;
    int32_t     myVBVAOrigin;

    /** The primary screen layout last applied via VBVA (valid if mfVBVAScreenValid).
     * Linux guests repeat the same layout with every display update. */
    bool        mfVBVAScreenValid;
    void       *mpvVBVAScreen;
    uint32_t    mcxVBVAScreen;
    uint32_t    mcyVBVAScreen;
    uint32_t    mcbVBVAScreenLine;
    uint16_t    mcBitsVBVAScreen;
    uint16_t    mfVBVAScreenFlags;

    void handleResizeCompletedEMT (void);
    volatile uint32_t mu32ResizeStatus;

    enum {
        ResizeStatus_Void,
        ResizeStatus_InProgress,
        ResizeStatus_UpdateDisplayData
    };
};

#define DISPLAY_OID                          "e2ff0c7b-c02b-46d0-aa90-b9caf0f60561"

#endif /* !VBOX_INCLUDED_SRC_VBoxBFE_Display_h */
