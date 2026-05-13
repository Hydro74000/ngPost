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
    void onBrowse();
    void onStart();
    void onStop();
    void onInstall();
    void onUninstall();
    void onStateChanged(VpnManager::State newState);
    void onInstallStateChanged(bool installed);
    void onLogLine(QString const &line);

private:
    void _refreshUi();
    void _refreshSetupUi();
    void _commitConfigToManager();

    Ui::VpnSettingsDialog *_ui;
    VpnManager            *_manager;
};

#endif // VPNSETTINGSDIALOG_H
