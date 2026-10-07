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

#include "NntpConnection.h"
#include "NgPost.h"
#include "Poster.h"
#include "nntp/Nntp.h"
#include "nntp/NntpArticle.h"
#include "nntp/NntpFile.h"
#include "nntp/NntpServerParams.h"
#include "vpn/VpnDnsResolver.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnSocketBinder.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QFile>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslKey>
#include <QSslSocket>
#ifdef __USE_CONNECTION_TIMEOUT__
#include <QTimer>
#endif

NntpConnection::NntpConnection(NgPost *ngPost, int id, const NntpServerParams &srvParams)
    : QObject()
    , _conId(id)
    , _srvParams(srvParams)
    , _socket(nullptr)
    , _isConnected(false)
    , _logPrefix(QString("NntpCon #%1").arg(_conId))
    , _postingState(PostingState::NOT_CONNECTED)
    , _currentArticle(nullptr)
    , _currentArticlePreserved(false)
    , _nbDisconnected(0)
    , _authRejected(false)
    , _becameReady(false)
    , _ngPost(ngPost)
    , _poster(nullptr)
#ifdef __USE_CONNECTION_TIMEOUT__
    , _timeout(nullptr)
#endif
{
#if defined(__DEBUG__) && defined(LOG_CONSTRUCTORS)
    qDebug() << QString("Creation %1 %2 ssl").arg(_logPrefix).arg(_srvParams.useSSL ? "with" : "no");
#endif

    connect(this,
            &NntpConnection::startConnection,
            this,
            &NntpConnection::onStartConnection,
            Qt::QueuedConnection);
    connect(this,
            &NntpConnection::killConnection,
            this,
            &NntpConnection::onKillConnection,
            Qt::QueuedConnection);
}

NntpConnection::~NntpConnection()
{
#if defined(__DEBUG__) && defined(LOG_CONSTRUCTORS)
    qDebug() << "Destruction NntpConnection " << _logPrefix;
#endif
    if (_ngPost->debugMode())
        _log("Destructing connection..");

    // this should already have been triggered as the sockets lives in another thread
    if (_socket) {
        _shutdownSocket();
    }
#ifdef __USE_CONNECTION_TIMEOUT__
    if (_timeout)
        delete _timeout;
#endif
}

void NntpConnection::onStartConnection()
{
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
    _log("Starting connection...");
#endif
    // A reconnect signal can already be queued when a user/VPN pause publishes
    // its admission barrier. Do not even create a transport in that window;
    // resume() will emit a fresh startConnection once the route is healthy.
    if (_poster && _poster->isPaused())
        return;

    _lastTransportError.clear();
    _authRejected = false;
    if (_srvParams.useSSL)
        _socket = new QSslSocket();
    else
        _socket = new QTcpSocket();

    // Per-server `useVpn` decides by default; the global "Route ALL ngPost
    // connections through the VPN" override forces every connection into the
    // tunnel regardless of per-server flags.
    // Fail-closed in both cases: if the tunnel is required but not ready,
    // refuse the connection rather than leaking traffic in the clear.
    //
    // We do this BEFORE setSocketOption to avoid any chance of forcing the
    // underlying socket descriptor to be created with the wrong protocol
    // family (which would later cause an IPv4 bind to fail).
    VpnManager *vpn = _ngPost->vpnManager();
    bool routeViaVpn = _srvParams.useVpn
                    || (vpn && vpn->forceAllConnectionsThroughVpn());
    if (routeViaVpn) {
        if (!vpn || !vpn->isConnected() || vpn->tunIp().isNull()) {
            _error(tr("Server '%1' must route through the VPN but the tunnel is not connected")
                       .arg(_srvParams.host));
            deleteSocket();
            emit disconnected(this);
            return;
        }
        QString bindErr;
        bool bound = VpnSocketBinder::bind(_socket, vpn->tunIp(), &bindErr);
        if (!bound) {
            _error(tr("VPN bind failed on %1: %2 (local addresses visible to Qt: %3)")
                       .arg(vpn->tunIp().toString(),
                            bindErr,
                            VpnSocketBinder::localAddressSummary()));
            deleteSocket();
            emit disconnected(this);
            return;
        }
    }

    _socket->setSocketOption(QAbstractSocket::KeepAliveOption, true);
    _socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    _socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, NgPost::articleSize());

    connect(_socket,
            &QAbstractSocket::connected,
            this,
            &NntpConnection::onConnected,
            Qt::DirectConnection);
    connect(_socket,
            &QAbstractSocket::disconnected,
            this,
            &NntpConnection::onDisconnected,
            Qt::DirectConnection);
    connect(_socket, &QIODevice::readyRead, this, &NntpConnection::onReadyRead, Qt::DirectConnection);

    qRegisterMetaType<QAbstractSocket::SocketError>("SocketError");
    connect(_socket,
            SIGNAL(errorOccurred(QAbstractSocket::SocketError)),
            this,
            SLOT(onErrors(QAbstractSocket::SocketError)),
            Qt::DirectConnection);

    // Resolve through a DNS socket bound to the tunnel IP. Each NNTP
    // connection runs on a Poster thread, so the blocking lookup does not
    // freeze the GUI. On failure we fall back to the system resolver so the
    // post can continue, but we log that DNS may leak.
    if (routeViaVpn) {
        if (vpn && !vpn->dnsServer().isNull()) {
            QString dnsErr;
            auto const records = VpnDnsResolver::resolveA(
                _srvParams.host, vpn->dnsServer(), vpn->tunIp(), &dnsErr);
            if (!records.isEmpty()) {
                // When connectToHost is called with a QHostAddress (raw IP),
                // QSslSocket defaults peerVerifyName to that IP. Keep SNI and
                // certificate verification tied to the configured hostname.
                if (_srvParams.useSSL)
                    static_cast<QSslSocket *>(_socket)
                        ->setPeerVerifyName(_srvParams.host);
                _socket->connectToHost(records.first(), _srvParams.port);
                goto connect_done;
            }
            _error(tr("VPN DNS lookup failed for %1 via %2: %3 — falling back to "
                      "system resolver (DNS may leak)")
                       .arg(_srvParams.host,
                            vpn->dnsServer().toString(),
                            dnsErr.isEmpty() ? tr("unknown error") : dnsErr));
        }
    }
    _socket->connectToHost(_srvParams.host, _srvParams.port);
connect_done:

#ifdef __USE_CONNECTION_TIMEOUT__
    if (!_timeout) {
        _timeout = new QTimer();
        connect(_timeout, &QTimer::timeout, this, &NntpConnection::onTimeout);
    }
    _timeout->start(_ngPost->getSocketTimeout());
#endif
}

void NntpConnection::onKillConnection()
{
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
    qDebug() << "[killConnection] #" << _conId;
#endif
#ifdef __USE_CONNECTION_TIMEOUT__
    if (_timeout)
        _timeout->stop();
#endif

    if (_socket) {
        if (_ngPost->debugMode())
            _log("Killing connection..");

        _shutdownSocket();
    }
    // Pause/stop can cut an article after the socket write but before the
    // server reply. This is independent of whether the socket object survived
    // until the queued kill slot ran.
    _preserveCurrentArticleAfterTransportLoss(
        tr("connection killed before server confirmation"));
}

void NntpConnection::_preserveCurrentArticleAfterTransportLoss(QString const &reason)
{
    if (!_currentArticle)
        return;

    if (_currentArticlePreserved)
        return;

    // Persist the Message-ID used by the ambiguous attempt before replacing it:
    // it is the identifier that may already exist on the server and is needed
    // for diagnosis/history. A later resume must use a fresh Message-ID.
    _currentArticle->nntpFile()->markArticleUnknown(_currentArticle, reason);
    _currentArticlePreserved = true;
    _currentArticle->genNewId();

    // _currentArticle is deliberately kept. The reconnect path in
    // onDisconnected() reposts exactly this article, so clearing it here would
    // drop it. On the terminal paths the connection is finished either way and
    // _finishPosting() closes every transport before the counters are read.
}

//! Stops the socket from reaching this connection: data, errors, TLS errors.
void NntpConnection::_detachSocketSignals()
{
    disconnect(_socket, &QIODevice::readyRead, this, &NntpConnection::onReadyRead);
    disconnect(_socket,
               SIGNAL(errorOccurred(QAbstractSocket::SocketError)),
               this,
               SLOT(onErrors(QAbstractSocket::SocketError)));
    if (_srvParams.useSSL)
        disconnect(_socket,
                   SIGNAL(sslErrors(QList<QSslError>)),
                   this,
                   SLOT(onSslErrors(QList<QSslError>)));
}

//! Detaches every socket signal, closes the connection and deletes the socket.
void NntpConnection::_shutdownSocket()
{
    disconnect(_socket, &QAbstractSocket::disconnected, this, &NntpConnection::onDisconnected);
    _detachSocketSignals();
    _socket->disconnectFromHost();
    if (_socket->state() != QAbstractSocket::UnconnectedState)
        _socket->waitForDisconnected();
    deleteSocket();
}

void NntpConnection::_closeConnection(bool dropTransport)
{
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
    _log("closeConnection");
#endif
    if (_ngPost->debugMode())
        _log("Closing connection...");
#ifdef __USE_CONNECTION_TIMEOUT__
    if (_timeout)
        _timeout->stop();
#endif
    if (_socket && _isConnected) {
        _detachSocketSignals();

        if (dropTransport) {
            // A stalled or failed transport never flushes its write buffer:
            // disconnectFromHost() would wait for it until TCP gives up.
            // The test below is not redundant: abort() re-enters onDisconnected().
            // cppcheck-suppress nullPointerRedundantCheck
            _socket->abort();
            // From a connected state abort() emits disconnected(), and
            // onDisconnected() has already released the socket.
            if (_socket)
                onDisconnected();
        } else {
            _socket->disconnectFromHost(); // we will end up in NntpConnect::onDisconnected
        }
    } else // wrong host info or network down
    {
        _isConnected = false;
        if (_socket)
            deleteSocket();

        _preserveCurrentArticleAfterTransportLoss(
            tr("connection closed before server confirmation"));
        emit disconnected(this);
    }
}

void NntpConnection::onDisconnected()
{
    if (_socket) {
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
        // A peer disconnect is not by itself an application error: at the
        // end of a successful post it is the expected transport lifecycle.
        // Debug-only diagnostic -- the enclosing block is compiled out of
        // release builds, where _error() therefore never ran either.
        _log("> disconnected");
#endif
        _isConnected = false;

        deleteSocket();
    }
    if (_poster->isPosting() && !_poster->isPaused() && _postingState != PostingState::NO_MORE_FILES
        && !_authRejected && _nbDisconnected++ < NntpArticle::nbMaxTrySending()) {
        // Let's try to reconnect
        const QString server = QString("%1:%2").arg(_srvParams.host).arg(_srvParams.port);
        emit retryingConnection(
            server,
            QString("[%1] %2: %3 (%4)")
                .arg(_logPrefix,
                     server,
                     tr("Connection lost, trying to reconnect! (nb disconnected: %1)")
                         .arg(_nbDisconnected),
                     _lastTransportError.isEmpty() ? tr("Remote connection closed")
                                                   : _lastTransportError));
        _preserveCurrentArticleAfterTransportLoss(
            tr("connection lost before server confirmation"));

        emit startConnection();
    } else {
        if (!_lastTransportError.isEmpty() && !_poster->isPaused()
            && _postingState != PostingState::NO_MORE_FILES)
            _error(_lastTransportError);
        _preserveCurrentArticleAfterTransportLoss(
            tr("connection lost before server confirmation"));
        emit disconnected(this);
    }
}

void NntpConnection::onConnected()
{
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
    _log("> connected to server");
#endif
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
                &NntpConnection::onEncrypted,
                Qt::DirectConnection);
        emit sslSock->startClientEncryption();
    } else {
        _postingState = PostingState::CONNECTED;
        // We should receive the Hello Message
    }
}

void NntpConnection::onEncrypted()
{
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
    _log("> SSL handshake succeed");
#endif

    _postingState = PostingState::CONNECTED;
    // We should receive the Hello Message
}

void NntpConnection::onSslErrors(const QList<QSslError> &errors)
{
    QString err("Error SSL Socket:\n");
    for (int i = 0; i < errors.size(); ++i)
        err += QString("\t- %1\n").arg(errors[i].errorString());
    _error(err);
    _closeConnection();
}

void NntpConnection::onErrors(QAbstractSocket::SocketError)
{
    if (!_socket)
        return;
    _lastTransportError = QString("Error Socket: %1").arg(_socket->errorString());
    // Established transports get a bounded retry in onDisconnected(). A
    // recovered interruption is a diagnostic, not a permanently failed post.
    if (!_isConnected)
        _error(_lastTransportError);
    _closeConnection(true);
}

#ifdef __USE_CONNECTION_TIMEOUT__
void NntpConnection::onTimeout()
{
    _lastTransportError = QString("Socket Timeout (%1 ms)").arg(_ngPost->getSocketTimeout());
    if (!_isConnected)
        _error(_lastTransportError);
    _closeConnection(true);
}
#endif

void NntpConnection::onReadyRead()
{
    while (_isConnected && _socket->canReadLine()) {
        QByteArray line = _socket->readLine();
#ifdef __USE_CONNECTION_TIMEOUT__
        if (_timeout)
            _timeout->start(_ngPost->getSocketTimeout());
#endif

#if defined(__DEBUG__) && defined(LOG_NEWS_DATA)
        _log(QString("Data In: %1").arg(line.constData()));
#endif
        if (_postingState == PostingState::SENDING_ARTICLE) {
            _handlePostResponse(line);
        } else if (_postingState == PostingState::WAITING_ANSWER) {
            _handleArticleResponse(line);
        } else if (_postingState == PostingState::CONNECTED) {
            if (!_handleWelcome(line))
                return;
        } else if (_postingState == PostingState::AUTH_USER) {
            if (!_handleAuthUser(line))
                return;
        } else if (_postingState == PostingState::AUTH_PASS) {
            _handleAuthPass(line);
        }
    }
}

void NntpConnection::_handlePostResponse(QByteArray &line)
{
#if defined(__DEBUG__) && defined(LOG_POSTING_STEPS)
    _log(QString("post response: %1").arg(line.constData()));
#endif

    if (strncmp(line.constData(), Nntp::getResponse(340), 3) == 0) {
        _postingState = PostingState::WAITING_ANSWER;
        // A body is about to go over the wire. It is a new ambiguous
        // attempt even when this article was preserved on a previous
        // connection and retained for retry.
        _currentArticlePreserved = false;
        _currentArticle->write(this, _ngPost->aticleSignature()); // This will be done async
        if (_ngPost->dispPostingFile() && _currentArticle->isFirstArticle())
            emit _currentArticle->nntpFile()->startPosting();
    } else {
        //                if (++_nbErrors < NntpArticle::nbMaxTrySending())
        //                {
        //                    _socket->write(Nntp::POST);
        //                    if (_ngPost->debugMode())
        //                        _error(tr("ERROR on post command: %1").arg(line.constData()));
        //                }
        //                else
        //                {
        _postingState = PostingState::NOT_CONNECTED;
        _error(tr("Closing Connection due to ERROR on post command: '%2' (%1 skipped)\n")
                   .arg(_currentArticle->str())
                   .arg(line.constData()));
        //                    emit _currentArticle->failed(_currentArticle->size());
        _closeConnection();
        //                }
    }
}

void NntpConnection::_handleArticleResponse(QByteArray &line)
{
    if (strncmp(line.constData(), Nntp::getResponse(240), 3) == 0) {
        // Check if the server overwrite the Message-ID
        // 240 <5ed10f42$0$7342$f56682d5@speedium.nl> Article posted
        const char *lt = strchr(line.constData(), '<');
        if (lt) {
            const char *gt = strchr(lt, '>');
            if (gt) {
                line[static_cast<int>(gt - line.constData())] = '\0';
                QString newMsgId(lt + 1);
                if (_ngPost->debugFull())
                    _log(QString("the server has overwritten the Message-ID to : %1 "
                                 "(article: %2)")
                             .arg(newMsgId)
                             .arg(_currentArticle->id()));
                _currentArticle->overwriteMsgId(newMsgId);
            }
        }
        _postingState = PostingState::IDLE;
#if defined(__DEBUG__) && defined(LOG_POSTING_STEPS)
        _log(tr("POSTED: %1").arg(_currentArticle->str()));
#endif
#ifdef __DISP_ARTICLE_SERVER__
        if (_ngPost->debugMode())
            _log(tr("Article posted: %1 (on %2) %3")
                     .arg(_currentArticle->id())
                     .arg(_srvParams.host)
                     .arg(line.constData()));
#endif
        emit _currentArticle->posted(_currentArticle->size());
    } else {
#if defined(__DEBUG__) && defined(LOG_POSTING_STEPS)
        _error(
            tr("Error on posting article %1: %2").arg(_currentArticle->id()).arg(line.constData()));
#endif
        if (_currentArticle->tryResend()) {
            _postingState = PostingState::SENDING_ARTICLE;
            _socket->write(Nntp::POST);
            if (_ngPost->debugMode())
                _log(
                    tr("ReTry %1 (Error: '%2')").arg(_currentArticle->str()).arg(line.constData()));
        } else {
            _postingState = PostingState::IDLE;
            _error(tr("FAIL posting %1 (Error: '%2')")
                       .arg(_currentArticle->str())
                       .arg(line.constData()));
#ifdef __DISP_ARTICLE_SERVER__
            if (_ngPost->debugMode())
                _log(tr("Article FAIL: %1 (on %2) %3")
                         .arg(_currentArticle->id())
                         .arg(_srvParams.host)
                         .arg(line.constData()));
#endif

            // A complete NNTP reply is definitive. Once this
            // article's retry budget is exhausted it is failed; only
            // transport loss without a final reply is classified as
            // unknown.
            emit _currentArticle->failed(_currentArticle->size());
        }
    }
    if (_postingState == PostingState::IDLE) {
        _currentArticle = nullptr;
        _currentArticlePreserved = false;
        _sendNextArticle();
    }
}

bool NntpConnection::_handleWelcome(QByteArray &line)
{
    // Check welcome message
    if (strncmp(line.constData(), Nntp::getResponse(200), 3) != 0) {
        QString err("Reading welcome message. Should start with 200... Server message: ");
        err += line.constData();
        if (_ngPost->debugMode())
            _error(err);
        //#if defined(__DEBUG__) && defined(LOG_CONNECTION_ERRORS_BEFORE_EMIT_SIGNALS)
        //                _error(err);
        //#endif
        emit errorConnecting(tr("[Connection #%1] Error connecting to server %2:%3")
                                 .arg(_conId)
                                 .arg(_srvParams.host)
                                 .arg(_srvParams.port));
        _closeConnection();
    } else {
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
        _log("> received Hello Message");
#endif

        // Start authentication : send user info
        if (_srvParams.user.empty()) {
            _postingState = PostingState::IDLE;
            _becameReady = true;
            _sendNextArticle();
        } else {
            _postingState = PostingState::AUTH_USER;

            QByteArray const cmd = Nntp::authInfoUser(_srvParams.user);
            if (cmd.isEmpty()) {
                emit errorConnecting(tr("[Connection #%1] The configured user for %2:%3 contains a "
                                        "line break and cannot be sent")
                                         .arg(_conId)
                                         .arg(_srvParams.host)
                                         .arg(_srvParams.port));
                _authRejected = true;
                _closeConnection();
                return false;
            }
            _socket->write(cmd);
        }
    }
    return true;
}

bool NntpConnection::_handleAuthUser(QByteArray &line)
{
    // validate the reply
    if (strncmp(line.constData(), Nntp::getResponse(381), 2) != 0) {
        QString err("Wrong Authentication: response from '");
        err += Nntp::AUTHINFO_USER;
        err += "' should start with 38... resp: ";
        err += line.constData();
        if (_ngPost->debugMode())
            _error(err);
        //#if defined(__DEBUG__) && defined(LOG_CONNECTION_ERRORS_BEFORE_EMIT_SIGNALS)
        //                _error(err);
        //#endif
        emit errorConnecting(tr("[Connection #%1] Error sending user '%4' to server %2:%3")
                                 .arg(_conId)
                                 .arg(_srvParams.host)
                                 .arg(_srvParams.port)
                                 .arg(_srvParams.user.c_str()));
        _authRejected = true;
        _closeConnection();
    } else {
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
        _log("> AUTHINFO_USER succeed");
#endif

        // Continue authentication : send pass info
        _postingState = PostingState::AUTH_PASS;

        QByteArray const cmd = Nntp::authInfoPass(_srvParams.pass);
        if (cmd.isEmpty()) {
            emit errorConnecting(tr("[Connection #%1] The configured password for %2:%3 contains a "
                                    "line break and cannot be sent")
                                     .arg(_conId)
                                     .arg(_srvParams.host)
                                     .arg(_srvParams.port));
            _authRejected = true;
            _closeConnection();
            return false;
        }
        _socket->write(cmd);
    }
    return true;
}

void NntpConnection::_handleAuthPass(QByteArray &line)
{
    if (strncmp(line.constData(), Nntp::getResponse(281), 2) != 0) {
        QString err("Wrong Authentication: response from '");
        err += Nntp::AUTHINFO_PASS;
        err += "' should start with 28... resp: ";
        err += line.constData();
        if (_ngPost->debugMode())
            _error(err);
        //#if defined(__DEBUG__) && defined(LOG_CONNECTION_ERRORS_BEFORE_EMIT_SIGNALS)
        //                _error(err);
        //#endif
        emit errorConnecting(tr("[Connection #%1] Error authentication to server %2:%3 "
                                "with user '%4'")
                                 .arg(_conId)
                                 .arg(_srvParams.host)
                                 .arg(_srvParams.port)
                                 .arg(_srvParams.user.c_str()));
        _authRejected = true;
        _closeConnection();
    } else {
#if defined(__DEBUG__) && defined(LOG_CONNECTION_STEPS)
        _log("> AUTHINFO_PASS succeed => ready to POST \\o/");
#endif
        _postingState = PostingState::IDLE;
        _becameReady = true;
        _sendNextArticle();
    }
}

void NntpConnection::_sendNextArticle()
{
    if (_poster->isPaused())
        return;

    if (!_currentArticle) { // in case of error and reconnection, we repost the _currentArticle
        _currentArticle = _poster->getNextArticle(_logPrefix);
        _currentArticlePreserved = false;
    }

    // Pause can race with getNextArticle() on another thread. Hold an article
    // already dequeued for the later resume, and treat a null result as an
    // admission barrier rather than end-of-input.
    if (_poster->isPaused())
        return;

    if (_currentArticle) {
        _postingState = PostingState::SENDING_ARTICLE;
        if (_ngPost->debugFull())
            _log(tr("start sending article: %1").arg(_currentArticle->str()));
        _socket->write(Nntp::POST);
    } else {
        _postingState = PostingState::NO_MORE_FILES;
#ifdef __USE_CONNECTION_TIMEOUT__
        if (_timeout)
            _timeout->stop();
#endif
        if (_ngPost->debugMode())
            _log("No more articles");
        _closeConnection();
    }
}

void NntpConnection::setPoster(Poster *poster)
{
    _poster = poster;
    _logPrefix = QString("%1 {%2}").arg(poster->name(), _logPrefix);
}

QString NntpConnection::sslSupportInfo()
{
    return QString("SSL support: %1, build version: %2, system version: %3")
        .arg(QSslSocket::supportsSsl() ? "yes" : "no",
             QSslSocket::sslLibraryBuildVersionString(),
             QSslSocket::sslLibraryVersionString());
}

bool NntpConnection::supportsSsl()
{
    return QSslSocket::supportsSsl();
}
