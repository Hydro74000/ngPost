//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnSettingsDialog.h"
#include "ui_VpnSettingsDialog.h"

#include "vpn/VpnManager.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

VpnSettingsDialog::VpnSettingsDialog(VpnManager *manager, QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::VpnSettingsDialog)
    , _manager(manager)
{
    _ui->setupUi(this);

    _ui->backendCB->setCurrentIndex(
        _manager->backend() == VpnManager::Backend::OpenVPN ? 0 : 1);
    _ui->configPathEdit->setText(_manager->configPath());
    _ui->enabledCB->setChecked(_manager->autoConnect());

    connect(_ui->browseBtn,    &QPushButton::clicked, this, &VpnSettingsDialog::onBrowse);
    connect(_ui->startBtn,     &QPushButton::clicked, this, &VpnSettingsDialog::onStart);
    connect(_ui->stopBtn,      &QPushButton::clicked, this, &VpnSettingsDialog::onStop);
    connect(_ui->installBtn,   &QPushButton::clicked, this, &VpnSettingsDialog::onInstall);
    connect(_ui->uninstallBtn, &QPushButton::clicked, this, &VpnSettingsDialog::onUninstall);
    connect(_ui->buttonBox,    &QDialogButtonBox::rejected, this, &QDialog::accept);

    connect(_manager, &VpnManager::stateChanged,        this, &VpnSettingsDialog::onStateChanged);
    connect(_manager, &VpnManager::installStateChanged, this, &VpnSettingsDialog::onInstallStateChanged);
    connect(_manager, &VpnManager::logLine,             this, &VpnSettingsDialog::onLogLine);

    _refreshSetupUi();
    _refreshUi();
}

VpnSettingsDialog::~VpnSettingsDialog()
{
    _commitConfigToManager();
    delete _ui;
}

void VpnSettingsDialog::onBrowse()
{
    QString filter = (_ui->backendCB->currentIndex() == 0)
                         ? tr("OpenVPN config (*.ovpn *.conf);;All files (*)")
                         : tr("WireGuard config (*.conf);;All files (*)");
    QString start = _ui->configPathEdit->text();
    if (start.isEmpty())
        start = QDir::homePath();
    QString f = QFileDialog::getOpenFileName(this, tr("Select VPN config"), start, filter);
    if (!f.isEmpty())
        _ui->configPathEdit->setText(f);
}

void VpnSettingsDialog::onStart()
{
    _commitConfigToManager();
    if (_manager->configPath().isEmpty()) {
        onLogLine(tr("No config file selected."));
        return;
    }
    _manager->start();
}

void VpnSettingsDialog::onStop()
{
    _manager->stop();
}

void VpnSettingsDialog::onInstall()
{
    setEnabled(false);
    bool ok = _manager->runInstall();
    setEnabled(true);
    if (!ok) {
        QMessageBox::warning(this, tr("VPN install"),
                             tr("VPN install failed. See the log for details."));
    }
    _refreshSetupUi();
    _refreshUi();
}

void VpnSettingsDialog::onUninstall()
{
    if (QMessageBox::question(this, tr("Uninstall VPN helper"),
            tr("This will remove the privileged VPN helper and its Polkit rule. "
               "You will need to install it again before using the VPN. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    setEnabled(false);
    bool ok = _manager->runUninstall();
    setEnabled(true);
    if (!ok) {
        QMessageBox::warning(this, tr("VPN uninstall"),
                             tr("VPN uninstall failed. See the log for details."));
    }
    _refreshSetupUi();
    _refreshUi();
}

void VpnSettingsDialog::onStateChanged(VpnManager::State newState)
{
    Q_UNUSED(newState);
    _refreshUi();
}

void VpnSettingsDialog::onInstallStateChanged(bool installed)
{
    Q_UNUSED(installed);
    _refreshSetupUi();
    _refreshUi();
}

void VpnSettingsDialog::onLogLine(QString const &line)
{
    _ui->logView->appendPlainText(line);
}

void VpnSettingsDialog::_refreshUi()
{
    VpnManager::State s = _manager->state();
    QString text = VpnManager::stateToString(s);
    if (s == VpnManager::State::Connected)
        text += QStringLiteral(" — %1 (%2)").arg(_manager->tunInterface(),
                                                 _manager->tunIp().toString());
    _ui->stateLabel->setText(text);

    bool installed = _manager->isHelperInstalled();
    bool canStart = installed
        && (s == VpnManager::State::Disabled || s == VpnManager::State::Failed);
    bool canStop  = (s == VpnManager::State::Connected || s == VpnManager::State::Starting);
    _ui->startBtn->setEnabled(canStart);
    _ui->stopBtn->setEnabled(canStop);
    _ui->configBox->setEnabled(installed);
}

void VpnSettingsDialog::_refreshSetupUi()
{
    bool const installed = _manager->isHelperInstalled();
    _ui->installBtn->setVisible(!installed);
    _ui->uninstallBtn->setVisible(installed);
    if (installed) {
        _ui->setupLabel->setText(
            tr("VPN tunnel is installed. Connect / Disconnect will not prompt."));
        _ui->setupLabel->setStyleSheet(QStringLiteral("color: #2e7d32;"));
    } else {
        _ui->setupLabel->setText(
            tr("VPN tunnel requires a one-time setup (one password prompt)."));
        _ui->setupLabel->setStyleSheet(QStringLiteral("color: #c62828;"));
    }
}

void VpnSettingsDialog::_commitConfigToManager()
{
    // Each setter emits configChanged on actual change → NgPost auto-saves.
    _manager->setBackend(_ui->backendCB->currentIndex() == 0
                             ? VpnManager::Backend::OpenVPN
                             : VpnManager::Backend::WireGuard);
    _manager->setConfigPath(_ui->configPathEdit->text().trimmed());
    _manager->setAutoConnect(_ui->enabledCB->isChecked());
}
