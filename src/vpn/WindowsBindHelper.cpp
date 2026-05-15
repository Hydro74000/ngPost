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
#include <QNetworkInterface>

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

bool findInterfaceIndex(QHostAddress const &localIp, DWORD *ifIndex, QString *errMsg)
{
    for (QNetworkInterface const &iface : QNetworkInterface::allInterfaces()) {
        for (QNetworkAddressEntry const &entry : iface.addressEntries()) {
            if (entry.ip() == localIp) {
                uint const idx = iface.index();
                if (idx == 0) {
                    setErr(errMsg, QStringLiteral("interface index unavailable for %1")
                                       .arg(localIp.toString()));
                    return false;
                }
                *ifIndex = DWORD(idx);
                return true;
            }
        }
    }
    setErr(errMsg, QStringLiteral("no interface index found for %1")
                       .arg(localIp.toString()));
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

bool setOutgoingInterface(SOCKET s, QHostAddress const &localIp, QString *errMsg)
{
    DWORD ifIndex = 0;
    if (!findInterfaceIndex(localIp, &ifIndex, errMsg))
        return false;

    // Windows bind() pins the source address, but route selection can still
    // fail or pick another adapter. IP_UNICAST_IF pins outgoing IPv4 traffic
    // to the tunnel interface; the IF_INDEX value must be in network order.
    DWORD const opt = htonl(ifIndex);
    if (::setsockopt(s, IPPROTO_IP, IP_UNICAST_IF,
                     reinterpret_cast<const char *>(&opt), sizeof(opt)) != 0) {
        setErr(errMsg, QStringLiteral("IP_UNICAST_IF(%1) failed: WSA %2")
                           .arg(ifIndex)
                           .arg(WSAGetLastError()));
        return false;
    }
    return true;
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

    if (!setOutgoingInterface(s, localIp, errMsg)) {
        ::closesocket(s);
        return false;
    }

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

    if (!setOutgoingInterface(s, localIp, errMsg)) {
        ::closesocket(s);
        return false;
    }

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
