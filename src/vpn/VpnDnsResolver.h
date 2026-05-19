//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNDNSRESOLVER_H
#define VPNDNSRESOLVER_H

#include <QHostAddress>
#include <QList>
#include <QString>

class VpnDnsResolver
{
public:
    //! Resolve A records for `host` via DNS-over-TCP. The socket source IP is
    //! forced to `sourceIp` (so the query exits the right interface, eg. the
    //! VPN tunnel). `port` defaults to 53 — pass a different value only in
    //! tests that cannot bind privileged ports.
    static QList<QHostAddress> resolveA(QString const &host,
                                        QHostAddress const &dnsServer,
                                        QHostAddress const &sourceIp,
                                        QString *errMsg = nullptr,
                                        int timeoutMs = 5000,
                                        quint16 port = 53);
};

#endif // VPNDNSRESOLVER_H
