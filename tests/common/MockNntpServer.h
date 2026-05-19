//========================================================================
//
// tests/common/MockNntpServer.h — QProcess wrapper around the Python mock
// NNTP server defined in tests/fixtures/mock_nntp/server.py.
//
//========================================================================

#ifndef TESTS_MOCK_NNTP_SERVER_H
#define TESTS_MOCK_NNTP_SERVER_H

#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace ngpost::tests
{

//! Drives the Python mock NNTP server as a child process. The lifecycle is
//! tied to this object: destructor sends SIGTERM (waitForFinished 2s) so
//! tests don't leak listening sockets across runs.
class MockNntpServer
{
public:
    explicit MockNntpServer();
    ~MockNntpServer();

    //! Launch the server, return true on success. `extraArgs` is appended
    //! after the standard `--port 0 --port-file ... --dump-dir ... --log-file ...`
    //! flags, so tests can stack failure-injection knobs (e.g. `--fail-auth`).
    //!
    //! When `withTls` is true, the server also listens on an ephemeral TLS
    //! port and `sslPort()` returns it. The bundled self-signed cert at
    //! tests/fixtures/mock_nntp/certs/ is used.
    bool start(const QStringList &extraArgs = {}, bool withTls = false);

    //! Best-effort stop. Called automatically by ~MockNntpServer.
    void stop();

    //! Ephemeral TCP port chosen by the server (plain). 0 if not running.
    quint16 port() const { return _port; }

    //! Ephemeral TLS port chosen by the server (only set when start(true)).
    quint16 sslPort() const { return _sslPort; }

    //! Directory where the server writes one .eml file per accepted article.
    QString dumpDir() const { return _root.path() + QStringLiteral("/dump"); }

    //! Append-only log of accepted connections + commands. Tests grep this to
    //! verify peer IPs (Phase 5 VPN E2E) and command sequences.
    QString logFile() const { return _root.path() + QStringLiteral("/server.log"); }

    //! List of *.eml filenames currently sitting under dumpDir().
    QStringList receivedArticles() const;

    //! Convenience: full content of `<dumpDir>/<basename>.eml`.
    QByteArray readArticle(const QString &basename) const;

private:
    QTemporaryDir _root;
    QProcess      _proc;
    quint16       _port;
    quint16       _sslPort;
};

} // namespace ngpost::tests

#endif // TESTS_MOCK_NNTP_SERVER_H
