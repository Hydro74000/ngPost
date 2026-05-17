//========================================================================
//
// tests/common/MockNntpServer.cpp — implementation.
//
//========================================================================

#include "MockNntpServer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

namespace ngpost::tests
{

namespace
{

//! Absolute path to the bundled server.py. NGPOST_TESTS_ROOT is injected at
//! compile time by tests/common/common.pri so this works regardless of the
//! caller's working directory.
QString serverScriptPath()
{
    return QString::fromLatin1(NGPOST_TESTS_ROOT)
           + QStringLiteral("/fixtures/mock_nntp/server.py");
}

QString resolvePython()
{
    // Honor NGPOST_TEST_PYTHON for CI runners that ship a virtualenv.
    const QByteArray env = qgetenv("NGPOST_TEST_PYTHON");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);

    for (const char *cand : { "python3", "python" }) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(cand));
        if (!p.isEmpty())
            return p;
    }
    return QStringLiteral("python3");
}

} // namespace

MockNntpServer::MockNntpServer()
    : _root()
    , _proc()
    , _port(0)
    , _sslPort(0)
{
}

MockNntpServer::~MockNntpServer() { stop(); }

bool MockNntpServer::start(const QStringList &extraArgs, bool withTls)
{
    if (!_root.isValid())
        return false;

    QDir().mkpath(dumpDir());

    const QString portFile = _root.path() + QStringLiteral("/port");
    const QString sslPortFile = _root.path() + QStringLiteral("/ssl-port");

    QStringList args = {
        serverScriptPath(),
        QStringLiteral("--listen"), QStringLiteral("127.0.0.1"),
        QStringLiteral("--port"), QStringLiteral("0"),
        QStringLiteral("--port-file"), portFile,
        QStringLiteral("--dump-dir"), dumpDir(),
        QStringLiteral("--log-file"), logFile(),
    };
    if (withTls) {
        const QString certsDir
            = QString::fromLatin1(NGPOST_TESTS_ROOT)
              + QStringLiteral("/fixtures/mock_nntp/certs");
        args += QStringList{
            QStringLiteral("--ssl-port"), QStringLiteral("0"),
            QStringLiteral("--ssl-port-file"), sslPortFile,
            QStringLiteral("--ssl-cert"), certsDir + QStringLiteral("/cert.pem"),
            QStringLiteral("--ssl-key"),  certsDir + QStringLiteral("/key.pem"),
        };
    }
    args += extraArgs;

    _proc.setProgram(resolvePython());
    _proc.setArguments(args);
    _proc.setProcessChannelMode(QProcess::MergedChannels);
    _proc.start();
    if (!_proc.waitForStarted(5000))
        return false;

    auto readPort = [](const QString &path, quint16 &out) {
        if (!QFile::exists(path))
            return false;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        bool ok = false;
        quint16 p = static_cast<quint16>(f.readAll().trimmed().toUInt(&ok));
        if (ok && p > 0) {
            out = p;
            return true;
        }
        return false;
    };

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000) {
        readPort(portFile, _port);
        if (withTls)
            readPort(sslPortFile, _sslPort);
        const bool ready = (_port != 0) && (!withTls || _sslPort != 0);
        if (ready)
            return true;
        if (_proc.state() != QProcess::Running)
            return false;
        QThread::msleep(50);
    }
    return false;
}

void MockNntpServer::stop()
{
    if (_proc.state() == QProcess::NotRunning)
        return;

    _proc.terminate();
    if (!_proc.waitForFinished(2000)) {
        _proc.kill();
        _proc.waitForFinished(1000);
    }
    _port = 0;
    _sslPort = 0;
}

QStringList MockNntpServer::receivedArticles() const
{
    QDir d(dumpDir());
    return d.entryList({ QStringLiteral("*.eml") }, QDir::Files, QDir::Name);
}

QByteArray MockNntpServer::readArticle(const QString &basename) const
{
    QFile f(QDir(dumpDir()).absoluteFilePath(basename));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace ngpost::tests
