//========================================================================
//
// tests/common/MockDnsServer.h — QProcess wrapper around the Python TCP
// DNS mock at tests/fixtures/mock_dns/tcp_dns.py.
//
//========================================================================

#ifndef TESTS_MOCK_DNS_SERVER_H
#define TESTS_MOCK_DNS_SERVER_H

#include <QHash>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace ngpost::tests
{

class MockDnsServer
{
public:
    MockDnsServer();
    ~MockDnsServer();

    //! `answers` maps lower-cased hostnames to IPv4 strings. Pass extra CLI
    //! flags via `extraArgs` (eg. `--truncate-after 1`).
    bool start(const QHash<QString, QString> &answers, const QStringList &extraArgs = {});

    void stop();

    quint16 port() const { return _port; }
    QString logFile() const { return _root.path() + QStringLiteral("/server.log"); }

private:
    QTemporaryDir _root;
    QProcess      _proc;
    quint16       _port;
};

} // namespace ngpost::tests

#endif // TESTS_MOCK_DNS_SERVER_H
