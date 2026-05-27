//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnSettingsDialog.h"
#include "VpnProfileEditDialog.h"
#include "ui_VpnSettingsDialog.h"

#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QTextBrowser>
#include <QVBoxLayout>

VpnSettingsDialog::VpnSettingsDialog(VpnManager *manager, QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::VpnSettingsDialog)
    , _manager(manager)
    , _populating(false)
{
    _ui->setupUi(this);
    setWindowTitle(tr("VPN actions"));

    _ui->setupBox->hide();
    _ui->optionsBox->hide();
    _ui->newProfileBtn->hide();
    _ui->editProfileBtn->hide();
    _ui->deleteProfileBtn->hide();
    _ui->profilesBox->setTitle(tr("Connection"));
    _ui->profileLabel->setText(tr("Profile to connect:"));

    connect(_ui->startBtn,         &QPushButton::clicked, this, &VpnSettingsDialog::onStart);
    connect(_ui->stopBtn,          &QPushButton::clicked, this, &VpnSettingsDialog::onStop);
    connect(_ui->installBtn,       &QPushButton::clicked, this, &VpnSettingsDialog::onInstall);
    connect(_ui->uninstallBtn,     &QPushButton::clicked, this, &VpnSettingsDialog::onUninstall);
    connect(_ui->newProfileBtn,    &QPushButton::clicked, this, &VpnSettingsDialog::onNewProfile);
    connect(_ui->editProfileBtn,   &QPushButton::clicked, this, &VpnSettingsDialog::onEditProfile);
    connect(_ui->deleteProfileBtn, &QPushButton::clicked, this, &VpnSettingsDialog::onDeleteProfile);
    connect(_ui->autoConnectCB,    &QCheckBox::toggled,   this, &VpnSettingsDialog::onAutoConnectToggled);
    connect(_ui->profileCB,        QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VpnSettingsDialog::onActiveProfileChanged);
    connect(_ui->buttonBox,        &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(_ui->licencesLink,     &QLabel::linkActivated,      this, &VpnSettingsDialog::onAboutLicences);

    connect(_manager, &VpnManager::stateChanged,        this, &VpnSettingsDialog::onStateChanged);
    connect(_manager, &VpnManager::installStateChanged, this, &VpnSettingsDialog::onInstallStateChanged);
    connect(_manager, &VpnManager::profilesChanged,     this, &VpnSettingsDialog::onProfilesChanged);
    connect(_manager, &VpnManager::logLine,             this, &VpnSettingsDialog::onLogLine);

    _refreshSetupUi();
    _refreshProfilesUi();
    _refreshUi();
}

VpnSettingsDialog::~VpnSettingsDialog()
{
    delete _ui;
}

void VpnSettingsDialog::onAutoConnectToggled(bool checked)
{
    Q_UNUSED(checked);
}

void VpnSettingsDialog::onActiveProfileChanged(int idx)
{
    Q_UNUSED(idx);
}

void VpnSettingsDialog::onStart()
{
    const QString name = _ui->profileCB->currentData().toString();
    if (name.isEmpty()) {
        onLogLine(tr("No VPN profile selected."));
        return;
    }
    _manager->start(name);
}

void VpnSettingsDialog::onStop()
{
    _manager->stop();
}

void VpnSettingsDialog::onNewProfile()
{
    VpnProfile fresh;
    VpnProfileEditDialog dlg(_manager, QString(), fresh, this);
    dlg.exec();
}

void VpnSettingsDialog::onEditProfile()
{
    VpnProfile const *p = _manager->activeProfile();
    if (!p) {
        QMessageBox::information(this, tr("No profile"),
                                 tr("Select a profile to edit."));
        return;
    }
    VpnProfileEditDialog dlg(_manager, p->name, *p, this);
    dlg.exec();
}

void VpnSettingsDialog::onDeleteProfile()
{
    VpnProfile const *p = _manager->activeProfile();
    if (!p) return;
    QString name = p->name;
    if (QMessageBox::question(this, tr("Delete VPN profile"),
            tr("Delete profile '%1'?\n\nIts config file under <configDir>/vpn/ "
               "and its credentials in the keychain will be removed.").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    _manager->removeProfile(name);
}

void VpnSettingsDialog::onInstall()
{
    setEnabled(false);
    bool ok = _manager->runInstall();
    setEnabled(true);
    if (!ok)
        QMessageBox::warning(this, tr("VPN install"),
                             tr("VPN install failed. See the log for details."));
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
    if (!ok)
        QMessageBox::warning(this, tr("VPN uninstall"),
                             tr("VPN uninstall failed. See the log for details."));
    _refreshSetupUi();
    _refreshUi();
}

void VpnSettingsDialog::onStateChanged(VpnManager::State)
{
    _refreshUi();
}

void VpnSettingsDialog::onInstallStateChanged(bool)
{
    _refreshSetupUi();
    _refreshUi();
}

void VpnSettingsDialog::onProfilesChanged()
{
    _refreshProfilesUi();
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
    bool hasActive = !_ui->profileCB->currentData().toString().isEmpty();
    bool canStart  = installed && hasActive
                  && (s == VpnManager::State::Disabled || s == VpnManager::State::Failed);
    bool canStop   = (s == VpnManager::State::Connected || s == VpnManager::State::Starting);
    _ui->startBtn->setEnabled(canStart);
    _ui->stopBtn->setEnabled(canStop);
    _ui->profilesBox->setEnabled(true);
    _ui->profileCB->setEnabled(s == VpnManager::State::Disabled || s == VpnManager::State::Failed);
    _ui->optionsBox->setEnabled(false);

    _ui->editProfileBtn->setEnabled(false);
    _ui->deleteProfileBtn->setEnabled(false);
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

void VpnSettingsDialog::_refreshProfilesUi()
{
    _populating = true;
    _ui->profileCB->clear();
    QList<VpnProfile> const &profiles = _manager->profiles();
    for (VpnProfile const &p : profiles) {
        QString label = QString("%1 [%2]").arg(p.name,
            VpnManager::backendToString(p.backend));
        _ui->profileCB->addItem(label, p.name);
    }
    QString active = _manager->runtimeProfileName();
    if (active.isEmpty())
        active = _manager->activeProfileName();
    if (!active.isEmpty()) {
        int idx = _manager->findProfileIndex(active);
        if (idx >= 0)
            _ui->profileCB->setCurrentIndex(idx);
    }
    _populating = false;
}

void VpnSettingsDialog::onAboutLicences(const QString &)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("VPN components — licence notices"));
    dlg.resize(580, 460);
    auto *layout = new QVBoxLayout(&dlg);
    auto *browser = new QTextBrowser(&dlg);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(
        "<style>"
        "body { font-family: sans-serif; font-size: 9pt; }"
        "h3   { margin-bottom: 2px; }"
        "ul   { margin-top: 2px; }"
        "code { background: #f0f0f0; padding: 1px 3px; border-radius: 2px; }"
        "</style>"
        "<p>The VPN features of ngPost use the following third-party programs, "
        "each distributed under its own licence. ngPost does not modify these "
        "programs; it invokes them as independent system processes.</p>"

        "<h3>OpenVPN Community</h3>"
        "<ul>"
        "<li>Licence: GNU General Public Licence v2 (with OpenSSL exception)</li>"
        "<li>Copyright: © 2002–2024 OpenVPN Inc.</li>"
        "<li>Source: <a href=\"https://github.com/OpenVPN/openvpn\">"
        "https://github.com/OpenVPN/openvpn</a></li>"
        "</ul>"

        "<h3>WireGuard for Windows</h3>"
        "<ul>"
        "<li>Licence: GNU General Public Licence v2 or later</li>"
        "<li>Copyright: © 2015–2024 Jason A. Donenfeld</li>"
        "<li>Source: <a href=\"https://git.zx2c4.com/wireguard-windows\">"
        "https://git.zx2c4.com/wireguard-windows</a></li>"
        "</ul>"

        "<h3>wireguard-tools (<code>wg</code>)</h3>"
        "<ul>"
        "<li>Licence: GNU General Public Licence v2 only</li>"
        "<li>Copyright: © 2015–2024 Jason A. Donenfeld</li>"
        "<li>Source: <a href=\"https://git.zx2c4.com/wireguard-tools\">"
        "https://git.zx2c4.com/wireguard-tools</a></li>"
        "</ul>"

        "<h3>wireguard-go</h3>"
        "<ul>"
        "<li>Licence: MIT</li>"
        "<li>Copyright: © 2017–2024 WireGuard LLC</li>"
        "<li>Source: <a href=\"https://git.zx2c4.com/wireguard-go\">"
        "https://git.zx2c4.com/wireguard-go</a></li>"
        "</ul>"

        "<p>Full licence texts are available at the links above and in "
        "<code>THIRD_PARTY_LICENSES.md</code> in the installation directory.</p>"
    ));
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    layout->addWidget(browser);
    layout->addWidget(box);
    dlg.exec();
}
