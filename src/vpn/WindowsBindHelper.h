//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef WINDOWSBINDHELPER_H
#define WINDOWSBINDHELPER_H

#include <QAbstractSocket>
#include <QHostAddress>
#include <QString>

#ifdef Q_OS_WIN

//! Windows-only Winsock helpers for VPN source binding.
//!
//! Qt can see a just-created OpenVPN/WireGuard address before Winsock accepts
//! bind() on it. We use a raw socket both to probe readiness and to hand an
//! already-bound descriptor to QAbstractSocket.
class WindowsBindHelper
{
public:
    //! Try a temporary AF_INET bind to localIp:0, then close it immediately.
    //! This is the Windows readiness check used before ngPost starts posting.
    static bool canBindLocalAddress(QHostAddress const &localIp,
                                    QString *errMsg = nullptr);

    //! Create AF_INET socket, bind to localIp:0, attach via setSocketDescriptor.
    //! On failure, *errMsg gets a human-readable description and false is returned.
    static bool bindAndAttach(QAbstractSocket *socket,
                              QHostAddress const &localIp,
                              QString *errMsg = nullptr);
};

#endif // Q_OS_WIN

#endif // WINDOWSBINDHELPER_H
