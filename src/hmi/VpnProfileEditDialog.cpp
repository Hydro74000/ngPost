//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
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
#include <QSaveFile>

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

bool sameFile(QString const &left, QString const &right)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    QFileInfo const leftInfo(left);
    QFileInfo const rightInfo(right);
    QString const leftCanonical  = leftInfo.canonicalFilePath();
    QString const rightCanonical = rightInfo.canonicalFilePath();
    if (!leftCanonical.isEmpty() && !rightCanonical.isEmpty())
        return leftCanonical.compare(rightCanonical, pathCase) == 0;
    return leftInfo.absoluteFilePath().compare(rightInfo.absoluteFilePath(), pathCase) == 0;
}

bool writeProfileAtomically(QString const &destinationPath,
                            QByteArray const &contents,
                            QString *error)
{
    constexpr QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly)) {
        if (error)
            *error = destination.errorString();
        return false;
    }
    // VPN profiles may contain inline passwords or WireGuard private keys.
    // Apply owner-only permissions to the temporary inode before it can be
    // atomically published, and treat failure as a failed save.
    if (!destination.setPermissions(ownerOnly)) {
        if (error)
            *error = QStringLiteral("Could not restrict the file to owner read/write: %1")
                         .arg(destination.errorString());
        destination.cancelWriting();
        return false;
    }
    if (destination.write(contents) != contents.size()) {
        if (error)
            *error = destination.errorString();
        destination.cancelWriting();
        return false;
    }
    if (!destination.commit()) {
        if (error)
            *error = destination.errorString();
        return false;
    }
    return true;
}

// Publish a replacement only after the complete source has been read and
// written. The previous imported profile therefore survives a read, write or
// disk-full failure, and selecting that same profile file again is a no-op.
bool copyProfileAtomically(QString const &sourcePath,
                           QString const &destinationPath,
                           QString       *error)
{
    if (sameFile(sourcePath, destinationPath)) {
        QFile destination(destinationPath);
        if (!destination.setPermissions(QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner)) {
            if (error)
                *error = QStringLiteral("Could not restrict the file to owner read/write: %1")
                             .arg(destination.errorString());
            return false;
        }
        return true;
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = source.errorString();
        return false;
    }

    const QByteArray contents = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        if (error)
            *error = source.errorString();
        return false;
    }
    return writeProfileAtomically(destinationPath, contents, error);
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

    QString writeError;
    if (!writeProfileAtomically(configFileAbsPath, result, &writeError)) {
        QMessageBox::warning(this,
                             tr("Inline fallback failed"),
                             tr("Could not write %1: %2").arg(configFileAbsPath, writeError));
        return false;
    }
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
    const int existingIndex = _manager->findProfileIndex(_oldName);
    if (existingIndex >= 0)
        p.configBaseDir = _manager->profiles().at(existingIndex).configBaseDir;

    // Determine the destination filename inside <configDir>/vpn/.
    if (!sourceFile.isEmpty()) {
        QFileInfo srcInfo(sourceFile);
        QString ext = "." + srcInfo.suffix();
        if (ext == ".") ext = (p.backend == VpnManager::Backend::OpenVPN) ? ".ovpn" : ".conf";
        p.configFileName = sanitizeForFilename(name) + ext;
    } else {
        // No new file: keep the existing one's basename.
        if (existingIndex >= 0)
            p.configFileName = _manager->profiles().at(existingIndex).configFileName;
    }

    QString destAbs = p.absoluteConfigPath();

    // Different display names may collapse to the same sanitized filename.
    // Never let one profile silently replace another one's active config.
    for (int i = 0; i < _manager->profiles().size(); ++i) {
        if (i == existingIndex)
            continue;
        const VpnProfile &other = _manager->profiles().at(i);
        if (sameFile(destAbs, other.absoluteConfigPath())) {
            QMessageBox::warning(this,
                                 tr("Configuration file already used"),
                                 tr("This profile name would use the same configuration file as "
                                    "'%1'. Please choose another name.")
                                     .arg(other.name));
            return;
        }
    }

    // Saving is a transaction from the user's perspective. Keep the previous
    // file so a declined credential fallback or a manager/service failure can
    // roll the import back completely.
    const bool destinationExisted = QFileInfo::exists(destAbs);
    QByteArray previousContents;
    if (destinationExisted) {
        QFile previous(destAbs);
        if (!previous.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this,
                                 tr("Save failed"),
                                 tr("Could not preserve the existing VPN configuration %1: %2")
                                     .arg(destAbs, previous.errorString()));
            return;
        }
        previousContents = previous.readAll();
        if (previous.error() != QFileDevice::NoError) {
            QMessageBox::warning(this,
                                 tr("Save failed"),
                                 tr("Could not preserve the existing VPN configuration %1: %2")
                                     .arg(destAbs, previous.errorString()));
            return;
        }
    }
    bool rollbackAttempted = false;
    bool rollbackSucceeded = false;
    auto rollbackFile = [&]() -> bool {
        if (rollbackAttempted)
            return rollbackSucceeded;
        rollbackAttempted = true;
        QString rollbackError;
        rollbackSucceeded = destinationExisted
            ? writeProfileAtomically(destAbs, previousContents, &rollbackError)
            : (!QFileInfo::exists(destAbs) || QFile::remove(destAbs));
        if (!rollbackSucceeded)
            QMessageBox::warning(this,
                                 tr("Rollback failed"),
                                 tr("Could not restore the previous VPN configuration %1: %2")
                                     .arg(destAbs, rollbackError));
        return rollbackSucceeded;
    };

    if (!sourceFile.isEmpty()) {
        QString copyError;
        if (!copyProfileAtomically(sourceFile, destAbs, &copyError)) {
            QMessageBox::warning(this, tr("Import failed"),
                                 tr("Could not copy %1 to %2: %3")
                                     .arg(sourceFile, destAbs, copyError));
            return;
        }
    }

    // Credentials (OpenVPN only). If user typed user+pass, persist them.
    bool wroteInline = false;
    if (p.backend == VpnManager::Backend::OpenVPN) {
        QString user = _ui->userEdit->text();
        QString pass = _ui->passEdit->text();
        if (!user.isEmpty() || !pass.isEmpty()) {
            if (!_persistCredentials(name, user, pass, destAbs, &wroteInline)) {
                // user chose to abort the save when keychain unavailable
                rollbackFile();
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
        ok = _manager->updateProfile(_oldName, p, _stagingFileChanged,
                                     rollbackFile);

    if (!ok) {
        rollbackFile();
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not save the profile."));
        return;
    }

    accept();
}
