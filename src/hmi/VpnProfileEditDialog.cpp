//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnProfileEditDialog.h"
#include "ui_VpnProfileEditDialog.h"

#include "utils/PathHelper.h"
#include "vpn/VpnManager.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>

#include <qt6keychain/keychain.h>
using QKeychain::ReadPasswordJob;
using QKeychain::WritePasswordJob;
using QKeychain::Job;

namespace {
constexpr char kKeychainService[] = "ngPost-vpn";

// Sanitize the profile name into something safe to use as a filename basename.
// Keep ASCII alphanumerics, dashes, underscores and dots; replace the rest
// with '_'. This is the *basename derivation* fallback when copying.
QString sanitizeForFilename(QString const &s)
{
    QString out = s;
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), "_");
    if (out.isEmpty()) out = QStringLiteral("profile");
    return out;
}
}

VpnProfileEditDialog::VpnProfileEditDialog(VpnManager *manager,
                                           QString const &oldName,
                                           VpnProfile const &edit,
                                           QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::VpnProfileEditDialog)
    , _manager(manager)
    , _oldName(oldName)
    , _stagingFilePath()
    , _stagingFileChanged(false)
{
    _ui->setupUi(this);
    setWindowTitle(oldName.isEmpty() ? tr("New VPN profile")
                                     : tr("Edit VPN profile"));

    _ui->nameEdit->setText(edit.name);
    _ui->backendCB->setCurrentIndex(edit.backend == VpnManager::Backend::OpenVPN ? 0 : 1);
    // When editing an existing profile, show the imported file path read-only.
    if (!edit.configFileName.isEmpty())
        _ui->configPathEdit->setText(edit.absoluteConfigPath());

    connect(_ui->browseBtn,  &QPushButton::clicked, this, &VpnProfileEditDialog::onBrowse);
    connect(_ui->buttonBox,  &QDialogButtonBox::accepted, this, &VpnProfileEditDialog::onAccept);
    connect(_ui->buttonBox,  &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_ui->backendCB,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VpnProfileEditDialog::onBackendChanged);

    onBackendChanged(_ui->backendCB->currentIndex());

    // Pre-fill auth fields from keychain when editing an existing profile
    // (best-effort, no error if missing).
    if (!oldName.isEmpty() && edit.hasAuth
        && edit.backend == VpnManager::Backend::OpenVPN)
        _loadCredentialsFromKeychain();
}

VpnProfileEditDialog::~VpnProfileEditDialog()
{
    delete _ui;
}

void VpnProfileEditDialog::onBackendChanged(int idx)
{
    // WireGuard authentication uses key material in the .conf — no
    // separate user/password.
    bool isOpenVpn = (idx == 0);
    _ui->authBox->setEnabled(isOpenVpn);
    _ui->authBox->setVisible(isOpenVpn);
}

void VpnProfileEditDialog::onBrowse()
{
    QString filter = (_ui->backendCB->currentIndex() == 0)
                         ? tr("OpenVPN config (*.ovpn *.conf);;All files (*)")
                         : tr("WireGuard config (*.conf);;All files (*)");
    QString start = QDir::homePath();
    QString f = QFileDialog::getOpenFileName(this, tr("Select VPN config"), start, filter);
    if (f.isEmpty())
        return;
    _stagingFilePath    = f;
    _stagingFileChanged = true;
    _ui->configPathEdit->setText(f);
}

void VpnProfileEditDialog::_loadCredentialsFromKeychain()
{
    ReadPasswordJob *job = new ReadPasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(false);
    job->setKey(_oldName);
    QEventLoop loop;
    connect(job, &Job::finished, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    if (job->error() == QKeychain::NoError) {
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(job->textData().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject o = doc.object();
            _ui->userEdit->setText(o.value("user").toString());
            _ui->passEdit->setText(o.value("pass").toString());
        }
    }
    delete job;
}

bool VpnProfileEditDialog::_persistCredentials(QString const &profileName,
                                                QString const &user, QString const &pass,
                                                QString const &configFileAbsPath,
                                                bool *wroteInlineFallback)
{
    *wroteInlineFallback = false;
    if (user.isEmpty() && pass.isEmpty())
        return true; // nothing to persist

    // Try the keychain first.
    QJsonObject o;
    o["user"] = user;
    o["pass"] = pass;
    QString json = QJsonDocument(o).toJson(QJsonDocument::Compact);

    WritePasswordJob *job = new WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(false);
    job->setKey(profileName);
    job->setTextData(json);
    QEventLoop loop;
    connect(job, &Job::finished, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    bool keychainOk = (job->error() == QKeychain::NoError);
    QString keychainErr = job->errorString();
    delete job;

    if (keychainOk)
        return true;

    // Keychain unavailable → ask the user.
    QMessageBox::StandardButton res = QMessageBox::question(
        this,
        tr("Keychain unavailable"),
        tr("ngPost cannot reach the system keychain (%1).\n\n"
           "Do you want to store the credentials directly inside the VPN "
           "config file (%2, mode 600)?\n\n"
           "If you decline, the profile will be saved WITHOUT credentials. "
           "OpenVPN will then either prompt at connection time (and fail in "
           "the non-interactive helper) or use whatever auth-user-pass "
           "directive is already in the .ovpn.")
            .arg(keychainErr, configFileAbsPath),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (res != QMessageBox::Yes)
        return false; // user declined, no creds stored

    // Inline fallback: prepend an auth-user-pass embedded block to the .ovpn,
    // chmod 600 to limit exposure. We rewrite a fresh copy so we don't
    // append more than once on subsequent edits.
    QFile f(configFileAbsPath);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Inline fallback failed"),
                             tr("Could not open %1 for reading.").arg(configFileAbsPath));
        return false;
    }
    QByteArray original = f.readAll();
    f.close();

    QByteArray result;
    // Strip any existing inline auth block to avoid stacking.
    QString text = QString::fromUtf8(original);
    QRegularExpression authBlock(
        QStringLiteral("<auth-user-pass>.*?</auth-user-pass>\\s*"),
        QRegularExpression::DotMatchesEverythingOption);
    text.remove(authBlock);
    // Replace bare `auth-user-pass` directives so openvpn doesn't try to
    // prompt; we'll rely solely on our inline block.
    text.replace(QRegularExpression(QStringLiteral("^\\s*auth-user-pass\\b[^\n]*"),
                                    QRegularExpression::MultilineOption),
                 QStringLiteral("# auth-user-pass replaced by ngPost inline block"));

    QString inlineBlock = QStringLiteral("\n<auth-user-pass>\n%1\n%2\n</auth-user-pass>\n")
                              .arg(user, pass);
    result = (text + inlineBlock).toUtf8();

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Inline fallback failed"),
                             tr("Could not open %1 for writing.").arg(configFileAbsPath));
        return false;
    }
    f.write(result);
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    *wroteInlineFallback = true;
    return true;
}

void VpnProfileEditDialog::onAccept()
{
    QString name = _ui->nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Missing name"),
                             tr("Please give the profile a name."));
        return;
    }
    // If creating or renaming, check uniqueness.
    if (name != _oldName && _manager->findProfileIndex(name) >= 0) {
        QMessageBox::warning(this, tr("Name already used"),
                             tr("Another VPN profile is already named '%1'.").arg(name));
        return;
    }

    // Did we pick a new file?
    QString sourceFile = _stagingFileChanged ? _stagingFilePath : QString();

    // For new profiles, require a config file.
    if (_oldName.isEmpty() && sourceFile.isEmpty()) {
        QMessageBox::warning(this, tr("Missing config"),
                             tr("Please pick a .ovpn or .conf file to import."));
        return;
    }

    VpnProfile p;
    p.name           = name;
    p.backend        = (_ui->backendCB->currentIndex() == 0)
                           ? VpnManager::Backend::OpenVPN
                           : VpnManager::Backend::WireGuard;

    // Determine the destination filename inside <configDir>/vpn/.
    if (!sourceFile.isEmpty()) {
        QFileInfo srcInfo(sourceFile);
        QString ext = "." + srcInfo.suffix();
        if (ext == ".") ext = (p.backend == VpnManager::Backend::OpenVPN) ? ".ovpn" : ".conf";
        p.configFileName = sanitizeForFilename(name) + ext;
    } else {
        // No new file: keep the existing one's basename.
        int idx = _manager->findProfileIndex(_oldName);
        if (idx >= 0)
            p.configFileName = _manager->profiles().at(idx).configFileName;
    }

    QString destAbs = p.absoluteConfigPath();
    if (!sourceFile.isEmpty()) {
        // Copy into <configDir>/vpn/, overwriting if needed.
        if (QFile::exists(destAbs))
            QFile::remove(destAbs);
        if (!QFile::copy(sourceFile, destAbs)) {
            QMessageBox::warning(this, tr("Import failed"),
                                 tr("Could not copy %1 to %2.").arg(sourceFile, destAbs));
            return;
        }
        QFile::setPermissions(destAbs, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // Credentials (OpenVPN only). If user typed user+pass, persist them.
    bool wroteInline = false;
    if (p.backend == VpnManager::Backend::OpenVPN) {
        QString user = _ui->userEdit->text();
        QString pass = _ui->passEdit->text();
        if (!user.isEmpty() || !pass.isEmpty()) {
            if (!_persistCredentials(name, user, pass, destAbs, &wroteInline)) {
                // user chose to abort the save when keychain unavailable
                return;
            }
            p.hasAuth = true;
        } else {
            p.hasAuth = false;
        }
    }

    // Commit to VpnManager.
    bool ok;
    if (_oldName.isEmpty())
        ok = _manager->addProfile(p);
    else
        ok = _manager->updateProfile(_oldName, p);

    if (!ok) {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not save the profile."));
        return;
    }

    accept();
}
