// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#include "ExternalToolResolver.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace externaltool
{
QString modeName(PathMode mode)
{
    return mode == PathMode::Custom ? QStringLiteral("custom") : QStringLiteral("auto");
}
bool parseMode(const QString &text, PathMode &mode)
{
    if (text.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0)
        mode = PathMode::Automatic;
    else if (text.compare(QLatin1String("custom"), Qt::CaseInsensitive) == 0)
        mode = PathMode::Custom;
    else
        return false;
    return true;
}
bool executable(const QString &path)
{
    const QFileInfo file(path);
    return !path.isEmpty() && file.isFile() && file.isExecutable();
}
QString toolForFile(const QString &path)
{
    QString name = QFileInfo(path).fileName().toLower();
    if (name.endsWith(QLatin1String(".exe")))
        name.chop(4);
    if (name == QLatin1String("parpar"))
        return QStringLiteral("parpar");
    if (name == QLatin1String("par2"))
        return QStringLiteral("par2cmdline");
    if (name == QLatin1String("par2j") || name == QLatin1String("par2j64"))
        return QStringLiteral("multipar");
    if (name == QLatin1String("rar"))
        return QStringLiteral("rar");
    if (name == QLatin1String("7z") || name == QLatin1String("7za") || name == QLatin1String("7zz"))
        return QStringLiteral("7zip");
    return {};
}
QString archiverForFile(const QString &path)
{
    // Looser than toolForFile(): 7-Zip ships under several names, and "7z"
    // anywhere in the file name is how ngPost has always recognised it.
    if (QFileInfo(path).fileName().contains(QLatin1String("7z"), Qt::CaseInsensitive))
        return QStringLiteral("7zip");
    return toolForFile(path) == QLatin1String("rar") ? QStringLiteral("rar") : QString();
}
static QStringList names(const QString &tool)
{
    QStringList result;
    if (tool == QLatin1String("parpar"))
        result << QStringLiteral("parpar");
    else if (tool == QLatin1String("par2cmdline"))
        result << QStringLiteral("par2");
    else if (tool == QLatin1String("rar"))
        result << QStringLiteral("rar");
    else if (tool == QLatin1String("7zip"))
        result << QStringLiteral("7zz") << QStringLiteral("7z") << QStringLiteral("7za");
#ifdef Q_OS_WIN
    else if (tool == QLatin1String("multipar"))
        result << QStringLiteral("par2j64") << QStringLiteral("par2j");
    for (QString &name : result)
        name += QStringLiteral(".exe");
#endif
    return result;
}
Resolved resolve(const QString &tool,
                 PathMode mode,
                 const QString &customPath,
                 const QString &appDir,
                 const QStringList &systemPaths)
{
    if (mode == PathMode::Custom)
        return { tool, customPath, executable(customPath) ? Origin::Custom : Origin::Missing };
    const QStringList kinds = tool == QLatin1String("auto")
        ? QStringList{ QStringLiteral("parpar"),
                       QStringLiteral("par2cmdline"),
                       QStringLiteral("multipar") }
        : QStringList{ tool };
    // Prefer the bundle as a whole before looking at any system installation.
    for (const QString &kind : kinds)
        for (const QString &name : names(kind)) {
            const QString path = QDir(appDir).filePath(name);
            if (executable(path))
                return { kind, path, Origin::Bundled };
        }
    for (const QString &kind : kinds)
        for (const QString &name : names(kind)) {
            // Explicit directories, rather than an empty findExecutable list
            // (which Qt interprets as the real PATH).
            for (const QString &dir : systemPaths) {
                const QString path = QDir(dir).filePath(name);
                if (executable(path))
                    return { kind, QFileInfo(path).absoluteFilePath(), Origin::System };
            }
        }
    return { tool, {}, Origin::Missing };
}
Resolved resolve(const QString &tool, PathMode mode, const QString &customPath)
{
    QStringList paths = qEnvironmentVariable("PATH").split(QDir::listSeparator(),
                                                           Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    for (const char *variable : { "PROGRAMFILES", "PROGRAMFILES(X86)" }) {
        const QString root = qEnvironmentVariable(variable);
        if (root.isEmpty())
            continue;
        paths << QDir(root).filePath(QStringLiteral("WinRAR"))
              << QDir(root).filePath(QStringLiteral("7-Zip"))
              << QDir(root).filePath(QStringLiteral("QuickPar"))
              << QDir(root).filePath(QStringLiteral("MultiPar"));
    }
#endif
    return resolve(tool, mode, customPath, QCoreApplication::applicationDirPath(), paths);
}
QString bundledTool(const QString &path, const QString &appDir)
{
    if (!QDir::isAbsolutePath(path))
        return {};
    QString tool = toolForFile(path);
    if (tool.isEmpty())
        return {};
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString parent = QFileInfo(clean).absolutePath();
#ifdef Q_OS_WIN
    const auto sensitivity = Qt::CaseInsensitive;
#else
    const auto sensitivity = Qt::CaseSensitive;
#endif
    if (parent.compare(QDir::cleanPath(appDir), sensitivity) == 0)
        return tool;
#ifdef Q_OS_LINUX
    static const QRegularExpression mount(QStringLiteral(
                                              "^/(?:[^/]+/)*\\.mount_ngpost[^/]+/usr/bin/[^/]+$"),
                                          QRegularExpression::CaseInsensitiveOption);
    if (mount.match(clean).hasMatch())
        return tool;
#endif
    return {};
}
QString bundledTool(const QString &path)
{
    return bundledTool(path, QCoreApplication::applicationDirPath());
}
}
