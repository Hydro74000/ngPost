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

private:
    void _handleLine(QString const &line);

    QProcess  *_proc;
    QByteArray _stdoutBuffer;
    bool       _readySignaled;
};

#endif // WIREGUARDBACKEND_H
