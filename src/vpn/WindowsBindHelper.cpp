//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "WindowsBindHelper.h"

#ifdef Q_OS_WIN

// Qt already brings winsock2 via QtNetwork. We make sure it's defined BEFORE
// windows.h (which winsock2.h does for us if pulled first), then include
// ws2tcpip for IPv4 helpers.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

bool WindowsBindHelper::bindAndAttach(QAbstractSocket *socket,
                                      QHostAddress const &localIp,
                                      QString *errMsg)
{
    auto setErr = [errMsg](QString const &m) { if (errMsg) *errMsg = m; };

    if (!socket) {
        setErr(QStringLiteral("null socket"));
        return false;
    }
    if (localIp.protocol() != QAbstractSocket::IPv4Protocol) {
        setErr(QStringLiteral("only IPv4 tun addresses supported"));
        return false;
    }

    // WSAStartup has already been performed by Qt's network module — no need
    // to repeat it from here.
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        setErr(QStringLiteral("WSASocketW failed: WSA %1").arg(WSAGetLastError()));
        return false;
    }

    // Deliberately do NOT set SO_EXCLUSIVEADDRUSE or SO_REUSEADDR. Live
    // probing on Windows 11 against an OpenVPN DCO adapter showed that
    // setting both — which is exactly what Qt does when bound through
    // QAbstractSocket::bind() — fails the SO_REUSEADDR setsockopt with
    // WSAEINVAL, after which bind() rejects the virtual adapter IP. With
    // neither option set, raw bind() to the tun IP succeeds.

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(localIp.toIPv4Address());

    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int err = WSAGetLastError();
        ::closesocket(s);
        setErr(QStringLiteral("raw bind() failed: WSA %1").arg(err));
        return false;
    }

    // Hand the bound descriptor over to Qt. State must be BoundState so the
    // subsequent connectToHost / connectToHostEncrypted is happy. ReadWrite
    // is what QAbstractSocket uses internally for client sockets.
    if (!socket->setSocketDescriptor(static_cast<qintptr>(s),
                                     QAbstractSocket::BoundState,
                                     QIODevice::ReadWrite)) {
        ::closesocket(s);
        setErr(QStringLiteral("setSocketDescriptor refused: %1")
                   .arg(socket->errorString()));
        return false;
    }
    return true;
}

#endif // Q_OS_WIN
