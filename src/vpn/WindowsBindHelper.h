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

//! Phase 5d follow-up: Qt's QAbstractSocket::bind() on Windows fails with
//! WSAEADDRNOTAVAIL when targeting a virtual adapter IP (OpenVPN DCO /
//! Wintun / TAP-Windows6), even though a raw WSASocket+bind to the same
//! address succeeds. Root cause confirmed via live probe on a Windows VM:
//! Qt's socket creation sets SO_EXCLUSIVEADDRUSE; the BindMode flag then
//! tries to set SO_REUSEADDR which fails with WSAEINVAL on the now-
//! exclusive socket. Qt swallows the setsockopt error and proceeds to
//! bind(), but the underlying TCP/IP stack rejects the address as
//! "not available" on the virtual adapter. There is no Qt public API to
//! disable SO_EXCLUSIVEADDRUSE at creation time.
//!
//! Workaround: do the socket creation + bind ourselves with the minimal
//! options Qt would normally use, then hand the bound descriptor over via
//! QAbstractSocket::setSocketDescriptor(BoundState). After that,
//! connectToHost() (or connectToHostEncrypted() for QSslSocket) drives
//! the rest of the lifecycle normally.
class WindowsBindHelper
{
public:
    //! Create AF_INET socket, bind to localIp:0, attach via setSocketDescriptor.
    //! On failure, *errMsg gets a human-readable description and false is returned.
    static bool bindAndAttach(QAbstractSocket *socket,
                              QHostAddress const &localIp,
                              QString *errMsg = nullptr);
};

#endif // Q_OS_WIN

#endif // WINDOWSBINDHELPER_H
