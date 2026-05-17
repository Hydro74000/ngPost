//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNSOCKETBINDER_H
#define VPNSOCKETBINDER_H

#include <QAbstractSocket>
#include <QHostAddress>
#include <QString>

class VpnSocketBinder
{
public:
    static bool bind(QAbstractSocket *socket,
                     QHostAddress const &localIp,
                     QString *errMsg = nullptr);

    static QString localAddressSummary();
};

#endif // VPNSOCKETBINDER_H
