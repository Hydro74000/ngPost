//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "WireGuardBackend.h"

#include "VpnManager.h"
#include "VpnProtocol.h"
#ifdef Q_OS_WIN
#include "WindowsSecurity.h"
#include "WindowsServiceControl.h"
#include <QElapsedTimer>
#include <QThread>
#endif

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTimer>

WireGuardBackend::WireGuardBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
    , _protocolV2Seen(false)
#ifdef Q_OS_WIN
    , _winServiceName()
    , _winConfigPath()
    , _winIface()
    , _winPollAttempts(0)
    , _winPollTimer(nullptr)
    , _winWatchdog(nullptr)
#endif
{}

namespace
{
QString bundledVpnBinDir()
{
#ifdef Q_OS_LINUX
    QString const dir = QCoreApplication::applicationDirPath() + QStringLiteral("/vpn");
    if (QFileInfo::exists(dir + QStringLiteral("/wg"))
        || QFileInfo::exists(dir + QStringLiteral("/wireguard-go")))
        return dir;
#endif
    return QString();
}
}

QString WireGuardBackend::serviceNameFromConfig(QString const &configPath)
{
    // The Windows service installed by wireguard.exe /installtunnelservice
    // is named "WireGuardTunnel$" + the conf file basename, without the
    // .conf extension. Pure path manipulation: no Windows API call, safe on
    // every OS so unit tests can exercise it without an SCM.
    QFileInfo fi(configPath);
    return QStringLiteral("WireGuardTunnel$") + fi.completeBaseName();
}

bool WireGuardBackend::parseInterfaceAddrAndDns(QString const &confContent,
                                                QString *ip, QString *dns)
{
    // We only need the [Interface] section. Capture from "[Interface]" to
    // the next bracketed header (or end of string).
    QRegularExpression reSection(
        QStringLiteral("(?ims)^\\s*\\[Interface\\]\\s*\\n(.*?)(?=^\\s*\\[|\\z)"));
    QRegularExpressionMatch ms = reSection.match(confContent);
    QString iface = ms.hasMatch() ? ms.captured(1) : confContent;

    QRegularExpression reAddr(
        QStringLiteral("(?im)^\\s*Address\\s*=\\s*(\\d+\\.\\d+\\.\\d+\\.\\d+)"));
    QRegularExpressionMatch m = reAddr.match(iface);
    if (!m.hasMatch())
        return false;
    if (ip)
        *ip = m.captured(1);

    if (dns) {
        dns->clear();
        QRegularExpression reDns(
            QStringLiteral("(?im)^\\s*DNS\\s*=\\s*([0-9.,\\s]+)"));
        m = reDns.match(iface);
        if (m.hasMatch())
            *dns = m.captured(1).split(',').first().trimmed();
    }
    return true;
}

WireGuardBackend::~WireGuardBackend()
{
#ifdef Q_OS_WIN
    if (_winWatchdog) {
        disconnect(_winWatchdog, nullptr, this, nullptr);
        if (!_winServiceName.isEmpty()) {
            // A timed-out shutdown must not kill the last parent-death guard.
            // Leave this independent process alive until ngPost exits.
            _winWatchdog->setParent(nullptr);
            connect(_winWatchdog, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                    _winWatchdog, &QObject::deleteLater);
        } else {
            _winWatchdog->kill();
            _winWatchdog->waitForFinished(2000);
            delete _winWatchdog;
        }
        _winWatchdog = nullptr;
    }
#endif
    if (_proc) {
        if (_proc->state() != QProcess::NotRunning) {
            _proc->closeWriteChannel();
            if (!_proc->waitForFinished(3000))
                _proc->kill();
        }
        delete _proc;
        _proc = nullptr;
    }
}

bool WireGuardBackend::start(QString const &configPathPacked)
{
    _resetRunState();
#ifdef Q_OS_WIN
    // Drive the registered WireGuard tunnel service via the Service Control
    // Manager. Keep locals scoped away from the Linux path below.
    {
        QString winCfg = configPathPacked;
        int nul = configPathPacked.indexOf(QChar(QChar::Null));
        if (nul >= 0)
            winCfg = configPathPacked.left(nul);
        return _startWindows(winCfg);
    }
#elif !defined(Q_OS_LINUX)
    Q_UNUSED(configPathPacked);
    _emitTerminationOnce(VpnTerminationKind::StartFailure,
                         VpnFailureKind::Configuration,
                         tr("Native VPN integration is currently Linux-only. "
                            "macOS support is in progress."));
    return false;
#endif

    if (_proc && _proc->state() != QProcess::NotRunning)
        return true;

    // The VpnManager may encode auth-file path after \0 for OpenVPN; WireGuard
    // doesn't use auth files (uses keys in the .conf) but we accept the format
    // and ignore the trailer.
    QString configPath = configPathPacked;
    int nulIdx = configPathPacked.indexOf(QChar(QChar::Null));
    if (nulIdx >= 0)
        configPath = configPathPacked.left(nulIdx);

    QFileInfo fi(configPath);
    if (!fi.exists() || !fi.isReadable()) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::Configuration,
                             tr("Config file not found or unreadable: %1").arg(configPath));
        return false;
    }

    QString helper = VpnManager::helperScriptPath();
    if (helper.isEmpty()) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Privileged helper script ngpost-vpn-helper.sh not found"));
        return false;
    }

    _readySignaled = false;
    _protocolV2Seen = false;
    _stdoutBuffer.clear();

    _proc = new QProcess(this);
    _proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_proc, &QProcess::readyReadStandardOutput,
            this, &WireGuardBackend::onReadyReadStdout);
    connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WireGuardBackend::onProcessFinished);
    connect(_proc, &QProcess::errorOccurred,
            this, &WireGuardBackend::onProcessError);

    QStringList helperArgs;
    helperArgs << helper << "wireguard" << fi.absoluteFilePath()
               << "--protocol" << "2"
               << "--owner-pid" << QString::number(QCoreApplication::applicationPid())
               << "--owner-start" << VpnManager::currentProcessStartTime()
               << "--wait-minutes" << QString::number(VpnManager::instance()
                       ? VpnManager::instance()->effectiveLeaseWaitMinutes() : 0);
    QString const binDir = helper == QString::fromLatin1(VpnManager::kInstalledHelperPath)
        ? QString() : bundledVpnBinDir();
    if (!binDir.isEmpty())
        helperArgs << "--bin-dir" << binDir;

    QString const launcher = VpnManager::helperLauncherProgram();
    QStringList args = VpnManager::helperLauncherPrefixArgs();
    args += helperArgs;

    emit logLine(tr("Launching VPN helper: %1 %2").arg(launcher, args.join(' ')));
    _proc->start(launcher, args);
    if (!_proc->waitForStarted(5000)) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Failed to start %1/helper").arg(launcher));
        delete _proc;
        _proc = nullptr;
        return false;
    }
    return true;
}

void WireGuardBackend::stop()
{
    _requestStop();
#ifdef Q_OS_WIN
    _stopWindows();
    return;
#endif
    if (!_proc) {
        _emitTerminationOnce(VpnTerminationKind::RequestedStop,
                             VpnFailureKind::None, QString());
        return;
    }
    if (_proc->state() == QProcess::NotRunning) {
        _emitTerminationOnce(VpnTerminationKind::RequestedStop,
                             VpnFailureKind::None, QString());
        return;
    }
    _proc->closeWriteChannel();
}

bool WireGuardBackend::restart(quint64 attemptId)
{
#ifdef Q_OS_WIN
    Q_UNUSED(attemptId);
    return false;
#else
    if (!_proc || _proc->state() == QProcess::NotRunning)
        return false;
    QByteArray command = QByteArrayLiteral("RESTART attempt_id=")
        + QByteArray::number(attemptId) + '\n';
    return _proc->write(command) == command.size();
#endif
}

void WireGuardBackend::setActive(bool active)
{
#ifndef Q_OS_WIN
    if (_proc && _proc->state() != QProcess::NotRunning)
        _proc->write(active ? "ACTIVE\n" : "IDLE\n");
#else
    Q_UNUSED(active);
#endif
}

void WireGuardBackend::stopAndWait(int timeoutMs)
{
    stop();
#ifdef Q_OS_WIN
    QElapsedTimer deadline;
    deadline.start();
    while (isRunning() && deadline.elapsed() < timeoutMs) {
        QThread::msleep(50);
        _pollWindowsStop();
    }
    return;
#endif
    if (_proc && _proc->state() != QProcess::NotRunning)
        _proc->waitForFinished(timeoutMs);
}

bool WireGuardBackend::isRunning() const
{
#ifdef Q_OS_WIN
    return !_winServiceName.isEmpty();
#endif
    return _proc && _proc->state() != QProcess::NotRunning;
}

void WireGuardBackend::onReadyReadStdout()
{
    if (!_proc)
        return;
    _stdoutBuffer.append(_proc->readAllStandardOutput());
    int nl;
    while ((nl = _stdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray rawLine = _stdoutBuffer.left(nl);
        _stdoutBuffer.remove(0, nl + 1);
        _handleLine(QString::fromLocal8Bit(rawLine).trimmed());
    }
}

void WireGuardBackend::_handleLine(QString const &line)
{
    if (line.isEmpty())
        return;
    VpnProtocol::Message const message = VpnProtocol::parse(line);
    using Type = VpnProtocol::Type;
    if (message.type == Type::Invalid) {
        emit logLine(line);
        return;
    }
    if (message.type == Type::Protocol) {
        _protocolV2Seen = true;
        return;
    }
    if (message.type == Type::Log) {
        emit logLine(message.detail);
        return;
    }
    if (message.type == Type::Waiting || message.type == Type::Busy) {
        emit statusLine(message.type == Type::Waiting
            ? tr("Waiting for VPN lease held by ngPost PID %1 (helper %2)")
                  .arg(message.fields.value(QStringLiteral("owner_pid")),
                       message.fields.value(QStringLiteral("helper_pid")))
            : tr("VPN lease is held by ngPost PID %1 (helper %2)")
                  .arg(message.fields.value(QStringLiteral("owner_pid")),
                       message.fields.value(QStringLiteral("helper_pid"))));
        if (message.type == Type::Waiting)
            return;
    }
    if (message.type == Type::Ready) {
        if (!_protocolV2Seen || message.legacy) {
            _emitTerminationOnce(VpnTerminationKind::StartFailure,
                                 VpnFailureKind::HelperOutdated,
                                 tr("The installed VPN helper is version 1; update it before connecting."));
            if (_proc)
                _proc->closeWriteChannel();
            return;
        }
        QHostAddress const ip(message.fields.value(QStringLiteral("ip")));
        QHostAddress dns;
        QString const dnsText = message.fields.value(QStringLiteral("dns"));
        if (!dnsText.isEmpty() && dnsText != QLatin1String("-"))
            dns = QHostAddress(dnsText);
        bool ok = false;
        quint64 const attemptId = message.fields.value(QStringLiteral("attempt_id")).toULongLong(&ok);
        if (ip.isNull() || !ok) {
            emit logLine(tr("Malformed READY from helper: %1").arg(line));
            return;
        }
        _readySignaled = true;
        _markReady();
        if (attemptId == 0)
            emit ready(message.fields.value(QStringLiteral("iface")), ip, dns);
        else
            emit restartReady(attemptId, message.fields.value(QStringLiteral("iface")), ip, dns);
        return;
    }
    if (message.type == Type::Suspect) {
        emit healthChanged(VpnBackendHealth::Suspect,
                           message.fields.value(QStringLiteral("reason")));
        return;
    }
    if (message.type == Type::Healthy) {
        emit healthChanged(VpnBackendHealth::Healthy, QString());
        return;
    }
    if (message.type == Type::Down) {
        emit healthChanged(VpnBackendHealth::Down,
                           message.fields.value(QStringLiteral("reason")));
        return;
    }
    if (message.type == Type::RestartFailed) {
        emit restartFailed(message.fields.value(QStringLiteral("attempt_id")).toULongLong(),
                           vpnFailureKindFromProtocol(
                               message.fields.value(QStringLiteral("failure")),
                               VpnFailureKind::TunnelLost),
                           message.detail);
        return;
    }
    VpnFailureKind failure = VpnFailureKind::Internal;
    if (message.legacy) failure = VpnFailureKind::HelperOutdated;
    else if (message.type == Type::Busy) failure = VpnFailureKind::LeaseBusy;
    else if (message.type == Type::LeaseTimeout) failure = VpnFailureKind::LeaseTimeout;
    else if (message.type == Type::LeaseUnavailable) failure = VpnFailureKind::LeaseUnavailable;
    else if (message.type == Type::RuntimeNotVolatile) failure = VpnFailureKind::RuntimeNotVolatile;
    else if (message.type == Type::UnattributedVpnState) failure = VpnFailureKind::UnattributedVpnState;
    else if (message.type == Type::LegacyOwnerActive) failure = VpnFailureKind::LeaseBusy;
    else if (message.type == Type::Error)
        failure = vpnFailureKindFromProtocol(
            message.fields.value(QStringLiteral("failure")));
    if (message.isTerminal()) {
        QString detail = message.detail;
        if (detail.isEmpty()) detail = line;
        _emitTerminationOnce(_wasReady ? VpnTerminationKind::UnexpectedExit
                                       : VpnTerminationKind::StartFailure,
                             failure, detail);
    }
}

void WireGuardBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    VpnTerminationKind const kind = _stopRequested
        ? VpnTerminationKind::RequestedStop
        : (_wasReady ? VpnTerminationKind::UnexpectedExit
                     : VpnTerminationKind::StartFailure);
    VpnFailureKind const failure = _stopRequested ? VpnFailureKind::None
        : (_wasReady ? VpnFailureKind::HelperExited : VpnFailureKind::HelperUnavailable);
    QString const detail = status == QProcess::CrashExit
        ? tr("VPN helper crashed") : tr("VPN helper exited with code %1").arg(exitCode);
    _emitTerminationOnce(kind, failure, detail);
}

void WireGuardBackend::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("pkexec/helper failed to start"));
    else if (err == QProcess::Crashed)
        _emitTerminationOnce(_wasReady ? VpnTerminationKind::UnexpectedExit
                                       : VpnTerminationKind::StartFailure,
                             _wasReady ? VpnFailureKind::HelperExited
                                       : VpnFailureKind::HelperUnavailable,
                             tr("helper process crashed"));
}

#ifdef Q_OS_WIN
// ===========================================================================
//  Windows backend — drive the per-tunnel WireGuard service via SCM.
// ===========================================================================
//
// At profile-import time on Windows, ngPost runs (with UAC elevation):
//   wireguard.exe /installtunnelservice <copy-of-.conf-under-configDir>
// This creates a service named "WireGuardTunnel$<basename>" that runs
// in SYSTEM context and brings up the tunnel via Wintun. The service is
// configured as start=demand. ngPost then applies an ACL via `sc sdset`
// granting START / STOP to the current user, so that subsequent runtime
// Connect / Disconnect issued from the unprivileged ngPost process do
// not trigger UAC.
//
// Runtime here (no UAC):
//   - Connect    : sc start WireGuardTunnel$<name>     → wait RUNNING
//   - Disconnect : sc stop  WireGuardTunnel$<name>
//   - Query info : wg.exe show <name>  → iface, ip, dns
//
// SCM queries use the native API: localized sc.exe text and a successful
// STOP request cannot prove that the service is actually stopped.

#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

bool WireGuardBackend::_queryTunnelInfo(QString *iface, QString *ip, QString *dns) const
{
    // After the service brings the tunnel up, query its state with wg.exe.
    // Only the machine's Program Files roots, never PATH or environment roots.
    QString wgPath;
    {
        for (QString const &root : VpnManager::windowsProgramFilesRoots()) {
            QString const candidate = QDir(root).filePath(QStringLiteral("WireGuard/wg.exe"));
            if (QFileInfo::exists(candidate)) {
                wgPath = candidate;
                break;
            }
        }
    }
    if (wgPath.isEmpty())
        return false;

    // wg show takes the iface name = service name without the prefix.
    QString tunnelName = _winServiceName;
    tunnelName.remove(QStringLiteral("WireGuardTunnel$"));

    QProcess p;
    p.start(wgPath, {QStringLiteral("show"), tunnelName});
    if (!p.waitForFinished(3000))
        return false;
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    if (p.exitCode() != 0 || out.isEmpty())
        return false;

    // wg show doesn't surface the local IP nor DNS directly — those live in
    // the [Interface] section of the .conf. Parse the .conf for them
    // instead; the service must have applied them as-is.
    QFile cfg(_winConfigPath);
    if (!cfg.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString content = QString::fromUtf8(cfg.readAll());
    cfg.close();

    if (!parseInterfaceAddrAndDns(content, ip, dns))
        return false;

    *iface = tunnelName;
    return true;
}

bool WireGuardBackend::_startWindows(QString const &configPath)
{
    QFileInfo fi(configPath);
    if (!fi.exists()) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::Configuration,
                             tr("WireGuard config not found: %1").arg(configPath));
        return false;
    }
    _winServiceName = serviceNameFromConfig(configPath);
    _winConfigPath = fi.absoluteFilePath();
    _winPollAttempts = 0;

    emit logLine(tr("Starting WireGuard service: %1").arg(_winServiceName));

    QString startError;
    if (!WindowsServiceControl::startDemand(_winServiceName, &startError)) {
        _finishWindowsRun(VpnTerminationKind::StartFailure,
                          VpnFailureKind::ProcessExited, startError);
        // Teardown owns this run until SCM confirms it is really stopped.
        return true;
    }

    // A service is not a child process, so it would otherwise outlive a killed
    // ngPost indefinitely. This small independent watcher opens the parent now,
    // waits on that process handle (not a reused PID), and stops this exact
    // WireGuard service after the handle is signalled. Arguments travel through
    // the environment so profile-controlled service names never enter code.
    _winWatchdog = new QProcess(this);
    QProcessEnvironment watchdogEnv = QProcessEnvironment::systemEnvironment();
    watchdogEnv.insert(QStringLiteral("NGPOST_VPN_PARENT_PID"),
                       QString::number(QCoreApplication::applicationPid()));
    watchdogEnv.insert(QStringLiteral("NGPOST_VPN_PARENT_START"),
                       VpnManager::currentProcessStartTime());
    watchdogEnv.insert(QStringLiteral("NGPOST_VPN_SERVICE"), _winServiceName);
    _winWatchdog->setProcessEnvironment(watchdogEnv);
    _winWatchdog->setStandardOutputFile(QProcess::nullDevice());
    _winWatchdog->setStandardErrorFile(QProcess::nullDevice());
    connect(_winWatchdog,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        if (!_stopRequested && !_winServiceName.isEmpty()) {
            _finishWindowsRun(_wasReady ? VpnTerminationKind::UnexpectedExit
                                           : VpnTerminationKind::StartFailure,
                                 VpnFailureKind::HelperExited,
                                 tr("WireGuard parent watchdog exited unexpectedly"));
        }
    });
    QString const watcher = QStringLiteral(
        "$ErrorActionPreference='Stop';"
        "try {"
        "$p=Get-Process -Id ([int]$env:NGPOST_VPN_PARENT_PID);"
        "if ($p.StartTime.ToFileTimeUtc() -eq ([Int64]$env:NGPOST_VPN_PARENT_START)) "
        "{$p.WaitForExit()}"
        "} finally {"
        "$s=[System.ServiceProcess.ServiceController]::new($env:NGPOST_VPN_SERVICE);"
        "if ($s.Status -ne 'Stopped') {$s.Stop(); $s.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))}"
        "}");
    _winWatchdog->start(WindowsSecurity::systemPowerShell(),
                        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                         QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                         QStringLiteral("-Command"), watcher});
    if (!_winWatchdog->waitForStarted(3000)) {
        _winWatchdog->deleteLater();
        _winWatchdog = nullptr;
        _finishWindowsRun(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not start the WireGuard parent watchdog"));
        return true;
    }

    // Poll briefly for the tunnel to actually come up (wg show returns OK).
    if (!_winPollTimer) {
        _winPollTimer = new QTimer(this);
        _winPollTimer->setSingleShot(false);
        connect(_winPollTimer, &QTimer::timeout, this, &WireGuardBackend::onWinPollTimer);
    }
    _winPollTimer->start(500);
    return true;
}

void WireGuardBackend::onWinPollTimer()
{
    if (_winStopTimer && _winStopTimer->isActive()) return;
    if (++_winPollAttempts > 40) { // 40 * 500ms = 20s
        _winPollTimer->stop();
        _finishWindowsRun(VpnTerminationKind::StartFailure,
                             VpnFailureKind::TunnelLost,
                             tr("WireGuard tunnel did not come up within 20s"));
        return;
    }
    QString iface, ip, dns;
    if (!_queryTunnelInfo(&iface, &ip, &dns))
        return;
    _winPollTimer->stop();
    QHostAddress ipAddr(ip);
    QHostAddress dnsAddr = dns.isEmpty() ? QHostAddress() : QHostAddress(dns);
    if (ipAddr.isNull()) {
        _finishWindowsRun(VpnTerminationKind::StartFailure,
                             VpnFailureKind::Configuration,
                             tr("Could not parse local IP from WireGuard config"));
        return;
    }
    _winIface = iface;
    _markReady();
    emit ready(iface, ipAddr, dnsAddr);
}

void WireGuardBackend::_stopWindows()
{
    _finishWindowsRun(VpnTerminationKind::RequestedStop, VpnFailureKind::None, {});
}

void WireGuardBackend::_finishWindowsRun(VpnTerminationKind kind,
                                        VpnFailureKind failure, QString const &detail)
{
    if (_winPollTimer)
        _winPollTimer->stop();
    _winPendingTermination.kind = kind;
    _winPendingTermination.failure = failure;
    _winPendingTermination.detail = detail;
    if (!_winStopTimer) {
        _winStopTimer = new QTimer(this);
        connect(_winStopTimer, &QTimer::timeout, this, &WireGuardBackend::_pollWindowsStop);
    }
    _winStopError.clear();
    emit stopPending(detail);
    // Always asynchronous: start() must return before a terminal callback
    // can destroy its backend; retain the lease throughout cleanup.
    _winStopTimer->start(500);
}

void WireGuardBackend::_pollWindowsStop()
{
    if (!_winStopTimer || !_winStopTimer->isActive()) return;
    QString error;
    auto const result = _winServiceName.isEmpty() ? WindowsServiceControl::StopResult::Stopped
        : WindowsServiceControl::stopStep(
            [&] { return WindowsServiceControl::query(_winServiceName, &error); },
            [&] { return WindowsServiceControl::requestStop(_winServiceName, &error); });
    if (result != WindowsServiceControl::StopResult::Stopped) {
        if (result == WindowsServiceControl::StopResult::Failed && error != _winStopError) {
            _winStopError = error;
            QString const message = tr("WireGuard stop is NOT confirmed (%1). The tunnel may still be active; retaining ownership and retrying.").arg(error);
            emit logLine(message);
            emit statusLine(message);
        }
        return;
    }
    _winStopTimer->stop();
    if (_winWatchdog) {
        disconnect(_winWatchdog, nullptr, this, nullptr);
        _winWatchdog->kill();
        _winWatchdog->waitForFinished(2000);
        _winWatchdog->deleteLater();
        _winWatchdog = nullptr;
    }
    _winServiceName.clear();
    _winConfigPath.clear();
    _winIface.clear();
    _emitTerminationOnce(_winPendingTermination.kind, _winPendingTermination.failure,
                         _winPendingTermination.detail);
}
#endif // Q_OS_WIN
