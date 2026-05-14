//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNSETTINGSDIALOG_H
#define VPNSETTINGSDIALOG_H

#include "vpn/VpnManager.h"

#include <QDialog>

namespace Ui
{
class VpnSettingsDialog;
}
class VpnManager;

class VpnSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VpnSettingsDialog(VpnManager *manager, QWidget *parent = nullptr);
    ~VpnSettingsDialog() override;

private slots:
    void onActiveProfileChanged(int idx);
    void onAutoConnectToggled(bool checked);
    void onStart();
    void onStop();
    void onInstall();
    void onUninstall();
    void onNewProfile();
    void onEditProfile();
    void onDeleteProfile();
    void onStateChanged(VpnManager::State newState);
    void onInstallStateChanged(bool installed);
    void onProfilesChanged();
    void onLogLine(QString const &line);

private:
    void _refreshUi();
    void _refreshSetupUi();
    void _refreshProfilesUi();

    Ui::VpnSettingsDialog *_ui;
    VpnManager            *_manager;
    bool                   _populating; //!< gate slot signals while refreshing combobox
};

#endif // VPNSETTINGSDIALOG_H
