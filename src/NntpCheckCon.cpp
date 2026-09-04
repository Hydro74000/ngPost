//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "NntpCheckCon.h"
#include <QSslSocket>
#include "NzbCheck.h"
#include "nntp/Nntp.h"
#include "vpn/VpnDnsResolver.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnSocketBinder.h"

NntpCheckCon::NntpCheckCon(NzbCheck *nzbCheck, int id, const NntpServerParams &srvParams)
    : QObject()
    , _nzbCheck(nzbCheck)
    , _id(id)
    , _srvParams(srvParams)
    , _socket(nullptr)
    , _isConnected(false)
    , _postingState(PostingState::NOT_CONNECTED)
    , _currentArticle()
    , _nbRetries(0)
    , _watchdog()
{
    // A server that accepts the TCP connection and then says nothing used to
    // hold the whole check open for ever: nothing in this class ever looked at
    // the socket timeout.
    _watchdog.setSingleShot(true);
    connect(&_watchdog, &QTimer::timeout, this, &NntpCheckCon::onWatchdogTimeout);

    connect(this,
            &NntpCheckCon::startConnection,
            this,
            &NntpCheckCon::onStartConnection,
            Qt::QueuedConnection);
    connect(this,
            &NntpCheckCon::killConnection,
            this,
            &NntpCheckCon::onKillConnection,
            Qt::QueuedConnection);
}

NntpCheckCon::~NntpCheckCon()
{
    if (_socket) {
        disconnect(_socket, &QAbstractSocket::disconnected, this, &NntpCheckCon::onDisconnected);
        disconnect(_socket, &QIODevice::readyRead, this, &NntpCheckCon::onReadyRead);
        _socket->disconnectFromHost();
        if (_socket->state() != QAbstractSocket::UnconnectedState)
            _socket->waitForDisconnected();
        _socket->deleteLater();
    }
}

void NntpCheckCon::onStartConnection()
{
    if (_srvParams.useSSL)
        _socket = new QSslSocket();
    else
        _socket = new QTcpSocket();

    // Per-server VPN bind plus global override: tunnel if either the server
    // has useVpn=true OR the global "Route ALL through VPN" toggle is on.
    VpnManager *vpn = VpnManager::instance();
    bool routeViaVpn = _srvParams.useVpn
                    || (vpn && vpn->forceAllConnectionsThroughVpn());
    if (routeViaVpn) {
        if (!vpn || !vpn->isConnected() || vpn->tunIp().isNull()) {
            _nzbCheck->error(tr("Server '%1' must route through the VPN but the tunnel is not connected")
                                 .arg(_srvParams.host));
            _socket->deleteLater();
            _socket = nullptr;
            emit disconnected(this);
            return;
        }
        QString bindErr;
        bool bound = VpnSocketBinder::bind(_socket, vpn->tunIp(), &bindErr);
        if (!bound) {
            _nzbCheck->error(tr("VPN bind failed on %1: %2 (local addresses visible to Qt: %3)")
                                 .arg(vpn->tunIp().toString(),
                                      bindErr,
                                      VpnSocketBinder::localAddressSummary()));
            _socket->deleteLater();
            _socket = nullptr;
            emit disconnected(this);
            return;
        }
    }

    _socket->setSocketOption(QAbstractSocket::KeepAliveOption, true);
    _socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(_socket,
            &QAbstractSocket::connected,
            this,
            &NntpCheckCon::onConnected,
            Qt::DirectConnection);
    connect(_socket,
            &QAbstractSocket::disconnected,
            this,
            &NntpCheckCon::onDisconnected,
            Qt::DirectConnection);
    connect(_socket, &QIODevice::readyRead, this, &NntpCheckCon::onReadyRead, Qt::DirectConnection);

    qRegisterMetaType<QAbstractSocket::SocketError>("SocketError");
    connect(_socket,
            SIGNAL(errorOccurred(QAbstractSocket::SocketError)),
            this,
            SLOT(onErrors(QAbstractSocket::SocketError)),
            Qt::DirectConnection);

    // Resolve through a DNS socket bound to the tunnel IP.
    bool connecting = false;
    if (routeViaVpn) {
        if (vpn && !vpn->dnsServer().isNull()) {
            QString dnsErr;
            auto const records = VpnDnsResolver::resolveA(
                _srvParams.host, vpn->dnsServer(), vpn->tunIp(), &dnsErr);
            if (!records.isEmpty()) {
                // Set peerVerifyName so the SSL cert is validated against
                // the original hostname rather than the resolved IP.
                if (_srvParams.useSSL)
                    static_cast<QSslSocket *>(_socket)
                        ->setPeerVerifyName(_srvParams.host);
                _socket->connectToHost(records.first(), _srvParams.port);
                connecting = true;
            } else {
                _nzbCheck->error(
                        tr("VPN DNS lookup failed for %1 via %2: %3 — falling back to system DNS")
                                .arg(_srvParams.host,
                                     vpn->dnsServer().toString(),
                                     dnsErr.isEmpty() ? tr("unknown error") : dnsErr));
            }
        }
    }
    if (!connecting)
        _socket->connectToHost(_srvParams.host, _srvParams.port);

    // Single arming point. The VPN branch used to return from the middle of
    // this function, leaving a tunnelled check with no watchdog at all.
    _watchdog.start(_nzbCheck->socketTimeOut());
}

void NntpCheckCon::_send(const char *cmd)
{
    if (!_socket)
        return;
    _socket->write(cmd);
    // Anything we ask for, we wait for -- with a deadline.
    _watchdog.start(_nzbCheck->socketTimeOut());
}

void NntpCheckCon::onKillConnection()
{
    _watchdog.stop();
    if (_socket) {
        disconnect(_socket, &QIODevice::readyRead, this, &NntpCheckCon::onReadyRead);
        disconnect(_socket, &QAbstractSocket::disconnected, this, &NntpCheckCon::onDisconnected);
        _socket->disconnectFromHost();
        if (_socket->state() != QAbstractSocket::UnconnectedState)
            _socket->waitForDisconnected();
        _socket->deleteLater();
        _socket = nullptr;
    }
}

void NntpCheckCon::onConnected()
{
    _isConnected = true;
    if (_srvParams.useSSL) {
        QSslSocket *sslSock = static_cast<QSslSocket *>(_socket);
        connect(sslSock,
                SIGNAL(sslErrors(QList<QSslError>)),
                this,
                SLOT(onSslErrors(QList<QSslError>)),
                Qt::DirectConnection);

        connect(sslSock,
                &QSslSocket::encrypted,
                this,
                &NntpCheckCon::onEncrypted,
                Qt::DirectConnection);
        emit sslSock->startClientEncryption();
    } else {
        if (_nzbCheck->debugMode())
            _nzbCheck->log(tr("[Con #%1] Connected").arg(_id));

        _postingState = PostingState::CONNECTED;
        // We should receive the Hello Message
    }
}

void NntpCheckCon::onEncrypted()
{
    if (_nzbCheck->debugMode())
        _nzbCheck->log(tr("[Con #%1] Connected").arg(_id));

    _postingState = PostingState::CONNECTED;
    // We should receive the Hello Message
}

void NntpCheckCon::onDisconnected()
{
    _watchdog.stop();
    if (_socket) {
        _isConnected = false;
        _socket->deleteLater();
        _socket = nullptr;
    }
    _finishOrRetry();
}

void NntpCheckCon::onWatchdogTimeout()
{
    _nzbCheck->error(tr("[Con #%1] %2:%3 stopped answering after %4 s, dropping the connection")
                             .arg(_id)
                             .arg(_srvParams.host)
                             .arg(_srvParams.port)
                             .arg(_nzbCheck->socketTimeOut() / 1000));
    if (!_socket) {
        _finishOrRetry();
        return;
    }
    disconnect(_socket, &QIODevice::readyRead, this, &NntpCheckCon::onReadyRead);
    bool const wasConnected = _isConnected;
    _socket->abort();
    // abort() only emits disconnected() when the socket had reached the
    // connected state; a stalled connect leaves us to close the loop ourselves.
    if (!wasConnected) {
        _socket->deleteLater();
        _socket      = nullptr;
        _isConnected = false;
        _finishOrRetry();
    }
}

void NntpCheckCon::_finishOrRetry()
{
    // The article that was in flight never got its answer. Hand it back, or it
    // is simply never checked and the run quietly reports fewer articles than
    // the nzb holds -- which is exactly what makes a check "incomplete".
    if (!_currentArticle.isNull()) {
        _nzbCheck->requeueArticle(_currentArticle);
        _currentArticle.clear();
    }

    if (_nzbCheck->hasArticlesLeft() && _nbRetries < _nzbCheck->maxRetries()) {
        ++_nbRetries;
        _nzbCheck->error(tr("[Con #%1] reconnecting (attempt %2 of %3)")
                                 .arg(_id)
                                 .arg(_nbRetries)
                                 .arg(_nzbCheck->maxRetries()));
        _postingState = PostingState::NOT_CONNECTED;
        emit startConnection();
        return;
    }

    emit disconnected(this);
}

void NntpCheckCon::onReadyRead()
{
    while (_isConnected && _socket->canReadLine()) {
        // Disarmed here and not before the loop: readyRead also fires on half a
        // line, and stopping the watchdog on that left a server free to send
        // one byte and then go quiet for ever.
        _watchdog.stop();
        QByteArray line = _socket->readLine();
        //        qDebug() << "line: " << line.constData();

        if (_postingState == PostingState::CHECKING_ARTICLE) {
            if (strncmp(line.constData(), Nntp::getResponse(430), 3) == 0)
                _nzbCheck->missingArticle(_currentArticle);

            _nzbCheck->articleChecked(_currentArticle);
            _currentArticle.clear(); // answered: no longer in flight
            _nbRetries = 0;          // the budget is per incident, not per run
            _postingState = PostingState::IDLE;
            _checkNextArticle();
        } else if (_postingState == PostingState::CONNECTED) {
            // Check welcome message
            if (strncmp(line.constData(), Nntp::getResponse(200), 3) != 0) {
                emit errorConnecting(tr("[Connection #%1] Error connecting to server %2:%3")
                                         .arg(_id)
                                         .arg(_srvParams.host)
                                         .arg(_srvParams.port));
                _closeConnection();
            } else {
                // Start authentication : send user info
                if (_srvParams.user.empty()) {
                    _postingState = PostingState::IDLE;
                    _checkNextArticle();
                } else {
                    _postingState = PostingState::AUTH_USER;

                    std::string cmd(Nntp::AUTHINFO_USER);
                    cmd += _srvParams.user;
                    cmd += Nntp::ENDLINE;
                    _send(cmd.c_str());
                }
            }
        } else if (_postingState == PostingState::AUTH_USER) {
            // validate the reply
            if (strncmp(line.constData(), Nntp::getResponse(381), 2) != 0) {
                emit errorConnecting(tr("[Connection #%1] Error sending user '%4' to server %2:%3")
                                         .arg(_id)
                                         .arg(_srvParams.host)
                                         .arg(_srvParams.port)
                                         .arg(_srvParams.user.c_str()));
                _closeConnection();
            } else {
                // Continue authentication : send pass info
                _postingState = PostingState::AUTH_PASS;

                std::string cmd(Nntp::AUTHINFO_PASS);
                cmd += _srvParams.pass;
                cmd += Nntp::ENDLINE;
                _send(cmd.c_str());
            }
        } else if (_postingState == PostingState::AUTH_PASS) {
            if (strncmp(line.constData(), Nntp::getResponse(281), 2) != 0) {
                emit errorConnecting(tr("[Connection #%1] Error authentication to server %2:%3 "
                                        "with user '%4'")
                                         .arg(_id)
                                         .arg(_srvParams.host)
                                         .arg(_srvParams.port)
                                         .arg(_srvParams.user.c_str()));
                _closeConnection();
            } else {
                _postingState = PostingState::IDLE;
                _checkNextArticle();
            }
        }
    }
}

void NntpCheckCon::onSslErrors(const QList<QSslError> &errors)
{
    QString err("Error SSL Socket:\n");
    for (int i = 0; i < errors.size(); ++i)
        err += QString("\t- %1\n").arg(errors[i].errorString());
    _nzbCheck->error(err);
    _closeConnection();
}

void NntpCheckCon::onErrors(QAbstractSocket::SocketError)
{
    _nzbCheck->error(QString("Error Socket: %1").arg(_socket->errorString()));
    _closeConnection();
}

void NntpCheckCon::_closeConnection()
{
    if (_socket && _isConnected) {
        disconnect(_socket, &QIODevice::readyRead, this, &NntpCheckCon::onReadyRead);
        _socket->disconnectFromHost();
    } else // wrong host info or network down
    {
        if (_socket)
            _socket->deleteLater();
        _socket = nullptr;
        _finishOrRetry();
    }
}

void NntpCheckCon::_checkNextArticle()
{
    _currentArticle = _nzbCheck->getNextArticle();

    if (!_currentArticle.isNull()) {
        if (_nzbCheck->debugMode())
            _nzbCheck->log(tr("[Con #%1] Checking article %2").arg(_id).arg(_currentArticle));

        _postingState = PostingState::CHECKING_ARTICLE;
        _send(QString("%1 %2\r\n")
                      .arg(Nntp::STAT)
                      .arg(_currentArticle)
                      .toLocal8Bit()
                      .constData());
    } else {
        _watchdog.stop();
        if (_nzbCheck->debugMode())
            _nzbCheck->log(tr("[Con #%1] No more Article").arg(_id));

        _postingState = PostingState::IDLE;
        _closeConnection();
    }
}
