/* $Id: UIExtraDataManager.cpp 113623 2026-03-27 12:55:38Z sergey.dubov@oracle.com $ */
/** @file
 * VBox Qt GUI - UIExtraDataManager class implementation.
 */

/*
 * Copyright (C) 2010-2026 Oracle and/or its affiliates.
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
#include <QFontDatabase>
#include <QMetaEnum>
#include <QMutex>
#include <QRegularExpression>

/* GUI includes: */
#include "UICommon.h"
#include "UIConverter.h"
#include "UIDesktopWidgetWatchdog.h"
#include "UIExtraDataManager.h"
#include "UIGlobalSession.h"
#include "UIHostComboEditor.h"
#include "UILocalMachineStuff.h"
#include "UIMainEventListener.h"
#include "UINotificationMessage.h"
#include "UISettingsDefs.h"
#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
# include "UIExtraDataManagerWindow.h"
#endif

/* COM includes: */
#include "CEventListener.h"
#include "CEventSource.h"
#include "CMachine.h"

/* Namespaces: */
using namespace UIExtraDataDefs;
using namespace UISettingsDefs;


/** Private QObject extension
  * providing UIExtraDataManager with the CVirtualBox event-source. */
class UIExtraDataEventHandler : public QObject
{
    Q_OBJECT;

signals:

    /** Notifies about 'extra-data change' event: */
    void sigExtraDataChange(const QUuid &uMachineID, const QString &strKey, const QString &strValue);

public:

    /** Constructs event proxy object on the basis of passed @a pParent. */
    UIExtraDataEventHandler(QObject *pParent);
    /** Destructs event proxy object. */
    ~UIExtraDataEventHandler();

protected slots:

    /** Preprocess 'extra-data can change' event: */
    void sltPreprocessExtraDataCanChange(const QUuid &uMachineID, const QString &strKey, const QString &strValue, bool &fVeto, QString &strVetoReason);
    /** Preprocess 'extra-data change' event: */
    void sltPreprocessExtraDataChange(const QUuid &uMachineID, const QString &strKey, const QString &strValue);

protected:

    /** @name Prepare/Cleanup cascade.
      * @{ */
        /** Prepares all. */
        void prepare();
        /** Prepares listener. */
        void prepareListener();
        /** Prepares connections. */
        void prepareConnections();

        /** Cleanups connections. */
        void cleanupConnections();
        /** Cleanups listener. */
        void cleanupListener();
        /** Cleanups all. */
        void cleanup();
    /** @} */

private:

    /** Holds the Qt event listener instance. */
    ComObjPtr<UIMainEventListenerImpl> m_pQtListener;
    /** Holds the COM event listener instance. */
    CEventListener m_comEventListener;

    /** Protects sltPreprocessExtraDataChange. */
    QMutex m_mutex;
};


/*********************************************************************************************************************************
*   Class UIExtraDataEventHandler implementation.                                                                                *
*********************************************************************************************************************************/

UIExtraDataEventHandler::UIExtraDataEventHandler(QObject *pParent)
    : QObject(pParent)
{
    /* Prepare: */
    prepare();
}

UIExtraDataEventHandler::~UIExtraDataEventHandler()
{
    /* Cleanup: */
    cleanup();
}

void UIExtraDataEventHandler::prepare()
{
    /* Prepare: */
    prepareListener();
    prepareConnections();
}

void UIExtraDataEventHandler::prepareListener()
{
    /* Create event listener instance: */
    m_pQtListener.createObject();
    m_pQtListener->init(new UIMainEventListener, this);
    m_comEventListener = CEventListener(m_pQtListener);

    /* Get VirtualBox: */
    const CVirtualBox comVBox = gpGlobalSession->virtualBox();
    /* Get VirtualBox event source: */
    CEventSource comEventSourceVBox = comVBox.GetEventSource();
    Assert(comVBox.isOk());

    /* Enumerate all the required event-types: */
    QVector<KVBoxEventType> eventTypes;
    eventTypes
        << KVBoxEventType_OnExtraDataCanChange
        << KVBoxEventType_OnExtraDataChanged;

    /* Register event listener for VirtualBox event source: */
    comEventSourceVBox.RegisterListener(m_comEventListener, eventTypes, FALSE /* active? */);
    Assert(comEventSourceVBox.isOk());

    /* Register event sources in their listeners as well: */
    m_pQtListener->getWrapped()->registerSource(comEventSourceVBox, m_comEventListener);
}

void UIExtraDataEventHandler::prepareConnections()
{
    /* Create direct (sync) connections for signals of main listener: */
    connect(m_pQtListener->getWrapped(), &UIMainEventListener::sigExtraDataCanChange,
            this, &UIExtraDataEventHandler::sltPreprocessExtraDataCanChange,
            Qt::DirectConnection);
    connect(m_pQtListener->getWrapped(), &UIMainEventListener::sigExtraDataChange,
            this, &UIExtraDataEventHandler::sltPreprocessExtraDataChange,
            Qt::DirectConnection);
}

void UIExtraDataEventHandler::cleanupConnections()
{
    /* Nothing for now. */
}

void UIExtraDataEventHandler::cleanupListener()
{
    /* Unregister everything: */
    m_pQtListener->getWrapped()->unregisterSources();

    /* Make sure VBoxSVC is available: */
    if (!gpGlobalSession->isVBoxSVCAvailable())
        return;

    /* Get VirtualBox: */
    const CVirtualBox comVBox = gpGlobalSession->virtualBox();
    /* Get VirtualBox event source: */
    CEventSource comEventSourceVBox = comVBox.GetEventSource();
    Assert(comVBox.isOk());

    /* Unregister event listener for VirtualBox event source: */
    comEventSourceVBox.UnregisterListener(m_comEventListener);
}

void UIExtraDataEventHandler::cleanup()
{
    /* Cleanup: */
    cleanupConnections();
    cleanupListener();
}

void UIExtraDataEventHandler::sltPreprocessExtraDataCanChange(const QUuid &uMachineID, const QString &strKey, const QString &/* strValue */, bool & /* fVeto */, QString & /* strVetoReason */)
{
    /* Preprocess global 'extra-data can change' event: */
    if (uMachineID.isNull())
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Check whether global extra-data property can be applied: */
            /// @todo Here can be various extra-data flags handling.
            //       Generally we should check whether one or another flag feats some rule (like reg-exp).
            //       For each required strValue we should set fVeto = true; and fill strVetoReason = "with some text".
        }
    }
}

void UIExtraDataEventHandler::sltPreprocessExtraDataChange(const QUuid &uMachineID, const QString &strKey, const QString &strValue)
{
    /* Preprocess global 'extra-data change' event: */
    if (uMachineID.isNull())
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Apply global extra-data property: */
            /// @todo Here can be various extra-data flags handling.
            //       Generally we should push one or another flag to various instances which want to handle
            //       those flags independently from UIExtraDataManager. Remember to process each required strValue
            //       from under the m_mutex lock (since we are in another thread) and unlock that m_mutex afterwards.
        }
    }

    /* Motify listener about 'extra-data change' event: */
    emit sigExtraDataChange(uMachineID, strKey, strValue);
}


/*********************************************************************************************************************************
*   Class UIExtraDataManager implementation.                                                                                     *
*********************************************************************************************************************************/

/* static */
UIExtraDataManager *UIExtraDataManager::s_pInstance = 0;
const QUuid UIExtraDataManager::GlobalID;

/* static */
void UIExtraDataManager::create()
{
    AssertReturnVoid(!s_pInstance);
    new UIExtraDataManager;
    AssertPtrReturnVoid(s_pInstance);
    s_pInstance->prepare();
}

/* static */
void UIExtraDataManager::destroy()
{
    AssertPtrReturnVoid(s_pInstance);
    s_pInstance->cleanup();
    delete s_pInstance;
    AssertReturnVoid(!s_pInstance);
}

/* static */
UIExtraDataManager *UIExtraDataManager::instance()
{
    return s_pInstance;
}

#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
/* static */
void UIExtraDataManager::openWindow(QWidget *pCenterWidget)
{
    /* Pass to instance: */
    instance()->open(pCenterWidget);
}
#endif /* VBOX_GUI_WITH_EXTRADATA_MANAGER_UI */

void UIExtraDataManager::hotloadMachineExtraDataMap(const QUuid &uID)
{
    /* Make sure it is valid ID: */
    AssertMsgReturnVoid(!uID.isNull() && uID != GlobalID,
                        ("Invalid VM ID = {%s}\n", uID.toString().toUtf8().constData()));
    /* Which is not loaded yet: */
    AssertReturnVoid(!m_data.contains(uID));

    /* Search for corresponding machine: */
    CVirtualBox vbox = gpGlobalSession->virtualBox();
    CMachine machine = vbox.FindMachine(uID.toString());
    if (machine.isNull())
        return;

    /* Make sure at least empty map is created: */
    m_data[uID] = ExtraDataMap();

    /* Do not handle inaccessible machine: */
    if (!machine.GetAccessible())
        return;

    /* Load machine extra-data map: */
    foreach (const QString &strKey, machine.GetExtraDataKeys())
        m_data[uID][strKey] = machine.GetExtraData(strKey);

    /* Notifies about extra-data map acknowledged: */
    emit sigExtraDataMapAcknowledging(uID);
}

QString UIExtraDataManager::extraDataString(const QString &strKey, const QUuid &uID /* = GlobalID */)
{
    /* Get the actual value: */
    QString strValue = extraDataStringUnion(strKey, uID);
    /* If actual value is null we might be able to find old one: */
    if (strValue.isNull())
    {
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            strValue = extraDataStringUnion(strOldKey, uID);
            if (!strValue.isNull())
                break;
        }
    }
    /* Return null string if result is empty: */
    if (strValue.isEmpty())
        return QString();

    /* Returns corresponding value: */
    return strValue;
}

bool UIExtraDataManager::setExtraDataString(const QString &strKey,
                                            const QString &strValue,
                                            const QUuid &uID /* = GlobalID */,
                                            QWidget *pParent /* = 0 */)
{
    /* Make sure VBoxSVC is available: */
    if (!gpGlobalSession->isVBoxSVCAvailable())
        return false;

    /* Prepare result: */
    bool fResult = true;

    /* Hot-load machine extra-data map if necessary: */
    if (uID != GlobalID && !m_data.contains(uID))
        hotloadMachineExtraDataMap(uID);

    /* Access corresponding map: */
    ExtraDataMap &data = m_data[uID];

    /* [Re]cache passed value: */
    data[strKey] = strValue;

    /* Global extra-data: */
    if (uID == GlobalID)
    {
        /* Get global object: */
        CVirtualBox comVBox = gpGlobalSession->virtualBox();
        /* Update global extra-data: */
        comVBox.SetExtraData(strKey, strValue);
        if (!comVBox.isOk())
            fResult = UINotificationMessage::cannotSetExtraData(comVBox, strKey, strValue, pParent);
        /* Wipe out old keys: */
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            comVBox.SetExtraData(strOldKey, QString());
            if (!comVBox.isOk())
            {
                fResult = UINotificationMessage::cannotSetExtraData(comVBox, strOldKey, strValue, pParent);
                break;
            }
        }
    }
    /* Machine extra-data: */
    else
    {
        /* Search for corresponding machine: */
        CVirtualBox comVBox = gpGlobalSession->virtualBox();
        const CMachine comMachine = comVBox.FindMachine(uID.toString());
        AssertReturn(comVBox.isOk() && !comMachine.isNull(), false);
        /* Check the configuration access-level: */
        const KMachineState enmMachineState = comMachine.GetState();
        const KSessionState enmSessionState = comMachine.GetSessionState();
        const ConfigurationAccessLevel enmLevel = configurationAccessLevel(enmSessionState, enmMachineState);
        /* Prepare machine session: */
        CSession comSession;
        if (enmLevel == ConfigurationAccessLevel_Full)
            comSession = openSession(uID);
        else
            comSession = openExistingSession(uID);
        AssertReturn(!comSession.isNull(), false);
        /* Get machine from that session: */
        CMachine comSessionMachine = comSession.GetMachine();
        /* Update machine extra-data: */
        comSessionMachine.SetExtraData(strKey, strValue);
        if (!comSessionMachine.isOk())
            fResult = UINotificationMessage::cannotSetExtraData(comSessionMachine, strKey, strValue, pParent);
        /* Wipe out old keys: */
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            comSessionMachine.SetExtraData(strOldKey, QString());
            if (!comSessionMachine.isOk())
            {
                fResult = UINotificationMessage::cannotSetExtraData(comSessionMachine, strOldKey, strValue, pParent);
                break;
            }
        }
        comSession.UnlockMachine();
    }

    /* Return result: */
    return fResult;
}

QStringList UIExtraDataManager::extraDataStringList(const QString &strKey, const QUuid &uID /* = GlobalID */)
{
    /* Get the actual value: */
    QString strValue = extraDataStringUnion(strKey, uID);
    /* If actual value is null we might be able to find old one: */
    if (strValue.isNull())
    {
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            strValue = extraDataStringUnion(strOldKey, uID);
            if (!strValue.isNull())
                break;
        }
    }
    /* Return empty string list if result is empty: */
    if (strValue.isEmpty())
        return QStringList();

    /* Few old extra-data string-lists were separated with 'semicolon' symbol.
     * All new separated by 'comma'. We have to take that into account. */
    return strValue.split(QRegularExpression("[;,]"), Qt::SkipEmptyParts);
}

bool UIExtraDataManager::setExtraDataStringList(const QString &strKey,
                                                const QStringList &value,
                                                const QUuid &uID /* = GlobalID */,
                                                QWidget *pParent /* = 0 */)
{
    /* Make sure VBoxSVC is available: */
    if (!gpGlobalSession->isVBoxSVCAvailable())
        return false;

    /* Prepare result: */
    bool fResult = true;

    /* Hot-load machine extra-data map if necessary: */
    if (uID != GlobalID && !m_data.contains(uID))
        hotloadMachineExtraDataMap(uID);

    /* Access corresponding map: */
    ExtraDataMap &data = m_data[uID];

    /* [Re]cache passed value: */
    data[strKey] = value.join(",");

    /* Global extra-data: */
    if (uID == GlobalID)
    {
        /* Get global object: */
        CVirtualBox comVBox = gpGlobalSession->virtualBox();
        /* Update global extra-data: */
        comVBox.SetExtraDataStringList(strKey, value);
        if (!comVBox.isOk())
            fResult = UINotificationMessage::cannotSetExtraData(comVBox, strKey, value.join(","), pParent);
        /* Wipe out old keys: */
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            comVBox.SetExtraData(strOldKey, QString());
            if (!comVBox.isOk())
            {
                fResult = UINotificationMessage::cannotSetExtraData(comVBox, strOldKey, value.join(","), pParent);
                break;
            }
        }
    }
    /* Machine extra-data: */
    else
    {
        /* Search for corresponding machine: */
        CVirtualBox comVBox = gpGlobalSession->virtualBox();
        const CMachine comMachine = comVBox.FindMachine(uID.toString());
        AssertReturn(comVBox.isOk() && !comMachine.isNull(), false);
        /* Check the configuration access-level: */
        const KMachineState enmMachineState = comMachine.GetState();
        const KSessionState enmSessionState = comMachine.GetSessionState();
        const ConfigurationAccessLevel enmLevel = configurationAccessLevel(enmSessionState, enmMachineState);
        /* Prepare machine session: */
        CSession comSession;
        if (enmLevel == ConfigurationAccessLevel_Full)
            comSession = openSession(uID);
        else
            comSession = openExistingSession(uID);
        AssertReturn(!comSession.isNull(), false);
        /* Get machine from that session: */
        CMachine comSessionMachine = comSession.GetMachine();
        /* Update machine extra-data: */
        comSessionMachine.SetExtraDataStringList(strKey, value);
        if (!comSessionMachine.isOk())
            fResult = UINotificationMessage::cannotSetExtraData(comSessionMachine, strKey, value.join(","), pParent);
        /* Wipe out old keys: */
        foreach (const QString &strOldKey, g_mapOfObsoleteKeys.values(strKey))
        {
            comSessionMachine.SetExtraData(strOldKey, QString());
            if (!comSessionMachine.isOk())
            {
                fResult = UINotificationMessage::cannotSetExtraData(comSessionMachine, strOldKey, value.join(","), pParent);
                break;
            }
        }
        comSession.UnlockMachine();
    }

    /* Return result: */
    return fResult;
}

UIExtraDataManager::UIExtraDataManager()
    : m_pHandler(0)
{
    /* Connect to static instance: */
    s_pInstance = this;
}

UIExtraDataManager::~UIExtraDataManager()
{
    /* Disconnect from static instance: */
    s_pInstance = 0;
}

UIExtraDataMetaDefs::DialogType UIExtraDataManager::restrictedDialogTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::DialogType result = UIExtraDataMetaDefs::DialogType_Invalid;
    /* Get restricted runtime-menu-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedDialogs, uID))
    {
        UIExtraDataMetaDefs::DialogType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::DialogType>(strValue);
        if (value != UIExtraDataMetaDefs::DialogType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::DialogType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedDialogTypes(UIExtraDataMetaDefs::DialogType dialogs, const QUuid &uID)
{
    /* We have MenuType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("DialogType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle DialogType_All enum-value: */
    if (dialogs == UIExtraDataMetaDefs::DialogType_All)
        result << gpConverter->toInternalString(dialogs);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::DialogType enumValue =
                static_cast<UIExtraDataMetaDefs::DialogType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip DialogType_Invalid & DialogType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::DialogType_Invalid ||
                enumValue == UIExtraDataMetaDefs::DialogType_All)
                continue;
            if (dialogs & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedDialogs, result, uID);
}

UIColorThemeType UIExtraDataManager::colorTheme()
{
    return gpConverter->fromInternalString<UIColorThemeType>(extraDataString(GUI_ColorTheme));
}

bool UIExtraDataManager::setColorTheme(const UIColorThemeType &enmType, QWidget *pParent)
{
    return setExtraDataString(GUI_ColorTheme, gpConverter->toInternalString(enmType), GlobalID, pParent);
}

QStringList UIExtraDataManager::suppressedMessages(const QUuid &uID /* = GlobalID */)
{
    return extraDataStringList(GUI_SuppressMessages, uID);
}

bool UIExtraDataManager::setSuppressedMessages(const QStringList &list)
{
    return setExtraDataStringList(GUI_SuppressMessages, list);
}

#ifdef VBOX_NOTIFICATION_CENTER_WITH_KEEP_BUTTON
bool UIExtraDataManager::keepSuccessfullNotificationProgresses()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_NotificationCenter_KeepSuccessfullProgresses);
}

bool UIExtraDataManager::setKeepSuccessfullNotificationProgresses(bool fKeep)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_NotificationCenter_KeepSuccessfullProgresses, toFeatureAllowed(fKeep));
}
#endif /* VBOX_NOTIFICATION_CENTER_WITH_KEEP_BUTTON */

Qt::Alignment UIExtraDataManager::notificationCenterAlignment()
{
    const QString strValue = extraDataString(GUI_NotificationCenter_Alignment);
    return strValue.isEmpty() ? Qt::AlignTop : gpConverter->fromInternalString<Qt::Alignment>(strValue);
}

bool UIExtraDataManager::setNotificationCenterAlignment(Qt::Alignment enmOrder)
{
    const QString strValue = enmOrder == Qt::AlignTop ? QString() : gpConverter->toInternalString(enmOrder);
    return setExtraDataString(GUI_NotificationCenter_Alignment, strValue);
}

Qt::SortOrder UIExtraDataManager::notificationCenterOrder()
{
    const QString strValue = extraDataString(GUI_NotificationCenter_Order);
    return strValue.isEmpty() ? Qt::DescendingOrder : gpConverter->fromInternalString<Qt::SortOrder>(strValue);
}

bool UIExtraDataManager::setNotificationCenterOrder(Qt::SortOrder enmOrder)
{
    const QString strValue = enmOrder == Qt::DescendingOrder ? QString() : gpConverter->toInternalString(enmOrder);
    return setExtraDataString(GUI_NotificationCenter_Order, strValue);
}

bool UIExtraDataManager::preventBetaBuildLabel()
{
    return isFeatureAllowed(GUI_PreventBetaLabel);
}

#if !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG)
QString UIExtraDataManager::preventBetaBuildWarningForVersion()
{
    return extraDataString(GUI_PreventBetaWarning);
}
#endif /* !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG) */

#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
bool UIExtraDataManager::applicationUpdateEnabled()
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventApplicationUpdate);
}

QString UIExtraDataManager::applicationUpdateData()
{
    return extraDataString(GUI_UpdateDate);
}

bool UIExtraDataManager::setApplicationUpdateData(const QString &strValue, QWidget *pParent)
{
    return setExtraDataString(GUI_UpdateDate, strValue, GlobalID, pParent);
}

qulonglong UIExtraDataManager::applicationUpdateCheckCounter()
{
    /* Read subsequent update check counter value: */
    qulonglong uResult = 1;
    const QString strCheckCount = extraDataString(GUI_UpdateCheckCount);
    if (!strCheckCount.isEmpty())
    {
        bool ok = false;
        qulonglong uCheckCount = strCheckCount.toULongLong(&ok);
        if (ok) uResult = uCheckCount;
    }
    /* Return update check counter value: */
    return uResult;
}

bool UIExtraDataManager::incrementApplicationUpdateCheckCounter()
{
    /* Increment update check counter value: */
    return setExtraDataString(GUI_UpdateCheckCount, QString::number(applicationUpdateCheckCounter() + 1));
}
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */

bool UIExtraDataManager::legacyProgressHandlingRequested()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_Progress_LegacyMode);
}

bool UIExtraDataManager::guiFeatureEnabled(GUIFeatureType enmFeature)
{
    /* Acquire GUI feature list: */
    GUIFeatureType enmFeatures = GUIFeatureType_None;
    foreach (const QString &strValue, extraDataStringList(GUI_Customizations))
        enmFeatures = static_cast<GUIFeatureType>(enmFeatures | gpConverter->fromInternalString<GUIFeatureType>(strValue));
    /* Return whether the requested feature is enabled: */
    return enmFeatures & enmFeature;
}

QList<GlobalSettingsPageType> UIExtraDataManager::restrictedGlobalSettingsPages()
{
    /* Prepare result: */
    QList<GlobalSettingsPageType> result;
    /* Get restricted global-settings-pages: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedGlobalSettingsPages))
    {
        GlobalSettingsPageType value = gpConverter->fromInternalString<GlobalSettingsPageType>(strValue);
        if (value != GlobalSettingsPageType_Invalid)
            result << value;
    }
    /* Return result: */
    return result;
}

QList<MachineSettingsPageType> UIExtraDataManager::restrictedMachineSettingsPages(const QUuid &uID)
{
    /* Prepare result: */
    QList<MachineSettingsPageType> result;
    /* Get restricted machine-settings-pages: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedMachineSettingsPages, uID))
    {
        MachineSettingsPageType value = gpConverter->fromInternalString<MachineSettingsPageType>(strValue);
        if (value != MachineSettingsPageType_Invalid)
            result << value;
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::isSettingsInExpertMode()
{
#ifdef DEBUG
    /* For debug build return true by default: */
    if (extraDataString(GUI_Settings_ExpertMode).isEmpty())
        return true;
#endif

    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_Settings_ExpertMode);
}

bool UIExtraDataManager::setSettingsInExpertMode(bool fExpertMode, QWidget *pParent /* = 0 */)
{
    /* Store actual feature state, whether it is "true" or "false",
     * because absent state means default, depending on defines: */
    return setExtraDataString(GUI_Settings_ExpertMode, toFeatureState(fExpertMode), GlobalID, pParent);
}

QString UIExtraDataManager::languageId()
{
    /* Load language ID: */
    return extraDataString(GUI_LanguageID);
}

bool UIExtraDataManager::setLanguageId(const QString &strLanguageId, QWidget *pParent)
{
    /* Save language ID: */
    return setExtraDataString(GUI_LanguageID, strLanguageId, GlobalID, pParent);
}

MaximumGuestScreenSizePolicy UIExtraDataManager::maxGuestResolutionPolicy()
{
    /* Return maximum guest-screen resolution policy: */
    return gpConverter->fromInternalString<MaximumGuestScreenSizePolicy>(extraDataString(GUI_MaxGuestResolution));
}

bool UIExtraDataManager::setMaxGuestScreenResolution(MaximumGuestScreenSizePolicy enmPolicy,
                                                     const QSize &resolution /* = QSize() */,
                                                     QWidget *pParent /* = 0 */)
{
    /* If policy is 'Fixed' => call the wrapper: */
    if (enmPolicy == MaximumGuestScreenSizePolicy_Fixed)
        return setMaxGuestResolutionForPolicyFixed(resolution, pParent);
    /* Otherwise => just store the value: */
    else
        return setExtraDataString(GUI_MaxGuestResolution,
                                  gpConverter->toInternalString(enmPolicy),
                                  GlobalID,
                                  pParent);
}

QSize UIExtraDataManager::maxGuestResolutionForPolicyFixed()
{
    /* Acquire maximum guest-screen resolution policy: */
    const QString strPolicy = extraDataString(GUI_MaxGuestResolution);
    const MaximumGuestScreenSizePolicy enmPolicy = gpConverter->fromInternalString<MaximumGuestScreenSizePolicy>(strPolicy);

    /* Make sure maximum guest-screen resolution policy is really Fixed: */
    if (enmPolicy != MaximumGuestScreenSizePolicy_Fixed)
        return QSize();

    /* Parse maximum guest-screen resolution: */
    const QStringList values = strPolicy.split(',');
    int iWidth = values.at(0).toInt();
    int iHeight = values.at(1).toInt();
    if (iWidth <= 0)
        iWidth = 640;
    if (iHeight <= 0)
        iHeight = 480;

    /* Return maximum guest-screen resolution: */
    return QSize(iWidth, iHeight);
}

bool UIExtraDataManager::setMaxGuestResolutionForPolicyFixed(const QSize &resolution, QWidget *pParent)
{
    /* If resolution is 'empty' => call the wrapper: */
    if (resolution.isEmpty())
        return setMaxGuestScreenResolution(MaximumGuestScreenSizePolicy_Automatic,
                                           QSize(),
                                           pParent);
    /* Otherwise => just store the value: */
    else
        return setExtraDataString(GUI_MaxGuestResolution,
                                  QString("%1,%2").arg(resolution.width()).arg(resolution.height()),
                                  GlobalID,
                                  pParent);
}

bool UIExtraDataManager::activateHoveredMachineWindow()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_ActivateHoveredMachineWindow);
}

bool UIExtraDataManager::setActivateHoveredMachineWindow(bool fActivate, QWidget *pParent)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_ActivateHoveredMachineWindow, toFeatureAllowed(fActivate), GlobalID, pParent);
}

bool UIExtraDataManager::disableHostScreenSaver()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_DisableHostScreenSaver);
}

bool UIExtraDataManager::setDisableHostScreenSaver(bool fActivate, QWidget *pParent)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_DisableHostScreenSaver, toFeatureAllowed(fActivate), GlobalID, pParent);
}

QString UIExtraDataManager::hostKeyCombination()
{
    /* Acquire host-key combination: */
    QString strHostCombo = extraDataString(GUI_Input_HostKeyCombination);
    /* Invent some sane default if it's absolutely wrong or invalid: */
    QRegularExpression reTemplate("0|[1-9]\\d*(,[1-9]\\d*)?(,[1-9]\\d*)?");
    if (!reTemplate.match(strHostCombo).hasMatch() || !UIHostCombo::isValidKeyCombo(strHostCombo))
    {
#if   defined (VBOX_WS_MAC)
        strHostCombo = "55"; // QZ_LMETA
#elif defined (VBOX_WS_WIN)
        strHostCombo = "163"; // VK_RCONTROL
#elif defined (VBOX_WS_NIX)
        strHostCombo = "65508"; // XK_Control_R
#else
# warning "port me!"
#endif
    }
    /* Return host-key combination: */
    return strHostCombo;
}

bool UIExtraDataManager::setFontScaleFactor(int iFontScaleFactor, QWidget *pParent)
{
    if (iFontScaleFactor < UIExtraDataDefs::iFontScaleMin || iFontScaleFactor > UIExtraDataDefs::iFontScaleMax)
        return false;
    return setExtraDataString(GUI_FontScaleFactor, QString::number(iFontScaleFactor), GlobalID, pParent);
}

int UIExtraDataManager::fontScaleFactor()
{
    QString strFontScaleFactor = extraDataString(GUI_FontScaleFactor);
    bool fConversion = false;
    int iScaleFactor = strFontScaleFactor.toInt(&fConversion);
    if (!fConversion || iScaleFactor < UIExtraDataDefs::iFontScaleMin || iScaleFactor > UIExtraDataDefs::iFontScaleMax)
        return 100;
    return iScaleFactor;
}

bool UIExtraDataManager::setHostKeyCombination(const QString &strHostCombo, QWidget *pParent)
{
    /* Do not save anything if it's absolutely wrong or invalid: */
    QRegularExpression reTemplate("0|[1-9]\\d*(,[1-9]\\d*)?(,[1-9]\\d*)?");
    if (!reTemplate.match(strHostCombo).hasMatch() || !UIHostCombo::isValidKeyCombo(strHostCombo))
        return false;
    /* Define host-combo: */
    return setExtraDataString(GUI_Input_HostKeyCombination, strHostCombo, GlobalID, pParent);
}

QStringList UIExtraDataManager::shortcutOverrides(const QString &strPoolExtraDataID)
{
    if (strPoolExtraDataID == GUI_Input_SelectorShortcuts)
        return extraDataStringList(GUI_Input_SelectorShortcuts);
    if (strPoolExtraDataID == GUI_Input_MachineShortcuts)
        return extraDataStringList(GUI_Input_MachineShortcuts);
    return QStringList();
}

bool UIExtraDataManager::autoCaptureEnabled()
{
    /* Prepare auto-capture flag: */
    bool fAutoCapture = true /* indifferently */;
    /* Acquire whether the auto-capture is restricted: */
    QString strAutoCapture = extraDataString(GUI_Input_AutoCapture);
    /* Invent some sane default if it's empty: */
    if (strAutoCapture.isEmpty())
    {
#if defined(VBOX_WS_NIX) && defined(DEBUG)
        fAutoCapture = false;
#else
        fAutoCapture = true;
#endif
    }
    /* 'True' unless feature restricted: */
    else
        fAutoCapture = !isFeatureRestricted(GUI_Input_AutoCapture);
    /* Return auto-capture flag: */
    return fAutoCapture;
}

bool UIExtraDataManager::setAutoCaptureEnabled(bool fEnabled, QWidget *pParent)
{
    /* Store actual feature state, whether it is "true" or "false",
     * because absent state means default, different on various hosts: */
    return setExtraDataString(GUI_Input_AutoCapture, toFeatureState(fEnabled), GlobalID, pParent);
}

QString UIExtraDataManager::remappedScanCodes()
{
    /* Acquire remapped scan codes: */
    QString strRemappedScanCodes = extraDataString(GUI_RemapScancodes);
    /* Clear the record if it's absolutely wrong: */
    QRegularExpression reTemplate("(\\d+=\\d+,)*\\d+=\\d+");
    if (!reTemplate.match(strRemappedScanCodes).hasMatch())
        strRemappedScanCodes.clear();
    /* Return remapped scan codes: */
    return strRemappedScanCodes;
}

QString UIExtraDataManager::proxySettings()
{
    return extraDataString(GUI_ProxySettings);
}

bool UIExtraDataManager::setProxySettings(const QString &strSettings, QWidget *pParent)
{
    return setExtraDataString(GUI_ProxySettings, strSettings, GlobalID, pParent);
}

QString UIExtraDataManager::recentFolderForHardDrives()
{
    return extraDataString(GUI_RecentFolderHD);
}

QString UIExtraDataManager::recentFolderForOpticalDisks()
{
    return extraDataString(GUI_RecentFolderCD);
}

QString UIExtraDataManager::recentFolderForFloppyDisks()
{
    return extraDataString(GUI_RecentFolderFD);
}

bool UIExtraDataManager::setRecentFolderForHardDrives(const QString &strValue)
{
    return setExtraDataString(GUI_RecentFolderHD, strValue);
}

bool UIExtraDataManager::setRecentFolderForOpticalDisks(const QString &strValue)
{
    return setExtraDataString(GUI_RecentFolderCD, strValue);
}

bool UIExtraDataManager::setRecentFolderForFloppyDisks(const QString &strValue)
{
    return setExtraDataString(GUI_RecentFolderFD, strValue);
}

QStringList UIExtraDataManager::recentListOfHardDrives()
{
    return extraDataStringList(GUI_RecentListHD);
}

QStringList UIExtraDataManager::recentListOfOpticalDisks()
{
    return extraDataStringList(GUI_RecentListCD);
}

QStringList UIExtraDataManager::recentListOfFloppyDisks()
{
    return extraDataStringList(GUI_RecentListFD);
}

bool UIExtraDataManager::setRecentListOfHardDrives(const QStringList &value)
{
    return setExtraDataStringList(GUI_RecentListHD, value);
}

bool UIExtraDataManager::setRecentListOfOpticalDisks(const QStringList &value)
{
    return setExtraDataStringList(GUI_RecentListCD, value);
}

bool UIExtraDataManager::setRecentListOfFloppyDisks(const QStringList &value)
{
    return setExtraDataStringList(GUI_RecentListFD, value);
}

UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork UIExtraDataManager::restrictedNetworkAttachmentTypes()
{
    /* Prepare result: */
    UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork enmResult =
        UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork_Invalid;
    /* Get restricted network attachment types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedNetworkAttachmentTypes))
    {
        const UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork enmValue =
            gpConverter->fromInternalString<UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork>(strValue);
        if (enmValue != UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork_Invalid && !(enmResult & enmValue))
            enmResult = static_cast<UIExtraDataMetaDefs::DetailsElementOptionTypeNetwork>(enmResult | enmValue);
    }
    /* Return result: */
    return enmResult;
}

QString UIExtraDataManager::visoCreatorRecentFolder()
{
    return extraDataString(GUI_VISOCreator_RecentFolder);
}

bool UIExtraDataManager::setVISOCreatorRecentFolder(const QString &strValue)
{
    return setExtraDataString(GUI_VISOCreator_RecentFolder, strValue);
}

QRect UIExtraDataManager::visoCreatorDialogGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_VISOCreator_DialogGeometry, pWidget, pParentWidget, defaultGeometry);
}

void UIExtraDataManager::setVisoCreatorDialogGeometry(const QRect &geometry, bool fMaximized)
{
    setDialogGeometry(GUI_VISOCreator_DialogGeometry, geometry, fMaximized);
}

bool UIExtraDataManager::visoCreatorDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_VISOCreator_DialogGeometry);
}

QRect UIExtraDataManager::selectorWindowGeometry(QWidget *pWidget)
{
    return dialogGeometry(GUI_LastSelectorWindowPosition, pWidget);
}

bool UIExtraDataManager::selectorWindowShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_LastSelectorWindowPosition);
}

bool UIExtraDataManager::setSelectorWindowGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_LastSelectorWindowPosition, data);
}

QList<int> UIExtraDataManager::selectorWindowSplitterHints()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_SplitterSizes);

    /* Parse loaded data: */
    QList<int> hints;
    hints << (data.size() > 0 ? data[0].toInt() : 0);
    hints << (data.size() > 1 ? data[1].toInt() : 0);

    /* Return hints: */
    return hints;
}

bool UIExtraDataManager::setSelectorWindowSplitterHints(const QList<int> &hints)
{
    /* Parse passed hints: */
    QStringList data;
    data << (hints.size() > 0 ? QString::number(hints[0]) : QString());
    data << (hints.size() > 1 ? QString::number(hints[1]) : QString());

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_SplitterSizes, data);
}

bool UIExtraDataManager::selectorWindowToolBarVisible()
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Toolbar);
}

bool UIExtraDataManager::setSelectorWindowToolBarVisible(bool fVisible)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_Toolbar, toFeatureRestricted(!fVisible));
}

bool UIExtraDataManager::selectorWindowToolBarTextVisible()
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Toolbar_Text);
}

bool UIExtraDataManager::setSelectorWindowToolBarTextVisible(bool fVisible)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_Toolbar_Text, toFeatureRestricted(!fVisible));
}

void UIExtraDataManager::toolsPaneLastItemsChosen(UIToolType &enmTypeGlobal,
                                                  UIToolType &enmTypeMachine)
{
    /* Parse loaded data: */
    QList<UIToolType> result;
    foreach (const QString &strValue, extraDataStringList(GUI_Tools_LastItemsSelected))
    {
        const UIToolType enmType = gpConverter->fromInternalString<UIToolType>(strValue);
        if (enmType != UIToolType_Invalid)
            result << enmType;
    }

    /* Assign values: */
    enmTypeGlobal = result.value(0);
    if (!UIToolStuff::isTypeOfClass(enmTypeGlobal, UIToolClass_Global))
        enmTypeGlobal = UIToolType_Home;
    enmTypeMachine = result.value(1);
    if (!UIToolStuff::isTypeOfClass(enmTypeMachine, UIToolClass_Machine))
        enmTypeMachine = UIToolType_Details;
}

bool UIExtraDataManager::setToolsPaneLastItemsChosen(UIToolType enmTypeGlobal,
                                                     UIToolType enmTypeMachine)
{
    /* Serialize passed values: */
    const QList<UIToolType> currentTypes = QList<UIToolType>()
                                         << enmTypeGlobal
                                         << enmTypeMachine;
    QStringList data;
    foreach (const UIToolType &enmType, currentTypes)
        data << gpConverter->toInternalString(enmType);

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_Tools_LastItemsSelected, data);
}

QList<UIToolType> UIExtraDataManager::detachedTools()
{
    /* Parse loaded data: */
    QList<UIToolType> result;
    foreach (const QString &strValue, extraDataStringList(GUI_Tools_Detached))
    {
        const UIToolType enmType = gpConverter->fromInternalString<UIToolType>(strValue);
        if (enmType != UIToolType_Invalid)
            result << enmType;
    }

    /* Return result: */
    return result;
}

bool UIExtraDataManager::setDetachedTools(const QList<UIToolType> &tools)
{
    /* Serialize passed values: */
    QStringList data;
    foreach (const UIToolType &enmType, tools)
        data << gpConverter->toInternalString(enmType);

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_Tools_Detached, data);
}

bool UIExtraDataManager::isToolTextVisible()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_Tools_Text);
}

bool UIExtraDataManager::setToolTextVisible(bool fVisible)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_Tools_Text, toFeatureAllowed(fVisible));
}

UIChooserItemSize UIExtraDataManager::chooserItemSize()
{
    /* 'Large' unless another known size is stored: */
    const QString strSize = extraDataString(GUI_Chooser_ItemSize);
    if (strSize.compare("Small", Qt::CaseInsensitive) == 0)
        return UIChooserItemSize_Small;
    if (strSize.compare("Medium", Qt::CaseInsensitive) == 0)
        return UIChooserItemSize_Medium;
    return UIChooserItemSize_Large;
}

bool UIExtraDataManager::setChooserItemSize(UIChooserItemSize enmSize)
{
    /* Null-string (key removed) for the default size: */
    return setExtraDataString(GUI_Chooser_ItemSize,
                                enmSize == UIChooserItemSize_Small  ? QString("Small")
                              : enmSize == UIChooserItemSize_Medium ? QString("Medium")
                              : QString());
}

bool UIExtraDataManager::selectorWindowStatusBarVisible()
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Statusbar);
}

bool UIExtraDataManager::setSelectorWindowStatusBarVisible(bool fVisible)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_Statusbar, toFeatureRestricted(!fVisible));
}

QStringList UIExtraDataManager::knownMachineGroupDefinitionKeys()
{
    /* Acquire a list of known group definition keys: */
    QStringList result;
    foreach (const QString &strKey, m_data.value(GlobalID).keys())
        if (strKey.startsWith(GUI_GroupDefinitions))
        {
            QString strGroupID = strKey;
            strGroupID.remove(GUI_GroupDefinitions);
            result << strGroupID;
        }

    /* Return result: */
    return result;
}

QStringList UIExtraDataManager::machineGroupDefinitions(const QString &strGroupID)
{
    return extraDataStringList(GUI_GroupDefinitions + strGroupID);
}

bool UIExtraDataManager::setMachineGroupDefinitions(const QString &strGroupID, const QStringList &definitions)
{
    return setExtraDataStringList(GUI_GroupDefinitions + strGroupID, definitions);
}

QString UIExtraDataManager::selectorWindowLastItemChosen()
{
    return extraDataString(GUI_LastItemSelected);
}

bool UIExtraDataManager::setSelectorWindowLastItemChosen(const QString &strItemID)
{
    return setExtraDataString(GUI_LastItemSelected, strItemID);
}

QMap<DetailsElementType, bool> UIExtraDataManager::selectorWindowDetailsElements()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_Details_Elements);

    /* Desearialize passed elements: */
    QMap<DetailsElementType, bool> elements;
    foreach (QString strItem, data)
    {
        bool fOpened = true;
        if (strItem.endsWith("Closed", Qt::CaseInsensitive))
        {
            fOpened = false;
            strItem.remove("Closed", Qt::CaseInsensitive);
        }
        const DetailsElementType enmType = gpConverter->fromInternalString<DetailsElementType>(strItem);
        if (enmType != DetailsElementType_Invalid)
            elements[enmType] = fOpened;
    }

    /* If settings are empty: */
    if (elements.isEmpty())
    {
        /* Propose the defaults: */
        elements[DetailsElementType_General] = true;
        elements[DetailsElementType_Preview] = true;
        elements[DetailsElementType_System] = true;
        elements[DetailsElementType_Display] = true;
        elements[DetailsElementType_Storage] = true;
        elements[DetailsElementType_Audio] = true;
        elements[DetailsElementType_Network] = true;
        elements[DetailsElementType_USB] = true;
        elements[DetailsElementType_SF] = true;
        elements[DetailsElementType_Description] = true;
    }

    /* Return elements: */
    return elements;
}

bool UIExtraDataManager::setSelectorWindowDetailsElements(const QMap<DetailsElementType, bool> &elements)
{
    /* Prepare corresponding extra-data: */
    QStringList data;

    /* Searialize passed elements: */
    foreach (DetailsElementType enmType, elements.keys())
    {
        QString strValue = gpConverter->toInternalString(enmType);
        if (!elements[enmType])
            strValue += "Closed";
        data << strValue;
    }

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_Details_Elements, data);
}

PreviewUpdateIntervalType UIExtraDataManager::selectorWindowPreviewUpdateInterval()
{
    return gpConverter->fromInternalString<PreviewUpdateIntervalType>(extraDataString(GUI_Details_Elements_Preview_UpdateInterval));
}

bool UIExtraDataManager::setSelectorWindowPreviewUpdateInterval(PreviewUpdateIntervalType interval)
{
    return setExtraDataString(GUI_Details_Elements_Preview_UpdateInterval, gpConverter->toInternalString(interval));
}

QStringList UIExtraDataManager::vboxManagerDetailsPaneElementOptions(DetailsElementType enmElementType)
{
    /* Compose full key from GUI_Details_Elements and enmElementType: */
    QString strElementType = gpConverter->toInternalString(enmElementType);
    AssertReturn(!strElementType.isEmpty(), QStringList());
    strElementType[0] = strElementType.at(0).toUpper();
    const QString strFullKey = QString("%1/%2").arg(GUI_Details_Elements).arg(strElementType);

    /* Return option list: */
    return extraDataStringList(strFullKey);
}

bool UIExtraDataManager::setVBoxManagerDetailsPaneElementOptions(DetailsElementType enmElementType, const QStringList &options)
{
    /* Compose full key from GUI_Details_Elements and enmElementType: */
    QString strElementType = gpConverter->toInternalString(enmElementType);
    AssertReturn(!strElementType.isEmpty(), false);
    strElementType[0] = strElementType.at(0).toUpper();
    const QString strFullKey = QString("%1/%2").arg(GUI_Details_Elements).arg(strElementType);

    /* Store option list: */
    return setExtraDataStringList(strFullKey, options);
}

bool UIExtraDataManager::snapshotManagerDetailsExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_SnapshotManager_Details_Expanded);
}

bool UIExtraDataManager::setSnapshotManagerDetailsExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_SnapshotManager_Details_Expanded, toFeatureAllowed(fExpanded));
}

bool UIExtraDataManager::virtualMediaManagerDetailsExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_VirtualMediaManager_Details_Expanded);
}

bool UIExtraDataManager::setVirtualMediaManagerDetailsExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_VirtualMediaManager_Details_Expanded, toFeatureAllowed(fExpanded));
}

bool UIExtraDataManager::virtualMediaManagerSearchWidgetExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_VirtualMediaManager_Search_Widget_Expanded);
}

bool UIExtraDataManager::setVirtualMediaManagerSearchWidgetExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_VirtualMediaManager_Search_Widget_Expanded, toFeatureAllowed(fExpanded));
}

bool UIExtraDataManager::hostNetworkManagerDetailsExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_HostNetworkManager_Details_Expanded);
}

bool UIExtraDataManager::setHostNetworkManagerDetailsExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_HostNetworkManager_Details_Expanded, toFeatureAllowed(fExpanded));
}

QStringList UIExtraDataManager::cloudProfileManagerRestrictions()
{
    return extraDataStringList(GUI_CloudProfileManager_Restrictions);
}

bool UIExtraDataManager::setCloudProfileManagerRestrictions(const QStringList &restrictions)
{
    return setExtraDataStringList(GUI_CloudProfileManager_Restrictions, restrictions);
}

bool UIExtraDataManager::cloudProfileManagerDetailsExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_CloudProfileManager_Details_Expanded);
}

bool UIExtraDataManager::setCloudProfileManagerDetailsExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_CloudProfileManager_Details_Expanded, toFeatureAllowed(fExpanded));
}

QStringList UIExtraDataManager::cloudConsoleManagerApplications()
{
    /* Gather a list of keys matching required expression: */
    QStringList result;
    const QRegularExpression re(QString("^%1/([^/]+)$").arg(GUI_CloudConsoleManager_Application));
    foreach (const QString &strKey, m_data.value(GlobalID).keys())
    {
        const QRegularExpressionMatch mt = re.match(strKey);
        if (mt.hasMatch())
            result << mt.captured(1);
    }
    return result;
}

QStringList UIExtraDataManager::cloudConsoleManagerProfiles(const QString &strId)
{
    /* Gather a list of keys matching required expression: */
    QStringList result;
    const QRegularExpression re(QString("^%1/%2/([^/]+)$").arg(GUI_CloudConsoleManager_Application, strId));
    foreach (const QString &strKey, m_data.value(GlobalID).keys())
    {
        const QRegularExpressionMatch mt = re.match(strKey);
        if (mt.hasMatch())
            result << mt.captured(1);
    }
    return result;
}

QString UIExtraDataManager::cloudConsoleManagerApplication(const QString &strId)
{
    return extraDataString(QString("%1/%2").arg(GUI_CloudConsoleManager_Application, strId));
}

bool UIExtraDataManager::setCloudConsoleManagerApplication(const QString &strId, const QString &strDefinition)
{
    return setExtraDataString(QString("%1/%2").arg(GUI_CloudConsoleManager_Application, strId), strDefinition);
}

QString UIExtraDataManager::cloudConsoleManagerProfile(const QString &strApplicationId, const QString &strProfileId)
{
    return extraDataString(QString("%1/%2/%3").arg(GUI_CloudConsoleManager_Application, strApplicationId, strProfileId));
}

bool UIExtraDataManager::setCloudConsoleManagerProfile(const QString &strApplicationId, const QString &strProfileId, const QString &strDefinition)
{
    return setExtraDataString(QString("%1/%2/%3").arg(GUI_CloudConsoleManager_Application, strApplicationId, strProfileId), strDefinition);
}

QStringList UIExtraDataManager::cloudConsoleManagerRestrictions()
{
    return extraDataStringList(GUI_CloudConsoleManager_Restrictions);
}

bool UIExtraDataManager::setCloudConsoleManagerRestrictions(const QStringList &restrictions)
{
    return setExtraDataStringList(GUI_CloudConsoleManager_Restrictions, restrictions);
}

bool UIExtraDataManager::cloudConsoleManagerDetailsExpanded()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_CloudConsoleManager_Details_Expanded);
}

bool UIExtraDataManager::setCloudConsoleManagerDetailsExpanded(bool fExpanded)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_CloudConsoleManager_Details_Expanded, toFeatureAllowed(fExpanded));
}

QString UIExtraDataManager::cloudConsolePublicKeyPath()
{
    return extraDataString(GUI_CloudConsole_PublicKey_Path);
}

bool UIExtraDataManager::setCloudConsolePublicKeyPath(const QString &strPath)
{
    return setExtraDataString(GUI_CloudConsole_PublicKey_Path, strPath);
}

bool UIExtraDataManager::showMachineInVirtualBoxManagerChooser(const QUuid &uID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_HideFromManager, uID);
}

bool UIExtraDataManager::showMachineInVirtualBoxManagerDetails(const QUuid &uID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_HideDetails, uID);
}

bool UIExtraDataManager::machineReconfigurationEnabled(const QUuid &uID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventReconfiguration, uID);
}

bool UIExtraDataManager::machineSnapshotOperationsEnabled(const QUuid &uID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventSnapshotOperations, uID);
}

QStringList UIExtraDataManager::machineWindowIconNames(const QUuid &uID)
{
    return extraDataStringList(GUI_MachineWindowIcons, uID);
}

#ifndef VBOX_WS_MAC
QString UIExtraDataManager::machineWindowNamePostfix(const QUuid &uID)
{
    return extraDataString(GUI_MachineWindowNamePostfix, uID);
}
#endif /* !VBOX_WS_MAC */

QRect UIExtraDataManager::machineWindowGeometry(UIVisualStateType visualStateType, ulong uScreenIndex, const QUuid &uID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturn(QRect());
    }

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, uID);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Return geometry (loaded or null): */
    return fOk ? QRect(iX, iY, iW, iH) : QRect();
}

bool UIExtraDataManager::machineWindowShouldBeMaximized(UIVisualStateType visualStateType, ulong uScreenIndex, const QUuid &uID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturn(false);
    }

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, uID);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

bool UIExtraDataManager::setMachineWindowGeometry(UIVisualStateType visualStateType, ulong uScreenIndex, const QRect &geometry, bool fMaximized, const QUuid &uID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturn(false);
    }

    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(strKey, data, uID);
}

#ifndef VBOX_WS_MAC
bool UIExtraDataManager::menuBarEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_MenuBar_Enabled, uID);
}

bool UIExtraDataManager::setMenuBarEnabled(bool fEnabled, const QUuid &uID, QWidget *pParent /* = 0 */)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_MenuBar_Enabled, toFeatureRestricted(!fEnabled), uID, pParent);
}
#endif /* !VBOX_WS_MAC */

bool UIExtraDataManager::menuBarContextMenuEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_MenuBar_ContextMenu_Enabled, uID);
}

bool UIExtraDataManager::setMenuBarContextMenuEnabled(bool fEnabled, const QUuid &uID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_MenuBar_ContextMenu_Enabled, toFeatureRestricted(!fEnabled), uID);
}

UIExtraDataMetaDefs::MenuType UIExtraDataManager::restrictedRuntimeMenuTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuType result = UIExtraDataMetaDefs::MenuType_Invalid;
    /* Get restricted runtime-menu-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeMenus, uID))
    {
        UIExtraDataMetaDefs::MenuType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuTypes(UIExtraDataMetaDefs::MenuType types,
                                                       const QUuid &uID,
                                                       QWidget *pParent /* = 0 */)
{
    /* We have MenuType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuType enumValue =
                static_cast<UIExtraDataMetaDefs::MenuType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuType_Invalid & MenuType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeMenus, result, uID, pParent);
}

UIExtraDataMetaDefs::MenuApplicationActionType UIExtraDataManager::restrictedRuntimeMenuApplicationActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuApplicationActionType result = UIExtraDataMetaDefs::MenuApplicationActionType_Invalid;
    /* Get restricted runtime-application-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeApplicationMenuActions, uID))
    {
        UIExtraDataMetaDefs::MenuApplicationActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuApplicationActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuApplicationActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuApplicationActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuApplicationActionTypes(UIExtraDataMetaDefs::MenuApplicationActionType types,
                                                                        const QUuid &uID,
                                                                        QWidget *pParent /* = 0 */)
{
    /* We have MenuApplicationActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuApplicationActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuApplicationActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuApplicationActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuApplicationActionType enumValue =
                static_cast<UIExtraDataMetaDefs::MenuApplicationActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuApplicationActionType_Invalid & MenuApplicationActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuApplicationActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuApplicationActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeApplicationMenuActions, result, uID, pParent);
}

UIExtraDataMetaDefs::RuntimeMenuMachineActionType UIExtraDataManager::restrictedRuntimeMenuMachineActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuMachineActionType result = UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid;
    /* Get restricted runtime-machine-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeMachineMenuActions, uID))
    {
        UIExtraDataMetaDefs::RuntimeMenuMachineActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(strValue);
        /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
        if (value == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing)
        {
            result = UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing;
            break;
        }
        if (value != UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | value);
    }
    /* Defaults: */
    if (result == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid)
    {
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | UIExtraDataMetaDefs::RuntimeMenuMachineActionType_SaveState);
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | UIExtraDataMetaDefs::RuntimeMenuMachineActionType_PowerOff);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuMachineActionTypes(UIExtraDataMetaDefs::RuntimeMenuMachineActionType types,
                                                                    const QUuid &uID,
                                                                    QWidget *pParent /* = 0 */)
{
    /* We have RuntimeMenuMachineActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuMachineActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuMachineActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuMachineActionType enumValue =
                static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuMachineActionType_Invalid, RuntimeMenuMachineActionType_Nothing & RuntimeMenuMachineActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
    if (result.isEmpty())
        result << gpConverter->toInternalString(UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing);
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeMachineMenuActions, result, uID, pParent);
}

UIExtraDataMetaDefs::RuntimeMenuViewActionType UIExtraDataManager::restrictedRuntimeMenuViewActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuViewActionType result = UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid;
    /* Get restricted runtime-view-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeViewMenuActions, uID))
    {
        UIExtraDataMetaDefs::RuntimeMenuViewActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuViewActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuViewActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuViewActionTypes(UIExtraDataMetaDefs::RuntimeMenuViewActionType types,
                                                                 const QUuid &uID,
                                                                 QWidget *pParent /* = 0 */)
{
    /* We have RuntimeMenuViewActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuViewActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuViewActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuViewActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuViewActionType enumValue =
                static_cast<UIExtraDataMetaDefs::RuntimeMenuViewActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuViewActionType_Invalid & RuntimeMenuViewActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuViewActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeViewMenuActions, result, uID, pParent);
}

UIExtraDataMetaDefs::RuntimeMenuInputActionType UIExtraDataManager::restrictedRuntimeMenuInputActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuInputActionType result = UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid;
    /* Get restricted runtime-machine-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeInputMenuActions, uID))
    {
        UIExtraDataMetaDefs::RuntimeMenuInputActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuInputActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuInputActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuInputActionTypes(UIExtraDataMetaDefs::RuntimeMenuInputActionType types,
                                                                  const QUuid &uID,
                                                                  QWidget *pParent /* = 0 */)
{
    /* We have RuntimeMenuInputActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuInputActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuInputActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuInputActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuInputActionType enumValue =
                static_cast<UIExtraDataMetaDefs::RuntimeMenuInputActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuInputActionType_Invalid & RuntimeMenuInputActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuInputActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeInputMenuActions, result, uID, pParent);
}

UIExtraDataMetaDefs::RuntimeMenuDevicesActionType UIExtraDataManager::restrictedRuntimeMenuDevicesActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuDevicesActionType result = UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid;
    /* Get restricted runtime-devices-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeDevicesMenuActions, uID))
    {
        UIExtraDataMetaDefs::RuntimeMenuDevicesActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(strValue);
        /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
        if (value == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing)
        {
            result = UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing;
            break;
        }
        if (value != UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(result | value);
    }
    /* Defaults: */
    if (result == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid)
    {
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(result | UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_HardDrives);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuDevicesActionTypes(UIExtraDataMetaDefs::RuntimeMenuDevicesActionType types,
                                                                    const QUuid &uID,
                                                                    QWidget *pParent /* = 0 */)
{
    /* We have RuntimeMenuDevicesActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuDevicesActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuDevicesActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuDevicesActionType enumValue =
                static_cast<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuDevicesActionType_Invalid, RuntimeMenuDevicesActionType_Nothing & RuntimeMenuDevicesActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
    if (result.isEmpty())
        result << gpConverter->toInternalString(UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing);
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeDevicesMenuActions, result, uID, pParent);
}

#ifdef VBOX_WITH_DEBUGGER_GUI
UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType UIExtraDataManager::restrictedRuntimeMenuDebuggerActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType result = UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid;
    /* Get restricted runtime-debugger-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeDebuggerMenuActions, uID))
    {
        UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuDebuggerActionTypes(UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType types,
                                                                     const QUuid &uID,
                                                                     QWidget *pParent /* = 0 */)
{
    /* We have RuntimeMenuDebuggerActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuDebuggerActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuDebuggerActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType enumValue =
                static_cast<UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuDebuggerActionType_Invalid & RuntimeMenuDebuggerActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeDebuggerMenuActions, result, uID, pParent);
}
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef VBOX_WS_MAC
UIExtraDataMetaDefs::MenuWindowActionType UIExtraDataManager::restrictedRuntimeMenuWindowActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuWindowActionType result = UIExtraDataMetaDefs::MenuWindowActionType_Invalid;
    /* Get restricted runtime-window-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeWindowMenuActions, uID))
    {
        UIExtraDataMetaDefs::MenuWindowActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuWindowActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuWindowActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuWindowActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuWindowActionTypes(UIExtraDataMetaDefs::MenuWindowActionType types,
                                                                   const QUuid &uID,
                                                                   QWidget *pParent /* = 0 */)
{
    /* We have MenuWindowActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuWindowActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuWindowActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuWindowActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuWindowActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::MenuWindowActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuWindowActionType_Invalid & MenuWindowActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuWindowActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuWindowActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeWindowMenuActions, result, uID, pParent);
}
#endif /* VBOX_WS_MAC */

UIExtraDataMetaDefs::MenuHelpActionType UIExtraDataManager::restrictedRuntimeMenuHelpActionTypes(const QUuid &uID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuHelpActionType result = UIExtraDataMetaDefs::MenuHelpActionType_Invalid;
    /* Get restricted runtime-help-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeHelpMenuActions, uID))
    {
        UIExtraDataMetaDefs::MenuHelpActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuHelpActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuHelpActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuHelpActionType>(result | value);
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedRuntimeMenuHelpActionTypes(UIExtraDataMetaDefs::MenuHelpActionType types,
                                                                 const QUuid &uID,
                                                                 QWidget *pParent /* = 0 */)
{
    /* We have MenuHelpActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuHelpActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuHelpActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuHelpActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuHelpActionType enumValue =
                static_cast<UIExtraDataMetaDefs::MenuHelpActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuHelpActionType_Invalid && MenuHelpActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuHelpActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuHelpActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    return setExtraDataStringList(GUI_RestrictedRuntimeHelpMenuActions, result, uID, pParent);
}

UIVisualStateType UIExtraDataManager::restrictedVisualStates(const QUuid &uID)
{
    /* Prepare result: */
    UIVisualStateType result = UIVisualStateType_Invalid;
    /* Get restricted visual-state-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedVisualStates, uID))
    {
        UIVisualStateType value = gpConverter->fromInternalString<UIVisualStateType>(strValue);
        if (value != UIVisualStateType_Invalid)
            result = static_cast<UIVisualStateType>(result | value);
    }
    /* Return result: */
    return result;
}

UIVisualStateType UIExtraDataManager::requestedVisualState(const QUuid &uID)
{
    if (isFeatureAllowed(GUI_Fullscreen, uID)) return UIVisualStateType_Fullscreen;
    if (isFeatureAllowed(GUI_Seamless, uID)) return UIVisualStateType_Seamless;
    if (isFeatureAllowed(GUI_Scale, uID)) return UIVisualStateType_Scale;
    return UIVisualStateType_Normal;
}

bool UIExtraDataManager::setRequestedVisualState(UIVisualStateType visualState,
                                                 const QUuid &uID,
                                                 QWidget *pParent /* = 0 */)
{
    bool fResult = true;
    /* if (fResult) */
        fResult = setExtraDataString(GUI_Fullscreen,
                                     toFeatureAllowed(visualState == UIVisualStateType_Fullscreen),
                                     uID,
                                     pParent);
    if (fResult)
        fResult = setExtraDataString(GUI_Seamless,
                                     toFeatureAllowed(visualState == UIVisualStateType_Seamless),
                                     uID,
                                     pParent);
    if (fResult)
        fResult = setExtraDataString(GUI_Scale,
                                     toFeatureAllowed(visualState == UIVisualStateType_Scale),
                                     uID,
                                     pParent);
    return fResult;
}

#ifdef VBOX_WS_NIX
bool UIExtraDataManager::legacyFullscreenModeRequested()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_Fullscreen_LegacyMode);
}

bool UIExtraDataManager::distinguishMachineWindowGroups(const QUuid &uID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_DistinguishMachineWindowGroups, uID);
}

bool UIExtraDataManager::setDistinguishMachineWindowGroups(const QUuid &uID, bool fEnabled)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_DistinguishMachineWindowGroups, toFeatureAllowed(fEnabled), uID);
}
#endif /* VBOX_WS_NIX */

bool UIExtraDataManager::guestScreenAutoResizeEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_AutoresizeGuest, uID);
}

bool UIExtraDataManager::setGuestScreenAutoResizeEnabled(bool fEnabled, const QUuid &uID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_AutoresizeGuest, toFeatureRestricted(!fEnabled), uID);
}

bool UIExtraDataManager::lastGuestScreenVisibilityStatus(ulong uScreenIndex, const QUuid &uID)
{
    /* Not for primary screen: */
    if (uScreenIndex == 0)
        return true;

    /* Compose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastVisibilityStatusForGuestScreen, uScreenIndex);

    /* 'False' unless feature allowed: */
    return isFeatureAllowed(strKey, uID);
}

bool UIExtraDataManager::setLastGuestScreenVisibilityStatus(ulong uScreenIndex, bool fEnabled, const QUuid &uID)
{
    /* Not for primary screen: */
    if (uScreenIndex == 0)
        return false;

    /* Compose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastVisibilityStatusForGuestScreen, uScreenIndex);

    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(strKey, toFeatureAllowed(fEnabled), uID);
}

QSize UIExtraDataManager::lastGuestScreenSizeHint(ulong uScreenIndex, const QUuid &uID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastGuestSizeHint, uScreenIndex);

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, uID);

    /* Parse loaded data: */
    int iW = 0, iH = 0;
    bool fOk = data.size() == 2;
    do
    {
        if (!fOk) break;
        iW = data[0].toInt(&fOk);
        if (!fOk) break;
        iH = data[1].toInt(&fOk);
    }
    while (0);

    /* Return size (loaded or invalid): */
    return fOk ? QSize(iW, iH) : QSize();
}

bool UIExtraDataManager::setLastGuestScreenSizeHint(ulong uScreenIndex, const QSize &sizeHint, const QUuid &uID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastGuestSizeHint, uScreenIndex);

    /* Serialize passed values: */
    QStringList data;
    data << QString::number(sizeHint.width());
    data << QString::number(sizeHint.height());

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(strKey, data, uID);
}

int UIExtraDataManager::hostScreenForPassedGuestScreen(int iGuestScreenIndex, const QUuid &uID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_VirtualScreenToHostScreen, iGuestScreenIndex, true);

    /* Get value and convert it to index: */
    const QString strValue = extraDataString(strKey, uID);
    bool fOk = false;
    const int iHostScreenIndex = strValue.toULong(&fOk);

    /* Return corresponding index: */
    return fOk ? iHostScreenIndex : -1;
}

bool UIExtraDataManager::setHostScreenForPassedGuestScreen(int iGuestScreenIndex, int iHostScreenIndex, const QUuid &uID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_VirtualScreenToHostScreen, iGuestScreenIndex, true);

    /* Save passed index under corresponding value: */
    return setExtraDataString(strKey, iHostScreenIndex != -1 ? QString::number(iHostScreenIndex) : QString(), uID);
}

bool UIExtraDataManager::autoMountGuestScreensEnabled(const QUuid &uID)
{
    /* Show only if 'allowed' flag is set: */
    return isFeatureAllowed(GUI_AutomountGuestScreens, uID);
}

#ifndef VBOX_WS_MAC
bool UIExtraDataManager::miniToolbarEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_ShowMiniToolBar, uID);
}

bool UIExtraDataManager::setMiniToolbarEnabled(bool fEnabled, const QUuid &uID, QWidget *pParent /* = 0 */)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_ShowMiniToolBar, toFeatureRestricted(!fEnabled), uID, pParent);
}

bool UIExtraDataManager::autoHideMiniToolbar(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_MiniToolBarAutoHide, uID);
}

bool UIExtraDataManager::setAutoHideMiniToolbar(bool fAutoHide, const QUuid &uID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_MiniToolBarAutoHide, toFeatureRestricted(!fAutoHide), uID);
}

Qt::AlignmentFlag UIExtraDataManager::miniToolbarAlignment(const QUuid &uID)
{
    /* Return Qt::AlignBottom unless MiniToolbarAlignment_Top specified separately: */
    switch (gpConverter->fromInternalString<MiniToolbarAlignment>(extraDataString(GUI_MiniToolBarAlignment, uID)))
    {
        case MiniToolbarAlignment_Top: return Qt::AlignTop;
        default: break;
    }
    return Qt::AlignBottom;
}

bool UIExtraDataManager::setMiniToolbarAlignment(Qt::AlignmentFlag alignment, const QUuid &uID, QWidget *pParent /* = 0 */)
{
    /* Remove record unless Qt::AlignTop specified separately: */
    switch (alignment)
    {
        case Qt::AlignTop:
            return setExtraDataString(GUI_MiniToolBarAlignment,
                                      gpConverter->toInternalString(MiniToolbarAlignment_Top),
                                      uID,
                                      pParent);
        default:
            break;
    }
    return setExtraDataString(GUI_MiniToolBarAlignment,
                              QString(),
                              uID,
                              pParent);
}
#endif /* VBOX_WS_MAC */

bool UIExtraDataManager::statusBarEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_StatusBar_Enabled, uID);
}

bool UIExtraDataManager::setStatusBarEnabled(bool fEnabled, const QUuid &uID, QWidget *pParent /* = 0 */)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_StatusBar_Enabled, toFeatureRestricted(!fEnabled), uID, pParent);
}

bool UIExtraDataManager::statusBarContextMenuEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_StatusBar_ContextMenu_Enabled, uID);
}

bool UIExtraDataManager::setStatusBarContextMenuEnabled(bool fEnabled, const QUuid &uID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_StatusBar_ContextMenu_Enabled, toFeatureRestricted(!fEnabled), uID);
}

QList<IndicatorType> UIExtraDataManager::restrictedStatusBarIndicators(const QUuid &uID)
{
    /* Prepare result: */
    QList<IndicatorType> result;
    /* Get restricted status-bar indicators: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedStatusBarIndicators, uID))
    {
        const IndicatorType value = gpConverter->fromInternalString<IndicatorType>(strValue);
        if (value != IndicatorType_Invalid && !result.contains(value))
            result << value;
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::setRestrictedStatusBarIndicators(const QList<IndicatorType> &list,
                                                          const QUuid &uID,
                                                          QWidget *pParent /* = 0 */)
{
    /* Parse passed list: */
    QStringList data;
    foreach (const IndicatorType &indicatorType, list)
        data << gpConverter->toInternalString(indicatorType);

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_RestrictedStatusBarIndicators, data, uID, pParent);
}

QList<IndicatorType> UIExtraDataManager::statusBarIndicatorOrder(const QUuid &uID)
{
    /* Prepare result: */
    QList<IndicatorType> result;
    /* Get status-bar indicator order: */
    foreach (const QString &strValue, extraDataStringList(GUI_StatusBar_IndicatorOrder, uID))
    {
        const IndicatorType value = gpConverter->fromInternalString<IndicatorType>(strValue);
        if (value != IndicatorType_Invalid && !result.contains(value))
            result << value;
    }

    /* We should update the list with missing indicators: */
    for (int i = (int)IndicatorType_Invalid; i < (int)IndicatorType_Max; ++i)
    {
        /* Skip the IndicatorType_Invalid (we used it as start of this loop): */
        if (i == (int)IndicatorType_Invalid)
            continue;
        /* Skip the IndicatorType_KeyboardExtension (special handling): */
        if (i == (int)IndicatorType_KeyboardExtension)
            continue;

        /* Get the current one: */
        const IndicatorType enmCurrent = (IndicatorType)i;

        /* Skip the current one if it's present: */
        if (result.contains(enmCurrent))
            continue;

        /* Let's find the first of those which stays before it and is not missing: */
        IndicatorType enmPrevious = (IndicatorType)(enmCurrent - 1);
        while (enmPrevious != IndicatorType_Invalid && !result.contains(enmPrevious))
            enmPrevious = (IndicatorType)(enmPrevious - 1);

        /* Calculate position to insert missing one: */
        const int iInsertPosition = enmPrevious != IndicatorType_Invalid
                                  ? result.indexOf(enmPrevious) + 1
                                  : 0;

        /* Finally insert missing indicator at required position: */
        result.insert(iInsertPosition, enmCurrent);
    }

    /* Return result: */
    return result;
}

bool UIExtraDataManager::setStatusBarIndicatorOrder(const QList<IndicatorType> &list,
                                                    const QUuid &uID,
                                                    QWidget *pParent /* = 0 */)
{
    /* Parse passed list: */
    QStringList data;
    foreach (const IndicatorType &indicatorType, list)
        data << gpConverter->toInternalString(indicatorType);

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_StatusBar_IndicatorOrder, data, uID, pParent);
}

#ifdef VBOX_WS_MAC
bool UIExtraDataManager::realtimeDockIconUpdateEnabled(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_RealtimeDockIconUpdateEnabled, uID);
}

bool UIExtraDataManager::setRealtimeDockIconUpdateEnabled(bool fEnabled, const QUuid &uID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    return setExtraDataString(GUI_RealtimeDockIconUpdateEnabled, toFeatureRestricted(!fEnabled), uID);
}

int UIExtraDataManager::realtimeDockIconUpdateMonitor(const QUuid &uID)
{
    return extraDataString(GUI_RealtimeDockIconUpdateMonitor, uID).toInt();
}

bool UIExtraDataManager::setRealtimeDockIconUpdateMonitor(int iIndex, const QUuid &uID)
{
    return setExtraDataString(GUI_RealtimeDockIconUpdateMonitor, iIndex ? QString::number(iIndex) : QString(), uID);
}

bool UIExtraDataManager::dockIconDisableOverlay(const QUuid &uID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_DockIconDisableOverlay, uID);
}

bool UIExtraDataManager::setDockIconDisableOverlay(bool fDisabled, const QUuid &uID)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_DockIconDisableOverlay, toFeatureAllowed(fDisabled), uID);
}
#endif /* VBOX_WS_MAC */

bool UIExtraDataManager::passCADtoGuest(const QUuid &uID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_PassCAD, uID);
}

MouseCapturePolicy UIExtraDataManager::mouseCapturePolicy(const QUuid &uID)
{
    return gpConverter->fromInternalString<MouseCapturePolicy>(extraDataString(GUI_MouseCapturePolicy, uID));
}

GuruMeditationHandlerType UIExtraDataManager::guruMeditationHandlerType(const QUuid &uID)
{
    return gpConverter->fromInternalString<GuruMeditationHandlerType>(extraDataString(GUI_GuruMeditationHandler, uID));
}

bool UIExtraDataManager::hidLedsSyncState(const QUuid &uID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_HidLedsSync, uID);
}

double UIExtraDataManager::scaleFactor(const QUuid &uID, int iScreenIndex)
{
    /* Get corresponding extra-data for this machine: */
    QStringList data = extraDataStringList(GUI_ScaleFactor, uID);

    /* 1.0 is default scale factor: */
    if (data.size() == 0)
        return 1.0;

    int iIndex = iScreenIndex;
    /* use the 0th. scale factor in case we dont have a scale factor for @p iScreenIndex: */
    if (data.size() <= iScreenIndex)
        iIndex = 0;

    bool fOk = false;
    const double dScaleFactor = data.at(iIndex).toDouble(&fOk);
    if (!fOk)
        return 1.0;
    return dScaleFactor;
}

QList<double> UIExtraDataManager::scaleFactors(const QUuid &uID)
{
    /* Look for the scale factor for this machine first: */
    QStringList data = extraDataStringList(GUI_ScaleFactor, uID);

    QList<double> scaleFactorList;
    /* 1.0 is default scale factor: */
    if (data.size() == 0)
    {
        scaleFactorList.append(1.0);
        return scaleFactorList;
    }

    bool fOk = false;
    double scaleFactor;
    for (int i = 0; i < data.size(); ++i)
    {
        scaleFactor = data[i].toDouble(&fOk);
        if (!fOk)
            scaleFactor = 1.0;
        scaleFactorList.append(scaleFactor);
    }
    return scaleFactorList;
}

bool UIExtraDataManager::setScaleFactor(double dScaleFactor, const QUuid &uID, int iScreenIndex)
{
    QStringList data = extraDataStringList(GUI_ScaleFactor, uID);

    /* Just make sure that we have corresponding data item: */
    if (data.size() <= iScreenIndex)
    {
        for (int i = data.size(); i <= iScreenIndex; ++i)
            data.append(QString::number(1.0));
    }

    data[iScreenIndex] = QString::number(dScaleFactor);
    return setExtraDataStringList(GUI_ScaleFactor, data, uID);
}

bool UIExtraDataManager::setScaleFactors(const QList<double> &scaleFactors, const QUuid &uID, QWidget *pParent)
{
    QStringList data;
    for (int i = 0; i < scaleFactors.size(); ++i)
        data.append(QString::number(scaleFactors[i]));
    return setExtraDataStringList(GUI_ScaleFactor, data, uID, pParent);
}

ScalingOptimizationType UIExtraDataManager::scalingOptimizationType(const QUuid &uID)
{
    return gpConverter->fromInternalString<ScalingOptimizationType>(extraDataString(GUI_Scaling_Optimization, uID));
}

QRect UIExtraDataManager::sessionInformationDialogGeometry(QWidget *pWidget, QWidget *pParentWidget)
{
    return dialogGeometry(GUI_SessionInformationDialogGeometry, pWidget, pParentWidget);
}

bool UIExtraDataManager::sessionInformationDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_SessionInformationDialogGeometry);
}

bool UIExtraDataManager::setSessionInformationDialogGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_SessionInformationDialogGeometry, data);
}

bool UIExtraDataManager::setGuestControlProcessControlSplitterHints(const QList<int> &hints)
{
    QStringList data;
    data << (hints.size() > 0 ? QString::number(hints[0]) : QString());
    data << (hints.size() > 1 ? QString::number(hints[1]) : QString());

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_GuestControl_ProcessControlSplitterHints, data);
}

QList<int> UIExtraDataManager::guestControlProcessControlSplitterHints()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_GuestControl_ProcessControlSplitterHints);

    /* Parse loaded data: */
    QList<int> hints;
    hints << (data.size() > 0 ? data[0].toInt() : 0);
    hints << (data.size() > 1 ? data[1].toInt() : 0);

    /* Return hints: */
    return hints;
}

QRect UIExtraDataManager::fileManagerDialogGeometry(QWidget *pWidget, QWidget *pParentWidget)
{
    return dialogGeometry(GUI_GuestControl_FileManagerDialogGeometry, pWidget, pParentWidget);
}

bool UIExtraDataManager::fileManagerDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_GuestControl_FileManagerDialogGeometry);
}

void UIExtraDataManager::setFileManagerDialogGeometry(const QRect &geometry, bool fMaximized)
{
    setDialogGeometry(GUI_GuestControl_FileManagerDialogGeometry, geometry, fMaximized);
}

bool UIExtraDataManager::setFileManagerVisiblePanels(const QStringList &panelNameList)
{
    return setExtraDataStringList(GUI_GuestControl_FileManagerVisiblePanels, panelNameList);
}

QStringList UIExtraDataManager::fileManagerVisiblePanels()
{
    return extraDataStringList(GUI_GuestControl_FileManagerVisiblePanels);
}

QRect UIExtraDataManager::softKeyboardDialogGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_SoftKeyboard_DialogGeometry, pWidget, pParentWidget, defaultGeometry);
}

void UIExtraDataManager::setSoftKeyboardDialogGeometry(const QRect &geometry, bool fMaximized)
{
    setDialogGeometry(GUI_SoftKeyboard_DialogGeometry, geometry, fMaximized);
}

bool UIExtraDataManager::softKeyboardDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_SoftKeyboard_DialogGeometry);
}

bool UIExtraDataManager::setSoftKeyboardOptions(bool fHideNumPad, bool fHideOSMenuKeys, bool fMultimediaKeys)
{
    QStringList data;

    if (fHideNumPad)
        data << GUI_SoftKeyboard_HideNumPad;
    if (fHideOSMenuKeys)
        data << GUI_SoftKeyboard_HideOSMenuKeys;
    if (fMultimediaKeys)
        data << GUI_SoftKeyboard_HideMultimediaKeys;

    return setExtraDataStringList(GUI_SoftKeyboard_Options, data);
}

void UIExtraDataManager::softKeyboardOptions(bool &fOutHideNumPad, bool &fOutHideOSMenuKeys, bool &fOutHideMultimediaKeys)
{
    fOutHideNumPad = false;
    fOutHideOSMenuKeys = false;
    const QStringList data = extraDataStringList(GUI_SoftKeyboard_Options);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_SoftKeyboard_HideNumPad)
            fOutHideNumPad = true;
        if (data[i] == GUI_SoftKeyboard_HideOSMenuKeys)
            fOutHideOSMenuKeys = true;
        if (data[i] == GUI_SoftKeyboard_HideMultimediaKeys)
            fOutHideMultimediaKeys = true;
    }
}

bool UIExtraDataManager::setSoftKeyboardColorTheme(const QStringList &colorStringList)
{
    return setExtraDataStringList(GUI_SoftKeyboard_ColorTheme, colorStringList);
}

QStringList UIExtraDataManager::softKeyboardColorTheme()
{
    return extraDataStringList(GUI_SoftKeyboard_ColorTheme);
}

bool UIExtraDataManager::setSoftKeyboardSelectedColorTheme(const QString &strColorThemeName)
{
    return setExtraDataString(GUI_SoftKeyboard_SelectedColorTheme, strColorThemeName);
}

QString UIExtraDataManager::softKeyboardSelectedColorTheme()
{
    return extraDataString(GUI_SoftKeyboard_SelectedColorTheme);
}

bool UIExtraDataManager::setSoftKeyboardSelectedLayout(const QUuid &uLayoutUid)
{
    return setExtraDataString(GUI_SoftKeyboard_SelectedLayout, uLayoutUid.toString());
}

QUuid UIExtraDataManager::softKeyboardSelectedLayout()
{
    return QUuid(extraDataString(GUI_SoftKeyboard_SelectedLayout));
}

bool UIExtraDataManager::setFileManagerOptions(bool fListDirectoriesFirst,
                                               bool fShowDeleteConfirmation,
                                               bool fShowHumanReadableSizes,
                                               bool fShowHiddenObjects,
                                               bool fAllowInteractiveColumnWidths)
{
    /* Serialize passed values: */
    QStringList data;

    if (fListDirectoriesFirst)
        data << GUI_GuestControl_FileManagerListDirectoriesFirst;
    if (fShowDeleteConfirmation)
        data << GUI_GuestControl_FileManagerShowDeleteConfirmation;
    if (fShowHumanReadableSizes)
        data << GUI_GuestControl_FileManagerShowHumanReadableSizes;
    if (fShowHiddenObjects)
        data << GUI_GuestControl_FileManagerShowHiddenObjects;
    if (fAllowInteractiveColumnWidths)
        data << GUI_GuestControl_FileManagerAllowInteractiveColumnWidths;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_GuestControl_FileManagerOptions, data);
}

bool UIExtraDataManager::fileManagerAllowInteractiveColumnWidths()
{
    const QStringList data = extraDataStringList(GUI_GuestControl_FileManagerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_GuestControl_FileManagerAllowInteractiveColumnWidths)
            return true;
    }
    return false;
}

bool UIExtraDataManager::fileManagerListDirectoriesFirst()
{
    const QStringList data = extraDataStringList(GUI_GuestControl_FileManagerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_GuestControl_FileManagerListDirectoriesFirst)
            return true;
    }
    return false;
}

bool UIExtraDataManager::fileManagerShowDeleteConfirmation()
{
    const QStringList data = extraDataStringList(GUI_GuestControl_FileManagerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_GuestControl_FileManagerShowDeleteConfirmation)
            return true;
    }
    return false;
}

bool UIExtraDataManager::fileManagerShowHumanReadableSizes()
{
    const QStringList data = extraDataStringList(GUI_GuestControl_FileManagerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_GuestControl_FileManagerShowHumanReadableSizes)
            return true;
    }
    return false;
}

bool UIExtraDataManager::fileManagerShowHiddenObjects()
{
    const QStringList data = extraDataStringList(GUI_GuestControl_FileManagerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_GuestControl_FileManagerShowHiddenObjects)
            return true;
    }
    return false;
}

QRect UIExtraDataManager::guestProcessControlDialogGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_GuestControl_ProcessControlDialogGeometry, pWidget, pParentWidget, defaultGeometry);
}

bool UIExtraDataManager::guestProcessControlDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_GuestControl_ProcessControlDialogGeometry);
}

void UIExtraDataManager::setGuestProcessControlDialogGeometry(const QRect &geometry, bool fMaximized)
{
    setDialogGeometry(GUI_GuestControl_ProcessControlDialogGeometry, geometry, fMaximized);
}

MachineCloseAction UIExtraDataManager::defaultMachineCloseAction(const QUuid &uID)
{
    return gpConverter->fromInternalString<MachineCloseAction>(extraDataString(GUI_DefaultCloseAction, uID));
}

MachineCloseAction UIExtraDataManager::restrictedMachineCloseActions(const QUuid &uID)
{
    /* Prepare result: */
    MachineCloseAction result = MachineCloseAction_Invalid;
    /* Get restricted machine-close-actions: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedCloseActions, uID))
    {
        MachineCloseAction value = gpConverter->fromInternalString<MachineCloseAction>(strValue);
        if (value != MachineCloseAction_Invalid)
            result = static_cast<MachineCloseAction>(result | value);
    }
    /* Return result: */
    return result;
}

MachineCloseAction UIExtraDataManager::lastMachineCloseAction(const QUuid &uID)
{
    return gpConverter->fromInternalString<MachineCloseAction>(extraDataString(GUI_LastCloseAction, uID));
}

bool UIExtraDataManager::setLastMachineCloseAction(MachineCloseAction machineCloseAction, const QUuid &uID)
{
    return setExtraDataString(GUI_LastCloseAction, gpConverter->toInternalString(machineCloseAction), uID);
}

QString UIExtraDataManager::machineCloseHookScript(const QUuid &uID)
{
    return extraDataString(GUI_CloseActionHook, uID);
}

bool UIExtraDataManager::discardStateOnPowerOff(const QUuid &uID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_DiscardStateOnPowerOff, uID);
}

#ifdef VBOX_WITH_DEBUGGER_GUI
QString UIExtraDataManager::debugFlagValue(const QString &strDebugFlagKey)
{
    return extraDataString(strDebugFlagKey).toLower().trimmed();
}
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
QRect UIExtraDataManager::extraDataManagerGeometry(QWidget *pWidget, QWidget *pParentWidget)
{
    return dialogGeometry(GUI_ExtraDataManager_Geometry, pWidget, pParentWidget);
}

bool UIExtraDataManager::extraDataManagerShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_ExtraDataManager_Geometry);
}

bool UIExtraDataManager::setExtraDataManagerGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_ExtraDataManager_Geometry, data);
}

QList<int> UIExtraDataManager::extraDataManagerSplitterHints(QWidget *pWidget)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_ExtraDataManager_SplitterHints);

    /* Parse loaded data: */
    int iLeft = 0, iRight = 0;
    bool fOk = data.size() == 2;
    do
    {
        if (!fOk) break;
        iLeft = data[0].toInt(&fOk);
        if (!fOk) break;
        iRight = data[1].toInt(&fOk);
    }
    while (0);

    /* Prepare hints (loaded or adviced): */
    QList<int> hints;
    if (fOk)
    {
        hints << iLeft;
        hints << iRight;
    }
    else
    {
        hints << (int)(pWidget->width() * .9 * (1.0 / 3));
        hints << (int)(pWidget->width() * .9 * (2.0 / 3));
    }

    /* Return hints: */
    return hints;
}

bool UIExtraDataManager::setExtraDataManagerSplitterHints(const QList<int> &hints)
{
    /* Parse passed hints: */
    QStringList data;
    data << (hints.size() > 0 ? QString::number(hints[0]) : QString());
    data << (hints.size() > 1 ? QString::number(hints[1]) : QString());

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_ExtraDataManager_SplitterHints, data);
}
#endif /* VBOX_GUI_WITH_EXTRADATA_MANAGER_UI */

QRect UIExtraDataManager::logWindowGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_LogWindowGeometry, pWidget, pParentWidget, defaultGeometry);
}

bool UIExtraDataManager::logWindowShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_LogWindowGeometry);
}

bool UIExtraDataManager::setLogWindowGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_LogWindowGeometry, data);
}

bool UIExtraDataManager::setLogViweverOptions(const QFont &font, bool wrapLines, bool showLineNumbers)
{
    /* Serialize passed values: */
    QStringList data;
    data << font.family();
    /* Make sure that we have some non-empty string as font style name: */
    QString strStyleName = font.styleName();
    if (strStyleName.isEmpty())
        data << GUI_LogViewerNoFontStyleName;
    else
        data << font.styleName();
    data << QString::number(font.pointSize());

    if (wrapLines)
        data << GUI_LogViewerWrapLinesEnabled;
    if (!showLineNumbers)
        data << GUI_LogViewerShowLineNumbersDisabled;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_LogViewerOptions, data);
}

bool UIExtraDataManager::logViewerWrapLines()
{
    const QStringList data = extraDataStringList(GUI_LogViewerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_LogViewerWrapLinesEnabled)
            return true;
    }
    return false;
}

bool UIExtraDataManager::logViewerShowLineNumbers()
{
    const QStringList data = extraDataStringList(GUI_LogViewerOptions);
    for (int i = 0; i < data.size(); ++i)
    {
        if (data[i] == GUI_LogViewerShowLineNumbersDisabled)
            return false;
    }
    return true;
}

QFont UIExtraDataManager::logViewerFont()
{
    const QStringList data = extraDataStringList(GUI_LogViewerOptions);
    if (data.size() < 3)
        return QFont();
    QString strFamily = data[0];
    QString strStyleName = data[1];
    if (strStyleName == GUI_LogViewerNoFontStyleName)
        strStyleName.clear();
    bool fOk = false;
    int iFontSize = data[2].toInt(&fOk);
    if (!fOk)
        iFontSize = 9;
    return QFontDatabase::font(strFamily, strStyleName, iFontSize);
}

bool UIExtraDataManager::setHelpBrowserLastUrlList(const QStringList &urlList)
{
    return setExtraDataStringList(GUI_HelpBrowser_LastURLList, urlList);
}

QStringList UIExtraDataManager::helpBrowserLastUrlList()
{
    return extraDataStringList(GUI_HelpBrowser_LastURLList);
}

bool UIExtraDataManager::setHelpBrowserZoomPercentage(int iZoomPercentage)
{
    return setExtraDataString(GUI_HelpBrowser_ZoomPercentage, QString::number(iZoomPercentage));
}

int UIExtraDataManager::helpBrowserZoomPercentage()
{
    return extraDataString(GUI_HelpBrowser_ZoomPercentage).toInt();
}

QRect UIExtraDataManager::helpBrowserDialogGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_HelpBrowser_DialogGeometry, pWidget, pParentWidget, defaultGeometry);
}

bool UIExtraDataManager::setHelpBrowserDialogGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    return setExtraDataStringList(GUI_HelpBrowser_DialogGeometry, data);
}

bool UIExtraDataManager::helpBrowserDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_HelpBrowser_DialogGeometry);
}

bool UIExtraDataManager::setHelpBrowserBookmarks(const QStringList &bookmarks)
{
    return setExtraDataStringList(GUI_HelpBrowser_Bookmarks, bookmarks);
}

QStringList UIExtraDataManager::helpBrowserBookmarks()
{
    return extraDataStringList(GUI_HelpBrowser_Bookmarks);
}

bool UIExtraDataManager::setVMActivityOverviewHiddenColumnList(const QStringList &hiddenColumnList)
{
    return setExtraDataStringList(GUI_VMActivityOverview_HiddenColumns, hiddenColumnList);
}

QStringList UIExtraDataManager::VMActivityOverviewHiddenColumnList()
{
    return extraDataStringList(GUI_VMActivityOverview_HiddenColumns);
}

bool UIExtraDataManager::VMActivityOverviewShowAllMachines()
{
    return isFeatureAllowed(GUI_VMActivityOverview_ShowAllMachines);
}

bool UIExtraDataManager::setVMActivityOverviewShowAllMachines(bool fShow)
{
    return setExtraDataString(GUI_VMActivityOverview_ShowAllMachines, toFeatureAllowed(fShow));
}

bool UIExtraDataManager::setVMActivityMonitorDataSeriesColors(const QStringList &colorList)
{
    return setExtraDataStringList(GUI_VMActivityMonitor_DataSeriesColors, colorList);
}

QStringList UIExtraDataManager::VMActivityMonitorDataSeriesColors()
{
    return extraDataStringList(GUI_VMActivityMonitor_DataSeriesColors);
}

bool UIExtraDataManager::VMActivityMonitorShowVMExits()
{
    return isFeatureAllowed(GUI_VMActivityMonitor_ShowVMExits);
}

bool UIExtraDataManager::setVMActivityMonitorShowVMExits(bool fShow)
{
    return setExtraDataString(GUI_VMActivityMonitor_ShowVMExits, toFeatureAllowed(fShow));
}

QRect UIExtraDataManager::mediumSelectorDialogGeometry(QWidget *pWidget, QWidget *pParentWidget, const QRect &defaultGeometry)
{
    return dialogGeometry(GUI_MediumSelector_DialogGeometry, pWidget, pParentWidget, defaultGeometry);
}

void UIExtraDataManager::setMediumSelectorDialogGeometry(const QRect &geometry, bool fMaximized)
{
    setDialogGeometry(GUI_MediumSelector_DialogGeometry, geometry, fMaximized);
}

bool UIExtraDataManager::mediumSelectorDialogShouldBeMaximized()
{
    return dialogShouldBeMaximized(GUI_MediumSelector_DialogGeometry);
}

void UIExtraDataManager::sltExtraDataChange(const QUuid &uMachineID, const QString &strKey, const QString &strValue)
{
    /* Re-cache value only if uMachineID known already: */
    if (m_data.contains(uMachineID))
    {
        if (!strValue.isEmpty())
            m_data[uMachineID][strKey] = strValue;
        else
            m_data[uMachineID].remove(strKey);
    }

    /* Global extra-data 'change' event: */
    if (uMachineID == GlobalID)
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Notification-center alignment? */
            if (strKey == GUI_NotificationCenter_Alignment)
                emit sigNotificationCenterAlignmentChange();
            /* Notification-center order? */
            if (strKey == GUI_NotificationCenter_Order)
                emit sigNotificationCenterOrderChange();
            /* Settings expert mode? */
            if (strKey == GUI_Settings_ExpertMode)
                emit sigSettingsExpertModeChange();
            /* Language changed? */
            if (strKey == GUI_LanguageID)
                emit sigLanguageChange(extraDataString(strKey));
            /* Selector UI shortcut changed? */
            else if (strKey == GUI_Input_SelectorShortcuts)
                emit sigSelectorUIShortcutChange();
            /* Runtime UI shortcut changed? */
            else if (strKey == GUI_Input_MachineShortcuts)
                emit sigRuntimeUIShortcutChange();
            /* Runtime UI host-key combintation changed? */
            else if (strKey == GUI_Input_HostKeyCombination)
                emit sigRuntimeUIHostKeyCombinationChange();
            /* Tool labels visibility changed? */
            else if (strKey == GUI_Tools_Text)
                emit sigToolLabelsVisibilityChange(isToolTextVisible());
            /* Chooser-pane VM item size changed? */
            else if (strKey == GUI_Chooser_ItemSize)
                emit sigChooserItemSizeChange();
            /* Cloud Profile Manager restrictions changed: */
            else if (strKey == GUI_CloudProfileManager_Restrictions)
                emit sigCloudProfileManagerRestrictionChange();
            /* Cloud Console Manager data changed: */
            else if (strKey.startsWith(QString(GUI_CloudConsoleManager_Application) + '/'))
                emit sigCloudConsoleManagerDataChange();
            /* Cloud Console Manager restrictions changed: */
            else if (strKey == GUI_CloudConsoleManager_Restrictions)
                emit sigCloudConsoleManagerRestrictionChange();
#if defined(VBOX_WS_NIX) || defined(VBOX_WS_WIN)
            else if (strKey == GUI_DisableHostScreenSaver)
                emit sigDisableHostScreenSaverStateChange(isFeatureAllowed(GUI_DisableHostScreenSaver));
#endif
            /* Details categories: */
            else if (strKey == GUI_Details_Elements)
                emit sigDetailsCategoriesChange();
            /* Details options: */
            else if (strKey.startsWith(QString(GUI_Details_Elements) + '/'))
            {
                QString strLeftover = strKey;
                strLeftover.remove(QString(GUI_Details_Elements) + '/');
                const DetailsElementType enmType = gpConverter->fromInternalString<DetailsElementType>(strLeftover);
                if (enmType != DetailsElementType_Invalid)
                    emit sigDetailsOptionsChange(enmType);
            }
            /* Font scaling factor has changed: */
            else if (strKey == GUI_FontScaleFactor)
                emit sigFontScaleFactorChanged(fontScaleFactor());
        }
    }
    /* Machine extra-data 'change' event: */
    else
    {
        /* Current VM only: */
        if (   uiCommon().uiType() == UIType_RuntimeUI
            && uMachineID == uiCommon().managedVMUuid())
        {
            /* HID LEDs sync state changed (allowed if not restricted)? */
            if (strKey == GUI_HidLedsSync)
                emit sigHidLedsSyncStateChange(!isFeatureRestricted(strKey, uMachineID));
#ifdef VBOX_WS_MAC
            /* 'Dock icon' appearance changed (allowed if not restricted)? */
            else if (strKey == GUI_RealtimeDockIconUpdateEnabled ||
                     strKey == GUI_RealtimeDockIconUpdateMonitor)
                emit sigDockIconAppearanceChange(!isFeatureRestricted(strKey, uMachineID));
            /* 'Dock icon overlay' appearance changed (restricted if not allowed)? */
            else if (strKey == GUI_DockIconDisableOverlay)
                emit sigDockIconOverlayAppearanceChange(isFeatureAllowed(strKey, uMachineID));
#endif /* VBOX_WS_MAC */
        }

        /* Menu-bar configuration change: */
        if (
#ifndef VBOX_WS_MAC
            strKey == GUI_MenuBar_Enabled ||
#endif /* !VBOX_WS_MAC */
            strKey == GUI_RestrictedRuntimeMenus ||
            strKey == GUI_RestrictedRuntimeApplicationMenuActions ||
            strKey == GUI_RestrictedRuntimeMachineMenuActions ||
            strKey == GUI_RestrictedRuntimeViewMenuActions ||
            strKey == GUI_RestrictedRuntimeInputMenuActions ||
            strKey == GUI_RestrictedRuntimeDevicesMenuActions ||
#ifdef VBOX_WITH_DEBUGGER_GUI
            strKey == GUI_RestrictedRuntimeDebuggerMenuActions ||
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef VBOX_WS_MAC
            strKey == GUI_RestrictedRuntimeWindowMenuActions ||
#endif /* VBOX_WS_MAC */
            strKey == GUI_RestrictedRuntimeHelpMenuActions)
            emit sigMenuBarConfigurationChange(uMachineID);
        /* Status-bar configuration change: */
        else if (strKey == GUI_StatusBar_Enabled ||
                 strKey == GUI_RestrictedStatusBarIndicators ||
                 strKey == GUI_StatusBar_IndicatorOrder)
            emit sigStatusBarConfigurationChange(uMachineID);
        /* Visual state change: */
        else if (strKey == GUI_Fullscreen ||
                 strKey == GUI_Seamless ||
                 strKey == GUI_Scale)
            emit sigVisualStateChange(uMachineID);
        /* Scale-factor change: */
        else if (strKey == GUI_ScaleFactor)
            emit sigScaleFactorChange(uMachineID);
        /* Scaling optimization type change: */
        else if (strKey == GUI_Scaling_Optimization)
            emit sigScalingOptimizationTypeChange(uMachineID);
    }

    /* Notify listeners: */
    emit sigExtraDataChange(uMachineID, strKey, strValue);
}

void UIExtraDataManager::prepare()
{
    /* Prepare global extra-data map: */
    prepareGlobalExtraDataMap();
    /* Prepare extra-data event-handler: */
    prepareExtraDataEventHandler();
    /* Prepare extra-data settings: */
    prepareExtraDataSettings();
}

void UIExtraDataManager::prepareGlobalExtraDataMap()
{
    /* Get CVirtualBox: */
    CVirtualBox vbox = gpGlobalSession->virtualBox();

    /* Make sure at least empty map is created: */
    m_data[GlobalID] = ExtraDataMap();

    /* Load global extra-data map: */
    foreach (const QString &strKey, vbox.GetExtraDataKeys())
        m_data[GlobalID][strKey] = vbox.GetExtraData(strKey);
}

void UIExtraDataManager::prepareExtraDataEventHandler()
{
    /* Create extra-data event-handler: */
    m_pHandler = new UIExtraDataEventHandler(this);
    /* Configure extra-data event-handler: */
    AssertPtrReturnVoid(m_pHandler);
    {
        /* Create queued (async) connections for signals of event proxy object: */
        connect(m_pHandler, &UIExtraDataEventHandler::sigExtraDataChange,
                this, &UIExtraDataManager::sltExtraDataChange,
                Qt::QueuedConnection);
    }
}

void UIExtraDataManager::prepareExtraDataSettings()
{
    /* For the case when Expert mode is undecided yet: */
    if (extraDataString(UIExtraDataDefs::GUI_Settings_ExpertMode).isNull())
    {
        /* If there are VMs already, mark the mode as Expert one, we've decided
         * that forcing all users to Basic mode by default is overkill a bit. */
        if (!gpGlobalSession->virtualBox().GetMachines().isEmpty())
            setSettingsInExpertMode(true);
    }
}

#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
void UIExtraDataManager::cleanupWindow()
{
    delete m_pWindow;
}
#endif /* VBOX_GUI_WITH_EXTRADATA_MANAGER_UI */

void UIExtraDataManager::cleanupExtraDataEventHandler()
{
    /* Destroy extra-data event-handler: */
    delete m_pHandler;
    m_pHandler = 0;
}

void UIExtraDataManager::cleanup()
{
    /* Cleanup extra-data event-handler: */
    cleanupExtraDataEventHandler();
#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
    /* Cleanup window: */
    cleanupWindow();
#endif /* VBOX_GUI_WITH_EXTRADATA_MANAGER_UI */
}

#ifdef VBOX_GUI_WITH_EXTRADATA_MANAGER_UI
void UIExtraDataManager::open(QWidget *pCenterWidget)
{
    /* If necessary: */
    if (!m_pWindow)
    {
        /* Create window: */
        m_pWindow = new UIExtraDataManagerWindow(pCenterWidget);
        /* Configure window connections: */
        connect(this, &UIExtraDataManager::sigExtraDataMapAcknowledging,
                m_pWindow.data(), &UIExtraDataManagerWindow::sltExtraDataMapAcknowledging);
        connect(this, &UIExtraDataManager::sigExtraDataChange,
                m_pWindow.data(), &UIExtraDataManagerWindow::sltExtraDataChange);
    }
    /* Show and raise window: */
    m_pWindow->showAndRaise();
}
#endif /* VBOX_GUI_WITH_EXTRADATA_MANAGER_UI */

QString UIExtraDataManager::extraDataStringUnion(const QString &strKey, const QUuid &uID)
{
    /* If passed uID differs from the GlobalID: */
    if (uID != GlobalID)
    {
        /* Search through the machine extra-data first: */
        MapOfExtraDataMaps::const_iterator itMap = m_data.constFind(uID);
        /* Hot-load machine extra-data map if necessary: */
        if (itMap == m_data.constEnd())
        {
            hotloadMachineExtraDataMap(uID);
            itMap = m_data.constFind(uID);
        }
        if (itMap != m_data.constEnd())
        {
            /* Return string if present in the map: */
            ExtraDataMap::const_iterator itValue = itMap->constFind(strKey);
            if (itValue != itMap->constEnd())
                return *itValue;
        }
    }

    /* Search through the global extra-data finally: */
    MapOfExtraDataMaps::const_iterator itMap = m_data.constFind(GlobalID);
    if (itMap != m_data.constEnd())
    {
        /* Return string if present in the map: */
        ExtraDataMap::const_iterator itValue = itMap->constFind(strKey);
        if (itValue != itMap->constEnd())
            return *itValue;
    }

    /* Not found, return null string: */
    return QString();
}

bool UIExtraDataManager::isFeatureAllowed(const QString &strKey, const QUuid &uID /* = GlobalID */)
{
    /* Get the value. Return 'false' if not found: */
    const QString strValue = extraDataStringUnion(strKey, uID);
    if (strValue.isNull())
        return false;

    /* Check corresponding value: */
    return    strValue.compare("true", Qt::CaseInsensitive) == 0
           || strValue.compare("yes", Qt::CaseInsensitive) == 0
           || strValue.compare("on", Qt::CaseInsensitive) == 0
           || strValue == "1";
}

bool UIExtraDataManager::isFeatureRestricted(const QString &strKey, const QUuid &uID /* = GlobalID */)
{
    /* Get the value. Return 'false' if not found: */
    const QString strValue = extraDataStringUnion(strKey, uID);
    if (strValue.isNull())
        return false;

    /* Check corresponding value: */
    return    strValue.compare("false", Qt::CaseInsensitive) == 0
           || strValue.compare("no", Qt::CaseInsensitive) == 0
           || strValue.compare("off", Qt::CaseInsensitive) == 0
           || strValue == "0";
}

QString UIExtraDataManager::toFeatureState(bool fState)
{
    return fState ? QString("true") : QString("false");
}

QString UIExtraDataManager::toFeatureAllowed(bool fAllowed)
{
    return fAllowed ? QString("true") : QString();
}

QString UIExtraDataManager::toFeatureRestricted(bool fRestricted)
{
    return fRestricted ? QString("false") : QString();
}

bool UIExtraDataManager::setDialogGeometry(const QString &strKey, const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Save corresponding extra-data: */
    return setExtraDataStringList(strKey, data);
}

QRect UIExtraDataManager::dialogGeometry(const QString &strKey,
                                         QWidget *pWidget,
                                         QWidget *pParentWidget /* = 0 */,
                                         const QRect &defaultGeometry /* = QRect() */)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Get available-geometry [of screen with point (iX, iY) if possible]: */
    const QRect availableGeometry = fOk ? gpDesktop->availableGeometry(QPoint(iX, iY)) :
                                          gpDesktop->availableGeometry();

    /* Use geometry (loaded or default): */
    QRect geometry = fOk
                   ? QRect(iX, iY, iW, iH)
                   : !defaultGeometry.isNull()
                   ? defaultGeometry
                   : QRect(QPoint(0, 0), availableGeometry.size() * .50 /* % */);

    /* Take hint-widget into account: */
    if (pWidget)
        geometry.setSize(geometry.size().expandedTo(pWidget->minimumSizeHint()));

    /* As a fallback, move default-geometry to pParentWidget' geometry center: */
    if (!fOk && pParentWidget)
        geometry.moveCenter(pParentWidget->geometry().center());
    /* As final fallback, move default-geometry to available-geometry' center: */
    else if (!fOk)
        geometry.moveCenter(availableGeometry.center());

    /* In Windows Qt fails to reposition out of screen window properly, so doing it ourselves: */
#ifdef VBOX_WS_WIN
    /* Make sure resulting geometry is within current bounds: */
    if (!availableGeometry.contains(geometry))
        geometry = UIDesktopWidgetWatchdog::getNormalized(geometry, QRegion(availableGeometry));
#endif /* VBOX_WS_WIN */

    /* Return result: */
    return geometry;
}

bool UIExtraDataManager::dialogShouldBeMaximized(const QString &strKey)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

/* static */
QString UIExtraDataManager::extraDataKeyPerScreen(const QString &strBase, ulong uScreenIndex, bool fSameRuleForPrimary /* = false */)
{
    return fSameRuleForPrimary || uScreenIndex ? strBase + QString::number(uScreenIndex) : strBase;
}


#include "UIExtraDataManager.moc"
