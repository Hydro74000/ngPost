// Differential probe: build against the pre-refactor and current source trees.
#include "NgPost.h"
#include "utils/PathHelper.h"

#include <QFile>
#include <QTextStream>

int main(int argc, char *argv[])
{
    // Both versions must run in the same disposable configuration directory.
    // An absent test override must never fall through to the user's profile.
    if (argc != 3 || qEnvironmentVariable("NGPOST_TEST_CONFIG_DIR").isEmpty())
        return 2;
    const QString input = QString::fromLocal8Bit(argv[1]);
    const QString output = QString::fromLocal8Bit(argv[2]);
    const QString config = PathHelper::configFilePath();
    if (QFile::exists(config) && !QFile::remove(config))
        return 3;
    if (!QFile::copy(input, config))
        return 4;
    NgPost app(argc, argv);
    const QString error = app.parseDefaultConfig();
    // Shipped templates can reference unavailable paths or tools. Keep their
    // diagnostics as part of the comparison, then exercise the full writer.
    QFile diagnostics(output + QStringLiteral(".errors"));
    if (!diagnostics.open(QIODevice::WriteOnly))
        return 5;
    if (diagnostics.write(error.toUtf8()) != error.toUtf8().size())
        return 6;
    app.saveConfig();
    QFile saved(config);
    QFile snapshot(output);
    if (!saved.open(QIODevice::ReadOnly) || !snapshot.open(QIODevice::WriteOnly))
        return 7;
    const QByteArray bytes = saved.readAll();
    return snapshot.write(bytes) == bytes.size() ? 0 : 8;
}
