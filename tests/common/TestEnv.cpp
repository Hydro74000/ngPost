//========================================================================
//
// tests/common/TestEnv.cpp — implementation.
//
//========================================================================

#include "TestEnv.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTcpServer>

namespace ngpost::tests
{

namespace
{

void setEnv(const char *key, const QByteArray &value)
{
    qputenv(key, value);
}

QString getEnv(const char *key, bool &had)
{
    had = qEnvironmentVariableIsSet(key);
    return had ? QString::fromLocal8Bit(qgetenv(key)) : QString();
}

} // namespace

HomeSandbox::HomeSandbox()
    : _root()
    , _hadHome(false)
    , _hadXdg(false)
    , _hadAppData(false)
    , _hadUserProfile(false)
    , _hadTestHome(false)
    , _hadTestConfigDir(false)
{
    Q_ASSERT(_root.isValid());

    _prevHome        = getEnv("HOME", _hadHome);
    _prevXdg         = getEnv("XDG_CONFIG_HOME", _hadXdg);
    _prevAppData     = getEnv("APPDATA", _hadAppData);
    _prevUserProfile = getEnv("USERPROFILE", _hadUserProfile);
    _prevTestHome      = getEnv("NGPOST_TEST_HOME", _hadTestHome);
    _prevTestConfigDir = getEnv("NGPOST_TEST_CONFIG_DIR", _hadTestConfigDir);

    QDir().mkpath(xdgConfigHome());
    const QString testConfigDir = xdgConfigHome() + QStringLiteral("/ngPost");
    QDir().mkpath(testConfigDir);

    setEnv("HOME", rootPath().toLocal8Bit());
    setEnv("XDG_CONFIG_HOME", xdgConfigHome().toLocal8Bit());
    setEnv("APPDATA", rootPath().toLocal8Bit());
    setEnv("USERPROFILE", rootPath().toLocal8Bit());
    setEnv("NGPOST_TEST_HOME", rootPath().toLocal8Bit());
    setEnv("NGPOST_TEST_CONFIG_DIR", testConfigDir.toLocal8Bit());
}

HomeSandbox::~HomeSandbox()
{
    auto restore = [](const char *key, bool had, const QString &prev) {
        if (had)
            qputenv(key, prev.toLocal8Bit());
        else
            qunsetenv(key);
    };
    restore("HOME", _hadHome, _prevHome);
    restore("XDG_CONFIG_HOME", _hadXdg, _prevXdg);
    restore("APPDATA", _hadAppData, _prevAppData);
    restore("USERPROFILE", _hadUserProfile, _prevUserProfile);
    restore("NGPOST_TEST_HOME", _hadTestHome, _prevTestHome);
    restore("NGPOST_TEST_CONFIG_DIR", _hadTestConfigDir, _prevTestConfigDir);
}

QString HomeSandbox::rootPath() const
{
    return _root.path();
}

QString HomeSandbox::xdgConfigHome() const
{
    return rootPath() + QStringLiteral("/.config");
}

quint16 allocLoopbackPort()
{
    QTcpServer srv;
    if (!srv.listen(QHostAddress::LocalHost, 0))
        return 0;
    quint16 p = srv.serverPort();
    srv.close();
    return p;
}

QString locateNgPostBinary()
{
    const QByteArray env = qgetenv("NGPOST_BIN");
    if (!env.isEmpty()) {
        QFileInfo fi(QString::fromLocal8Bit(env));
        if (fi.isExecutable())
            return fi.absoluteFilePath();
    }

    // Walk up from the test binary location to find the repo root, then look
    // for the built ngPost under src/. The test binary lives at
    // <repo>/tests/<category>/tst_<name>/tst_<name>; QCoreApplication exists
    // because every test that needs this helper uses QTEST_MAIN.
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        const QStringList candidates = {
            d.absoluteFilePath(QStringLiteral("src/ngPost")),
            d.absoluteFilePath(QStringLiteral("src/ngPost.app/Contents/MacOS/ngPost")),
            d.absoluteFilePath(QStringLiteral("src/release/ngPost.exe")),
        };
        for (const QString &c : candidates) {
            if (QFileInfo(c).isExecutable())
                return c;
        }
        if (!d.cdUp())
            break;
    }
    return QString();
}

} // namespace ngpost::tests
