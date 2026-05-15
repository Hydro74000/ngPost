//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "OpenVpnBackend.h"

#include "VpnManager.h"

#include <QFileInfo>
#include <QHostAddress>

#ifdef Q_OS_WIN
#  include <QLocalSocket>
#  include <QRegularExpression>
#  include <QTcpSocket>
#  include <QTimer>
#endif

OpenVpnBackend::OpenVpnBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
#ifdef Q_OS_WIN
    , _winServicePipe(nullptr)
    , _winMgmt(nullptr)
    , _winMgmtBuffer()
    , _winMgmtRetryTimer(nullptr)
    , _winMgmtRetryCount(0)
    , _winTunIface()
    , _winTunIp()
    , _winDnsIp()
#endif
{}

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
}

bool OpenVpnBackend::start(QString const &configPathPacked)
{
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
            emit failed(tr("Config file not found or unreadable: %1").arg(cfg));
            return false;
        }
        return _startWindowsViaInteractiveService(fi.absoluteFilePath(), auth);
    }
#elif !defined(Q_OS_LINUX)
    Q_UNUSED(configPathPacked);
    emit failed(tr("Native VPN integration is currently Linux/Windows-only. "
                   "macOS support is in progress."));
    return false;
#endif

    if (_proc && _proc->state() != QProcess::NotRunning)
        return true;

    // The caller (VpnManager::start) may pack an optional --auth-user-pass
    // file path after a NUL separator: "<configPath>\0<authFilePath>".
    // This avoids changing the VpnBackend interface signature.
    QString configPath, authFilePath;
    int nulIdx = configPathPacked.indexOf(QChar(QChar::Null));
    if (nulIdx >= 0) {
        configPath   = configPathPacked.left(nulIdx);
        authFilePath = configPathPacked.mid(nulIdx + 1);
    } else {
        configPath = configPathPacked;
    }

    QFileInfo fi(configPath);
    if (!fi.exists() || !fi.isReadable()) {
        emit failed(tr("Config file not found or unreadable: %1").arg(configPath));
        return false;
    }

    QString helper = VpnManager::helperScriptPath();
    if (helper.isEmpty()) {
        emit failed(tr("Privileged helper script ngpost-vpn-helper.sh not found. "
                       "Install it under /var/lib/ngpost/ or run from the AppImage."));
        return false;
    }

    _readySignaled = false;
    _stdoutBuffer.clear();

    _proc = new QProcess(this);
    // Keep stdout/stderr separated: stdout is our protocol channel.
    _proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_proc, &QProcess::readyReadStandardOutput, this, &OpenVpnBackend::onReadyReadStdout);
    connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &OpenVpnBackend::onProcessFinished);
    connect(_proc, &QProcess::errorOccurred, this, &OpenVpnBackend::onProcessError);

    QStringList args;
    args << helper << "openvpn" << fi.absoluteFilePath();
    if (!authFilePath.isEmpty())
        args << "--auth-file" << authFilePath;

    // Avoid logging the auth-file path verbatim. The path goes to pkexec
    // unchanged, but logs are user-visible.
    QStringList safeArgs = args;
    if (!authFilePath.isEmpty())
        safeArgs.replace(safeArgs.size() - 1, QStringLiteral("<authfile>"));
    emit logLine(tr("Launching VPN helper: pkexec %1").arg(safeArgs.join(' ')));

    _proc->start(QStringLiteral("pkexec"), args);
    if (!_proc->waitForStarted(5000)) {
        emit failed(tr("Failed to start pkexec/helper"));
        delete _proc;
        _proc = nullptr;
        return false;
    }
    return true;
}

void OpenVpnBackend::stop()
{
#ifdef Q_OS_WIN
    _stopWindows();
    return;
#endif
    if (!_proc) {
        emit stopped();
        return;
    }
    if (_proc->state() == QProcess::NotRunning) {
        emit stopped();
        return;
    }
    // The helper waits on its stdin. Closing it triggers a clean teardown:
    // policy routing removed, openvpn signalled, helper exits.
    // QProcess::finished will then drive us through onProcessFinished.
    _proc->closeWriteChannel();
}

void OpenVpnBackend::stopAndWait(int timeoutMs)
{
    stop();
#ifdef Q_OS_WIN
    Q_UNUSED(timeoutMs);
    return;
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
    return _winServicePipe != nullptr;
#endif
    return _proc && _proc->state() != QProcess::NotRunning;
}

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
    if (line.startsWith(QLatin1String("READY "))) {
        // READY <iface> <ip> [<dns>]
        QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            QHostAddress ip(parts.at(2));
            if (!ip.isNull()) {
                QHostAddress dns;
                if (parts.size() >= 4 && parts.at(3) != QLatin1String("-"))
                    dns = QHostAddress(parts.at(3));
                _readySignaled = true;
                emit ready(parts.at(1), ip, dns);
                return;
            }
        }
        emit logLine(tr("Malformed READY from helper: %1").arg(line));
    }
    else if (line.startsWith(QLatin1String("ERROR "))) {
        QString reason = line.mid(6);
        emit failed(reason);
    }
    else if (line.startsWith(QLatin1String("LOG "))) {
        emit logLine(line.mid(4));
    }
    else {
        emit logLine(line);
    }
}

void OpenVpnBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (!_readySignaled)
        emit failed(tr("helper exited (code %1) before tunnel was ready").arg(exitCode));
    emit stopped();
}

void OpenVpnBackend::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        emit failed(tr("pkexec/helper failed to start (binary missing or denied)"));
    else if (err == QProcess::Crashed)
        emit failed(tr("helper process crashed"));
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
// (127.0.0.1:7505) and use the standard management protocol to track state
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
          << "--management" << "127.0.0.1" << "7505"
          << "--management-hold"
          << "--management-query-passwords"
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
    _winMgmtBuffer.clear();
    _winMgmtRetryCount = 0;
    _winTunIface.clear();
    _winTunIp.clear();
    _winDnsIp.clear();

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
        emit failed(tr("Cannot reach OpenVPNServiceInteractive pipe. "
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

    emit logLine(tr("OpenVPN service: starting tunnel via %1 (mgmt on 127.0.0.1:7505)")
                     .arg(QFileInfo(configPath).fileName()));

    _winServicePipe->write(payload);
    if (!_winServicePipe->waitForBytesWritten(3000)) {
        emit failed(tr("Failed to send startup data to OpenVPN service pipe"));
        _winServicePipe->deleteLater();
        _winServicePipe = nullptr;
        return false;
    }

    // The service replies with an ack on the pipe (handled in
    // onWinPipeReadyRead). Meanwhile we set up the management socket
    // reconnect-with-backoff loop: openvpn won't bind 127.0.0.1:7505 until
    // it's been fully spawned, which can take a moment.
    if (!_winMgmtRetryTimer) {
        _winMgmtRetryTimer = new QTimer(this);
        _winMgmtRetryTimer->setSingleShot(true);
        connect(_winMgmtRetryTimer, &QTimer::timeout,
                this, [this]() {
            if (!_winMgmt) {
                _winMgmt = new QTcpSocket(this);
                connect(_winMgmt, &QTcpSocket::connected,
                        this, &OpenVpnBackend::onWinMgmtConnected);
                connect(_winMgmt, &QTcpSocket::readyRead,
                        this, &OpenVpnBackend::onWinMgmtReadyRead);
                connect(_winMgmt, &QTcpSocket::disconnected,
                        this, &OpenVpnBackend::onWinMgmtDisconnected);
            }
            if (_winMgmt->state() != QAbstractSocket::ConnectedState
                && _winMgmt->state() != QAbstractSocket::ConnectingState) {
                _winMgmt->abort();
                _winMgmt->connectToHost(QHostAddress::LocalHost, 7505);
            }
            // Re-arm the retry timer; we cancel it once management is up.
            if (++_winMgmtRetryCount < 40) // ~20s
                _winMgmtRetryTimer->start(500);
            else if (!_readySignaled)
                emit failed(tr("Could not connect to openvpn management on 127.0.0.1:7505"));
        });
    }
    _winMgmtRetryCount = 0;
    _winMgmtRetryTimer->start(500);
    return true;
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
        emit failed(tr("OpenVPN service refused start (code 0x%1): %2")
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
    emit logLine(tr("Connected to openvpn management socket"));
    // Subscribe to state notifications AND to log events. Without `log on all`,
    // openvpn won't push PUSH_REPLY lines to us, and we'd never see the
    // dhcp-option DNS X.X.X.X that we need for the DNS-leak fix on Windows.
    _winMgmt->write("state on\n");
    _winMgmt->write("log on all\n");
    _winMgmt->write("hold release\n");
}

void OpenVpnBackend::onWinMgmtReadyRead()
{
    if (!_winMgmt) return;
    _winMgmtBuffer.append(_winMgmt->readAll());
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
        emit failed(line.mid(7));
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
    if (!_readySignaled) {
        emit failed(tr("openvpn exited before the tunnel was ready"));
    }
    _readySignaled = false;
    emit stopped();
}

void OpenVpnBackend::_stopWindows()
{
    if (_winMgmt && _winMgmt->state() == QAbstractSocket::ConnectedState) {
        // openvpn handles signal SIGTERM cleanly and tears down the tunnel.
        _winMgmt->write("signal SIGTERM\n");
        _winMgmt->flush();
    } else if (_winServicePipe) {
        // Pipe still open but no management socket — nothing more we can do
        // from here. The pipe will close when the service notices openvpn
        // died, which onWinMgmtDisconnected handles.
        _winServicePipe->disconnectFromServer();
    } else {
        // Nothing running.
        emit stopped();
    }
}
#endif // Q_OS_WIN
