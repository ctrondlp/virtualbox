/* $Id$ */
/** @file
 * VBox Qt GUI - UIGpuMemoryLimitEditor class implementation.
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

/* Qt includes: */
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>

/* GUI includes: */
#include "UIGpuMemoryLimitEditor.h"

/* Defines: */
/** The largest limit offered, in megabytes (1 TB, far above any graphics card). */
#define GPU_MEMORY_LIMIT_MAX_MB     1048576
/** The spin-box step, in megabytes. */
#define GPU_MEMORY_LIMIT_STEP_MB    512


UIGpuMemoryLimitEditor::UIGpuMemoryLimitEditor(QWidget *pParent /* = 0 */)
    : UIEditor(pParent)
    , m_iValue(0)
    , m_pLayout(0)
    , m_pLabel(0)
    , m_pSpinBox(0)
{
    prepare();
}

void UIGpuMemoryLimitEditor::setValue(int iValue)
{
    /* Update cached value and spin-box if value has changed: */
    if (m_iValue != iValue)
    {
        m_iValue = iValue;
        if (m_pSpinBox)
            m_pSpinBox->setValue(m_iValue);
    }
}

int UIGpuMemoryLimitEditor::value() const
{
    return m_pSpinBox ? m_pSpinBox->value() : m_iValue;
}

int UIGpuMemoryLimitEditor::minimumLabelHorizontalHint() const
{
    return m_pLabel ? m_pLabel->minimumSizeHint().width() : 0;
}

void UIGpuMemoryLimitEditor::setMinimumLayoutIndent(int iIndent)
{
    if (m_pLayout)
        m_pLayout->setColumnMinimumWidth(0, iIndent);
}

void UIGpuMemoryLimitEditor::sltRetranslateUI()
{
    if (m_pLabel)
        m_pLabel->setText(tr("GPU &Memory Limit"));
    if (m_pSpinBox)
    {
        m_pSpinBox->setSpecialValueText(tr("No limit"));
        m_pSpinBox->setSuffix(QString(" %1").arg(tr("MB")));
        m_pSpinBox->setToolTip(tr("The most memory the Vulkan programs of the guest can allocate on the host graphics "
                                  "card together. An allocation over the limit fails in the guest as it does when the "
                                  "card is full, so the guest cannot take the memory the programs of the host need. "
                                  "0 means no limit."));
    }
}

void UIGpuMemoryLimitEditor::prepare()
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

        /* Create spin-box layout: */
        QHBoxLayout *pLayoutSpinBox = new QHBoxLayout;
        if (pLayoutSpinBox)
        {
            /* Create spin-box: */
            m_pSpinBox = new QSpinBox(this);
            if (m_pSpinBox)
            {
                if (m_pLabel)
                    m_pLabel->setBuddy(m_pSpinBox);
                m_pSpinBox->setRange(0, GPU_MEMORY_LIMIT_MAX_MB);
                m_pSpinBox->setSingleStep(GPU_MEMORY_LIMIT_STEP_MB);
                m_pSpinBox->setValue(m_iValue);
                pLayoutSpinBox->addWidget(m_pSpinBox);
            }

            /* Add stretch: */
            pLayoutSpinBox->addStretch();

            /* Add spin-box-layout into main-layout: */
            m_pLayout->addLayout(pLayoutSpinBox, 0, 1);
        }
    }

    /* Apply language settings: */
    sltRetranslateUI();
}
