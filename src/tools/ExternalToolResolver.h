// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef EXTERNALTOOLRESOLVER_H
#define EXTERNALTOOLRESOLVER_H
#include <QStringList>

namespace externaltool
{
enum class PathMode {
    Automatic,
    Custom
};
enum class Origin {
    Missing,
    Bundled,
    System,
    Custom
};
struct Resolved
{
    QString tool;
    QString path;
    Origin origin = Origin::Missing;
    bool available() const { return origin != Origin::Missing; }
};
QString modeName(PathMode mode);
bool parseMode(const QString &text, PathMode &mode);
bool executable(const QString &path);
QString toolForFile(const QString &path);
// The archiver an executable runs, from its file name: "7zip" when the name
// contains 7z (7z, 7za, 7zz, 7zr...), "rar" for rar, empty when it says neither.
QString archiverForFile(const QString &path);
// The overload with explicit search directories also lets tests simulate moving
// a bundle without touching the real application directory or system PATH.
Resolved resolve(const QString &tool,
                 PathMode mode,
                 const QString &customPath,
                 const QString &appDir,
                 const QStringList &systemPaths);
Resolved resolve(const QString &tool,
                 PathMode mode = PathMode::Automatic,
                 const QString &customPath = {});
// Returns the known tool identity only for a current bundle path or an old
// ngPost AppImage mount. An arbitrary missing custom path is never migrated.
QString bundledTool(const QString &path, const QString &appDir);
QString bundledTool(const QString &path);
}
#endif
