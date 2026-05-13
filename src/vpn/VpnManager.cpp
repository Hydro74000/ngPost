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
#include "WireGuardBackend.h"

#include "nntp/NntpServerParams.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

VpnManager *VpnManager::sInstance = nullptr;

VpnManager::VpnManager(QObject *parent)
    : QObject(parent)
    , _autoConnect(false)
    , _backend(Backend::OpenVPN)
    , _configPath()
    , _state(State::Disabled)
    , _tunIp()
    , _tunIface()
    , _currentBackend(nullptr)
    , _autoStartedByJob(false)
    , _activeJobsNeedingVpn(0)
    , _autoDisconnectTimer(new QTimer(this))
{
    _autoDisconnectTimer->setSingleShot(true);
    _autoDisconnectTimer->setInterval(kAutoDisconnectMs);
    connect(_autoDisconnectTimer, &QTimer::timeout,
            this, &VpnManager::onAutoDisconnectTimeout);

    if (!sInstance)
        sInstance = this;
}

void VpnManager::setAutoConnect(bool v)
{
    if (_autoConnect == v) return;
    _autoConnect = v;
    emit configChanged();
}

void VpnManager::setBackend(Backend b)
{
    if (_backend == b) return;
    _backend = b;
    emit configChanged();
}

void VpnManager::setConfigPath(QString const &p)
{
    if (_configPath == p) return;
    _configPath = p;
    emit configChanged();
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

bool VpnManager::start()
{
    if (_state == State::Starting || _state == State::Connected)
        return true;

    if (_configPath.isEmpty()) {
        emit logLine(tr("VPN: no config file selected"));
        _setState(State::Failed);
        return false;
    }

    _instantiateBackend();
    if (!_currentBackend) {
        _setState(State::Failed);
        return false;
    }

    _setState(State::Starting);
    if (!_currentBackend->start(_configPath)) {
        _setState(State::Failed);
        _destroyBackend();
        return false;
    }
    return true;
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

void VpnManager::onBackendReady(QString const &iface, QHostAddress const &ip)
{
    _tunIface = iface;
    _tunIp    = ip;
    emit logLine(tr("VPN: tunnel up on %1 (%2)").arg(iface, ip.toString()));
    // Policy routing is set up by the privileged helper script under the
    // same pkexec invocation that brought up the tunnel — nothing to do here.
    _setState(State::Connected);
}

void VpnManager::onBackendFailed(QString const &reason)
{
    emit logLine(tr("VPN: failed — %1").arg(reason));
    _tunIp = QHostAddress();
    _tunIface.clear();
    _setState(State::Failed);
    _destroyBackend();

    // If a job was waiting on us, surface the failure so the GUI can popup.
    if (_activeJobsNeedingVpn > 0) {
        emit vpnRequiredButUnavailable(JobBlockReason::VpnFailed, reason);
        _activeJobsNeedingVpn = 0;
        _autoStartedByJob = false;
    }
}

void VpnManager::onBackendStopped()
{
    emit logLine(tr("VPN: tunnel stopped"));
    _tunIp = QHostAddress();
    _tunIface.clear();
    _setState(State::Disabled);
    _destroyBackend();
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

#ifdef Q_OS_LINUX
    if (_backend == Backend::OpenVPN)
        _currentBackend = new OpenVpnBackend(this);
    else
        _currentBackend = new WireGuardBackend(this);
#else
    emit logLine(tr("VPN: backend not supported on this platform yet"));
    _currentBackend = nullptr;
    return;
#endif

    connect(_currentBackend, &VpnBackend::ready,    this, &VpnManager::onBackendReady);
    connect(_currentBackend, &VpnBackend::failed,   this, &VpnManager::onBackendFailed);
    connect(_currentBackend, &VpnBackend::stopped,  this, &VpnManager::onBackendStopped);
    connect(_currentBackend, &VpnBackend::logLine,  this, &VpnManager::logLine);
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
    return QFileInfo::exists(QString::fromLatin1(kInstalledHelperPath));
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
    QString const resDir = bundledResourcesDir();
    QString const installer = resDir + QStringLiteral("/ngpost-vpn-install.sh");
    if (!QFileInfo::exists(installer)) {
        emit logLine(tr("VPN: install script not found at %1").arg(installer));
        return false;
    }

    emit logLine(tr("Running VPN install: pkexec %1 %2").arg(installer, resDir));

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args;
    args << installer << resDir;
    p.start(QStringLiteral("pkexec"), args);
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
    QString const uninstaller = QStringLiteral("/var/lib/ngpost/ngpost-vpn-uninstall.sh");
    if (!QFileInfo::exists(uninstaller)) {
        emit logLine(tr("VPN: uninstall script not found at %1").arg(uninstaller));
        return false;
    }

    // Make sure no tunnel is active first.
    if (_state == State::Connected || _state == State::Starting)
        stop();

    emit logLine(tr("Running VPN uninstall: pkexec %1").arg(uninstaller));

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("pkexec"), {uninstaller});
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
        p->start(QStringLiteral("pkexec"),
                 {QString::fromLatin1(kInstalledHelperPath), QStringLiteral("cleanup")});
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
    if (!isHelperInstalled()) {
        reason = JobBlockReason::HelperNotInstalled;
        detail = tr("The VPN helper is not installed. Open the VPN dialog "
                    "and click Install.");
    } else if (_configPath.isEmpty()) {
        reason = JobBlockReason::NoConfigSelected;
        detail = tr("No VPN configuration file is selected.");
    } else {
        QFileInfo cfg(_configPath);
        if (!cfg.exists() || !cfg.isReadable()) {
            reason = JobBlockReason::ConfigUnreadable;
            detail = tr("The VPN configuration file is missing or unreadable: %1")
                         .arg(_configPath);
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
