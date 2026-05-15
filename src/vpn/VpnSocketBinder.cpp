//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnSocketBinder.h"

#ifdef Q_OS_WIN
#include "WindowsBindHelper.h"
#endif

#include <QNetworkInterface>
#include <QStringList>

bool VpnSocketBinder::bind(QAbstractSocket *socket,
                           QHostAddress const &localIp,
                           QString *errMsg)
{
    if (errMsg)
        errMsg->clear();
    if (!socket) {
        if (errMsg)
            *errMsg = QStringLiteral("null socket");
        return false;
    }

#ifdef Q_OS_WIN
    return WindowsBindHelper::bindAndAttach(socket, localIp, errMsg);
#else
    QAbstractSocket::BindMode bindFlags =
        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint;
    bool ok = socket->bind(localIp, 0, bindFlags);
    if (!ok && errMsg)
        *errMsg = socket->errorString();
    return ok;
#endif
}

QString VpnSocketBinder::localAddressSummary()
{
    QStringList visibleAddrs;
    for (QHostAddress const &a : QNetworkInterface::allAddresses())
        visibleAddrs << a.toString();
    return visibleAddrs.join(QStringLiteral(", "));
}
