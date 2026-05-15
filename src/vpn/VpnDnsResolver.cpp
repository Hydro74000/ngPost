//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnDnsResolver.h"

#include "VpnSocketBinder.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>

namespace {

struct CacheEntry
{
    QList<QHostAddress> addresses;
    qint64 expiresAtMs = 0;
};

QMutex sCacheMutex;
QHash<QString, CacheEntry> sCache;

void setErr(QString *errMsg, QString const &msg)
{
    if (errMsg)
        *errMsg = msg;
}

quint16 readU16(QByteArray const &buf, int pos)
{
    return (quint16(quint8(buf.at(pos))) << 8)
         |  quint16(quint8(buf.at(pos + 1)));
}

quint32 readU32(QByteArray const &buf, int pos)
{
    return (quint32(readU16(buf, pos)) << 16) | readU16(buf, pos + 2);
}

bool appendQName(QByteArray *out, QString const &host, QString *errMsg)
{
    QByteArray ace = QUrl::toAce(host.endsWith('.') ? host.left(host.size() - 1) : host);
    if (ace.isEmpty()) {
        setErr(errMsg, QStringLiteral("empty DNS name"));
        return false;
    }

    const QList<QByteArray> labels = ace.split('.');
    for (QByteArray const &label : labels) {
        if (label.isEmpty() || label.size() > 63) {
            setErr(errMsg, QStringLiteral("invalid DNS label in %1").arg(host));
            return false;
        }
        out->append(char(label.size()));
        out->append(label);
    }
    out->append(char(0));
    return true;
}

bool skipName(QByteArray const &buf, int *pos)
{
    int p = *pos;
    int jumps = 0;
    while (p < buf.size()) {
        quint8 len = quint8(buf.at(p));
        if ((len & 0xc0) == 0xc0) {
            if (p + 1 >= buf.size())
                return false;
            p += 2;
            *pos = p;
            return ++jumps < 16;
        }
        if ((len & 0xc0) != 0)
            return false;
        ++p;
        if (len == 0) {
            *pos = p;
            return true;
        }
        if (p + len > buf.size())
            return false;
        p += len;
    }
    return false;
}

QByteArray buildQuery(QString const &host, quint16 id, QString *errMsg)
{
    QByteArray msg;
    auto appendU16 = [&msg](quint16 v) {
        msg.append(char((v >> 8) & 0xff));
        msg.append(char(v & 0xff));
    };

    appendU16(id);
    appendU16(0x0100); // recursion desired
    appendU16(1);      // QDCOUNT
    appendU16(0);      // ANCOUNT
    appendU16(0);      // NSCOUNT
    appendU16(0);      // ARCOUNT

    if (!appendQName(&msg, host, errMsg))
        return QByteArray();
    appendU16(1); // A
    appendU16(1); // IN
    return msg;
}

bool waitForBytes(QTcpSocket *socket, QByteArray *buf, int bytes, QElapsedTimer *timer, int timeoutMs)
{
    while (buf->size() < bytes) {
        int left = timeoutMs - int(timer->elapsed());
        if (left <= 0 || !socket->waitForReadyRead(left))
            return false;
        buf->append(socket->readAll());
    }
    return true;
}

QList<QHostAddress> parseResponse(QByteArray const &msg, quint16 expectedId,
                                  int *minTtl, QString *errMsg)
{
    QList<QHostAddress> addresses;
    *minTtl = 60;

    if (msg.size() < 12) {
        setErr(errMsg, QStringLiteral("short DNS response"));
        return addresses;
    }
    if (readU16(msg, 0) != expectedId) {
        setErr(errMsg, QStringLiteral("mismatched DNS response id"));
        return addresses;
    }

    quint16 flags = readU16(msg, 2);
    if ((flags & 0x8000) == 0) {
        setErr(errMsg, QStringLiteral("DNS response missing QR bit"));
        return addresses;
    }
    int rcode = flags & 0x000f;
    if (rcode != 0) {
        setErr(errMsg, QStringLiteral("DNS server returned RCODE %1").arg(rcode));
        return addresses;
    }

    int qd = readU16(msg, 4);
    int an = readU16(msg, 6);
    int pos = 12;
    for (int i = 0; i < qd; ++i) {
        if (!skipName(msg, &pos) || pos + 4 > msg.size()) {
            setErr(errMsg, QStringLiteral("malformed DNS question"));
            return addresses;
        }
        pos += 4;
    }

    int ttl = 86400;
    for (int i = 0; i < an; ++i) {
        if (!skipName(msg, &pos) || pos + 10 > msg.size()) {
            setErr(errMsg, QStringLiteral("malformed DNS answer"));
            return addresses;
        }
        quint16 type = readU16(msg, pos); pos += 2;
        quint16 klass = readU16(msg, pos); pos += 2;
        quint32 recordTtl = readU32(msg, pos); pos += 4;
        quint16 rdLen = readU16(msg, pos); pos += 2;
        if (pos + rdLen > msg.size()) {
            setErr(errMsg, QStringLiteral("truncated DNS answer"));
            return addresses;
        }
        if (type == 1 && klass == 1 && rdLen == 4) {
            quint32 ipv4 = (quint32(quint8(msg.at(pos))) << 24)
                         | (quint32(quint8(msg.at(pos + 1))) << 16)
                         | (quint32(quint8(msg.at(pos + 2))) << 8)
                         |  quint32(quint8(msg.at(pos + 3)));
            addresses << QHostAddress(ipv4);
            ttl = qMin(ttl, int(recordTtl));
        }
        pos += rdLen;
    }

    if (addresses.isEmpty())
        setErr(errMsg, QStringLiteral("no A records returned"));
    else
        *minTtl = qBound(10, ttl, 300);
    return addresses;
}

} // namespace

QList<QHostAddress> VpnDnsResolver::resolveA(QString const &host,
                                            QHostAddress const &dnsServer,
                                            QHostAddress const &sourceIp,
                                            QString *errMsg,
                                            int timeoutMs)
{
    if (errMsg)
        errMsg->clear();

    QString const key = host.toLower() + QLatin1Char('|')
                      + dnsServer.toString() + QLatin1Char('|')
                      + sourceIp.toString();
    qint64 const now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker locker(&sCacheMutex);
        auto cached = sCache.constFind(key);
        if (cached != sCache.constEnd() && cached->expiresAtMs > now)
            return cached->addresses;
    }

    if (dnsServer.isNull()) {
        setErr(errMsg, QStringLiteral("VPN DNS server is not set"));
        return {};
    }
    if (sourceIp.isNull()) {
        setErr(errMsg, QStringLiteral("VPN source IP is not set"));
        return {};
    }

    quint16 id = quint16(QRandomGenerator::global()->generate() & 0xffff);
    QString buildErr;
    QByteArray query = buildQuery(host, id, &buildErr);
    if (query.isEmpty()) {
        setErr(errMsg, buildErr);
        return {};
    }

    QTcpSocket socket;
    QString bindErr;
    if (!VpnSocketBinder::bind(&socket, sourceIp, &bindErr)) {
        setErr(errMsg, QStringLiteral("DNS socket bind failed on %1: %2")
                           .arg(sourceIp.toString(), bindErr));
        return {};
    }

    QElapsedTimer timer;
    timer.start();
    socket.connectToHost(dnsServer, 53);
    if (!socket.waitForConnected(timeoutMs)) {
        setErr(errMsg, QStringLiteral("DNS TCP connect to %1 failed: %2")
                           .arg(dnsServer.toString(), socket.errorString()));
        return {};
    }

    QByteArray tcpQuery;
    tcpQuery.append(char((query.size() >> 8) & 0xff));
    tcpQuery.append(char(query.size() & 0xff));
    tcpQuery.append(query);

    if (socket.write(tcpQuery) != tcpQuery.size() || !socket.waitForBytesWritten(timeoutMs)) {
        setErr(errMsg, QStringLiteral("DNS TCP write to %1 failed: %2")
                           .arg(dnsServer.toString(), socket.errorString()));
        return {};
    }

    QByteArray in;
    if (!waitForBytes(&socket, &in, 2, &timer, timeoutMs)) {
        setErr(errMsg, QStringLiteral("DNS TCP response from %1 timed out")
                           .arg(dnsServer.toString()));
        return {};
    }
    int msgLen = readU16(in, 0);
    if (!waitForBytes(&socket, &in, 2 + msgLen, &timer, timeoutMs)) {
        setErr(errMsg, QStringLiteral("DNS TCP response from %1 was truncated")
                           .arg(dnsServer.toString()));
        return {};
    }

    int ttl = 60;
    QString parseErr;
    QList<QHostAddress> addresses = parseResponse(in.mid(2, msgLen), id, &ttl, &parseErr);
    if (addresses.isEmpty()) {
        setErr(errMsg, parseErr);
        return {};
    }

    {
        QMutexLocker locker(&sCacheMutex);
        sCache.insert(key, CacheEntry{addresses,
                                      QDateTime::currentMSecsSinceEpoch()
                                      + qint64(ttl) * 1000});
    }
    return addresses;
}
