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

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <QIODevice>

namespace {

void setErr(QString *errMsg, QString const &msg)
{
    if (errMsg)
        *errMsg = msg;
}

bool validateIpv4(QHostAddress const &localIp, QString *errMsg)
{
    if (localIp.protocol() == QAbstractSocket::IPv4Protocol)
        return true;
    setErr(errMsg, QStringLiteral("only IPv4 tun addresses supported"));
    return false;
}

SOCKET createTcpSocket(QString *errMsg)
{
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        setErr(errMsg, QStringLiteral("WSASocketW failed: WSA %1").arg(WSAGetLastError()));
    return s;
}

sockaddr_in makeIpv4Sockaddr(QHostAddress const &localIp)
{
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(localIp.toIPv4Address());
    return addr;
}

bool rawBind(SOCKET s, QHostAddress const &localIp, QString *errMsg)
{
    sockaddr_in addr = makeIpv4Sockaddr(localIp);
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        setErr(errMsg, QStringLiteral("raw bind() failed: WSA %1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

} // namespace

bool WindowsBindHelper::canBindLocalAddress(QHostAddress const &localIp,
                                            QString *errMsg)
{
    if (errMsg)
        errMsg->clear();
    if (!validateIpv4(localIp, errMsg))
        return false;

    SOCKET s = createTcpSocket(errMsg);
    if (s == INVALID_SOCKET)
        return false;

    bool ok = rawBind(s, localIp, errMsg);
    ::closesocket(s);
    return ok;
}

bool WindowsBindHelper::bindAndAttach(QAbstractSocket *socket,
                                      QHostAddress const &localIp,
                                      QString *errMsg)
{
    if (errMsg)
        errMsg->clear();
    if (!socket) {
        setErr(errMsg, QStringLiteral("null socket"));
        return false;
    }
    if (!validateIpv4(localIp, errMsg))
        return false;

    SOCKET s = createTcpSocket(errMsg);
    if (s == INVALID_SOCKET)
        return false;

    if (!rawBind(s, localIp, errMsg)) {
        ::closesocket(s);
        return false;
    }

    // Hand the bound descriptor over to Qt. State must be BoundState so the
    // subsequent connectToHost / connectToHostEncrypted is happy. ReadWrite
    // is what QAbstractSocket uses internally for client sockets.
    if (!socket->setSocketDescriptor(static_cast<qintptr>(s),
                                     QAbstractSocket::BoundState,
                                     QIODevice::ReadWrite)) {
        ::closesocket(s);
        setErr(errMsg, QStringLiteral("setSocketDescriptor refused: %1")
                           .arg(socket->errorString()));
        return false;
    }
    return true;
}

#endif // Q_OS_WIN
