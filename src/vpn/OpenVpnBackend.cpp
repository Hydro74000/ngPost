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

OpenVpnBackend::OpenVpnBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
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

bool OpenVpnBackend::start(QString const &configPath)
{
    if (_proc && _proc->state() != QProcess::NotRunning)
        return true;

    QFileInfo fi(configPath);
    if (!fi.exists() || !fi.isReadable()) {
        emit failed(tr("Config file not found or unreadable: %1").arg(configPath));
        return false;
    }

    QString helper = VpnManager::helperScriptPath();
    if (helper.isEmpty()) {
        emit failed(tr("Privileged helper script ngpost-vpn-helper.sh not found. "
                       "Install it under /usr/lib/ngpost/ or run from the AppImage."));
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

    emit logLine(tr("Launching VPN helper: pkexec %1").arg(args.join(' ')));
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
        // READY <iface> <ip>
        QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            QHostAddress ip(parts.at(2));
            if (!ip.isNull()) {
                _readySignaled = true;
                emit ready(parts.at(1), ip);
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
