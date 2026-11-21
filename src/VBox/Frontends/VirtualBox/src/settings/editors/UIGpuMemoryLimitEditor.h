/* $Id$ */
/** @file
 * VBox Qt GUI - UIGpuMemoryLimitEditor class declaration.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIGpuMemoryLimitEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIGpuMemoryLimitEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QGridLayout;
class QLabel;
class QSpinBox;

/** UIEditor sub-class used as an editor for the limit of the host GPU memory the guest can allocate
  * through the virtio-gpu (IGraphicsAdapter::gpuMemoryLimit), in megabytes, 0 for no limit. */
class SHARED_LIBRARY_STUFF UIGpuMemoryLimitEditor : public UIEditor
{
    Q_OBJECT;

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIGpuMemoryLimitEditor(QWidget *pParent = 0);

    /** Defines editor @a iValue in megabytes, 0 for no limit. */
    void setValue(int iValue);
    /** Returns editor value in megabytes, 0 for no limit. */
    int value() const;

    /** Returns minimum layout hint. */
    int minimumLabelHorizontalHint() const;
    /** Defines minimum layout @a iIndent. */
    void setMinimumLayoutIndent(int iIndent);

private slots:

    /** Handles translation event. */
    virtual void sltRetranslateUI() RT_OVERRIDE RT_FINAL;

private:

    /** Prepares all. */
    void prepare();

    /** Holds the value, in megabytes. */
    int  m_iValue;

    /** Holds the main layout instance. */
    QGridLayout *m_pLayout;
    /** Holds the label instance. */
    QLabel      *m_pLabel;
    /** Holds the spin-box instance. */
    QSpinBox    *m_pSpinBox;
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIGpuMemoryLimitEditor_h */
