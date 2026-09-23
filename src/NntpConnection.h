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

#ifndef NNTPCONNECTION_H
#define NNTPCONNECTION_H

#include <QSslError>
#include <QTcpSocket>

struct NntpServerParams;
class NntpArticle;
class NgPost;
class Poster;

class QSslSocket;
class QByteArray;
#ifdef __USE_CONNECTION_TIMEOUT__
class QTimer;
#endif

/*!
 * \brief NntpConnection is a client side Active Object that is made to Post Articles in an async way
 *
 * the
 */
class NntpConnection : public QObject
{
    Q_OBJECT

private:
    enum class PostingState {
        NOT_CONNECTED = 0,
        CONNECTED,
        AUTH_USER,
        AUTH_PASS,
        IDLE,
        SENDING_ARTICLE,
        WAITING_ANSWER,
        NO_MORE_FILES
    };

    const int _conId;                   //!< connection id
    const NntpServerParams &_srvParams; //!< server parameters

    //! Owned, without a Qt parent on purpose: created by onStartConnection() in
    //! this connection's thread, released by deleteSocket() with deleteLater() and
    //! nulled, so a queued event can never reach a deleted socket. Not a
    //! unique_ptr: a QObject living in another thread must not be deleted at once.
    QTcpSocket *_socket;
    bool _isConnected;   //!< to avoid to rely on iSocket && iSocket->isOpen()

    QString _logPrefix; //!< log prefix: NntpConnection[<iSocketDescriptor>]

    PostingState _postingState;
    NntpArticle *_currentArticle;
    bool _currentArticlePreserved;
    ushort _nbDisconnected;
    QString _lastTransportError;
    //! The server refused our credentials (or they cannot be sent) on the
    //! latest attempt. Replaying them at once cannot succeed, so the immediate
    //! reconnect is skipped and PostingJob decides from the backoff cycle.
    bool _authRejected;
    //! Reached the posting state since PostingJob last asked: proof that these
    //! credentials worked, which takeBecameReady() consumes.
    bool _becameReady;

    NgPost *_ngPost;
    Poster *_poster;
#ifdef __USE_CONNECTION_TIMEOUT__
    QTimer *_timeout;
#endif

public:
    /*!
     * \brief NntpConnection constructor
     * \param id          : connection id
     * \param ssl         : should the connection be encrypted?
     */
    explicit NntpConnection(NgPost *ngPost, int id, const NntpServerParams &srvParams);

    NntpConnection(const NntpConnection &) = delete;
    NntpConnection(const NntpConnection &&) = delete;
    NntpConnection &operator=(const NntpConnection &) = delete;
    NntpConnection &operator=(const NntpConnection &&) = delete;

    ~NntpConnection() override; //!< destructor: delete the QTcpSocket

    inline int getId() const; //!< NntpConnection id: iSocketDescriptor

    inline void write(const QByteArray &aBuffer); //!< write on the socket

    //! Write \a size bytes on the socket. Every call site knows the length
    //! already, and letting QIODevice recover it means a strlen over a
    //! ~700 KB article body on each send.
    inline void write(const char *aBuffer, qint64 size);

    inline void resetErrorCount();
    inline bool isConnected() const;

    //! Only meaningful once disconnected() was emitted: the connection is then
    //! idle until the next startConnection.
    inline bool authenticationRejected() const;
    //! Whether the connection reached the posting state since the previous
    //! call. Same idle-only contract as authenticationRejected().
    inline bool takeBecameReady();

    void setPoster(Poster *poster);

    inline bool hasNoMoreFiles() const;

    static QString sslSupportInfo();
    static bool supportsSsl();

signals:
    void startConnection();
    void killConnection();

    //    void error(QTcpSocket::SocketError socketerror); //!< Socket Error
    void socketError(QString aError); //!< Error during socket creation (ssl or not)
    void errorConnecting(QString aError);
    void disconnected(NntpConnection *con);
    void retryingConnection(QString server, QString detail);
    void log(QString msg, bool newline = true) const;
    void error(QString msg) const;

public slots:
    void onStartConnection();
    void onKillConnection();

    void onConnected();
    void onEncrypted();

    void onDisconnected(); //!< Handle disconnection

    void onReadyRead();                               //!< To be overridden in Child class
    void onSslErrors(const QList<QSslError> &errors); //!< SSL errors handler
    void onErrors(QAbstractSocket::SocketError);      //!< Socket errors handler

#ifdef __USE_CONNECTION_TIMEOUT__
    void onTimeout();
#endif

private:
    inline void _log(const QString &aMsg) const;       //!< log function for QString
    inline void _log(const char *aMsg) const;          //!< log function for char *
    inline void _log(const std::string &aMsg) const;   //!< log function for std::string
    inline void _error(const QString &aMsg) const;     //!< log function for QString
    inline void _error(const char *aMsg) const;        //!< log function for char *
    inline void _error(const std::string &aMsg) const; //!< log function for std::string

    void _handlePostResponse(QByteArray &line);
    void _handleArticleResponse(QByteArray &line);
    bool _handleWelcome(QByteArray &line);
    bool _handleAuthUser(QByteArray &line);
    void _handleAuthPass(QByteArray &line);

    void _sendNextArticle();
    void _closeConnection(bool dropTransport = false);
    void _detachSocketSignals();
    void _shutdownSocket();
    //! A transport close while an article is awaiting a definitive NNTP
    //! response is ambiguous: the server may already have accepted it. Keep
    //! the article resumable independently of VPN state and NO_RESUME_AUTO.
    void _preserveCurrentArticleAfterTransportLoss(QString const &reason);

    inline void deleteSocket();
};

int NntpConnection::getId() const
{
    return _conId;
}

void NntpConnection::write(const QByteArray &aBuffer)
{
    _socket->write(aBuffer);
}
void NntpConnection::write(const char *aBuffer, qint64 size)
{
    _socket->write(aBuffer, size);
}

void NntpConnection::resetErrorCount()
{
    _nbDisconnected = 0;
}
bool NntpConnection::isConnected() const
{
    return _isConnected;
}

bool NntpConnection::authenticationRejected() const
{
    return _authRejected;
}

bool NntpConnection::takeBecameReady()
{
    bool const ready = _becameReady;
    _becameReady = false;
    return ready;
}

bool NntpConnection::hasNoMoreFiles() const
{
    return _postingState == PostingState::NO_MORE_FILES;
}

void NntpConnection::_log(const char *aMsg) const
{
    emit log(QString("[%1] %2").arg(_logPrefix).arg(aMsg));
}
void NntpConnection::_log(const QString &aMsg) const
{
    emit log(QString("[%1] %2").arg(_logPrefix).arg(aMsg));
}
void NntpConnection::_log(const std::string &aMsg) const
{
    emit log(QString("[%1] %2").arg(_logPrefix).arg(QString::fromStdString(aMsg)));
}

void NntpConnection::_error(const char *aMsg) const
{
    emit error(QString("[%1] %2").arg(_logPrefix).arg(aMsg));
}
void NntpConnection::_error(const QString &aMsg) const
{
    emit error(QString("[%1] %2").arg(_logPrefix).arg(aMsg));
}
void NntpConnection::_error(const std::string &aMsg) const
{
    emit error(QString("[%1] %2").arg(_logPrefix).arg(QString::fromStdString(aMsg)));
}

void NntpConnection::deleteSocket()
{
    _isConnected = false;
    _socket->close();
    _socket->deleteLater();
    _socket = nullptr;
}

#endif // NNTPCONNECTION_H
