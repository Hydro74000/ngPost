//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef OPENVPNBACKEND_H
#define OPENVPNBACKEND_H

#include "VpnBackend.h"

#include <QProcess>

class QLocalSocket;
class QTcpSocket;
class QTimer;

class OpenVpnBackend : public VpnBackend
{
    Q_OBJECT
public:
    explicit OpenVpnBackend(QObject *parent = nullptr);
    ~OpenVpnBackend() override;

    bool start(QString const &configPath) override;
    void stop() override;
    void stopAndWait(int timeoutMs) override;
    bool isRunning() const override;

private slots:
    // Linux helper-script protocol
    void onReadyReadStdout();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);

#ifdef Q_OS_WIN
    // OpenVPNServiceInteractive named pipe protocol
    void onWinPipeReadyRead();
    void onWinPipeDisconnected();
    // openvpn --management TCP socket on 127.0.0.1:7505
    void onWinMgmtConnected();
    void onWinMgmtReadyRead();
    void onWinMgmtDisconnected();
#endif

private:
    void _handleLine(QString const &line);
#ifdef Q_OS_WIN
    bool _startWindowsViaInteractiveService(QString const &configPath,
                                            QString const &authFilePath);
    void _stopWindows();
    void _parseMgmtLine(QString const &line);
    QString _buildOpenVpnOptions(QString const &configPath,
                                  QString const &authFilePath) const;
#endif

    QProcess  *_proc;            //!< Linux: the pkexec/helper subprocess.
    QByteArray _stdoutBuffer;
    bool       _readySignaled;

#ifdef Q_OS_WIN
    QLocalSocket *_winServicePipe; //!< \\.\pipe\openvpn\service
    QTcpSocket   *_winMgmt;        //!< openvpn --management 127.0.0.1:7505
    QByteArray    _winMgmtBuffer;
    QTimer       *_winMgmtRetryTimer;
    int           _winMgmtRetryCount;
    QString       _winTunIface;
    QHostAddress  _winTunIp;
    QHostAddress  _winDnsIp;
#endif
};

#endif // OPENVPNBACKEND_H
