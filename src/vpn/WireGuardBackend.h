//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef WIREGUARDBACKEND_H
#define WIREGUARDBACKEND_H

#include "VpnBackend.h"

#include <QProcess>

class WireGuardBackend : public VpnBackend
{
    Q_OBJECT
public:
    explicit WireGuardBackend(QObject *parent = nullptr);
    ~WireGuardBackend() override;

    bool start(QString const &configPath) override;
    void stop() override;
    void stopAndWait(int timeoutMs) override;
    bool isRunning() const override;

private slots:
    void onReadyReadStdout();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);

#ifdef Q_OS_WIN
    void onWinPollTimer();
#endif

private:
    void _handleLine(QString const &line);
#ifdef Q_OS_WIN
    //! Windows path: start/stop a WireGuard tunnel via SCM. The tunnel is
    //! assumed to already be registered as service `WireGuardTunnel$<name>`
    //! (done by the user at profile import time via wireguard.exe
    //! /installtunnelservice + sc sdset for runtime ACL).
    bool _startWindows(QString const &configPath);
    void _stopWindows();
    QString _serviceNameFromConfig(QString const &configPath) const;
    bool    _queryTunnelInfo(QString *iface, QString *ip, QString *dns) const;
#endif

    QProcess  *_proc;
    QByteArray _stdoutBuffer;
    bool       _readySignaled;

#ifdef Q_OS_WIN
    QString  _winServiceName;   //!< "WireGuardTunnel$<profileBase>"
    QString  _winIface;
    int      _winPollAttempts;
    class QTimer *_winPollTimer;
#endif
};

#endif // WIREGUARDBACKEND_H
