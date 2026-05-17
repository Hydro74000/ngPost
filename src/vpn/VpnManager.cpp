//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnManager.h"

#include "OpenVpnBackend.h"
#include "VpnBackend.h"
#include "VpnProfile.h"
#include "WireGuardBackend.h"
#ifdef Q_OS_WIN
#include "WindowsBindHelper.h"
#endif

#include "nntp/NntpServerParams.h"
#include "utils/PathHelper.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>

#include <qt6keychain/keychain.h>
using QKeychain::ReadPasswordJob;
using QKeychain::WritePasswordJob;
using QKeychain::DeletePasswordJob;
using QKeychain::Job;

namespace {
constexpr char kKeychainService[] = "ngPost-vpn";
}

VpnManager *VpnManager::sInstance = nullptr;

VpnManager::VpnManager(QObject *parent)
    : QObject(parent)
    , _autoConnect(false)
    , _state(State::Disabled)
    , _tunIp()
    , _tunIface()
    , _dnsServer()
    , _currentBackend(nullptr)
    , _tunPollTimer(new QTimer(this))
    , _tunPollAttempts(0)
    , _profiles()
    , _activeProfileName()
    , _runtimeAuthFilePath()
    , _autoStartedByJob(false)
    , _activeJobsNeedingVpn(0)
    , _autoDisconnectTimer(new QTimer(this))
{
    _autoDisconnectTimer->setSingleShot(true);
    _autoDisconnectTimer->setInterval(kAutoDisconnectMs);
    connect(_autoDisconnectTimer, &QTimer::timeout,
            this, &VpnManager::onAutoDisconnectTimeout);

    _tunPollTimer->setSingleShot(false);
    _tunPollTimer->setInterval(kTunPollIntervalMs);
    connect(_tunPollTimer, &QTimer::timeout,
            this, &VpnManager::_pollTunIpAvailability);

    if (!sInstance)
        sInstance = this;
}

void VpnManager::setAutoConnect(bool v)
{
    if (_autoConnect == v) return;
    _autoConnect = v;
    emit configChanged();
}

VpnManager::Backend VpnManager::backend() const
{
    VpnProfile const *p = activeProfile();
    return p ? p->backend : Backend::OpenVPN;
}

QString VpnManager::configPath() const
{
    VpnProfile const *p = activeProfile();
    return p ? p->absoluteConfigPath() : QString();
}

// --- Profile management ---------------------------------------------------

VpnProfile const *VpnManager::activeProfile() const
{
    int idx = findProfileIndex(_activeProfileName);
    return idx >= 0 ? &_profiles.at(idx) : nullptr;
}

int VpnManager::findProfileIndex(QString const &name) const
{
    for (int i = 0; i < _profiles.size(); ++i)
        if (_profiles.at(i).name == name)
            return i;
    return -1;
}

void VpnManager::setActiveProfileName(QString const &name)
{
    if (_activeProfileName == name) return;
    _activeProfileName = name;
    emit configChanged();
    emit profilesChanged();
}

bool VpnManager::addProfile(VpnProfile const &p)
{
    if (!p.isValid()) return false;
    if (findProfileIndex(p.name) >= 0) return false;
#ifdef Q_OS_WIN
    // A WireGuard profile only makes sense on Windows if its tunnel service
    // was registered with runtime ACL grants. Trigger the elevated installer
    // once when the profile is created.
    if (p.backend == Backend::WireGuard) {
        if (!registerWindowsWireGuardTunnel(p.absoluteConfigPath()))
            return false;
    }
#endif
    _profiles << p;
    if (_activeProfileName.isEmpty())
        _activeProfileName = p.name;
    emit configChanged();
    emit profilesChanged();
    return true;
}

bool VpnManager::updateProfile(QString const &oldName, VpnProfile const &p)
{
    int idx = findProfileIndex(oldName);
    if (idx < 0 || !p.isValid()) return false;
    // If renaming, ensure the new name doesn't collide with another profile.
    if (p.name != oldName && findProfileIndex(p.name) >= 0) return false;
    _profiles[idx] = p;
    if (_activeProfileName == oldName)
        _activeProfileName = p.name;
    emit configChanged();
    emit profilesChanged();
    return true;
}

bool VpnManager::removeProfile(QString const &name)
{
    int idx = findProfileIndex(name);
    if (idx < 0) return false;
    VpnProfile victim = _profiles.takeAt(idx);

#ifdef Q_OS_WIN
    // Tear down the Windows service registered for a WG profile. Reuse the
    // single source of truth so dev/test and runtime can't drift.
    if (victim.backend == Backend::WireGuard) {
        unregisterWindowsWireGuardTunnel(
            WireGuardBackend::serviceNameFromConfig(victim.configFileName));
    }
#endif

    // Delete the .ovpn/.conf file we copied into vpn/ at import.
    QString f = victim.absoluteConfigPath();
    if (!f.isEmpty())
        QFile::remove(f);

    // Best-effort: drop credentials from the keychain. Async, fire-and-forget.
    DeletePasswordJob *job = new DeletePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(victim.name);
    job->start();

    // If we removed the active one, pick another (or none).
    if (_activeProfileName == name)
        _activeProfileName = _profiles.isEmpty() ? QString() : _profiles.first().name;

    emit configChanged();
    emit profilesChanged();
    return true;
}

void VpnManager::setProfilesFromConfig(QList<VpnProfile> const &profiles,
                                       QString const &activeName)
{
    _profiles = profiles;
    if (!activeName.isEmpty() && findProfileIndex(activeName) >= 0)
        _activeProfileName = activeName;
    else if (!_profiles.isEmpty())
        _activeProfileName = _profiles.first().name;
    else
        _activeProfileName.clear();
    emit profilesChanged();
}

VpnManager::~VpnManager()
{
    _cancelAutoDisconnect();
    if (_currentBackend) {
        // Synchronously wait for the privileged helper to run its EXIT trap
        // so we don't leave the tunnel + policy routing + openvpn behind
        // when the user quits. 5s is plenty given the trap is mostly
        // local-only ip(8) commands.
        if (_currentBackend->isRunning())
            _currentBackend->stopAndWait(5000);
        delete _currentBackend;
        _currentBackend = nullptr;
    }
    if (sInstance == this)
        sInstance = nullptr;
}

// --- Credentials helpers --------------------------------------------------

namespace {
// Blocking keychain lookup so start() stays synchronous. The keychain APIs
// are async but a local event loop walks them in <1s typically.
bool readCredentialsBlocking(QString const &profileName, QString *user, QString *pass)
{
    // Brace-init to dodge C++ "most vexing parse" — without braces, the
    // declaration is interpreted as a function prototype taking a QString.
    ReadPasswordJob job{QString::fromLatin1(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(profileName);
    QEventLoop loop;
    QObject::connect(&job, &Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    if (job.error() != QKeychain::NoError)
        return false;
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(job.textData().toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    QJsonObject o = doc.object();
    *user = o.value("user").toString();
    *pass = o.value("pass").toString();
    return !user->isEmpty() && !pass->isEmpty();
}

// Write a freshly-allocated temp file containing "user\npass\n" with mode
// 600 inside <vpn runtime dir>. Returns the absolute path or empty on error.
QString writeAuthFile(QString const &user, QString const &pass)
{
    QString tmpl = PathHelper::vpnRuntimeDir() + "/auth-XXXXXX";
    QTemporaryFile *tf = new QTemporaryFile(tmpl);
    tf->setAutoRemove(false);
    tf->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!tf->open()) {
        delete tf;
        return QString();
    }
    QByteArray data = user.toUtf8() + "\n" + pass.toUtf8() + "\n";
    tf->write(data);
    tf->close();
    QString path = tf->fileName();
    delete tf;
    return path;
}
} // namespace

bool VpnManager::start()
{
    if (_state == State::Starting || _state == State::Connected)
        return true;

    VpnProfile const *p = activeProfile();
    if (!p) {
        emit logLine(tr("VPN: no active profile — nothing to start"));
        _setState(State::Failed);
        return false;
    }

    QString cfgPath = p->absoluteConfigPath();
    if (cfgPath.isEmpty() || !QFile::exists(cfgPath)) {
        emit logLine(tr("VPN: config file missing for profile '%1' (%2)").arg(p->name, cfgPath));
        _setState(State::Failed);
        return false;
    }

    _instantiateBackend();
    if (!_currentBackend) {
        _setState(State::Failed);
        return false;
    }

    // For OpenVPN profiles flagged hasAuth, look up credentials in the
    // keychain. If found, write them to a short-lived auth file (chmod 600)
    // under vpn runtime dir; pass its path to the backend so openvpn picks
    // it up via --auth-user-pass. Cleaned up on stop / failure.
    QString authFilePath;
    if (p->backend == Backend::OpenVPN && p->hasAuth) {
        QString user, pass;
        if (readCredentialsBlocking(p->name, &user, &pass)) {
            authFilePath = writeAuthFile(user, pass);
            if (authFilePath.isEmpty())
                emit logLine(tr("VPN: could not create auth file — openvpn will prompt"));
        } else {
            emit logLine(tr("VPN: no credentials in keychain for '%1' (relying on .ovpn inline)").arg(p->name));
        }
    }
    _runtimeAuthFilePath = authFilePath;

    _setState(State::Starting);
    {
        QString line = tr("VPN: connecting profile '%1' (%2)…").arg(
            p->name,
            p->backend == Backend::OpenVPN ? QStringLiteral("OpenVPN") : QStringLiteral("WireGuard"));
        emit logLine(line);
        emit statusLine(line);
    }
    // The backend interface accepts a single configPath today. To pass the
    // auth-file path, we encode it after a NUL separator. OpenVpnBackend
    // unpacks it and adds --auth-user-pass to the openvpn argv.
    QString packed = cfgPath;
    if (!authFilePath.isEmpty())
        packed += QChar(QChar::Null) + authFilePath;

    if (!_currentBackend->start(packed)) {
        _setState(State::Failed);
        _destroyBackend();
        _shredRuntimeAuthFile();
        return false;
    }
    return true;
}

void VpnManager::_shredRuntimeAuthFile()
{
    if (_runtimeAuthFilePath.isEmpty())
        return;
    QFile f(_runtimeAuthFilePath);
    if (f.exists()) {
        // Overwrite with zeros before unlinking so traces don't sit on disk.
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray zeros(static_cast<int>(f.size()), '\0');
            f.write(zeros);
            f.close();
        }
        f.remove();
    }
    _runtimeAuthFilePath.clear();
}

void VpnManager::stop()
{
    if (_state == State::Disabled || _state == State::Stopping)
        return;

    _setState(State::Stopping);
    if (_currentBackend)
        _currentBackend->stop();
    else
        _setState(State::Disabled);
}

void VpnManager::onBackendReady(QString const &iface, QHostAddress const &ip,
                                QHostAddress const &dns)
{
    _tunIface = iface;
    _tunIp    = ip;
    _dnsServer = dns;
    emit logLine(tr("VPN: backend reports ready (%1, %2) — waiting for the address to become bindable")
                     .arg(iface, ip.toString()));
    // Windows can expose a tunnel address while it is still Tentative; Qt sees
    // it, but Winsock rejects bind() with WSAEADDRNOTAVAIL. Non-Windows
    // backends only need the address to appear in the local interface list.
    _tunPollAttempts = 0;
    _pollTunIpAvailability();
}

void VpnManager::_pollTunIpAvailability()
{
    if (_tunIp.isNull()) {
        _tunPollTimer->stop();
        return;
    }
    QList<QHostAddress> all = QNetworkInterface::allAddresses();

#ifdef Q_OS_WIN
    QString bindErr;
    if (WindowsBindHelper::canBindLocalAddress(_tunIp, &bindErr)) {
        _tunPollTimer->stop();
        emit logLine(tr("VPN: tun IP %1 became bindable after %2 ms")
                         .arg(_tunIp.toString())
                         .arg(_tunPollAttempts * kTunPollIntervalMs));
        _completeReady();
        return;
    }
    int const maxAttempts = kTunPollMaxAttemptsWindows;
#else
    if (all.contains(_tunIp)) {
        _tunPollTimer->stop();
        emit logLine(tr("VPN: kernel acknowledged tun IP %1 after %2 ms")
                         .arg(_tunIp.toString())
                         .arg(_tunPollAttempts * kTunPollIntervalMs));
        _completeReady();
        return;
    }
    int const maxAttempts = kTunPollMaxAttempts;
#endif

    ++_tunPollAttempts;
    if (_tunPollAttempts >= maxAttempts) {
        _tunPollTimer->stop();
        QStringList visibleAddrs;
        for (QHostAddress const &a : all)
            visibleAddrs << a.toString();
#ifdef Q_OS_WIN
        QString reason = tr("VPN: tun IP %1 was reported by the backend but "
                            "Winsock never accepted bind() (waited %2 ms; "
                            "last error: %3). Visible local addresses: %4. "
                            "Aborting to avoid a leaking bind.")
                             .arg(_tunIp.toString())
                             .arg(maxAttempts * kTunPollIntervalMs)
                             .arg(bindErr.isEmpty() ? tr("unknown") : bindErr)
                             .arg(visibleAddrs.join(", "));
#else
        QString reason = tr("VPN: tun IP %1 was reported by the backend but "
                            "the kernel never bound it to an interface "
                            "(waited %2 ms). Visible local addresses: %3. "
                            "Aborting to avoid a leaking bind.")
                             .arg(_tunIp.toString())
                             .arg(maxAttempts * kTunPollIntervalMs)
                             .arg(visibleAddrs.join(", "));
#endif
        onBackendFailed(reason);
        return;
    }
    if (!_tunPollTimer->isActive())
        _tunPollTimer->start();
}

void VpnManager::_completeReady()
{
    QString summary = !_dnsServer.isNull()
        ? tr("VPN: connected on %1 (%2), DNS %3")
              .arg(_tunIface, _tunIp.toString(), _dnsServer.toString())
        : tr("VPN: connected on %1 (%2) — no DNS captured, system resolver in use")
              .arg(_tunIface, _tunIp.toString());
    emit logLine(summary);
    emit statusLine(summary);
    _setState(State::Connected);
}

void VpnManager::onBackendFailed(QString const &reason)
{
    QString line = tr("VPN: failed — %1").arg(reason);
    emit logLine(line);
    emit statusLine(line);
    _tunPollTimer->stop();
    _tunIp = QHostAddress();
    _tunIface.clear();
    _dnsServer = QHostAddress();
    _setState(State::Failed);
    _destroyBackend();
    _shredRuntimeAuthFile();

    // If a job was waiting on us, surface the failure so the GUI can popup.
    if (_activeJobsNeedingVpn > 0) {
        emit vpnRequiredButUnavailable(JobBlockReason::VpnFailed, reason);
        _activeJobsNeedingVpn = 0;
        _autoStartedByJob = false;
    }
}

void VpnManager::onBackendStopped()
{
    QString line = tr("VPN: tunnel stopped");
    emit logLine(line);
    emit statusLine(line);
    _tunPollTimer->stop();
    _tunIp = QHostAddress();
    _tunIface.clear();
    _dnsServer = QHostAddress();
    _setState(State::Disabled);
    _destroyBackend();
    _shredRuntimeAuthFile();
    // Reaching Disabled clears the auto-started bookkeeping; a future job
    // will re-trigger via onJobStarted from scratch.
    _autoStartedByJob = false;
    _activeJobsNeedingVpn = 0;
    _cancelAutoDisconnect();
}

void VpnManager::_setState(State s)
{
    if (_state == s)
        return;
    _state = s;
    emit stateChanged(s);
}

void VpnManager::_instantiateBackend()
{
    _destroyBackend();

    VpnProfile const *p = activeProfile();
    Backend backend = p ? p->backend : Backend::OpenVPN;

#if defined(Q_OS_LINUX)
    if (backend == Backend::OpenVPN)
        _currentBackend = new OpenVpnBackend(this);
    else
        _currentBackend = new WireGuardBackend(this);
#elif defined(Q_OS_WIN)
    // Windows backends use the installed VPN services; the C++ backend
    // classes are shared, while start()/stop() differ per OS.
    if (backend == Backend::OpenVPN)
        _currentBackend = new OpenVpnBackend(this);
    else
        _currentBackend = new WireGuardBackend(this);
#else
    Q_UNUSED(backend);
    emit logLine(tr("VPN: backend not supported on this platform yet"));
    _currentBackend = nullptr;
    return;
#endif

    connect(_currentBackend, &VpnBackend::ready,      this, &VpnManager::onBackendReady);
    connect(_currentBackend, &VpnBackend::failed,     this, &VpnManager::onBackendFailed);
    connect(_currentBackend, &VpnBackend::stopped,    this, &VpnManager::onBackendStopped);
    connect(_currentBackend, &VpnBackend::logLine,    this, &VpnManager::logLine);
    connect(_currentBackend, &VpnBackend::statusLine, this, &VpnManager::statusLine);
}

void VpnManager::_destroyBackend()
{
    if (_currentBackend) {
        _currentBackend->deleteLater();
        _currentBackend = nullptr;
    }
}

QString VpnManager::backendToString(Backend b)
{
    return b == Backend::OpenVPN ? QStringLiteral("openvpn") : QStringLiteral("wireguard");
}

VpnManager::Backend VpnManager::backendFromString(QString const &s, bool *ok)
{
    QString v = s.trimmed().toLower();
    if (ok)
        *ok = true;
    if (v == "wireguard" || v == "wg")
        return Backend::WireGuard;
    if (v == "openvpn" || v == "ovpn")
        return Backend::OpenVPN;
    if (ok)
        *ok = false;
    return Backend::OpenVPN;
}

namespace
{
QStringList parseLauncherEnv()
{
    QByteArray env = qgetenv("NGPOST_HELPER_LAUNCHER");
    if (env.isEmpty())
        return {};
    QString s = QString::fromLocal8Bit(env);
    return s.split(QRegularExpression(QStringLiteral("\\s+")),
                   Qt::SkipEmptyParts);
}
}

QString VpnManager::helperLauncherProgram()
{
    auto const parts = parseLauncherEnv();
    if (parts.isEmpty())
        return QStringLiteral("pkexec");
    return parts.first();
}

QStringList VpnManager::helperLauncherPrefixArgs()
{
    auto const parts = parseLauncherEnv();
    if (parts.size() <= 1)
        return {};
    return parts.mid(1);
}

QString VpnManager::helperScriptPath()
{
    // Priority: the system-installed path is the only one the polkit rule
    // whitelists. If it's present, it's the canonical location.
    if (QFileInfo::exists(QString::fromLatin1(kInstalledHelperPath)))
        return QString::fromLatin1(kInstalledHelperPath);

    // Dev fallback: helper next to the binary or in the source tree. This
    // path WILL prompt for a password (no polkit rule applies to it).
    QString const appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/ngpost-vpn-helper.sh");
    candidates << appDir + QStringLiteral("/vpn/scripts/ngpost-vpn-helper.sh");

    for (QString const &p : candidates) {
        QFileInfo fi(p);
        if (fi.exists() && fi.isFile())
            return fi.absoluteFilePath();
    }
    return QString();
}

bool VpnManager::isHelperInstalled() const
{
#ifdef Q_OS_WIN
    // There is no ngPost-owned helper to install on Windows. We rely on
    // OpenVPN Community and WireGuard for Windows; the setup installer
    // chain-installs them.
    //
    // We consider the VPN feature "available" if EITHER:
    //   - wireguard.exe is in a known location (WG support)
    //   - OpenVPN Community is installed (OpenVPN support)
    QStringList wgCandidates = {
        QStringLiteral("C:/Program Files/WireGuard/wireguard.exe"),
        QStringLiteral("C:/Program Files (x86)/WireGuard/wireguard.exe"),
    };
    for (auto const &p : wgCandidates) {
        if (QFileInfo::exists(p))
            return true;
    }
    QStringList ovpnCandidates = {
        QStringLiteral("C:/Program Files/OpenVPN/bin/openvpn.exe"),
        QStringLiteral("C:/Program Files (x86)/OpenVPN/bin/openvpn.exe"),
    };
    for (auto const &p : ovpnCandidates) {
        if (QFileInfo::exists(p))
            return true;
    }
    return false;
#else
    return QFileInfo::exists(QString::fromLatin1(kInstalledHelperPath));
#endif
}

QString VpnManager::bundledResourcesDir() const
{
    // The installer needs a directory containing the 3 scripts + polkit
    // template. In the AppImage everything lands in usr/bin/ next to the
    // binary; for dev builds the in-tree sources live one level deeper.
    QString const appDir = QCoreApplication::applicationDirPath();

    // AppImage layout: helper.sh + install.sh + uninstall.sh + rules.in
    // all directly next to the ngPost binary.
    QFileInfo bundled(appDir + QStringLiteral("/ngpost-vpn-install.sh"));
    if (bundled.exists())
        return appDir;

    // Dev layout: install.sh in vpn/scripts/, rules.in in vpn/polkit/.
    // For the installer to find everything in one place we have to stage a
    // temp directory. Build it under the user's runtime dir.
    QString stage = QDir::tempPath() + QStringLiteral("/ngpost-vpn-install-stage");
    QDir().mkpath(stage);
    QFile::remove(stage + "/ngpost-vpn-helper.sh");
    QFile::remove(stage + "/ngpost-vpn-install.sh");
    QFile::remove(stage + "/ngpost-vpn-uninstall.sh");
    QFile::remove(stage + "/49-ngpost-vpn.rules.in");
    QString const src = appDir + QStringLiteral("/vpn");
    QFile::copy(src + "/scripts/ngpost-vpn-helper.sh",    stage + "/ngpost-vpn-helper.sh");
    QFile::copy(src + "/scripts/ngpost-vpn-install.sh",   stage + "/ngpost-vpn-install.sh");
    QFile::copy(src + "/scripts/ngpost-vpn-uninstall.sh", stage + "/ngpost-vpn-uninstall.sh");
    QFile::copy(src + "/polkit/49-ngpost-vpn.rules.in",   stage + "/49-ngpost-vpn.rules.in");
    QFile::setPermissions(stage + "/ngpost-vpn-helper.sh",    QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);
    QFile::setPermissions(stage + "/ngpost-vpn-install.sh",   QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);
    QFile::setPermissions(stage + "/ngpost-vpn-uninstall.sh", QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);
    return stage;
}

bool VpnManager::runInstall()
{
#ifdef Q_OS_WIN
    // On Windows, the prerequisites (OpenVPN Community + WireGuard for
    // Windows) are installed by ngPost's main setup wizard. There's nothing
    // to install at runtime. Block-scope the local so it doesn't clash with
    // the Linux path's `ok` below when MSVC parses the whole function.
    {
        bool ok = isHelperInstalled();
        if (ok)
            emit logLine(tr("VPN backend prerequisites detected (OpenVPN / WireGuard for Windows)."));
        else
            emit logLine(tr("Install OpenVPN Community and/or WireGuard for Windows, then re-check. "
                            "These are normally bundled with the ngPost setup."));
        emit installStateChanged(ok);
        return ok;
    }
#endif

    QString const resDir = bundledResourcesDir();
    QString const installer = resDir + QStringLiteral("/ngpost-vpn-install.sh");
    if (!QFileInfo::exists(installer)) {
        emit logLine(tr("VPN: install script not found at %1").arg(installer));
        return false;
    }

    QString const launcher = helperLauncherProgram();
    emit logLine(tr("Running VPN install: %1 %2 %3").arg(launcher, installer, resDir));

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = helperLauncherPrefixArgs();
    args << installer << resDir;
    p.start(launcher, args);
    if (!p.waitForFinished(60000)) {
        emit logLine(tr("VPN install: timed out"));
        return false;
    }
    QString out = QString::fromLocal8Bit(p.readAll()).trimmed();
    if (!out.isEmpty())
        emit logLine(out);

    bool ok = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0
               && isHelperInstalled());
    emit installStateChanged(ok);
    return ok;
}

bool VpnManager::runUninstall()
{
#ifdef Q_OS_WIN
    // The Windows prerequisites are owned by their respective vendors'
    // installers. ngPost's uninstall doesn't touch them. The user can
    // remove them via "Add or Remove Programs" if they want.
    emit logLine(tr("On Windows, uninstall OpenVPN Community / WireGuard for "
                    "Windows from \"Add or Remove Programs\" if you want to "
                    "fully remove VPN support."));
    emit installStateChanged(false);
    return true;
#endif

    QString const uninstaller = QStringLiteral("/var/lib/ngpost/ngpost-vpn-uninstall.sh");
    if (!QFileInfo::exists(uninstaller)) {
        emit logLine(tr("VPN: uninstall script not found at %1").arg(uninstaller));
        return false;
    }

    // Make sure no tunnel is active first.
    if (_state == State::Connected || _state == State::Starting)
        stop();

    QString const launcher = helperLauncherProgram();
    emit logLine(tr("Running VPN uninstall: %1 %2").arg(launcher, uninstaller));

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = helperLauncherPrefixArgs();
    args << uninstaller;
    p.start(launcher, args);
    if (!p.waitForFinished(30000)) {
        emit logLine(tr("VPN uninstall: timed out"));
        return false;
    }
    QString out = QString::fromLocal8Bit(p.readAll()).trimmed();
    if (!out.isEmpty())
        emit logLine(out);

    bool ok = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0
               && !isHelperInstalled());
    emit installStateChanged(!ok ? true : false);
    return ok;
}

#ifdef Q_OS_WIN
namespace {
// Locate a bundled PowerShell script: scripts/win/<name> next to the binary
// (Inno Setup deploys them as <appdir>\scripts\win\). Returns absolute
// path or empty string if not found.
QString findWinScript(QString const &name)
{
    QString dir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << dir + "/scripts/win/" + name;
    candidates << dir + "/vpn/scripts/win/" + name; // in-tree dev layout
    for (QString const &p : candidates) {
        if (QFileInfo::exists(p))
            return p;
    }
    return QString();
}

// Run a PowerShell script elevated (UAC) via Start-Process -Verb RunAs.
// Blocks until the elevated PS exits. Returns the inner exit code, or
// negative on failure to even launch.
int runElevatedPowerShell(QString const &script, QStringList const &args)
{
    // Quote each arg for inclusion in a single PowerShell ArgumentList.
    auto quote = [](QString const &s) {
        QString q = s;
        q.replace("'", "''");
        return "'" + q + "'";
    };
    QStringList psArgs;
    psArgs << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
           << "-File" << script;
    psArgs.append(args);
    QStringList quoted;
    for (QString const &a : psArgs)
        quoted << quote(a);

    QString innerArgList = quoted.join(",");
    QString outerScript =
        QStringLiteral("$p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList %1; exit $p.ExitCode")
            .arg(innerArgList);

    QProcess p;
    p.start(QStringLiteral("powershell.exe"),
            QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                          << "-Command" << outerScript);
    if (!p.waitForFinished(120000))
        return -1;
    return p.exitCode();
}
} // namespace

bool VpnManager::registerWindowsWireGuardTunnel(QString const &confAbsPath)
{
    QString script = findWinScript(QStringLiteral("install-wg-tunnel.ps1"));
    if (script.isEmpty()) {
        emit logLine(tr("install-wg-tunnel.ps1 not found in app bundle"));
        return false;
    }
    QString user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    int code = runElevatedPowerShell(script,
        { QStringLiteral("-ConfPath"), confAbsPath,
          QStringLiteral("-InvokerUser"), user });
    if (code != 0) {
        emit logLine(tr("WireGuard tunnel install failed (exit %1)").arg(code));
        return false;
    }
    emit logLine(tr("WireGuard tunnel service registered."));
    return true;
}

bool VpnManager::unregisterWindowsWireGuardTunnel(QString const &serviceName)
{
    QString script = findWinScript(QStringLiteral("uninstall-wg-tunnel.ps1"));
    if (script.isEmpty()) {
        emit logLine(tr("uninstall-wg-tunnel.ps1 not found in app bundle"));
        return false;
    }
    int code = runElevatedPowerShell(script,
        { QStringLiteral("-ServiceName"), serviceName });
    if (code != 0) {
        emit logLine(tr("WireGuard tunnel uninstall failed (exit %1)").arg(code));
        return false;
    }
    emit logLine(tr("WireGuard tunnel service removed."));
    return true;
}
#endif // Q_OS_WIN

void VpnManager::runStartupCleanup()
{
    if (!isHelperInstalled())
        return;

    // Defer to the next event loop iteration so we don't block startup.
    QTimer::singleShot(0, this, [this]() {
        QProcess *p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, p](int code, QProcess::ExitStatus) {
                    QString const out = QString::fromLocal8Bit(p->readAll()).trimmed();
                    if (out.contains(QLatin1String("CLEANED")) && !out.isEmpty())
                        emit logLine(tr("Startup: cleaned up stale VPN state"));
                    if (code != 0 && !out.isEmpty())
                        emit logLine(QStringLiteral("[startup-cleanup] ") + out);
                    p->deleteLater();
                });
        QStringList args = helperLauncherPrefixArgs();
        args << QString::fromLatin1(kInstalledHelperPath) << QStringLiteral("cleanup");
        p->start(helperLauncherProgram(), args);
    });
}

bool VpnManager::jobNeedsVpn(QList<NntpServerParams *> const &activeServers) const
{
    if (_autoConnect)
        return true;
    for (NntpServerParams *srv : activeServers)
        if (srv && srv->enabled && srv->useVpn)
            return true;
    return false;
}

VpnManager::Admission
VpnManager::admitJob(QList<NntpServerParams *> const &activeServers)
{
    if (!jobNeedsVpn(activeServers))
        return Admission::Proceed;

    // Diagnose blocking conditions.
    JobBlockReason reason = JobBlockReason::None;
    QString detail;
    VpnProfile const *active = activeProfile();
    QString activeCfgPath = active ? active->absoluteConfigPath() : QString();

    // The real capability check is "can we locate a helper script we can
    // spawn", not "did the user run Install". The backends use
    // helperScriptPath() — which also resolves the in-tree dev copy — so this
    // gate must agree with them, otherwise a dev/CI build with a valid
    // in-tree helper gets blocked here and the job hangs forever in the
    // pending queue.
    if (helperScriptPath().isEmpty()) {
        reason = JobBlockReason::HelperNotInstalled;
        detail = tr("The VPN helper is not installed. Open the VPN dialog "
                    "and click Install.");
    } else if (!active || activeCfgPath.isEmpty()) {
        reason = JobBlockReason::NoConfigSelected;
        detail = tr("No active VPN profile / configuration is selected.");
    } else {
        QFileInfo cfg(activeCfgPath);
        if (!cfg.exists() || !cfg.isReadable()) {
            reason = JobBlockReason::ConfigUnreadable;
            detail = tr("The VPN configuration file is missing or unreadable: %1")
                         .arg(activeCfgPath);
        } else if (_state == State::Failed) {
            reason = JobBlockReason::VpnFailed;
            detail = tr("The last VPN attempt failed. Open the VPN dialog and try again.");
        }
    }
    if (reason != JobBlockReason::None) {
        emit vpnRequiredButUnavailable(reason, detail);
        return Admission::Blocked;
    }

    // VPN is fine. Already Connected -> proceed. Otherwise kick off start
    // and tell the caller to wait.
    if (_state == State::Connected)
        return Admission::Proceed;

    if (_state == State::Disabled) {
        _autoStartedByJob = true;
        emit logLine(tr("Auto-starting VPN for incoming job..."));
        if (!start()) {
            emit vpnRequiredButUnavailable(JobBlockReason::VpnFailed,
                                           tr("Could not start the VPN."));
            _autoStartedByJob = false;
            return Admission::Blocked;
        }
    }
    // Either Starting (was already starting) or Disabled→Starting (we just
    // kicked it) — caller queues the job and waits for Connected.
    return Admission::Wait;
}

void VpnManager::retainForJob()
{
    _cancelAutoDisconnect();
    ++_activeJobsNeedingVpn;
}

void VpnManager::releaseForJob()
{
    if (_activeJobsNeedingVpn > 0)
        --_activeJobsNeedingVpn;

    // Schedule the grace timer silently. The user only sees a log line if
    // the timer actually fires — i.e., no follow-up job arrived. Otherwise
    // the message would be misleading whenever the queue continues right
    // after a cancel/finish.
    if (_activeJobsNeedingVpn == 0 && _autoStartedByJob
        && _state == State::Connected) {
        _autoDisconnectTimer->start();
    }
}

void VpnManager::onAutoDisconnectTimeout()
{
    // Double-check at fire time: a job may have started in the grace window.
    // We log here (not at scheduling time) so the user only sees the message
    // when the disconnect actually happens.
    if (_activeJobsNeedingVpn > 0)
        return;
    if (!_autoStartedByJob)
        return;
    emit logLine(tr("Queue empty for %1 s — disconnecting VPN")
                     .arg(kAutoDisconnectMs / 1000));
    _autoStartedByJob = false;
    stop();
}

void VpnManager::_cancelAutoDisconnect()
{
    if (_autoDisconnectTimer->isActive())
        _autoDisconnectTimer->stop();
}

QString VpnManager::stateToString(State s)
{
    switch (s) {
    case State::Disabled:  return tr("disabled");
    case State::Starting:  return tr("starting...");
    case State::Connected: return tr("connected");
    case State::Stopping:  return tr("stopping...");
    case State::Failed:    return tr("failed");
    }
    return QString();
}
