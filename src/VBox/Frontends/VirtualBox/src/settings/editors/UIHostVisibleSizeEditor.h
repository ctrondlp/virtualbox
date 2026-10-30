/* $Id$ */
/** @file
 * VBox Qt GUI - UIHostVisibleSizeEditor class declaration.
 */

/*
 * Copyright (C) 2019-2026 Oracle and/or its affiliates.
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

#ifndef FEQT_INCLUDED_SRC_settings_editors_UIHostVisibleSizeEditor_h
#define FEQT_INCLUDED_SRC_settings_editors_UIHostVisibleSizeEditor_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* GUI includes: */
#include "UIEditor.h"

/* Forward declarations: */
class QComboBox;
class QGridLayout;
class QLabel;

/** UIEditor sub-class used as a host visible memory window size editor of the virtio-gpu graphics controller.
  * The device maps the window as one 64-bit region, so only powers of two from 64 MiB to 16 GiB are offered. */
class SHARED_LIBRARY_STUFF UIHostVisibleSizeEditor : public UIEditor
{
    Q_OBJECT;

public:

    /** Constructs editor passing @a pParent to the base-class. */
    UIHostVisibleSizeEditor(QWidget *pParent = 0);

    /** Defines editor @a iValue in megabytes. */
    void setValue(int iValue);
    /** Returns editor value in megabytes. */
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

    /** Populates combo. */
    void populateCombo();

    /** Holds the value to be selected, in megabytes. */
    int  m_iValue;

    /** Holds the main layout instance. */
    QGridLayout *m_pLayout;
    /** Holds the label instance. */
    QLabel      *m_pLabel;
    /** Holds the combo instance. */
    QComboBox   *m_pCombo;
};

#endif /* !FEQT_INCLUDED_SRC_settings_editors_UIHostVisibleSizeEditor_h */
