/* $Id$ */
/** @file
 * VBox Qt GUI - UIHostVisibleSizeEditor class implementation.
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

/* Qt includes: */
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>

/* GUI includes: */
#include "UIHostVisibleSizeEditor.h"

/* Defines: */
#define HOST_VISIBLE_SIZE_MIN_MB   64
#define HOST_VISIBLE_SIZE_MAX_MB   16384
#define HOST_VISIBLE_SIZE_DEF_MB   1024


UIHostVisibleSizeEditor::UIHostVisibleSizeEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent)
    , m_iValue(HOST_VISIBLE_SIZE_DEF_MB)
    , m_pLayout(0)
    , m_pLabel(0)
    , m_pCombo(0)
{
    prepare();
}

void UIHostVisibleSizeEditor::setValue(int iValue)
{
    /* Update cached value and combo if value has changed: */
    if (m_iValue != iValue)
    {
        m_iValue = iValue;
        populateCombo();
    }
}

int UIHostVisibleSizeEditor::value() const
{
    return m_pCombo ? m_pCombo->currentData().toInt() : m_iValue;
}

int UIHostVisibleSizeEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIHostVisibleSizeEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent);
}

void UIHostVisibleSizeEditor::sltRetranslateUI()
{
    if (m_pLabel)
        m_pLabel->setText(tr("Host &Visible Memory"));
    if (m_pCombo)
    {
        for (int i = 0; i < m_pCombo->count(); ++i)
        {
            const int iMB = m_pCombo->itemData(i).toInt();
            m_pCombo->setItemText(i, iMB >= 1024 ? tr("%1 GB").arg(iMB / 1024) : tr("%1 MB").arg(iMB));
        }
        m_pCombo->setToolTip(tr("Size of the address window the guest can map host GPU memory into when "
                                "using 3D acceleration. It is address space only, it is not backed by "
                                "memory until the guest maps something into it."));
    }
}

void UIHostVisibleSizeEditor::prepare()
{
    /* Create main layout: */
    m_pLayout = new QGridLayout(this);
    if (m_pLayout)
    {
        m_pLayout->setContentsMargins(0, 0, 0, 0);

        /* Create label: */
        m_pLabel = new QLabel(this);
        if (m_pLabel)
        {
            m_pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_pLayout->addWidget(m_pLabel, 0, 0);
        }

        /* Create combo layout: */
        QHBoxLayout *pLayoutCombo = new QHBoxLayout;
        if (pLayoutCombo)
        {
            /* Create combo: */
            m_pCombo = new QComboBox(this);
            if (m_pCombo)
            {
                if (m_pLabel)
                    m_pLabel->setBuddy(m_pCombo);
                m_pCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
                pLayoutCombo->addWidget(m_pCombo);
            }

            /* Add stretch: */
            pLayoutCombo->addStretch();

            /* Add combo-layout into main-layout: */
            m_pLayout->addLayout(pLayoutCombo, 0, 1);
        }
    }

    /* Populate combo: */
    populateCombo();

    /* Apply language settings: */
    sltRetranslateUI();
}

void UIHostVisibleSizeEditor::populateCombo()
{
    if (!m_pCombo)
        return;

    /* Offer the powers of two the API accepts: */
    m_pCombo->clear();
    for (int iMB = HOST_VISIBLE_SIZE_MIN_MB; iMB <= HOST_VISIBLE_SIZE_MAX_MB; iMB *= 2)
        m_pCombo->addItem(QString(), iMB);

    /* A value from a settings file edited by hand may not be one of them; keep it selectable: */
    int iIndex = m_pCombo->findData(m_iValue);
    if (iIndex == -1)
    {
        m_pCombo->addItem(QString(), m_iValue);
        iIndex = m_pCombo->count() - 1;
    }
    m_pCombo->setCurrentIndex(iIndex);

    /* Retranslate finally: */
    sltRetranslateUI();
}
