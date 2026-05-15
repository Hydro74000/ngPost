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
    static QList<QHostAddress> resolveA(QString const &host,
                                        QHostAddress const &dnsServer,
                                        QHostAddress const &sourceIp,
                                        QString *errMsg = nullptr,
                                        int timeoutMs = 5000);
};

#endif // VPNDNSRESOLVER_H
