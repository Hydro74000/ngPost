//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNBACKEND_H
#define VPNBACKEND_H

#include <QHostAddress>
#include <QObject>
#include <QString>

class VpnBackend : public QObject
{
    Q_OBJECT
public:
    explicit VpnBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~VpnBackend() override = default;

    virtual bool start(QString const &configPath) = 0;
    virtual void stop()                           = 0;
    virtual bool isRunning() const                = 0;

    //! Synchronous shutdown: signal stop then wait for the underlying
    //! process to actually exit, so the helper script has time to run its
    //! cleanup trap (tear down policy routing, kill openvpn). Used on app
    //! close so we don't leave the tunnel up.
    virtual void stopAndWait(int timeoutMs) {
        stop();
        Q_UNUSED(timeoutMs);
    }

signals:
    //! Tunnel is up. `dnsServer` may be null if the backend couldn't capture
    //! the DNS pushed by the VPN; in that case ngPost falls back to system DNS
    //! and the user is warned of the leak risk.
    void ready(QString const &tunInterface, QHostAddress const &tunIp,
               QHostAddress const &dnsServer = QHostAddress());
    void failed(QString const &reason);
    void stopped();
    void logLine(QString const &line);
};

#endif // VPNBACKEND_H
