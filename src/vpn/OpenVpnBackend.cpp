//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "OpenVpnBackend.h"

#include "VpnManager.h"
#include "VpnProtocol.h"
#include "utils/PathHelper.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHostAddress>

#ifdef Q_OS_WIN
#  include <QDir>
#  include <QElapsedTimer>
#  include <QFile>
#  include <QLocalSocket>
#  include <QPointer>
#  include <QRegularExpression>
#  include <QTemporaryFile>
#  include <QTcpServer>
#  include <QTcpSocket>
#  include <QThread>
#  include <QTimer>
#  include <QUuid>
#  include "WindowsSecurity.h"
#endif

OpenVpnBackend::OpenVpnBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
    , _protocolV2Seen(false)
#ifdef Q_OS_WIN
    , _winServicePipe(nullptr)
    , _winMgmt(nullptr)
    , _winMgmtBuffer()
    , _winMgmtRetryTimer(nullptr)
    , _winMgmtRetryCount(0)
    , _winMgmtPort(0)
    , _winMgmtPassword()
    , _winMgmtPasswordPath()
    , _winMgmtAuthenticated(false)
    , _winStopRequested(false)
    , _winTunIface()
    , _winTunIp()
    , _winDnsIp()
#endif
{}

namespace
{
QString bundledVpnBinDir()
{
#ifdef Q_OS_LINUX
    QString const dir = QCoreApplication::applicationDirPath() + QStringLiteral("/vpn");
    if (QFileInfo::exists(dir + QStringLiteral("/openvpn")))
        return dir;
#endif
    return QString();
}

#if defined(Q_OS_WIN) || defined(NGPOST_TESTING)
bool hasWindowsActivity(bool servicePipeConnected,
                        bool hasManagementSocket,
                        bool retryTimerActive)
{
    return servicePipeConnected || hasManagementSocket || retryTimerActive;
}

char const kMgmtPasswordPrompt[]   = "ENTER PASSWORD:";
int const  kMgmtPasswordPromptSize = int(sizeof(kMgmtPasswordPrompt)) - 1;

//! openvpn writes its management password prompt without a trailing newline, so
//! a reader that only ever cuts on '\n' waits for a line openvpn will never
//! send: the connection sits on "authenticating" for ever. Give the prompt the
//! newline it lacks, in place, and it leaves by the same door as every other
//! management line -- one code path answers it, and whatever sits around it in
//! the buffer is left untouched. Consuming the prompt instead would take
//! everything before it along, and a line preceding a re-prompt is precisely
//! what says why the first password was refused.
void terminateMgmtPasswordPrompt(QByteArray &buffer)
{
    auto const idx = buffer.indexOf(kMgmtPasswordPrompt);
    if (idx < 0)
        return;
    auto const end = idx + kMgmtPasswordPromptSize;
    if (end < buffer.size() && (buffer.at(end) == '\n' || buffer.at(end) == '\r'))
        return; // already a line of its own
    buffer.insert(end, '\n');
}
#endif
}

OpenVpnBackend::~OpenVpnBackend()
{
    if (_proc) {
        if (_proc->state() != QProcess::NotRunning) {
            _proc->closeWriteChannel();
            if (!_proc->waitForFinished(3000))
                _proc->kill();
        }
        delete _proc;
        _proc = nullptr;
    }
#ifdef Q_OS_WIN
    _removeWinManagementPassword();
#endif
}

bool OpenVpnBackend::start(QString const &configPathPacked)
{
    _resetRunState();
#ifdef Q_OS_WIN
    // Drive OpenVPN via the official interactive service on Windows. Keep
    // this block scoped because MSVC parses both branches in one function.
    {
        QString cfg = configPathPacked;
        QString auth;
        int nul = configPathPacked.indexOf(QChar(QChar::Null));
        if (nul >= 0) {
            cfg  = configPathPacked.left(nul);
            auth = configPathPacked.mid(nul + 1);
        }
        QFileInfo fi(cfg);
        if (!fi.exists() || !fi.isReadable()) {
            _emitTerminationOnce(VpnTerminationKind::StartFailure,
                                 VpnFailureKind::Configuration,
                                 tr("Config file not found or unreadable: %1").arg(cfg));
            return false;
        }
        return _startWindowsViaInteractiveService(fi.absoluteFilePath(), auth);
    }
#elif !defined(Q_OS_LINUX)
    Q_UNUSED(configPathPacked);
    _emitTerminationOnce(VpnTerminationKind::StartFailure,
                         VpnFailureKind::Configuration,
                         tr("Native VPN integration is currently Linux/Windows-only. "
                            "macOS support is in progress."));
    return false;
#endif

    if (_proc && _proc->state() != QProcess::NotRunning)
        return true;

    // The caller (VpnManager::start) may pack an optional base64 credential
    // transport record after a NUL separator. It is sent through stdin and
    // never becomes an argv entry; the helper materialises its private /run
    // copy only after acquiring the lease.
    QString configPath, authPipePayload;
    int nulIdx = configPathPacked.indexOf(QChar(QChar::Null));
    if (nulIdx >= 0) {
        configPath   = configPathPacked.left(nulIdx);
        authPipePayload = configPathPacked.mid(nulIdx + 1);
    } else {
        configPath = configPathPacked;
    }

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
                             tr("Privileged helper script ngpost-vpn-helper.sh not found. "
                                "Install it under /var/lib/ngpost/ or run from the AppImage."));
        return false;
    }

    _readySignaled = false;
    _protocolV2Seen = false;
    _stdoutBuffer.clear();

    _proc = new QProcess(this);
    // Keep stdout/stderr separated: stdout is our protocol channel.
    _proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_proc, &QProcess::readyReadStandardOutput, this, &OpenVpnBackend::onReadyReadStdout);
    connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &OpenVpnBackend::onProcessFinished);
    connect(_proc, &QProcess::errorOccurred, this, &OpenVpnBackend::onProcessError);

    QStringList helperArgs;
    helperArgs << helper << "openvpn" << fi.absoluteFilePath()
               << "--protocol" << "2"
               << "--owner-pid" << QString::number(QCoreApplication::applicationPid())
               << "--owner-start" << VpnManager::currentProcessStartTime()
               << "--wait-minutes" << QString::number(VpnManager::instance()
                       ? VpnManager::instance()->effectiveLeaseWaitMinutes() : 0);
    // An installed helper uses the stable, root-owned tool copies installed
    // beside it. Only an in-bundle development helper needs an explicit path.
    QString const binDir = helper == QString::fromLatin1(VpnManager::kInstalledHelperPath)
        ? QString() : bundledVpnBinDir();
    if (!binDir.isEmpty())
        helperArgs << "--bin-dir" << binDir;
    if (!authPipePayload.isEmpty())
        helperArgs << "--auth-stdin";

    // Compose the final argv. The launcher is normally `pkexec`, but
    // `NGPOST_HELPER_LAUNCHER` can swap it for `sudo` etc. (see
    // VpnManager::helperLauncherProgram).
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
    if (!authPipePayload.isEmpty()) {
        QByteArray const command = QByteArrayLiteral("AUTH ")
            + authPipePayload.toLatin1() + '\n';
        if (_proc->write(command) != command.size()
            || (_proc->bytesToWrite() > 0 && !_proc->waitForBytesWritten(5000))) {
            _emitTerminationOnce(VpnTerminationKind::StartFailure,
                                 VpnFailureKind::Authentication,
                                 tr("Failed to send credentials to the VPN helper"));
            _proc->closeWriteChannel();
            return false;
        }
    }
    return true;
}

void OpenVpnBackend::stop()
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
    // The helper waits on its stdin. Closing it triggers a clean teardown:
    // policy routing removed, openvpn signalled, helper exits.
    // QProcess::finished will then drive us through onProcessFinished.
    _proc->closeWriteChannel();
}

bool OpenVpnBackend::restart(quint64 attemptId)
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

void OpenVpnBackend::setActive(bool active)
{
#ifndef Q_OS_WIN
    if (_proc && _proc->state() != QProcess::NotRunning)
        _proc->write(active ? "ACTIVE\n" : "IDLE\n");
#else
    Q_UNUSED(active);
#endif
}

void OpenVpnBackend::stopAndWait(int timeoutMs)
{
#ifdef Q_OS_WIN
    if (!isRunning()) {
        stop();
        return;
    }

    QElapsedTimer deadline;
    deadline.start();
    auto remaining = [&]() {
        return qMax(0, timeoutMs - static_cast<int>(deadline.elapsed()));
    };

    _winStopRequested = true;
    if (_winServicePipe)
        _winServicePipe->disconnectFromServer();

    // The interactive-service pipe normally disappears before the management
    // socket is created. Connect here immediately instead of relying on the
    // retry timer/event loop, which is commonly already stopping at shutdown.
    _ensureWinManagementSocket();
    QPointer<QTcpSocket> management = _winMgmt;
    while (management && management->state() != QAbstractSocket::ConnectedState
           && remaining() > 0) {
        management->abort();
        management->connectToHost(QHostAddress::LocalHost, _winMgmtPort);
        if (management->waitForConnected(qMin(500, remaining())))
            break;
        if (remaining() > 0)
            QThread::msleep(static_cast<unsigned long>(qMin(100, remaining())));
    }

    if (management && management->state() == QAbstractSocket::ConnectedState) {
        if (!_winMgmtAuthenticated)
            management->write(_winMgmtPassword.toUtf8() + '\n');
        management->write("signal SIGTERM\n");
        management->flush();
        if (management->bytesToWrite() > 0 && remaining() > 0)
            management->waitForBytesWritten(remaining());
        if (management && management->state() != QAbstractSocket::UnconnectedState
            && remaining() > 0)
            management->waitForDisconnected(remaining());
    }
    return;
#else
    stop();
#endif
    if (_proc && _proc->state() != QProcess::NotRunning) {
        // Allow the privileged helper's EXIT trap to run cleanup before we
        // tear down the QProcess. Without this wait, the helper would be
        // SIGKILL'd by Qt on QProcess destruction (kernel level, with our
        // uid we can't SIGKILL a root process anyway) — leaving openvpn,
        // tun device, ip rule and route table 4242 as orphans on shutdown.
        _proc->waitForFinished(timeoutMs);
    }
}

bool OpenVpnBackend::isRunning() const
{
#ifdef Q_OS_WIN
    return hasWindowsActivity(
        _winServicePipe && _winServicePipe->state() == QLocalSocket::ConnectedState,
        _winMgmt != nullptr,
        _winMgmtRetryTimer && _winMgmtRetryTimer->isActive());
#endif
    return _proc && _proc->state() != QProcess::NotRunning;
}

#ifdef NGPOST_TESTING
bool OpenVpnBackend::windowsActivityForTest(bool servicePipeConnected,
                                            bool hasManagementSocket,
                                            bool retryTimerActive)
{
    return hasWindowsActivity(servicePipeConnected, hasManagementSocket, retryTimerActive);
}

void OpenVpnBackend::terminateMgmtPasswordPromptForTest(QByteArray &buffer)
{
    terminateMgmtPasswordPrompt(buffer);
}
#endif

void OpenVpnBackend::onReadyReadStdout()
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

void OpenVpnBackend::_handleLine(QString const &line)
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
        VpnBackendHealth const health =
            message.fields.value(QStringLiteral("reason")) == QLatin1String("RECONNECTING")
                ? VpnBackendHealth::RecoveringInternally : VpnBackendHealth::Suspect;
        emit healthChanged(health, message.fields.value(QStringLiteral("reason")));
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
        if (detail.isEmpty())
            detail = line;
        _emitTerminationOnce(_wasReady ? VpnTerminationKind::UnexpectedExit
                                       : VpnTerminationKind::StartFailure,
                             failure, detail);
    }
}

void OpenVpnBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    VpnTerminationKind const kind = _stopRequested
        ? VpnTerminationKind::RequestedStop
        : (_wasReady ? VpnTerminationKind::UnexpectedExit
                     : VpnTerminationKind::StartFailure);
    VpnFailureKind const failure = _stopRequested ? VpnFailureKind::None
        : (_wasReady ? VpnFailureKind::HelperExited : VpnFailureKind::HelperUnavailable);
    QString detail = status == QProcess::CrashExit
        ? tr("VPN helper crashed")
        : tr("VPN helper exited with code %1").arg(exitCode);
    _emitTerminationOnce(kind, failure, detail);
}

void OpenVpnBackend::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("pkexec/helper failed to start (binary missing or denied)"));
    else if (err == QProcess::Crashed)
        _emitTerminationOnce(_wasReady ? VpnTerminationKind::UnexpectedExit
                                       : VpnTerminationKind::StartFailure,
                             _wasReady ? VpnFailureKind::HelperExited
                                       : VpnFailureKind::HelperUnavailable,
                             tr("helper process crashed"));
}

#ifdef Q_OS_WIN
// ===========================================================================
//  Windows backend — drive openvpn via OpenVPNServiceInteractive
// ===========================================================================
//
// On Windows ngPost does NOT run pkexec / a custom service. It talks directly
// to the named pipe \\.\pipe\openvpn\service exposed by OpenVPN Community's
// "OpenVPNServiceInteractive" (installed by openvpn's MSI, runs as SYSTEM).
// The service accepts a single startup message — a triple of UTF-16 wide
// strings, NUL-separated:
//
//      <working_directory>\0<openvpn_command_line_options>\0<stdin_data>\0
//
// The service spawns openvpn.exe elevated, parses the options, and returns
// an ack. Once openvpn is up we connect to its --management TCP port
// (a dynamic loopback port) and use the standard management protocol to track state
// changes and signal a clean SIGTERM at disconnect.

QString OpenVpnBackend::_buildOpenVpnOptions(QString const &configPath,
                                              QString const &authFilePath) const
{
    // We need quoting around paths that may contain spaces.
    auto q = [](QString const &s) -> QString {
        return QStringLiteral("\"") + s + QStringLiteral("\"");
    };

    // --management-hold makes openvpn wait for the management interface to
    // send "hold release" before it actually starts. This guarantees we
    // never miss a state transition (we always connect mgmt first).
    // --pull-filter ignore redirect-gateway prevents openvpn from taking
    // over the machine. The explicit high-metric default route gives sockets
    // pinned to the tunnel interface a usable route without winning over the
    // normal system default route for unrelated apps.
    QStringList parts;
    parts << "--config" << q(configPath)
          << "--management" << "127.0.0.1" << QString::number(_winMgmtPort)
          << q(_winMgmtPasswordPath)
          << "--management-hold"
          << "--management-query-passwords"
          << "--management-signal"
          << "--remap-usr1" << "SIGTERM"
          << "--pull-filter" << "ignore" << "redirect-gateway"
          << "--pull-filter" << "ignore" << "block-outside-dns"
          << "--route" << "0.0.0.0" << "0.0.0.0" << "vpn_gateway" << "9999"
          << "--verb" << "3";
    if (!authFilePath.isEmpty())
        parts << "--auth-user-pass" << q(authFilePath);
    return parts.join(' ');
}

bool OpenVpnBackend::_startWindowsViaInteractiveService(QString const &configPath,
                                                        QString const &authFilePath)
{
    if (_winServicePipe) {
        emit logLine(tr("OpenVPN backend already running"));
        return true;
    }

    _readySignaled = false;
    _winStopRequested = false;
    _winMgmtBuffer.clear();
    _winMgmtRetryCount = 0;
    _winMgmtAuthenticated = false;
    _winTunIface.clear();
    _winTunIp.clear();
    _winDnsIp.clear();

    QTcpServer portProbe;
    if (!portProbe.listen(QHostAddress::LocalHost, 0)) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not allocate a loopback OpenVPN management port"));
        return false;
    }
    _winMgmtPort = portProbe.serverPort();
    portProbe.close();

    _winMgmtPassword = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString const runtimeDir = PathHelper::vpnRuntimeDir();
    if (!WindowsSecurity::protectOwnerAndSystem(runtimeDir)) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not secure the OpenVPN runtime directory"));
        return false;
    }
    QTemporaryFile passwordFile(runtimeDir
                                + QStringLiteral("/management-XXXXXX"));
    passwordFile.setAutoRemove(false);
    if (!passwordFile.open()) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not create the OpenVPN management password file"));
        return false;
    }
    QByteArray const password = _winMgmtPassword.toUtf8() + '\n';
    if (passwordFile.write(password) != password.size() || !passwordFile.flush()) {
        passwordFile.close();
        passwordFile.remove();
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not write the OpenVPN management password file"));
        return false;
    }
    _winMgmtPasswordPath = passwordFile.fileName();
    passwordFile.close();
    if (!QFile::setPermissions(_winMgmtPasswordPath,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || !WindowsSecurity::protectOwnerAndSystem(_winMgmtPasswordPath)) {
        QFile::remove(_winMgmtPasswordPath);
        _winMgmtPasswordPath.clear();
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Could not secure the OpenVPN management password file"));
        return false;
    }

    // Open the OpenVPNServiceInteractive named pipe.
    _winServicePipe = new QLocalSocket(this);
    connect(_winServicePipe, &QLocalSocket::readyRead,
            this, &OpenVpnBackend::onWinPipeReadyRead);
    connect(_winServicePipe, &QLocalSocket::disconnected,
            this, &OpenVpnBackend::onWinPipeDisconnected);

    _winServicePipe->connectToServer(QStringLiteral("openvpn\\service"));
    if (!_winServicePipe->waitForConnected(3000)) {
        // Likely causes:
        //  - OpenVPN Community not installed (no service exists)
        //  - OpenVPNServiceInteractive installed but Start-Type=Manual and
        //    nobody started it (we don't auto-elevate to start a service)
        //  - the user does not have Connect permission to the pipe ACL
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Cannot reach OpenVPNServiceInteractive pipe. "
                                "Open Services.msc, set \"OpenVPN Interactive Service\" "
                                "to Automatic, and start it. If it is not present, "
                                "re-run the ngPost setup with the OpenVPN Community option."));
        _winServicePipe->deleteLater();
        _winServicePipe = nullptr;
        return false;
    }

    QFileInfo fi(configPath);
    QString workingDir = fi.absolutePath();
    QString options    = _buildOpenVpnOptions(configPath, authFilePath);
    QString stdInput;  // empty: auth comes from auth-user-pass file, not stdin

    // Marshal the three UTF-16 LE strings, each NUL-terminated.
    auto encode = [](QString const &s) {
        QByteArray b((const char *)s.utf16(), (s.size() + 1) * 2);
        return b;
    };
    QByteArray payload;
    payload.append(encode(workingDir));
    payload.append(encode(options));
    payload.append(encode(stdInput));

    emit logLine(tr("OpenVPN service: starting tunnel via %1 (management on loopback port %2)")
                     .arg(QFileInfo(configPath).fileName()).arg(_winMgmtPort));

    _winServicePipe->write(payload);
    if (!_winServicePipe->waitForBytesWritten(3000)) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::HelperUnavailable,
                             tr("Failed to send startup data to OpenVPN service pipe"));
        _winServicePipe->deleteLater();
        _winServicePipe = nullptr;
        return false;
    }

    // The service replies with an ack on the pipe (handled in
    // onWinPipeReadyRead). Meanwhile we set up the management socket
    // reconnect-with-backoff loop: openvpn won't bind its management port until
    // it's been fully spawned, which can take a moment.
    if (!_winMgmtRetryTimer) {
        _winMgmtRetryTimer = new QTimer(this);
        _winMgmtRetryTimer->setSingleShot(true);
        connect(_winMgmtRetryTimer, &QTimer::timeout,
                this, [this]() {
            _ensureWinManagementSocket();
            if (_winMgmt->state() != QAbstractSocket::ConnectedState
                && _winMgmt->state() != QAbstractSocket::ConnectingState) {
                _winMgmt->abort();
                _winMgmt->connectToHost(QHostAddress::LocalHost, _winMgmtPort);
            }
            // Re-arm the retry timer; we cancel it once management is up.
            if (++_winMgmtRetryCount < 40) // ~20s
                _winMgmtRetryTimer->start(500);
            else if (!_readySignaled)
                _emitTerminationOnce(VpnTerminationKind::StartFailure,
                                     VpnFailureKind::HelperUnavailable,
                                     tr("Could not connect to OpenVPN management on loopback port %1")
                                         .arg(_winMgmtPort));
        });
    }
    _winMgmtRetryCount = 0;
    _winMgmtRetryTimer->start(500);
    return true;
}

void OpenVpnBackend::_ensureWinManagementSocket()
{
    if (_winMgmt)
        return;
    _winMgmt = new QTcpSocket(this);
    connect(_winMgmt, &QTcpSocket::connected,
            this, &OpenVpnBackend::onWinMgmtConnected);
    connect(_winMgmt, &QTcpSocket::readyRead,
            this, &OpenVpnBackend::onWinMgmtReadyRead);
    connect(_winMgmt, &QTcpSocket::disconnected,
            this, &OpenVpnBackend::onWinMgmtDisconnected);
}

void OpenVpnBackend::onWinPipeReadyRead()
{
    if (!_winServicePipe) return;
    QByteArray data = _winServicePipe->readAll();
    // OpenVPNServiceInteractive returns a UTF-16 LE text response shaped like:
    //   "0x<errno>\n0x<pid_or_lasterror>\n<message>\0"
    // On success errno == 0 and the second line carries openvpn.exe's PID.
    if (data.size() < 2) return;
    QString resp = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(data.constData()),
        data.size() / 2);
    QStringList lines = resp.split(QChar('\n'));
    bool ok = false;
    quint32 errCode = lines.value(0).trimmed().toUInt(&ok, 16);
    if (!ok) {
        emit logLine(tr("OpenVPN service: unparsable response (%1 bytes): %2")
                         .arg(data.size()).arg(resp.left(200)));
        return;
    }
    if (errCode != 0) {
        QString detail = lines.mid(1).join(' ').trimmed();
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             VpnFailureKind::ProcessExited,
                             tr("OpenVPN service refused start (code 0x%1): %2")
                                 .arg(errCode, 8, 16, QChar('0')).arg(detail));
        _stopWindows();
        return;
    }
    QString pidLine = lines.value(1).trimmed();
    emit logLine(tr("OpenVPN service spawned openvpn.exe (%1)").arg(pidLine));
}

void OpenVpnBackend::onWinPipeDisconnected()
{
    // The service drops the pipe once openvpn is spawned. That's expected;
    // we keep going through the management socket.
    if (_winServicePipe) {
        _winServicePipe->deleteLater();
        _winServicePipe = nullptr;
    }
}

void OpenVpnBackend::onWinMgmtConnected()
{
    // We're talking to openvpn. Cancel the retry loop and subscribe to
    // state changes, then release the management-hold so the tunnel can
    // actually negotiate.
    if (_winMgmtRetryTimer)
        _winMgmtRetryTimer->stop();
    emit logLine(tr("Connected to openvpn management socket; authenticating"));
}

void OpenVpnBackend::onWinMgmtReadyRead()
{
    if (!_winMgmt) return;
    _winMgmtBuffer.append(_winMgmt->readAll());

    // Only while the prompt is still expected: afterwards the same bytes can
    // legitimately appear inside a >LOG: line, and splitting one in two would
    // be a new bug of the same family.
    if (!_winMgmtAuthenticated)
        terminateMgmtPasswordPrompt(_winMgmtBuffer);

    int nl;
    while ((nl = _winMgmtBuffer.indexOf('\n')) >= 0) {
        QByteArray line = _winMgmtBuffer.left(nl).trimmed();
        _winMgmtBuffer.remove(0, nl + 1);
        if (!line.isEmpty())
            _parseMgmtLine(QString::fromUtf8(line));
    }
}

void OpenVpnBackend::_parseMgmtLine(QString const &line)
{
    if (line.startsWith(QLatin1String(kMgmtPasswordPrompt))) {
        if (_winMgmt) {
            _winMgmt->write(_winMgmtPassword.toUtf8() + '\n');
            // openvpn answers nothing until it has the whole line, so a write
            // left sitting in the socket buffer is the stall all over again.
            _winMgmt->flush();
        }
        return;
    }
    if (line.startsWith(QLatin1String("SUCCESS: password is correct"))) {
        _winMgmtAuthenticated = true;
        if (!_winMgmt)
            return;
        if (_winStopRequested) {
            _winMgmt->write("signal SIGTERM\n");
            _winMgmt->flush();
            return;
        }
        // Subscribe after authentication. The management connection is also
        // the parent-liveness channel on Windows (--management-signal plus
        // --remap-usr1 SIGTERM).
        _winMgmt->write("state on all\n");
        _winMgmt->write("log on all\n");
        _winMgmt->write("hold release\n");
        return;
    }
    // openvpn management protocol — state notifications are prefixed with '>'
    // and follow:
    //   >STATE:timestamp,STATE,description,localIp,serverIp,serverPort,localPort,tunIface
    // We only care about CONNECTED for the ready signal.
    if (line.startsWith(QLatin1String(">STATE:"))) {
        QStringList f = line.mid(7).split(',');
        if (f.size() >= 4 && f.at(1) == QLatin1String("CONNECTED")) {
            QHostAddress ip(f.at(3));
            QString iface = (f.size() >= 8) ? f.at(7) : QStringLiteral("openvpn");
            if (!ip.isNull() && !_readySignaled) {
                _readySignaled = true;
                _markReady();
                _winTunIp     = ip;
                _winTunIface  = iface;
                emit ready(iface, ip, _winDnsIp);
            }
        }
        return;
    }
    if (line.startsWith(QLatin1String(">HOLD:"))) {
        // openvpn is in management-hold — release it.
        if (_winMgmt) _winMgmt->write("hold release\n");
        return;
    }
    if (line.startsWith(QLatin1String(">LOG:"))) {
        // >LOG:timestamp,flags,message
        int comma = line.indexOf(',', line.indexOf(',', 5) + 1);
        QString msg = (comma > 0) ? line.mid(comma + 1) : line;
        // Try to capture a pushed DNS option from the log stream.
        if (msg.contains(QLatin1String("PUSH_REPLY"))) {
            QRegularExpression re(
                QStringLiteral("dhcp-option DNS ([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)"));
            QRegularExpressionMatch m = re.match(msg);
            if (m.hasMatch())
                _winDnsIp = QHostAddress(m.captured(1));
        }
        emit logLine(msg);
        return;
    }
    if (line.startsWith(QLatin1String(">FATAL:"))) {
        _emitTerminationOnce(_wasReady ? VpnTerminationKind::UnexpectedExit
                                       : VpnTerminationKind::StartFailure,
                             VpnFailureKind::ProcessExited, line.mid(7));
        _stopWindows();
        return;
    }
    if (line.startsWith(QLatin1String(">"))) {
        // other async event (PASSWORD, NEED-OK...) — ignore for now
        emit logLine(line);
        return;
    }
    // direct command response — surface unconditionally
    emit logLine(line);
}

void OpenVpnBackend::onWinMgmtDisconnected()
{
    // openvpn exited. Tear everything down.
    if (_winMgmt) {
        _winMgmt->deleteLater();
        _winMgmt = nullptr;
    }
    if (_winMgmtRetryTimer)
        _winMgmtRetryTimer->stop();
    if (_winServicePipe) {
        _winServicePipe->deleteLater();
        _winServicePipe = nullptr;
    }
    VpnTerminationKind const kind = _winStopRequested
        ? VpnTerminationKind::RequestedStop
        : (_wasReady ? VpnTerminationKind::UnexpectedExit
                     : VpnTerminationKind::StartFailure);
    VpnFailureKind const failure = _winStopRequested ? VpnFailureKind::None
        : (_wasReady ? VpnFailureKind::ProcessExited : VpnFailureKind::HelperUnavailable);
    _readySignaled = false;
    _winStopRequested = false;
    _winMgmtAuthenticated = false;
    _removeWinManagementPassword();
    _emitTerminationOnce(kind, failure, kind == VpnTerminationKind::RequestedStop
        ? QString() : tr("OpenVPN process exited"));
}

void OpenVpnBackend::_stopWindows()
{
    _winStopRequested = true;
    if (_winMgmt && _winMgmt->state() == QAbstractSocket::ConnectedState) {
        // openvpn handles signal SIGTERM cleanly and tears down the tunnel.
        if (!_winMgmtAuthenticated)
            _winMgmt->write(_winMgmtPassword.toUtf8() + '\n');
        _winMgmt->write("signal SIGTERM\n");
        _winMgmt->flush();
    } else if (_winServicePipe) {
        // Pipe still open but no management socket — nothing more we can do
        // from here. The pipe will close when the service notices openvpn
        // died, which onWinMgmtDisconnected handles.
        _winServicePipe->disconnectFromServer();
    } else if (!_winMgmtRetryTimer || !_winMgmtRetryTimer->isActive()) {
        // Nothing running.
        _emitTerminationOnce(VpnTerminationKind::RequestedStop,
                             VpnFailureKind::None, QString());
    }
}

void OpenVpnBackend::_removeWinManagementPassword()
{
    if (!_winMgmtPasswordPath.isEmpty()) {
        QFile passwordFile(_winMgmtPasswordPath);
        if (passwordFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            passwordFile.write(QByteArray(_winMgmtPassword.toUtf8().size(), '\0'));
            passwordFile.close();
        }
        passwordFile.remove();
    }
    _winMgmtPassword.clear();
    _winMgmtPasswordPath.clear();
}
#endif // Q_OS_WIN
