//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "WireGuardBackend.h"

#include "VpnManager.h"

#include <QFileInfo>
#include <QHostAddress>

WireGuardBackend::WireGuardBackend(QObject *parent)
    : VpnBackend(parent)
    , _proc(nullptr)
    , _stdoutBuffer()
    , _readySignaled(false)
{}

WireGuardBackend::~WireGuardBackend()
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

bool WireGuardBackend::start(QString const &configPath)
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
        emit failed(tr("Privileged helper script ngpost-vpn-helper.sh not found"));
        return false;
    }

    _readySignaled = false;
    _stdoutBuffer.clear();

    _proc = new QProcess(this);
    _proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_proc, &QProcess::readyReadStandardOutput,
            this, &WireGuardBackend::onReadyReadStdout);
    connect(_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WireGuardBackend::onProcessFinished);
    connect(_proc, &QProcess::errorOccurred,
            this, &WireGuardBackend::onProcessError);

    QStringList args;
    args << helper << "wireguard" << fi.absoluteFilePath();

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

void WireGuardBackend::stop()
{
    if (!_proc) {
        emit stopped();
        return;
    }
    if (_proc->state() == QProcess::NotRunning) {
        emit stopped();
        return;
    }
    _proc->closeWriteChannel();
}

void WireGuardBackend::stopAndWait(int timeoutMs)
{
    stop();
    if (_proc && _proc->state() != QProcess::NotRunning)
        _proc->waitForFinished(timeoutMs);
}

bool WireGuardBackend::isRunning() const
{
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
    if (line.startsWith(QLatin1String("READY "))) {
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
        emit failed(line.mid(6));
    }
    else if (line.startsWith(QLatin1String("LOG "))) {
        emit logLine(line.mid(4));
    }
    else {
        emit logLine(line);
    }
}

void WireGuardBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if (!_readySignaled)
        emit failed(tr("helper exited (code %1) before tunnel was ready").arg(exitCode));
    emit stopped();
}

void WireGuardBackend::onProcessError(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        emit failed(tr("pkexec/helper failed to start"));
    else if (err == QProcess::Crashed)
        emit failed(tr("helper process crashed"));
}
