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

private:
    void _handleLine(QString const &line);

    QProcess  *_proc;            //!< Linux: the pkexec/helper subprocess.
    QByteArray _stdoutBuffer;
    bool       _readySignaled;
};

#endif // OPENVPNBACKEND_H
