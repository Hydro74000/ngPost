//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
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
#include "WindowsSecurity.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

#include "nntp/NntpServerParams.h"
#include "utils/PathHelper.h"
#include "utils/WindowsCommandLine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>

#include <qt6keychain/keychain.h>

#include <utility>

using QKeychain::ReadPasswordJob;
using QKeychain::WritePasswordJob;
using QKeychain::DeletePasswordJob;
using QKeychain::Job;

namespace {
constexpr char kKeychainService[] = "ngPost-vpn";

bool parseLinuxOwnerManifest(QByteArray bytes, qint64 *ownerPid,
                             QString *ownerStart)
{
    if (bytes.isEmpty() || bytes.size() >= 2048)
        return false;
    if (bytes.endsWith('\n'))
        bytes.chop(1);
    if (bytes.contains('\n') || bytes.contains('\r'))
        return false;

    static const QRegularExpression pattern(QStringLiteral(
        "^v=2 owner_pid=([0-9]+) owner_start=([0-9]+) helper_pid=[0-9]+ "
        "backend=(?:openvpn|wireguard) session=[a-zA-Z0-9_-]+ phase=[a-z_]+ "
        "config=[a-zA-Z0-9._~:/@+%\\-]* config_sha=(?:[0-9a-f]{64})? "
        "vpn_pid=[0-9]+ vpn_start=[0-9]+ exe=[a-zA-Z0-9._~:/@+%\\-]* "
        "iface=[a-zA-Z0-9_.\\-]+ tun_ip=(?:-|[0-9.]+) "
        "route=(?:0|1|intent) rule=(?:0|1|intent) "
        "interface=(?:0|1|intent) end=1$"));
    QRegularExpressionMatch const match = pattern.match(QString::fromLatin1(bytes));
    if (!match.hasMatch())
        return false;
    bool ok = false;
    qint64 const pid = match.captured(1).toLongLong(&ok);
    if (!ok || pid <= 0)
        return false;
    if (ownerPid)
        *ownerPid = pid;
    if (ownerStart)
        *ownerStart = match.captured(2);
    return true;
}

bool helperDeclaresProtocol2(QByteArray const &prefix)
{
    static const QRegularExpression marker(QStringLiteral(
        "(?:^|\\n)readonly[ \\t]+NGPOST_VPN_HELPER_PROTOCOL=2(?:\\r?\\n|$)"));
    static const QRegularExpression security(QStringLiteral(
        "(?:^|\\n)readonly[ \\t]+NGPOST_VPN_HELPER_SECURITY_REVISION=3(?:\\r?\\n|$)"));
    const QString text = QString::fromLatin1(prefix);
    return marker.match(text).hasMatch() && security.match(text).hasMatch();
}

bool helperPathDeclaresProtocol2(QString const &path)
{
    QFile helper(path);
    return helper.open(QIODevice::ReadOnly)
        && helperDeclaresProtocol2(helper.read(4096));
}
}

VpnManager *VpnManager::sInstance = nullptr;

VpnManager::VpnManager(QObject *parent)
    : QObject(parent)
    , _autoConnect(false)
    , _state(State::Disabled)
    , _health(VpnHealth::Healthy)
    , _tunIp()
    , _tunIface()
    , _dnsServer()
    , _currentBackend(nullptr)
    , _backendStartInProgress(false)
    , _backendFailedDuringStart(false)
    , _nextRunId(0)
    , _currentAttemptId(0)
    , _tunPollTimer(new QTimer(this))
    , _tunPollAttempts(0)
    , _profiles()
    , _activeProfileName()
    , _runtimeAuthFilePath()
    , _autoStartedByJob(false)
    , _activeJobsNeedingVpn(0)
    , _autoDisconnectTimer(new QTimer(this))
    , _cliMode(false)
    , _leaseWaitMinutes(5)
    , _recoveryMaxAttempts(0)
    , _recoveryAttempts(0)
    , _externalRecoveryAttempts(0)
    , _recoveryActive(false)
    , _recoveryReason(FailureKind::None)
    , _recoveryTimer(new QTimer(this))
    , _healthyResetTimer(new QTimer(this))
#ifdef Q_OS_WIN
    , _windowsLeaseHandle(nullptr)
#endif
{
    qRegisterMetaType<BackendTermination>();
    qRegisterMetaType<VpnFailureKind>();
    qRegisterMetaType<VpnBackendHealth>();
    _autoDisconnectTimer->setSingleShot(true);
    _autoDisconnectTimer->setInterval(kAutoDisconnectMs);
    connect(_autoDisconnectTimer, &QTimer::timeout,
            this, &VpnManager::onAutoDisconnectTimeout);

    _recoveryTimer->setSingleShot(true);
    connect(_recoveryTimer, &QTimer::timeout,
            this, &VpnManager::_performExternalRestart);
    _healthyResetTimer->setSingleShot(true);
    _healthyResetTimer->setInterval(60000);
    connect(_healthyResetTimer, &QTimer::timeout, this, [this]() {
        if (_state == State::Connected && _health == VpnHealth::Healthy
            && !_recoveryActive) {
            _recoveryAttempts = 0;
            _externalRecoveryAttempts = 0;
        }
    });

    _tunPollTimer->setSingleShot(false);
    _tunPollTimer->setInterval(kTunPollIntervalMs);
    connect(_tunPollTimer, &QTimer::timeout,
            this, &VpnManager::_pollTunIpAvailability);

    if (!sInstance)
        sInstance = this;
}

void VpnManager::setLeaseWaitMinutes(int minutes)
{
    _leaseWaitMinutes = qBound(0, minutes, 1440);
}

void VpnManager::setRecoveryMaxAttempts(int attempts)
{
    _recoveryMaxAttempts = qBound(0, attempts, 1000);
}

QString VpnManager::currentProcessStartTime()
{
#ifdef Q_OS_LINUX
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (!stat.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("0");
    QByteArray const record = stat.readAll().trimmed();
    int const closeParen = record.lastIndexOf(')');
    if (closeParen < 0)
        return QStringLiteral("0");
    QList<QByteArray> const fields = record.mid(closeParen + 2).split(' ');
    // The first token after comm is field 3; starttime is field 22.
    return fields.size() > 19 ? QString::fromLatin1(fields.at(19))
                              : QStringLiteral("0");
#elif defined(Q_OS_WIN)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return QStringLiteral("0");
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    // The Windows watchdog compares this against
    // [Process]::StartTime.ToFileTimeUtc() in PowerShell. Process.StartTime
    // comes back as a Local DateTime, and ToFileTimeUtc() converts a Local
    // value to UTC before making the file time, so it reproduces exactly the
    // FILETIME GetProcessTimes returned here. The two must keep round-tripping:
    // a mismatch would make the watchdog conclude the PID was reused and tear
    // down a perfectly healthy tunnel.
    return QString::number(value.QuadPart);
#else
    // Only reachable where NGPOST_VPN_SUPPORTED is not defined, i.e. where no
    // lease exists to bind to a PID. A real value would need proc_pidinfo or
    // KERN_PROC_PID on macOS; it is deliberately not written until there is a
    // VPN backend that would consume it, rather than shipped untested.
    static_assert(!VpnManager::vpnPlatformSupported(),
                  "a platform with VPN support owes a real process start time");
    return QStringLiteral("0");
#endif
}

#ifdef Q_OS_WIN
bool VpnManager::_acquireWindowsLease(QString *detail)
{
    if (_windowsLeaseHandle)
        return true;

    // A default mutex DACL is often limited to the creating logon session.
    // The lease is deliberately machine-wide: every authenticated desktop or
    // service account may wait/release it, while SYSTEM retains full control.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            // CreateMutex opens an existing object with MUTEX_ALL_ACCESS, so
            // authenticated owners need GA rather than only SYNCHRONIZE.
            L"D:P(A;;GA;;;SY)(A;;GA;;;AU)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        if (detail)
            *detail = tr("The machine-wide VPN lease security descriptor could not be created "
                         "(Windows error %1).").arg(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE handle = CreateMutexW(&attributes, FALSE, L"Global\\ngPost.VpnLease.v1");
    DWORD const createError = GetLastError();
    LocalFree(descriptor);
    if (!handle) {
        if (detail)
            *detail = tr("The machine-wide VPN lease could not be opened (Windows error %1).")
                          .arg(createError);
        return false;
    }
    DWORD const wait = WaitForSingleObject(handle, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        if (detail)
            *detail = wait == WAIT_TIMEOUT
                ? tr("Another ngPost instance owns the machine-wide VPN lease. %1")
                      .arg(_windowsOwnerDiagnostic())
                : tr("The machine-wide VPN lease could not be acquired (Windows error %1).")
                      .arg(GetLastError());
        CloseHandle(handle);
        return false;
    }
    _windowsLeaseHandle = handle;
    return true;
}

namespace {
QString windowsOwnerPath()
{
    // The lease is machine-wide, so its diagnostic record must not disappear
    // into the current user's AppData tree. ProgramData also lets another
    // interactive session at least identify that machine-wide state exists.
    QString root = QString::fromLocal8Bit(qgetenv("ProgramData"));
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(root).filePath(QStringLiteral("ngPost/vpn-runtime/owner-v1"));
}

}

bool VpnManager::_publishWindowsOwner(Backend selectedBackend, QString *detail)
{
    QString const path = windowsOwnerPath();
    QString const dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath) || !WindowsSecurity::protectOwnerAndSystem(dirPath)) {
        if (detail)
            *detail = tr("Could not create or secure the Windows VPN runtime directory: %1")
                          .arg(dirPath);
        return false;
    }
    QByteArray const record = QStringLiteral(
        "v=1 owner_pid=%1 owner_start=%2 backend=%3 since=%4 end=1\n")
        .arg(QCoreApplication::applicationPid())
        .arg(currentProcessStartTime())
        .arg(backendToString(selectedBackend))
        .arg(QDateTime::currentSecsSinceEpoch()).toLatin1();
    QSaveFile owner(path);
    if (!owner.open(QIODevice::WriteOnly) || owner.write(record) != record.size()
        || !owner.commit() || !WindowsSecurity::protectOwnerAndSystem(path)) {
        owner.cancelWriting();
        if (detail)
            *detail = tr("Could not publish or secure the Windows VPN owner manifest: %1")
                          .arg(path);
        return false;
    }
    return true;
}

QString VpnManager::_windowsOwnerDiagnostic() const
{
    QFile owner(windowsOwnerPath());
    if (!owner.open(QIODevice::ReadOnly | QIODevice::Text))
        return tr("Owner metadata is unavailable.");
    QString const line = QString::fromLatin1(owner.readLine()).trimmed();
    if (!line.endsWith(QLatin1String("end=1")) || line.size() >= 512)
        return tr("Owner metadata is malformed.");
    return line;
}

void VpnManager::_releaseWindowsLease()
{
    if (!_windowsLeaseHandle)
        return;
    HANDLE handle = static_cast<HANDLE>(_windowsLeaseHandle);
    QFile::remove(windowsOwnerPath());
    ReleaseMutex(handle);
    CloseHandle(handle);
    _windowsLeaseHandle = nullptr;
}
#endif

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

bool VpnManager::forceAllConnectionsThroughVpn() const
{
    return _autoConnect && vpnFeatureAvailable() && _activeProfileUsable();
}

bool VpnManager::shouldConfirmMasterSwitchWithoutProfile(QString *detail) const
{
    if (!_autoConnect || !isHelperInstalled())
        return false;
    if (_activeProfileUsable(detail))
        return false;

    if (detail && detail->isEmpty())
        *detail = tr("No active VPN profile / configuration is selected.");
    return true;
}

bool VpnManager::_activeProfileUsable(QString *detail) const
{
    VpnProfile const *active = activeProfile();
    QString const activeCfgPath = active ? active->absoluteConfigPath() : QString();
    if (!active || activeCfgPath.isEmpty()) {
        if (detail)
            *detail = tr("No active VPN profile / configuration is selected.");
        return false;
    }

    QFileInfo const cfg(activeCfgPath);
    if (!cfg.exists() || !cfg.isReadable()) {
        if (detail)
            *detail = tr("The VPN configuration file is missing or unreadable: %1")
                         .arg(activeCfgPath);
        return false;
    }

    if (detail)
        detail->clear();
    return true;
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

bool VpnManager::updateProfile(QString const &oldName, VpnProfile const &p,
                               bool configFileChanged,
                               ConfigRollback restorePreviousConfig)
{
    int idx = findProfileIndex(oldName);
    if (idx < 0 || !p.isValid()) return false;
    // If renaming, ensure the new name doesn't collide with another profile.
    if (p.name != oldName && findProfileIndex(p.name) >= 0) return false;

#if defined(Q_OS_WIN) || defined(NGPOST_TESTING)
    VpnProfile const oldProfile = _profiles.at(idx);
    bool const oldWireGuard = oldProfile.backend == Backend::WireGuard;
    bool const newWireGuard = p.backend == Backend::WireGuard;
    QString const oldConfig = oldProfile.absoluteConfigPath();
    QString const newConfig = p.absoluteConfigPath();
    QString const oldService = WireGuardBackend::serviceNameFromConfig(oldConfig);
    QString const newService = WireGuardBackend::serviceNameFromConfig(newConfig);
    bool const sameService = oldService.compare(newService, Qt::CaseInsensitive) == 0;
    bool const sameConfig = oldConfig.compare(newConfig, Qt::CaseInsensitive) == 0;
    bool const refreshWireGuard = oldWireGuard != newWireGuard
        || (oldWireGuard && newWireGuard
            && (configFileChanged || !sameService || !sameConfig));

    // Re-registering a running service would stop the tunnel behind the
    // manager's back while it still reports Connected. Make the user stop it
    // explicitly; the profile file transaction in the dialog will roll back.
    if (refreshWireGuard && _activeProfileName == oldName
        && (_state == State::Starting || _state == State::Connected
            || _state == State::Reconnecting || _state == State::Stopping)) {
        emit logLine(tr("Disconnect the active VPN before changing its WireGuard configuration."));
        return false;
    }

    if (oldWireGuard && newWireGuard && refreshWireGuard) {
        if (sameService) {
            // SCM cannot hold two services with the same name. Remove the old
            // registration first. If installing its replacement fails and both
            // registrations refer to the same overwritten file, restore that
            // file *before* asking WireGuard to recreate the old service.
            if (!unregisterWindowsWireGuardTunnel(oldService))
                return false;
            if (!registerWindowsWireGuardTunnel(newConfig)) {
                bool oldConfigAvailable = true;
                if (configFileChanged && sameConfig) {
                    oldConfigAvailable = restorePreviousConfig
                        && restorePreviousConfig();
                    if (!oldConfigAvailable)
                        emit logLine(tr("Could not restore the previous WireGuard "
                                        "configuration before recreating %1.")
                                         .arg(oldService));
                }
                if (oldConfigAvailable
                    && !registerWindowsWireGuardTunnel(oldConfig))
                    emit logLine(tr("WireGuard service rollback failed for %1.").arg(oldService));
                return false;
            }
        } else {
            // Preserve the old working service until its replacement exists.
            if (!registerWindowsWireGuardTunnel(newConfig))
                return false;
            if (!unregisterWindowsWireGuardTunnel(oldService)) {
                if (!unregisterWindowsWireGuardTunnel(newService))
                    emit logLine(tr("Could not remove the replacement WireGuard service %1 "
                                    "after the old service failed to uninstall.")
                                     .arg(newService));
                return false;
            }
        }
    } else if (!oldWireGuard && newWireGuard) {
        if (!registerWindowsWireGuardTunnel(newConfig))
            return false;
    } else if (oldWireGuard && !newWireGuard) {
        if (!unregisterWindowsWireGuardTunnel(oldService))
            return false;
    }
#else
    Q_UNUSED(configFileChanged);
    Q_UNUSED(restorePreviousConfig);
#endif

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
    _pendingBackendCleanup = {};
    if (_currentBackend) {
        VpnBackend *backend = _currentBackend;
        _currentBackend = nullptr;
        QObject::disconnect(backend, nullptr, this, nullptr);
        // Synchronously wait for the privileged helper to run its EXIT trap
        // so we don't leave the tunnel + policy routing + openvpn behind
        // when the user quits. 5s is plenty given the trap is mostly
        // local-only ip(8) commands.
        if (backend->isRunning())
            backend->stopAndWait(5000);
        delete backend;
    }
    _shredRuntimeAuthFile();
#ifdef Q_OS_WIN
    _releaseWindowsLease();
#endif
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

#ifdef Q_OS_WIN
// OpenVPNServiceInteractive needs a pathname on Windows.  The file is
// short-lived, owner-only and shredded as soon as the backend stops.
QString writeAuthFile(QString const &user, QString const &pass)
{
    QString const runtimeDir = PathHelper::vpnRuntimeDir();
    if (!WindowsSecurity::protectOwnerAndSystem(runtimeDir))
        return QString();
    QTemporaryFile tf(runtimeDir + QStringLiteral("/auth-XXXXXX"));
    tf.setAutoRemove(false);
    if (!tf.open())
        return QString();
    QByteArray data = user.toUtf8() + "\n" + pass.toUtf8() + "\n";
    if (tf.write(data) != data.size() || !tf.flush()) {
        tf.remove();
        return QString();
    }
    QString const path = tf.fileName();
    tf.close();
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || !WindowsSecurity::protectOwnerAndSystem(path)) {
        QFile::remove(path);
        return QString();
    }
    return path;
}
#endif
} // namespace

bool VpnManager::start()
{
    if (_state == State::Stopping) return false;
    _backendFailedDuringStart = false;
    if (_state == State::Starting || _state == State::Connected)
        return true;
#ifdef Q_OS_WIN
    if (qobject_cast<WireGuardBackend *>(_currentBackend) && _currentBackend->isRunning()) {
        emit statusLine(tr("VPN: previous WireGuard service shutdown is not confirmed."));
        return false;
    }
#endif

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

#ifdef Q_OS_LINUX
    // Do not discover an outdated helper from its legacy READY record: by
    // then v1 may already have deleted or replaced globally named resources.
    // The installed script is root-owned and readable, so its immutable v2
    // declaration is a safe, non-privileged capability check.
    if (!helperPathDeclaresProtocol2(helperScriptPath())) {
        QString const detail = tr("The installed VPN helper needs security revision 3. Open VPN settings and reinstall it with administrator authentication before connecting. An old helper's passwordless authorization remains unsafe until this migration succeeds.");
        _backendFailedDuringStart = true;
        _setState(State::Failed);
        emit logLine(detail);
        emit statusLine(detail);
        if (_autoStartedByJob || _activeJobsNeedingVpn > 0)
            emit vpnRequiredButUnavailable(JobBlockReason::HelperOutdated, detail);
        return false;
    }
#endif

#ifdef Q_OS_WIN
    QString leaseDetail;
    if (!_acquireWindowsLease(&leaseDetail)) {
        _setState(State::LeaseBusy);
        emit vpnRequiredButUnavailable(JobBlockReason::LeaseBusy, leaseDetail);
        emit logLine(leaseDetail);
        return false;
    }
    if (!_publishWindowsOwner(p->backend, &leaseDetail)) {
        _releaseWindowsLease();
        _setState(State::Failed);
        emit logLine(leaseDetail);
        emit vpnRequiredButUnavailable(JobBlockReason::VpnFailed, leaseDetail);
        return false;
    }
#endif

    _instantiateBackend();
    if (!_currentBackend) {
        _setState(State::Failed);
#ifdef Q_OS_WIN
        _releaseWindowsLease();
#endif
        return false;
    }

    // For OpenVPN profiles flagged hasAuth, look up credentials in the
    // keychain. Windows needs a short-lived owner-only file for its vendor
    // service. Linux sends a base64 transport record over helper stdin; the
    // privileged helper alone materialises the 0600 /run copy.
    QString authFilePath;
    QString authPipePayload;
    if (p->backend == Backend::OpenVPN && p->hasAuth) {
        QString user, pass;
        if (readCredentialsBlocking(p->name, &user, &pass)) {
#ifdef Q_OS_WIN
            authFilePath = writeAuthFile(user, pass);
            if (authFilePath.isEmpty()) {
                QString const detail = tr("VPN: could not create the protected OpenVPN authentication file");
                emit logLine(detail);
                _setState(State::Failed);
                _stopAndDestroyBackend();
                _releaseWindowsLease();
                emit vpnRequiredButUnavailable(JobBlockReason::VpnFailed, detail);
                return false;
            }
#else
            // Linux transports the credential material over the already
            // private helper stdin pipe.  Only the privileged helper writes
            // it to its 0600 file below /run; no persistent source copy and
            // no secret command-line argument are created.
            authPipePayload = QString::fromLatin1(
                (user.toUtf8() + '\n' + pass.toUtf8() + '\n').toBase64());
#endif
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
    // The backend interface accepts a single QString today. The NUL trailer
    // carries either the Windows auth pathname or the Linux pipe payload and
    // is never logged or placed on an external process command line.
    QString packed = cfgPath;
#ifdef Q_OS_WIN
    if (!authFilePath.isEmpty())
        packed += QChar(QChar::Null) + authFilePath;
#else
    if (!authPipePayload.isEmpty())
        packed += QChar(QChar::Null) + authPipePayload;
#endif

    _backendStartInProgress = true;
    _currentBackend->beginRun(++_nextRunId);
    bool const started = _currentBackend->start(packed);
    return _finishBackendStart(started);
}

bool VpnManager::_finishBackendStart(bool started)
{
    _backendStartInProgress = false;
    if (!started || _backendFailedDuringStart) {
        // A synchronous protocol failure can already have selected a more
        // precise public state while start() was on the stack. In particular,
        // keep LeaseBusy so admission and the UI do not degrade it to a
        // generic failure merely because the backend returned false.
        if (_state != State::LeaseBusy)
            _setState(State::Failed);
        State const failureState = _state;
        if (!_stopAndDestroyBackend([this, failureState] {
                _setState(failureState);
                _finishBackendStart(false);
            })) return false;
        _shredRuntimeAuthFile();
#ifdef Q_OS_WIN
        if (!_recoveryActive)
            _releaseWindowsLease();
#endif
        return false;
    }
    return true;
}

void VpnManager::_shredRuntimeAuthFile()
{
    // One policy for start failure, restart and recovery exhaustion: do not
    // remove credentials while a backend requiring confirmed stop still owns
    // the tunnel. Each completion path calls us again after termination.
    if (_currentBackend && _currentBackend->requiresConfirmedStop()
        && _currentBackend->isRunning()) return;
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
    // User stop cancels any pending restart/start-failure continuation.
    _pendingBackendCleanup = {};
    if (_state == State::Disabled || _state == State::Stopping)
        return;

    _recoveryActive = false;
    _recoveryTimer->stop();
    _healthyResetTimer->stop();
    _setState(State::Stopping);
    if (_currentBackend)
        _currentBackend->stop();
    else {
        _clearTunnelIdentity();
        _setHealth(VpnHealth::Healthy);
        _setState(State::Disabled);
        _autoStartedByJob = false;
        _activeJobsNeedingVpn = 0;
#ifdef Q_OS_WIN
        _releaseWindowsLease();
#endif
    }
}

void VpnManager::disconnectByUser()
{
    if (_activeJobsNeedingVpn > 0)
        emit manualDisconnectRequested();
    stop();
}

void VpnManager::onBackendReady(QString const &iface, QHostAddress const &ip,
                                QHostAddress const &dns)
{
    if (_state == State::Stopping) return;
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
        BackendTermination termination;
        termination.runId = _currentBackend ? _currentBackend->runId() : _nextRunId;
        termination.kind = VpnTerminationKind::UnexpectedExit;
        termination.failure = FailureKind::TunnelLost;
        termination.wasReady = true;
        termination.detail = reason;
        onBackendTerminated(termination);
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
    _recoveryTimer->stop();
    _recoveryActive = false;
    _recoveryReason = FailureKind::None;
    _setHealth(VpnHealth::Healthy);
    // A helper recreated after an unexpected exit starts in IDLE even though
    // VpnManager deliberately retained the suspended job. Reassert the
    // manager's authoritative activity count before announcing Connected so
    // WireGuard liveness supervision cannot remain disabled after recovery.
    if (_currentBackend)
        _currentBackend->setActive(_activeJobsNeedingVpn > 0);
    _setState(State::Connected);
    _healthyResetTimer->start();
}

void VpnManager::_setHealth(VpnHealth health)
{
    if (_health == health)
        return;
    _health = health;
    if (health != VpnHealth::Healthy)
        _healthyResetTimer->stop();
    if (health == VpnHealth::Suspect
        || health == VpnHealth::RecoveringInternally
        || health == VpnHealth::Restarting)
        _cancelAutoDisconnect();
}

bool VpnManager::_consumeRecoveryAttempt()
{
    if (_recoveryMaxAttempts > 0 && _recoveryAttempts >= _recoveryMaxAttempts)
        return false;
    ++_recoveryAttempts;
    return true;
}

void VpnManager::onBackendHealthChanged(VpnBackendHealth health, QString const &reason)
{
    if (_state == State::Stopping) return;
    if (health == VpnBackendHealth::Healthy) {
        // HEALTHY is only a liveness transition. READY remains the sole source
        // of the bindable interface/address identity.
        if (_tunIp.isNull())
            return;
        if (_health != VpnHealth::Healthy || _state == State::Reconnecting)
            emit statusLine(tr("VPN: tunnel health restored"));
        _completeReady();
        return;
    }
    if (health == VpnBackendHealth::Suspect) {
        _setHealth(VpnHealth::Suspect);
        emit statusLine(tr("VPN: tunnel health is uncertain — %1").arg(reason));
        return;
    }
    if (health == VpnBackendHealth::RecoveringInternally) {
        if (_recoveryActive && _health == VpnHealth::RecoveringInternally)
            return; // repeated management notifications belong to one episode
        if (!_consumeRecoveryAttempt()) {
            _finishRecoveryExhausted(FailureKind::TunnelLost,
                                     tr("OpenVPN recovery budget exhausted"));
            return;
        }
        _recoveryActive = true;
        _recoveryReason = FailureKind::TunnelLost;
        _setHealth(VpnHealth::RecoveringInternally);
        _setState(State::Reconnecting);
        emit vpnInterrupted(_recoveryReason);
        emit statusLine(tr("VPN: OpenVPN is reconnecting internally (attempt %1)")
                            .arg(_recoveryAttempts));
        _recoveryTimer->start(180000);
        return;
    }
    if (_recoveryActive && _health == VpnHealth::RecoveringInternally) {
        // OpenVPN may die (or reach its 180 s deadline) while its soft
        // reconnect episode is already accounted for. Promote that same
        // episode immediately to an external rebuild instead of letting the
        // internal-recovery timer delay a confirmed DOWN.
        _recoveryReason = FailureKind::TunnelLost;
        _scheduleExternalRestart(_recoveryReason);
        return;
    }
    requestRecovery(FailureKind::TunnelLost);
}

void VpnManager::onBackendRestartReady(quint64 attemptId, QString const &iface,
                                       QHostAddress const &ip, QHostAddress const &dns)
{
    if (attemptId != _currentAttemptId)
        return;
    onBackendReady(iface, ip, dns);
}

void VpnManager::onBackendRestartFailed(quint64 attemptId, FailureKind failure,
                                        QString const &detail)
{
    if (attemptId != _currentAttemptId || !_recoveryActive)
        return;
    emit logLine(tr("VPN restart attempt %1 failed: %2").arg(attemptId).arg(detail));
    _recoveryReason = failure;
    _scheduleExternalRestart(failure);
}

void VpnManager::requestRecovery(FailureKind reason)
{
    if (_state == State::Stopping && _currentBackend && _currentBackend->isRunning()) return;
    // Recovery is an operational response to a running backend/supervisor
    // incident. Configuration, authentication and lease failures are startup
    // decisions and must remain terminal rather than entering an unrelated
    // reconnect loop. In particular, NNTP failures never call this API.
    switch (reason) {
    case FailureKind::TunnelLost:
    case FailureKind::HelperExited:
    case FailureKind::ProcessExited:
    case FailureKind::Internal:
        break;
    default:
        return;
    }
    if (_recoveryActive)
        return;
    _recoveryActive = true;
    _recoveryReason = reason;
    _setHealth(VpnHealth::Restarting);
    _setState(State::Reconnecting);
    _clearTunnelIdentity();
    _cancelAutoDisconnect();
    emit vpnInterrupted(reason);
    _scheduleExternalRestart(reason);
}

void VpnManager::_scheduleExternalRestart(FailureKind reason)
{
    Q_UNUSED(reason);
    static const int delays[] = {0, 5, 15, 30, 60, 120};
    int delay = _externalRecoveryAttempts < 6
        ? delays[_externalRecoveryAttempts]
        : 300;
    _setHealth(VpnHealth::Restarting);
    _setState(State::Reconnecting);
    emit statusLine(tr("VPN: restart scheduled in %1 second(s)").arg(delay));
    _recoveryTimer->start(delay * 1000);
}

void VpnManager::_performExternalRestart()
{
    if (!_recoveryActive)
        return;
    if (!_consumeRecoveryAttempt()) {
        _finishRecoveryExhausted(_recoveryReason,
                                 tr("VPN recovery budget exhausted"));
        return;
    }

    ++_externalRecoveryAttempts;
    ++_currentAttemptId;
    emit statusLine(tr("VPN: external restart attempt %1 (recovery attempt %2)")
                        .arg(_externalRecoveryAttempts)
                        .arg(_recoveryAttempts));
    if (_currentBackend && _currentBackend->isRunning()
        && _currentBackend->restart(_currentAttemptId))
        return;

    if (!_stopAndDestroyBackend([this] { _resumeExternalRestart(); })) return;
    _resumeExternalRestart();
}

void VpnManager::_resumeExternalRestart()
{
    _shredRuntimeAuthFile();
    if (!_recoveryActive) return;
    _setState(State::Reconnecting);
    if (!start() && _recoveryActive) {
        if (_pendingBackendCleanup || _state == State::Stopping) return;
        if (_recoveryMaxAttempts > 0 && _recoveryAttempts >= _recoveryMaxAttempts)
            _finishRecoveryExhausted(_recoveryReason, tr("VPN restart failed"));
        else
            _scheduleExternalRestart(_recoveryReason);
    }
}

void VpnManager::_finishRecoveryExhausted(FailureKind reason, QString const &detail)
{
    _recoveryTimer->stop();
    _healthyResetTimer->stop();
    _recoveryActive = false;
    _setHealth(VpnHealth::Restarting);
    if (!_stopAndDestroyBackend([this, reason, detail] {
            _finishRecoveryExhausted(reason, detail);
        })) return;
    _shredRuntimeAuthFile();
    _clearTunnelIdentity();
    _setState(State::Failed);
#ifdef Q_OS_WIN
    _releaseWindowsLease();
#endif
    emit statusLine(tr("VPN: %1").arg(detail));
    emit recoveryExhausted(reason);
}

bool VpnManager::retryVpn()
{
    if (_activeJobsNeedingVpn <= 0 && !_autoStartedByJob)
        return false;
    _recoveryAttempts = 0;
    _externalRecoveryAttempts = 0;
    _recoveryActive = false;
    requestRecovery(_recoveryReason == FailureKind::None
                        ? FailureKind::TunnelLost : _recoveryReason);
    return true;
}

void VpnManager::_clearTunnelIdentity()
{
    _tunPollTimer->stop();
    _tunIp = QHostAddress();
    _tunIface.clear();
    _dnsServer = QHostAddress();
}

VpnManager::JobBlockReason VpnManager::_blockReasonForFailure(FailureKind failure) const
{
    switch (failure) {
    case FailureKind::LeaseBusy: return JobBlockReason::LeaseBusy;
    case FailureKind::HelperOutdated: return JobBlockReason::HelperOutdated;
    case FailureKind::UnattributedVpnState: return JobBlockReason::UnattributedVpnState;
    default: return JobBlockReason::VpnFailed;
    }
}

void VpnManager::onBackendStopPending(QString const &detail)
{
    _recoveryTimer->stop();
    _healthyResetTimer->stop();
    _tunPollTimer->stop();
    _setState(State::Stopping);
    emit statusLine(detail.isEmpty() ? tr("VPN: waiting for confirmed service shutdown…") : detail);
    if (_activeJobsNeedingVpn > 0) emit vpnInterrupted(FailureKind::TunnelLost);
}

void VpnManager::onBackendTerminated(BackendTermination const &termination)
{
    // Ignore a delayed event from a backend which has already been superseded.
    if (_currentBackend && termination.runId != _currentBackend->runId())
        return;

    if (_pendingBackendCleanup) {
        // Only confirmed shutdown may release the backend or resume work.
        if (!_currentBackend || _currentBackend->isRunning()) return;
        auto resume = std::move(_pendingBackendCleanup);
        _pendingBackendCleanup = {};
        _clearTunnelIdentity();
        _cancelAutoDisconnect();
        _destroyBackend();
        resume();
        return;
    }

#ifdef Q_OS_WIN
    // Manager-side failures (e.g. bind timeout) also need confirmed teardown.
    auto wg = qobject_cast<WireGuardBackend *>(_currentBackend);
    if (wg && wg->isRunning()) {
        wg->failAndStop(termination);
        return;
    }
#endif

    _clearTunnelIdentity();
    _cancelAutoDisconnect();

    if (termination.kind == VpnTerminationKind::RequestedStop) {
        emit logLine(tr("VPN: tunnel stopped"));
        emit statusLine(tr("VPN: tunnel stopped"));
        _recoveryActive = false;
        _recoveryTimer->stop();
        _healthyResetTimer->stop();
        _setHealth(VpnHealth::Healthy);
        _setState(State::Disabled);
        _destroyBackend();
        _shredRuntimeAuthFile();
        _autoStartedByJob = false;
        _activeJobsNeedingVpn = 0;
#ifdef Q_OS_WIN
        _releaseWindowsLease();
#endif
        return;
    }

    QString const line = tr("VPN: failed — %1").arg(termination.detail);
    emit logLine(line);
    emit statusLine(line);

    if (termination.kind == VpnTerminationKind::UnexpectedExit
        && termination.wasReady && (_activeJobsNeedingVpn > 0 || _autoStartedByJob)) {
        _destroyBackend();
        _shredRuntimeAuthFile();
        auto const reason = termination.failure == FailureKind::None
                            ? FailureKind::HelperExited : termination.failure;
        if (_recoveryActive) _scheduleExternalRestart(reason);
        else requestRecovery(reason);
        return;
    }

    if (_recoveryActive) {
        _recoveryReason = termination.failure;
        if (_backendStartInProgress)
            _backendFailedDuringStart = true;
        else {
            _destroyBackend();
            _shredRuntimeAuthFile();
            _scheduleExternalRestart(termination.failure);
        }
        return;
    }

    _setState(termination.failure == FailureKind::LeaseBusy
                  ? State::LeaseBusy : State::Failed);
    if (_backendStartInProgress)
        _backendFailedDuringStart = true;
    else
        _stopAndDestroyBackend();
    _shredRuntimeAuthFile();

    bool const waitingJob = _autoStartedByJob || _activeJobsNeedingVpn > 0;
    if (waitingJob)
        emit vpnRequiredButUnavailable(_blockReasonForFailure(termination.failure),
                                       termination.detail);
    if (!termination.wasReady) {
        _activeJobsNeedingVpn = 0;
        _autoStartedByJob = false;
#ifdef Q_OS_WIN
        _releaseWindowsLease();
#endif
    }
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
    connect(_currentBackend, &VpnBackend::restartReady, this, &VpnManager::onBackendRestartReady);
    connect(_currentBackend, &VpnBackend::restartFailed, this, &VpnManager::onBackendRestartFailed);
    connect(_currentBackend, &VpnBackend::healthChanged, this, &VpnManager::onBackendHealthChanged);
    connect(_currentBackend, &VpnBackend::terminated, this, &VpnManager::onBackendTerminated);
    connect(_currentBackend, &VpnBackend::stopPending, this, &VpnManager::onBackendStopPending);
    connect(_currentBackend, &VpnBackend::logLine,    this, &VpnManager::logLine);
    connect(_currentBackend, &VpnBackend::statusLine, this, &VpnManager::statusLine);
}

void VpnManager::_destroyBackend()
{
    if (_currentBackend) {
        QObject::disconnect(_currentBackend, nullptr, this, nullptr);
        _currentBackend->deleteLater();
        _currentBackend = nullptr;
    }
}

bool VpnManager::_stopAndDestroyBackend(std::function<void()> resume)
{
    VpnBackend *backend = _currentBackend;
    if (!backend)
        return true;

    if (backend->requiresConfirmedStop() && backend->isRunning()) {
        if (_pendingBackendCleanup) return false;
        _pendingBackendCleanup = resume ? std::move(resume) : [] {};
        // Keep connections so stopPending and confirmed termination reach us.
        // Install the continuation first: stop() may emit synchronously.
        backend->stop();
        return false;
    }

    // Clear the manager pointer first and detach every backend -> manager
    // connection before stopAndWait(). A backend may report its typed terminal
    // event synchronously and must not re-enter manager cleanup here.
    _currentBackend = nullptr;
    QObject::disconnect(backend, nullptr, this, nullptr);
    if (backend->isRunning())
        backend->stopAndWait(5000);
    backend->deleteLater();
    return true;
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

#ifdef Q_OS_WIN
QStringList VpnManager::windowsProgramFilesRoots()
{
    // Query machine configuration in both registry views, not user-controlled
    // environment variables or a hardcoded system drive.
    QStringList roots;
    for (auto format : {QSettings::Registry64Format, QSettings::Registry32Format}) {
        QSettings settings(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"), format);
        for (auto const &key : {QStringLiteral("ProgramFilesDir"), QStringLiteral("ProgramFilesDir (x86)")}) {
            QString const root = settings.value(key).toString();
            if (QDir::isAbsolutePath(root) && !roots.contains(root)) roots << root;
        }
    }
    return roots;
}
#endif

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
    for (auto const &root : windowsProgramFilesRoots()) {
        QDir const dir(root);
        // Either backend is enough: wireguard.exe gives WG support, the
        // OpenVPN Community binary gives OpenVPN support.
        if (QFileInfo::exists(dir.filePath(QStringLiteral("WireGuard/wireguard.exe")))
            || QFileInfo::exists(dir.filePath(QStringLiteral("OpenVPN/bin/openvpn.exe"))))
            return true;
    }
    return false;
#else
    return helperPathDeclaresProtocol2(QString::fromLatin1(kInstalledHelperPath));
#endif
}

bool VpnManager::vpnFeatureAvailable() const
{
#if !defined(NGPOST_VPN_SUPPORTED)
    // No native integration on this platform. Answering "unavailable" here is
    // what keeps the whole feature inert -- the master switch, the routing and
    // job admission all read this one answer.
    return false;
#elif defined(Q_OS_WIN)
    return isHelperInstalled();
#else
    return !helperScriptPath().isEmpty();
#endif
}

bool VpnManager::stageBundledResources(QString const &destination,
                                       QString *error) const
{
    QString const appDir = QCoreApplication::applicationDirPath();
    bool const appImageLayout =
        QFileInfo::exists(appDir + QStringLiteral("/ngpost-vpn-install.sh"));
    QStringList const files = {
        QStringLiteral("ngpost-vpn-helper.sh"),
        QStringLiteral("ngpost-vpn-install.sh"),
        QStringLiteral("ngpost-vpn-uninstall.sh"),
        QStringLiteral("49-ngpost-vpn.rules.in"),
    };

    for (QString const &file : files) {
        QString source;
        if (appImageLayout) {
            source = appDir + QLatin1Char('/') + file;
        } else if (file.endsWith(QStringLiteral(".rules.in"))) {
            source = appDir + QStringLiteral("/vpn/polkit/") + file;
        } else {
            source = appDir + QStringLiteral("/vpn/scripts/") + file;
        }

        QFileInfo const sourceInfo(source);
        if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
            if (error)
                *error = tr("required VPN resource is missing or unreadable: %1")
                             .arg(source);
            return false;
        }

        QString const target = destination + QLatin1Char('/') + file;
        if (!QFile::copy(source, target)) {
            if (error)
                *error = tr("could not stage VPN resource: %1").arg(file);
            return false;
        }
    }

    QFileDevice::Permissions const scriptPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::ExeOther;
    for (QString const &file : files) {
        if (!file.endsWith(QStringLiteral(".sh")))
            continue;
        QString const target = destination + QLatin1Char('/') + file;
        if (!QFile::setPermissions(target, scriptPermissions)) {
            if (error)
                *error = tr("could not make staged VPN resource executable: %1")
                             .arg(file);
            return false;
        }
    }

    // Keep AppImage-provided VPN executables stable across a brutal parent
    // death. The installer copies these optional files to /var/lib/ngpost/bin;
    // a stale-session cleanup can then validate /proc/<pid>/exe even after the
    // transient AppImage mount has disappeared. Source builds may omit them
    // and use the system tools instead.
    QString const bundledBinDir = appDir + QStringLiteral("/vpn");
    QString const stagedBinDir = destination + QStringLiteral("/bin");
    for (QString const &name : {QStringLiteral("openvpn"),
                                QStringLiteral("wireguard-go"),
                                QStringLiteral("wg")}) {
        QString const source = bundledBinDir + QLatin1Char('/') + name;
        QFileInfo const sourceInfo(source);
        if (!sourceInfo.isFile() || !sourceInfo.isExecutable())
            continue;
        if (!QDir().mkpath(stagedBinDir)
            || !QFile::copy(source, stagedBinDir + QLatin1Char('/') + name)) {
            if (error)
                *error = tr("could not stage bundled VPN executable: %1").arg(name);
            return false;
        }
        if (!QFile::setPermissions(stagedBinDir + QLatin1Char('/') + name,
                                  scriptPermissions)) {
            if (error)
                *error = tr("could not secure staged VPN executable: %1").arg(name);
            return false;
        }
    }
    return true;
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

    QTemporaryDir stage(QDir::tempPath()
                        + QStringLiteral("/ngpost-vpn-install-XXXXXX"));
    if (!stage.isValid()) {
        emit logLine(tr("VPN: could not create a temporary install directory"));
        return false;
    }

    QString error;
    if (!stageBundledResources(stage.path(), &error)) {
        emit logLine(tr("VPN: %1").arg(error));
        return false;
    }

    QString const resDir = stage.path();
    QString const installer = resDir + QStringLiteral("/ngpost-vpn-install.sh");
    QString const launcher = helperLauncherProgram();
    QString const shell = QStringLiteral("/bin/bash");
    emit logLine(tr("Running VPN install: %1 %2 %3")
                     .arg(launcher, shell + QLatin1Char(' ') + installer, resDir));

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = helperLauncherPrefixArgs();
    args << shell << installer << resDir;
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
    if (_state == State::Connected || _state == State::Starting
        || _state == State::Reconnecting)
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
    QString const powershell = WindowsSecurity::systemPowerShell();
    if (powershell.isEmpty()) return -1;
    QStringList psArgs;
    psArgs << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
           << "-File" << script;
    psArgs.append(args);
    // Start-Process joins ArgumentList without preserving PS quoting. Supply
    // ONE string already serialized for the child's Win32 argument parser.
    QString innerArgList = WindowsCommandLine::powershellLiteral(
        WindowsCommandLine::serialize(psArgs));
    QString outerScript =
        QStringLiteral("$ErrorActionPreference='Stop'; $p = Start-Process -FilePath %1 -Verb RunAs -Wait -PassThru -ArgumentList %2; exit $p.ExitCode")
            .arg(WindowsCommandLine::powershellLiteral(powershell), innerArgList);

    QProcess p;
    p.start(powershell,
            QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                          << "-Command" << outerScript);
    if (!p.waitForFinished(120000) || p.exitStatus() != QProcess::NormalExit)
        return -1;
    return p.exitCode();
}
} // namespace

bool VpnManager::registerWindowsWireGuardTunnel(QString const &confAbsPath)
{
#ifdef NGPOST_TESTING
    if (_testRegisterWireGuardService)
        return _testRegisterWireGuardService(confAbsPath);
#endif
    QString script = findWinScript(QStringLiteral("install-wg-tunnel.ps1"));
    if (script.isEmpty()) {
        emit logLine(tr("install-wg-tunnel.ps1 not found in app bundle"));
        return false;
    }
    QString const sid = WindowsSecurity::currentUserSid();
    if (sid.isEmpty()) {
        emit logLine(tr("Could not determine the caller SID; refusing tunnel installation."));
        return false;
    }
    int code = runElevatedPowerShell(script,
        { QStringLiteral("-ConfPath"), confAbsPath,
          QStringLiteral("-InvokerSid"), sid });
    if (code != 0) {
        emit logLine(tr("WireGuard tunnel install failed (exit %1)").arg(code));
        return false;
    }
    emit logLine(tr("WireGuard tunnel service registered."));
    return true;
}

bool VpnManager::unregisterWindowsWireGuardTunnel(QString const &serviceName)
{
#ifdef NGPOST_TESTING
    if (_testUnregisterWireGuardService)
        return _testUnregisterWireGuardService(serviceName);
#endif
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

#if defined(NGPOST_TESTING) && !defined(Q_OS_WIN)
bool VpnManager::registerWindowsWireGuardTunnel(QString const &confAbsPath)
{
    return !_testRegisterWireGuardService || _testRegisterWireGuardService(confAbsPath);
}

bool VpnManager::unregisterWindowsWireGuardTunnel(QString const &serviceName)
{
    return !_testUnregisterWireGuardService || _testUnregisterWireGuardService(serviceName);
}
#endif

#ifdef NGPOST_TESTING
void VpnManager::setWireGuardServiceHooksForTest(WireGuardServiceHook registerHook,
                                                 WireGuardServiceHook unregisterHook)
{
    _testRegisterWireGuardService = std::move(registerHook);
    _testUnregisterWireGuardService = std::move(unregisterHook);
}

void VpnManager::setBackendForTest(VpnBackend *backend, State state)
{
    if (!_stopAndDestroyBackend()) return;
    _currentBackend = backend;
    if (_currentBackend) {
        _currentBackend->setParent(this);
        connect(_currentBackend, &VpnBackend::ready, this, &VpnManager::onBackendReady);
        connect(_currentBackend, &VpnBackend::restartReady, this, &VpnManager::onBackendRestartReady);
        connect(_currentBackend, &VpnBackend::restartFailed, this, &VpnManager::onBackendRestartFailed);
        connect(_currentBackend, &VpnBackend::healthChanged, this, &VpnManager::onBackendHealthChanged);
        connect(_currentBackend, &VpnBackend::terminated, this, &VpnManager::onBackendTerminated);
        connect(_currentBackend, &VpnBackend::stopPending, this, &VpnManager::onBackendStopPending);
        connect(_currentBackend, &VpnBackend::logLine, this, &VpnManager::logLine);
        connect(_currentBackend, &VpnBackend::statusLine, this, &VpnManager::statusLine);
        _currentBackend->beginRun(++_nextRunId);
    }
    _setState(state);
}

bool VpnManager::linuxOwnerManifestForTest(QByteArray bytes, qint64 *ownerPid,
                                           QString *ownerStart)
{
    return parseLinuxOwnerManifest(bytes, ownerPid, ownerStart);
}

bool VpnManager::helperDeclaresProtocol2ForTest(QByteArray const &prefix)
{
    return helperDeclaresProtocol2(prefix);
}
#endif

void VpnManager::runStartupStaleCleanup()
{
#ifdef Q_OS_WIN
    // Kernel mutex ownership disappears with its process. Acquiring it here is
    // therefore enough to prove that a ProgramData owner record is stale; no
    // elevation or vendor-service mutation is involved in this preflight.
    QString detail;
    if (_acquireWindowsLease(&detail))
        _releaseWindowsLease();
    else if (!detail.isEmpty())
        emit logLine(detail);

    // An OpenVPN management password file is shredded and removed by the
    // backend destructor, which a killed ngPost never runs. Those leftovers
    // are the one piece of Windows VPN state that outlives a crash, so sweep
    // them here: we hold no tunnel yet, and any file still present belongs to
    // a run that is over.
    QDir runtimeDir(PathHelper::vpnRuntimeDir());
    const auto stale = runtimeDir.entryInfoList({QStringLiteral("management-*")},
                                                QDir::Files | QDir::Hidden);
    for (QFileInfo const &leftover : stale) {
        QFile file(leftover.absoluteFilePath());
        qint64 const size = leftover.size();
        if (size > 0 && file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QByteArray(static_cast<int>(qMin<qint64>(size, 4096)), '\0'));
            file.close();
        }
        file.remove();
    }
    return;
#elif defined(Q_OS_LINUX)
    // Cheap, unprivileged preflight. The overwhelmingly common path performs
    // no pkexec and therefore never displays an authentication dialog.
    QTimer::singleShot(0, this, [this]() {
        QString const manifestPath = QStringLiteral("/run/ngpost-vpn/owner-v2");
        bool const manifestExists = QFileInfo::exists(manifestPath);
        bool const ifaceExists = QFileInfo::exists(QStringLiteral("/sys/class/net/ngpost-wg0"));
        bool const legacyPidExists = QFileInfo::exists(QStringLiteral("/run/ngpost-vpn-openvpn.pid"));
        bool const legacyMarkerExists = QFileInfo::exists(QStringLiteral("/run/ngpost-vpn.running"));

        auto commandHasOutput = [](QString const &program, QStringList const &args) {
            QProcess probe;
            probe.start(program, args);
            if (!probe.waitForFinished(1000)) {
                probe.kill();
                return false;
            }
            return probe.exitCode() == 0 && !probe.readAllStandardOutput().trimmed().isEmpty();
        };
        bool const ruleExists = commandHasOutput(QStringLiteral("ip"),
            {QStringLiteral("rule"), QStringLiteral("show"), QStringLiteral("priority"),
             QStringLiteral("1042")});
        bool const routeExists = commandHasOutput(QStringLiteral("ip"),
            {QStringLiteral("route"), QStringLiteral("show"), QStringLiteral("table"),
             QStringLiteral("4242")});
        bool const anyArtifact = ifaceExists || legacyPidExists || legacyMarkerExists
                              || ruleExists || routeExists;
        if (!manifestExists && !anyArtifact)
            return;

        auto processMatches = [](qint64 pid, QString const &start) {
            if (pid <= 0)
                return false;
            QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
            if (!stat.open(QIODevice::ReadOnly | QIODevice::Text))
                return false;
            QByteArray const record = stat.readAll().trimmed();
            int const closeParen = record.lastIndexOf(')');
            QList<QByteArray> const fields = closeParen >= 0
                ? record.mid(closeParen + 2).split(' ') : QList<QByteArray>();
            return fields.size() > 19
                && (start.isEmpty() || start == QLatin1String("0")
                    || QString::fromLatin1(fields.at(19)) == start);
        };

        if (manifestExists) {
            QFile manifest(manifestPath);
            if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
                emit logLine(tr("VPN startup preflight could not read %1; no cleanup attempted")
                                 .arg(manifestPath));
                return;
            }
            qint64 ownerPid = 0;
            QString ownerStart;
            if (!parseLinuxOwnerManifest(manifest.read(2048), &ownerPid, &ownerStart)) {
                emit logLine(tr("VPN startup manifest is malformed; no cleanup attempted"));
                return;
            }
            if (processMatches(ownerPid, ownerStart))
                return;
            if (!isHelperInstalled()) {
                emit logLine(tr("An orphaned VPN session was detected, but the v2 helper is not installed"));
                return;
            }
            if (!helperPathDeclaresProtocol2(QString::fromLatin1(kInstalledHelperPath))) {
                emit logLine(tr("An orphaned VPN session was detected, but the installed helper is version 1; no cleanup attempted"));
                return;
            }

            QProcess *p = new QProcess(this);
            p->setProcessChannelMode(QProcess::MergedChannels);
            connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [this, p](int code, QProcess::ExitStatus) {
                        QString const out = QString::fromLocal8Bit(p->readAll()).trimmed();
                        if (out.contains(QLatin1String("CLEANED")))
                            emit logLine(tr("Startup: cleaned an orphaned VPN v2 session"));
                        if (code != 0 && !out.isEmpty())
                            emit logLine(QStringLiteral("[startup-cleanup] ") + out);
                        p->deleteLater();
                    });
            QStringList args = helperLauncherPrefixArgs();
            args << QString::fromLatin1(kInstalledHelperPath)
                 << QStringLiteral("cleanup-stale") << QStringLiteral("--nonblocking")
                 << QStringLiteral("--protocol") << QStringLiteral("2")
                 << QStringLiteral("--owner-pid")
                 << QString::number(QCoreApplication::applicationPid())
                 << QStringLiteral("--owner-start") << currentProcessStartTime();
            p->start(helperLauncherProgram(), args);
            return;
        }

        // Positive-only recognition of a v1 helper. Failure to recognise one
        // never authorises deletion: it remains unattributed and requires the
        // same explicit destructive action. Recognising it disables that action.
        qint64 legacyHelperPid = 0;
        QDir procDir(QStringLiteral("/proc"));
        QStringList const pidDirs = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (QString const &pidText : pidDirs) {
            bool pidOk = false;
            qint64 const pid = pidText.toLongLong(&pidOk);
            if (!pidOk || pid <= 0)
                continue;
            QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
            if (!cmdline.open(QIODevice::ReadOnly))
                continue;
            QList<QByteArray> const argv = cmdline.readAll().split('\0');
            for (int i = 0; i + 1 < argv.size(); ++i) {
                if (QFileInfo(QString::fromLocal8Bit(argv.at(i))).fileName()
                        != QLatin1String("ngpost-vpn-helper.sh"))
                    continue;
                QByteArray const action = argv.at(i + 1);
                if (action == "openvpn" || action == "wireguard") {
                    legacyHelperPid = pid;
                    break;
                }
            }
            if (legacyHelperPid > 0)
                break;
        }

        bool legacyOwnerActive = legacyHelperPid > 0;
        qint64 legacyPid = 0;
        if (legacyPidExists) {
            QFile pidFile(QStringLiteral("/run/ngpost-vpn-openvpn.pid"));
            if (pidFile.open(QIODevice::ReadOnly | QIODevice::Text))
                legacyPid = QString::fromLatin1(pidFile.readLine()).trimmed().toLongLong();
            if (processMatches(legacyPid, QString())) {
                QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(legacyPid));
                if (cmdline.open(QIODevice::ReadOnly)) {
                    QByteArray const args = cmdline.readAll();
                    legacyOwnerActive = legacyOwnerActive
                        || (args.contains("openvpn")
                            && args.contains("--management")
                            && args.contains("127.0.0.1") && args.contains("7505"));
                }
            }
        }
        // Machine-readable diagnostic for a bug report, not prose: kept out of
        // the translation catalogue so no locale can reshape the keys.
        QString const diagnostic =
            QStringLiteral("interface=%1 rule=%2 table=%3 legacy_pid=%4 legacy_helper_pid=%5")
            .arg(ifaceExists ? QStringLiteral("ngpost-wg0") : QStringLiteral("-"))
            .arg(ruleExists ? QStringLiteral("1042") : QStringLiteral("-"))
            .arg(routeExists ? QStringLiteral("4242") : QStringLiteral("-"))
            .arg(legacyPid)
            .arg(legacyHelperPid);
        emit logLine((legacyOwnerActive ? QStringLiteral("LEGACY_OWNER_ACTIVE ")
                                        : QStringLiteral("UNATTRIBUTED_VPN_STATE "))
                     + diagnostic);
        emit unattributedVpnStateDetected(diagnostic, legacyOwnerActive);
    });
#else
    // No VPN integration on this platform, so there is no VPN state to be
    // stale. Silence here is deliberate rather than incidental: every probe
    // above is a Linux path or a Linux command, and reaching them on another
    // Unix would only ever produce a confident "nothing found" about a
    // question this build never asks.
    return;
#endif
}

bool VpnManager::cleanupUnattributed(bool confirmed)
{
#ifdef Q_OS_WIN
    Q_UNUSED(confirmed);
    return false;
#else
    if (!confirmed || !isHelperInstalled())
        return false;
    if (!helperPathDeclaresProtocol2(QString::fromLatin1(kInstalledHelperPath))) {
        emit logLine(tr("The installed VPN helper is version 1; no cleanup was attempted."));
        return false;
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = helperLauncherPrefixArgs();
    args << QString::fromLatin1(kInstalledHelperPath)
         << QStringLiteral("cleanup-unattributed") << QStringLiteral("--yes")
         << QStringLiteral("--protocol") << QStringLiteral("2")
         << QStringLiteral("--owner-pid")
         << QString::number(QCoreApplication::applicationPid())
         << QStringLiteral("--owner-start") << currentProcessStartTime();
    p.start(helperLauncherProgram(), args);
    if (!p.waitForFinished(30000)) {
        p.kill();
        emit logLine(tr("VPN cleanup timed out"));
        return false;
    }
    QString const output = QString::fromLocal8Bit(p.readAll()).trimmed();
    if (!output.isEmpty())
        emit logLine(output);
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
#endif
}

bool VpnManager::jobNeedsVpn(QList<NntpServerParams *> const &activeServers) const
{
    // Use the effective master switch (forceAllConnectionsThroughVpn), which
    // is neutralised when the VPN is not installed/configured/selected -- a
    // hand-edited VPN_AUTO_CONNECT must not force the job to need the VPN in
    // that case. Per-server useVpn remains fail-closed below.
    if (forceAllConnectionsThroughVpn())
        return true;
    // Per-server useVpn stays fail-closed on every platform, including those
    // with no VPN integration at all. Ignoring it there would silently post in
    // the clear to a server the user deliberately marked as VPN-only, which is
    // a worse outcome than refusing the job: the refusal is visible and the
    // user can clear the flag, whereas the clear-text post cannot be undone.
    for (NntpServerParams *srv : activeServers)
        if (srv && srv->enabled && srv->useVpn)
            return true;
    return false;
}

VpnManager::Admission
VpnManager::admitJob(QList<NntpServerParams *> const &activeServers)
{
    return admitJob(activeServers, jobNeedsVpn(activeServers));
}

VpnManager::Admission
VpnManager::admitJob(QList<NntpServerParams *> const &activeServers,
                     bool vpnRequirementFrozen)
{
    QStringList serverUseVpnNames;
    for (NntpServerParams *srv : activeServers) {
        if (!srv || !srv->enabled || !srv->useVpn)
            continue;
        QString name = srv->host.trimmed();
        serverUseVpnNames << (name.isEmpty() ? tr("(unnamed server)") : name);
    }
    bool const perServerVpnRequested = !serverUseVpnNames.isEmpty();

    // The master switch was requested (config VPN_AUTO_CONNECT) but the VPN
    // cannot be used on this machine or has no usable selected profile: ignore
    // the global switch rather than blocking. Per-server `useVpn` is handled
    // below and remains fail-closed.
    // On a platform with no VPN integration the switch is not "on but
    // unusable", it is simply not a switch. Saying anything about a helper
    // would send the user looking for something that does not exist.
    if (_autoConnect && vpnPlatformSupported()) {
        if (!vpnFeatureAvailable()) {
            emit statusLine(tr("Master switch (VPN_AUTO_CONNECT) is ON but no VPN "
                               "helper is installed - ignoring the master switch. "
                               "Per-server 'Use VPN' is still enforced."));
        } else {
            QString profileDetail;
            if (!_activeProfileUsable(&profileDetail)) {
                emit statusLine(tr("Master switch (VPN_AUTO_CONNECT) is ON but the "
                                   "VPN is not correctly configured (%1) - ignoring "
                                   "the master switch. Per-server 'Use VPN' is still "
                                   "enforced.")
                                .arg(profileDetail));
            }
        }
    }

    // This overload receives the immutable decision stored on the job. A
    // false value must stay false even if the live server/master settings were
    // changed while that job waited behind another post.
    if (!vpnRequirementFrozen)
        return Admission::Proceed;

    // Diagnose blocking conditions.
    JobBlockReason reason = JobBlockReason::None;
    QString detail;
    VpnProfile const *active = activeProfile();
    QString activeCfgPath = active ? active->absoluteConfigPath() : QString();

    // capabilityMissing mirrors vpnFeatureAvailable() — "can we spawn a helper",
    // not "did the user run Install" — so admission agrees with the master
    // switch and per-connection routing. See vpnFeatureAvailable() for the
    // platform rationale (in-tree dev/CI helper on *nix, binaries on Windows).
    bool const capabilityMissing = !vpnFeatureAvailable();
    if (capabilityMissing) {
        reason = JobBlockReason::HelperNotInstalled;
        // Telling a macOS user to install a helper would send them looking for
        // something that does not exist for their system. Name the real
        // situation, and the one action that actually unblocks them.
        detail = vpnPlatformSupported()
            ? tr("The VPN helper is not installed. Open the VPN dialog "
                 "and click Install.")
            : tr("ngPost has no VPN support on this operating system.");
    } else if (!active || activeCfgPath.isEmpty()) {
        reason = JobBlockReason::NoConfigSelected;
        detail = tr("No active VPN profile / configuration is selected.");
    } else {
        QFileInfo cfg(activeCfgPath);
        if (!cfg.exists() || !cfg.isReadable()) {
            reason = JobBlockReason::ConfigUnreadable;
            detail = tr("The VPN configuration file is missing or unreadable: %1")
                         .arg(activeCfgPath);
        } else if (_state == State::Failed || _state == State::LeaseBusy) {
            reason = _state == State::LeaseBusy ? JobBlockReason::LeaseBusy
                                                : JobBlockReason::VpnFailed;
            detail = _state == State::LeaseBusy
                ? tr("Another ngPost instance owns the machine-wide VPN lease.")
                : tr("The last VPN attempt failed. Open the VPN dialog and try again.");
        }
    }
    if (reason != JobBlockReason::None) {
        if (perServerVpnRequested) {
            detail = vpnPlatformSupported()
                ? tr("Use VPN is enabled for NNTP server(s): %1.\n\n"
                     "The VPN is not correctly configured: %2\n\n"
                     "Open the VPN options with the VPN button and check the "
                     "VPN configuration, or edit the server configuration and "
                     "disable VPN by clearing its Use VPN checkbox.")
                      .arg(serverUseVpnNames.join(QStringLiteral(", ")), detail)
                // There is no VPN button to send them to, so the only honest
                // instruction is the one that works: clear the flag. The job is
                // refused rather than quietly posted in the clear.
                : tr("Use VPN is enabled for NNTP server(s): %1, but %2\n\n"
                     "Edit the server configuration and clear its Use VPN "
                     "setting to post to it without a tunnel.")
                      .arg(serverUseVpnNames.join(QStringLiteral(", ")), detail);
            emit statusLine(detail);
        }
        emit vpnRequiredButUnavailable(reason, detail);
        return Admission::Blocked;
    }

    // VPN is fine. Already Connected -> proceed. Otherwise kick off start
    // and tell the caller to wait.
    if (_state == State::Connected && _health == VpnHealth::Healthy)
        return Admission::Proceed;

    if (_state == State::Disabled) {
        _autoStartedByJob = true;
        emit logLine(tr("Auto-starting VPN for incoming job..."));
        if (!start()) {
            // A backend that reported failure during start() already supplied
            // its precise reason. Do not produce a
            // second popup with a generic message.
            if (!_backendFailedDuringStart)
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
    if (_currentBackend)
        _currentBackend->setActive(true);
}

void VpnManager::releaseForJob()
{
    if (_activeJobsNeedingVpn > 0)
        --_activeJobsNeedingVpn;
    if (_activeJobsNeedingVpn == 0 && _currentBackend)
        _currentBackend->setActive(false);

    // A tunnel that is no longer healthy must not be kept forever merely
    // because the guarded idle timer is intentionally disabled while health
    // is uncertain.  Once the last retained job has gone away there is
    // nothing left to recover, so tear an auto-started tunnel down directly.
    if (_activeJobsNeedingVpn == 0 && _autoStartedByJob
        && (_recoveryActive || _state == State::Reconnecting
            || (_state == State::Connected && _health != VpnHealth::Healthy))) {
        _autoStartedByJob = false;
        stop();
        return;
    }

    if (_activeJobsNeedingVpn == 0
        && (_state == State::Disabled || _state == State::Failed
            || _state == State::LeaseBusy)) {
        _autoStartedByJob = false;
        return;
    }

    // Schedule the grace timer silently. The user only sees a log line if
    // the timer actually fires — i.e., no follow-up job arrived. Otherwise
    // the message would be misleading whenever the queue continues right
    // after a cancel/finish.
    if (_activeJobsNeedingVpn == 0 && _autoStartedByJob
        && _state == State::Connected && _health == VpnHealth::Healthy
        && !_recoveryActive) {
        _autoDisconnectTimer->start();
    }
}

void VpnManager::onAutoDisconnectTimeout()
{
    // Double-check at fire time: a job may have started in the grace window.
    // We log here (not at scheduling time) so the user only sees the message
    // when the disconnect actually happens.
    if (_state != State::Connected
        || _health != VpnHealth::Healthy
        || _recoveryActive
        || _activeJobsNeedingVpn > 0
        || !_autoStartedByJob)
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
    case State::LeaseBusy: return tr("VPN lease busy");
    case State::Reconnecting: return tr("reconnecting...");
    case State::Stopping:  return tr("stopping...");
    case State::Failed:    return tr("failed");
    }
    return QString();
}
