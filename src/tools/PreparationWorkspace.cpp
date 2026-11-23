// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Folders of a prepared set and the proof that they are ours.
//
//========================================================================

#include "tools/PreparationWorkspace.h"

#include "utils/NativeFileIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{

void fail(QString *error, const QString &reason)
{
    if (error)
        *error = reason;
}

Qt::CaseSensitivity pathCase()
{
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString markerContent(const QString &setId)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{ { QStringLiteral("ngPostArtifactSet"), setId } })
            .toJson(QJsonDocument::Compact));
}

//! Deletes @p path and what it holds without ever following a link: a
//! symbolic link or a Windows junction is removed itself, its target never
//! visited. (QDir::removeRecursively() descends into junctions.)
bool removeWithoutFollowing(const QString &path)
{
    const auto id = NativeFileIdentity::of(path);
    if (!id)
        return true; // already gone
    if (id->link) {
#ifdef Q_OS_WIN
        // A junction or a folder link is a folder entry: removed as one.
        return id->directory ? QDir().rmdir(path) : QFile::remove(path);
#else
        return QFile::remove(path);
#endif
    }
    if (!id->directory)
        return QFile::remove(path);
    const QStringList entries = QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                     | QDir::NoDotAndDotDot);
    for (const QString &entry : entries)
        if (!removeWithoutFollowing(path + QLatin1Char('/') + entry))
            return false;
    return QDir().rmdir(path);
}

//! Not a link, and still where it was recorded once links are resolved.
bool plainFolder(const QString &path)
{
    const auto id = NativeFileIdentity::of(path);
    return id && id->directory && !id->link
        && QFileInfo(path).canonicalFilePath().compare(QDir::cleanPath(path), pathCase()) == 0;
}

} // namespace

std::optional<PreparationWorkspace> PreparationWorkspace::plan(const QString &root,
                                                               qint64 postId,
                                                               int generation,
                                                               QString *error)
{
    const QFileInfo info(root);
    if (root.isEmpty() || !info.isAbsolute() || !info.isDir() || postId <= 0 || generation <= 0) {
        fail(error, QStringLiteral("workspace_root: %1").arg(root));
        return std::nullopt;
    }
    PreparationWorkspace w;
    // Recorded resolved: a root reached through a link (/tmp on macOS) is
    // compared with what the links lead to.
    w.root = info.canonicalFilePath();
    w.setDir = QStringLiteral("%1/ngpost-%2-g%3").arg(w.root).arg(postId).arg(generation);
    w.archiveDir = w.setDir + QStringLiteral("/archives");
    w.stagingDir = w.setDir + QStringLiteral("/staging");
    w.markerPath = w.setDir + QStringLiteral("/.ngpost-set");
    return w;
}

bool PreparationWorkspace::containsStrictly(const QString &path) const
{
    if (root.isEmpty() || path.isEmpty())
        return false;
    // "link/.." names the parent of the link's target, not of the link: a
    // path that climbs is never taken for ours. Checked as given, before
    // anything (QFileInfo included) folds it away.
    if (QDir::fromNativeSeparators(path).split(QLatin1Char('/')).contains(QStringLiteral("..")))
        return false;
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (!clean.startsWith(prefix, pathCase()) || clean.size() <= prefix.size())
        return false;
    // A link (or a Windows junction) anywhere below the root may lead out of
    // it: never followed, the path is not ours. What does not exist yet
    // cannot be one.
    QString walked = root;
    for (const QString &part : clean.mid(prefix.size()).split(QLatin1Char('/'))) {
        walked += QLatin1Char('/') + part;
        const auto id = NativeFileIdentity::of(walked);
        if (!id)
            break;
        if (id->link)
            return false;
    }
    return true;
}

bool PreparationWorkspace::create(const QString &setId,
                                  NativeFileIdentity *marker,
                                  QString *error) const
{
    if (setId.isEmpty() || !containsStrictly(setDir) || !plainFolder(root)) {
        fail(error, QStringLiteral("workspace_root: %1").arg(root));
        return false;
    }
    // Whatever is already there may be someone's: never adopted.
    if (NativeFileIdentity::of(setDir)) {
        fail(error, QStringLiteral("workspace_collision: %1").arg(setDir));
        return false;
    }
    QDir parent(root);
    if (!parent.mkdir(setDir) || !parent.mkdir(archiveDir) || !parent.mkdir(stagingDir)) {
        fail(error, QStringLiteral("workspace_create: %1").arg(setDir));
        return false;
    }
    QFile file(markerPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write(markerContent(setId).toUtf8()) < 0 || !file.flush()) {
        fail(error, QStringLiteral("workspace_marker: %1").arg(file.errorString()));
        return false;
    }
    file.close();
    const auto identity = NativeFileIdentity::of(markerPath);
    if (!identity) {
        fail(error, QStringLiteral("workspace_marker: %1").arg(markerPath));
        return false;
    }
    if (marker)
        *marker = *identity;
    return true;
}

PreparationWorkspace::Status PreparationWorkspace::verify(const QString &setId,
                                                          const QString &markerIdentityJson,
                                                          QString *why) const
{
    if (!NativeFileIdentity::of(setDir)) {
        fail(why, QStringLiteral("workspace_missing: %1").arg(setDir));
        return Status::Missing;
    }
    if (!containsStrictly(setDir) || !plainFolder(setDir)) {
        fail(why, QStringLiteral("workspace_outside: %1").arg(setDir));
        return Status::Ambiguous;
    }
    for (const QString &sub : { archiveDir, stagingDir }) {
        if (NativeFileIdentity::of(sub) && !plainFolder(sub)) {
            fail(why, QStringLiteral("workspace_link: %1").arg(sub));
            return Status::Ambiguous;
        }
    }
    const auto recorded = NativeFileIdentity::fromJson(markerIdentityJson);
    const auto present = NativeFileIdentity::of(markerPath);
    if (!recorded || !present || present->link || !present->sameFile(*recorded)) {
        fail(why, QStringLiteral("workspace_marker_replaced: %1").arg(markerPath));
        return Status::Ambiguous;
    }
    QFile file(markerPath);
    if (!file.open(QIODevice::ReadOnly)
        || QString::fromUtf8(file.readAll()) != markerContent(setId)) {
        fail(why, QStringLiteral("workspace_marker_content: %1").arg(markerPath));
        return Status::Ambiguous;
    }
    return Status::Ours;
}

bool PreparationWorkspace::removeArchives(const QString &setId,
                                          const QString &markerIdentityJson,
                                          bool retained,
                                          QString *error) const
{
    if (retained) {
        fail(error, QStringLiteral("retained_by_keep_rar: %1").arg(setDir));
        return false;
    }
    QString why;
    switch (verify(setId, markerIdentityJson, &why)) {
    case Status::Missing: return true; // nothing left to remove
    case Status::Ambiguous: fail(error, why); return false;
    case Status::Ours: break;
    }
    // Our own folder: its links are removed, never followed.
    if (!removeWithoutFollowing(archiveDir)) {
        fail(error, QStringLiteral("workspace_remove: %1").arg(archiveDir));
        return false;
    }
    QDir set(setDir);
    // Sources may still be in the staging folder: it goes only when empty.
    if (NativeFileIdentity::of(stagingDir) && !set.rmdir(stagingDir))
        return true;
    // The marker last, and only from a folder holding nothing else: a folder
    // without it is never ours again.
    const QStringList left = set.entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                           | QDir::NoDotAndDotDot);
    if (left != QStringList{ QStringLiteral(".ngpost-set") })
        return true;
    if (!QFile::remove(markerPath) || !QDir(root).rmdir(setDir)) {
        fail(error, QStringLiteral("workspace_remove: %1").arg(setDir));
        return false;
    }
    return true;
}
