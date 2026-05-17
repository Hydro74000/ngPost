//========================================================================
//
// tests/common/MockDnsServer.cpp — implementation.
//
//========================================================================

#include "MockDnsServer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QThread>

namespace ngpost::tests
{

namespace
{

QString dnsScriptPath()
{
    return QString::fromLatin1(NGPOST_TESTS_ROOT)
           + QStringLiteral("/fixtures/mock_dns/tcp_dns.py");
}

QString resolvePython()
{
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

MockDnsServer::MockDnsServer()
    : _root()
    , _proc()
    , _port(0)
{
}

MockDnsServer::~MockDnsServer() { stop(); }

bool MockDnsServer::start(const QHash<QString, QString> &answers, const QStringList &extraArgs)
{
    if (!_root.isValid())
        return false;

    const QString portFile = _root.path() + QStringLiteral("/port");

    QStringList args = {
        dnsScriptPath(),
        QStringLiteral("--listen"), QStringLiteral("127.0.0.1"),
        QStringLiteral("--port"), QStringLiteral("0"),
        QStringLiteral("--port-file"), portFile,
        QStringLiteral("--log-file"), logFile(),
    };
    for (auto it = answers.constBegin(); it != answers.constEnd(); ++it) {
        args << QStringLiteral("--answer")
             << (it.key() + QLatin1Char('=') + it.value());
    }
    args += extraArgs;

    _proc.setProgram(resolvePython());
    _proc.setArguments(args);
    _proc.setProcessChannelMode(QProcess::MergedChannels);
    _proc.start();
    if (!_proc.waitForStarted(5000))
        return false;

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000) {
        if (QFile::exists(portFile)) {
            QFile f(portFile);
            if (f.open(QIODevice::ReadOnly)) {
                bool ok = false;
                quint16 p = static_cast<quint16>(f.readAll().trimmed().toUInt(&ok));
                if (ok && p > 0) {
                    _port = p;
                    return true;
                }
            }
        }
        if (_proc.state() != QProcess::Running)
            return false;
        QThread::msleep(50);
    }
    return false;
}

void MockDnsServer::stop()
{
    if (_proc.state() == QProcess::NotRunning)
        return;
    _proc.terminate();
    if (!_proc.waitForFinished(2000)) {
        _proc.kill();
        _proc.waitForFinished(1000);
    }
    _port = 0;
}

} // namespace ngpost::tests
