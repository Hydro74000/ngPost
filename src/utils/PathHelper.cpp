//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "PathHelper.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThread>

#include <algorithm>

#if defined(Q_OS_UNIX)
#  include <cerrno>
#  include <cstring>
#  include <unistd.h>
#endif

namespace PathHelper
{

namespace
{

#ifdef NGPOST_TESTING
QString testEnvPath(const char *name)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? QString() : QString::fromLocal8Bit(value);
}
#endif

//! Root the ngPost directory hangs from — the *generic* location, which is
//! exactly the parent QStandardPaths uses for its "App" counterpart:
//!   Linux    $XDG_CONFIG_HOME (fallback ~/.config)     <- GenericConfigLocation
//!   Windows  C:/Users/<user>/AppData/Local             <- GenericConfigLocation
//!   macOS    ~/Library/Application Support             <- GenericDataLocation
//! On macOS this deliberately follows AppLocalDataLocation rather than
//! AppConfigLocation (~/Library/Preferences), because Apple's guidelines put
//! application data in "Application Support" and that is where every previous
//! ngPost wrote. Appending "ngPost" ourselves is what makes the result
//! independent of the program file name.
QString genericConfigRoot()
{
#ifdef NGPOST_TESTING
    // QStandardPaths uses different environment variables on each platform
    // (LOCALAPPDATA on Windows, HOME on macOS, XDG_CONFIG_HOME on Linux) and
    // may cache them. A single explicit root keeps migration tests inside the
    // HomeSandbox on every CI runner.
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/.config");
#endif
#if defined(Q_OS_MAC)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
}

bool samePath(const QString &a, const QString &b)
{
    QFileInfo ai(a);
    QFileInfo bi(b);
    const QString ac = ai.canonicalFilePath();
    const QString bc = bi.canonicalFilePath();
    const Qt::CaseSensitivity cs =
#if defined(Q_OS_WIN)
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    if (!ac.isEmpty() && !bc.isEmpty())
        return ac.compare(bc, cs) == 0;

    return QDir::cleanPath(ai.absoluteFilePath())
               .compare(QDir::cleanPath(bi.absoluteFilePath()), cs) == 0;
}

ConfigMigrationResult result(ConfigMigrationStatus status,
                             const QString &legacyPath,
                             const QString &newPath,
                             const QString &backupPath = QString(),
                             const QString &error = QString())
{
    ConfigMigrationResult r;
    r.status     = status;
    r.legacyPath = legacyPath;
    r.newPath    = newPath;
    r.backupPath = backupPath;
    r.error      = error;
    return r;
}

//! "<path>.save", or a timestamped variant when that name is taken. Same
//! shape as backupPathFor(), different suffix: .bak marks a file ngPost made
//! a copy of, .save marks a file ngPost stopped reading.
QString savePathFor(const QString &path)
{
    const QString simple = path + QStringLiteral(".save");
    if (!QFile::exists(simple))
        return simple;

    const QFileInfo fi(path);
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = QStringLiteral("%1/%2.%3.save")
                            .arg(fi.absolutePath(), fi.fileName(), stamp);
    for (int i = 1; QFile::exists(candidate); ++i)
        candidate = QStringLiteral("%1/%2.%3.%4.save")
                        .arg(fi.absolutePath(), fi.fileName(), stamp)
                        .arg(i);
    return candidate;
}

//! Short-lived openvpn auth files. They are recreated on demand and are the
//! only secrets on disk, so the migration never duplicates them.
bool isVpnRuntimeDir(const QString &parentName, const QString &name)
{
    const Qt::CaseSensitivity cs =
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    return parentName.compare(QStringLiteral("vpn"), cs) == 0
        && name.compare(QStringLiteral("runtime"), cs) == 0;
}

//! Publish a completely written temporary file without replacing a target
//! that another process or the user created while migration was running.
bool publishTemporaryNoClobber(const QString &temporary,
                               const QString &target,
                               QString *error)
{
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() || targetInfo.isSymLink()) {
        if (error)
            *error = QStringLiteral("already present in the new folder");
        QFile::remove(temporary);
        return false;
    }

#if defined(Q_OS_UNIX)
    const QByteArray from = QFile::encodeName(temporary);
    const QByteArray to = QFile::encodeName(target);
    // link(2) is atomic and fails with EEXIST: unlike rename(2), it can never
    // replace a destination created in the last microsecond. Both names are in
    // the same directory/filesystem; remove the private staging name only
    // after the public link exists.
    if (::link(from.constData(), to.constData()) == 0) {
        QFile::remove(temporary);
        return true;
    }
    if (error)
        *error = QStringLiteral("cannot publish it without overwriting (%1)")
                     .arg(QString::fromLocal8Bit(std::strerror(errno)));
    QFile::remove(temporary);
    return false;
#else
    // QFile::rename() is no-clobber on Windows (MoveFile semantics). Keep the
    // explicit preflight above for a useful error and let the operation itself
    // close the remaining race.
    QFile staged(temporary);
    if (staged.rename(target))
        return true;
    if (error)
        *error = QStringLiteral("cannot publish it without overwriting (%1)")
                     .arg(staged.errorString());
    QFile::remove(temporary);
    return false;
#endif
}

QString stagingTemplateFor(const QString &target)
{
    return QFileInfo(target).absolutePath()
        + QStringLiteral("/.ngpost-migration-XXXXXX");
}

//! Copy a file through a private sibling, then publish it without clobbering.
bool copyFileAtomically(const QString &from, const QString &to, QString *error)
{
    const QFileInfo targetInfo(to);
    if (targetInfo.exists() || targetInfo.isSymLink()) {
        if (error)
            *error = QStringLiteral("already present in the new folder");
        return false;
    }

    QFile source(from);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read it (%1)").arg(source.errorString());
        return false;
    }

    QTemporaryFile target(stagingTemplateFor(to));
    target.setAutoRemove(false);
    if (!target.open()) {
        if (error)
            *error = QStringLiteral("cannot create it (%1)").arg(target.errorString());
        return false;
    }
    target.setPermissions(source.permissions());

    QByteArray block;
    while (!(block = source.read(1024 * 1024)).isEmpty()) {
        if (target.write(block) != block.size()) {
            if (error)
                *error = QStringLiteral("cannot copy it (%1)").arg(target.errorString());
            const QString temporary = target.fileName();
            target.close();
            QFile::remove(temporary);
            return false;
        }
    }
    if (source.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("cannot read it (%1)").arg(source.errorString());
        const QString temporary = target.fileName();
        target.close();
        QFile::remove(temporary);
        return false;
    }
    if (!target.flush()) {
        if (error)
            *error = QStringLiteral("cannot flush the copy (%1)").arg(target.errorString());
        const QString temporary = target.fileName();
        target.close();
        QFile::remove(temporary);
        return false;
    }
    const QString temporary = target.fileName();
    target.close();
    return publishTemporaryNoClobber(temporary, to, error);
}

bool writeFileAtomically(const QString &path,
                         const QByteArray &content,
                         QFileDevice::Permissions permissions,
                         QString *error)
{
    const QFileInfo destination(path);
    if (destination.exists() || destination.isSymLink()) {
        if (error)
            *error = QStringLiteral("already present in the new folder");
        return false;
    }

    QTemporaryFile file(stagingTemplateFor(path));
    file.setAutoRemove(false);
    if (!file.open()) {
        if (error)
            *error = QStringLiteral("cannot create it (%1)").arg(file.errorString());
        return false;
    }
    file.setPermissions(permissions);
    if (file.write(content) != content.size()) {
        if (error)
            *error = QStringLiteral("cannot write it (%1)").arg(file.errorString());
        const QString temporary = file.fileName();
        file.close();
        QFile::remove(temporary);
        return false;
    }
    if (!file.flush()) {
        if (error)
            *error = QStringLiteral("cannot flush the write (%1)").arg(file.errorString());
        const QString temporary = file.fileName();
        file.close();
        QFile::remove(temporary);
        return false;
    }
    const QString temporary = file.fileName();
    file.close();
    return publishTemporaryNoClobber(temporary, path, error);
}

//! Copy a directory tree, merging directories but never overwriting a file.
//! Symbolic links are deliberately left in place: following one could escape
//! the configuration folder, copy an arbitrary tree, or recurse forever.
bool copyDirRecursively(const QString &from,
                        const QString &to,
                        const QString &relative,
                        QStringList *skipped,
                        QString *error)
{
    const QFileInfo destination(to);
    if (destination.isSymLink()) {
        if (error)
            *error = QStringLiteral("target directory is a symbolic link");
        return false;
    }
    if (!QDir().mkpath(to)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(to);
        return false;
    }

    const QString parentName = QFileInfo(from).fileName();
    const QFileInfoList entries = QDir(from).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (isVpnRuntimeDir(parentName, entry.fileName()))
            continue;
        const QString rel = relative.isEmpty()
            ? entry.fileName()
            : relative + QLatin1Char('/') + entry.fileName();
        if (entry.isSymLink()) {
            if (skipped)
                *skipped << QStringLiteral("%1: symbolic link left untouched").arg(rel);
            continue;
        }
        if (!entry.isDir() && !entry.isFile()) {
            if (skipped)
                *skipped << QStringLiteral("%1: special file left untouched").arg(rel);
            continue;
        }
        const QString target = to + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            const QFileInfo targetInfo(target);
            if (targetInfo.isSymLink()
                || (targetInfo.exists() && !targetInfo.isDir())) {
                if (skipped)
                    *skipped << QStringLiteral("%1: already present in the new folder").arg(rel);
                continue;
            }
            if (!copyDirRecursively(entry.absoluteFilePath(), target, rel, skipped, error))
                return false;
        } else {
            const QFileInfo targetInfo(target);
            if (targetInfo.exists() || targetInfo.isSymLink()) {
                if (skipped)
                    *skipped << QStringLiteral("%1: already present in the new folder").arg(rel);
                continue;
            }
            if (!copyFileAtomically(entry.absoluteFilePath(), target, error))
                return false;
        }
    }
    return true;
}

//! Copy one entry into targetDir, leaving the source untouched.
bool copyEntry(const QFileInfo &entry,
               const QString &targetDir,
               QStringList *skipped,
               QString *error)
{
    const QString target = targetDir + QLatin1Char('/') + entry.fileName();

    if (entry.isDir())
        return copyDirRecursively(entry.absoluteFilePath(), target, entry.fileName(), skipped, error);

    return copyFileAtomically(entry.absoluteFilePath(), target, error);
}

//! Backups and already-retired files stay in the old directory: they are the
//! archive, not the live configuration.
bool isArchiveEntry(const QString &name)
{
    return name.endsWith(QStringLiteral(".save"))
        || name.endsWith(QStringLiteral(".bak"))
        || name.contains(QStringLiteral(".bak-"));
}

bool isHistoryEntry(const QString &name)
{
    const QString base = historyDbFileName();
    return name == base
        || name == base + QStringLiteral("-wal")
        || name == base + QStringLiteral("-shm")
        || name == base + QStringLiteral("-journal");
}

bool hasHistoryBundle(const QString &dir)
{
    const QString base = dir + QLatin1Char('/') + historyDbFileName();
    const auto occupiesPath = [](const QString &path) {
        const QFileInfo info(path);
        // QFileInfo::exists() is false for a broken symlink. It still occupies
        // the destination and must block adoption rather than be replaced.
        return info.exists() || info.isSymLink();
    };
    return occupiesPath(base)
        || occupiesPath(base + QStringLiteral("-wal"))
        || occupiesPath(base + QStringLiteral("-shm"))
        || occupiesPath(base + QStringLiteral("-journal"));
}

struct ConfigHistorySetting
{
    bool    present = false;
    QString value;
};

//! Obtain the same complete regular file twice, with unchanged size/mtime
//! around each read. Old ngPost versions save ngPost.conf by truncating and
//! rewriting it in place; adopting during that short window must fail safely
//! rather than publish an empty or half-written configuration.
bool readStableConfig(const QString &path,
                      QByteArray *content,
                      QFileDevice::Permissions *permissions,
                      QString *error)
{
    QByteArray previous;
    bool havePrevious = false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const QFileInfo before(path);
        if (before.isSymLink() || !before.exists() || !before.isFile()) {
            if (error)
                *error = before.isSymLink()
                    ? QStringLiteral("configuration file is a symbolic link")
                    : QStringLiteral("configuration file is not a regular file");
            return false;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot read it (%1)").arg(file.errorString());
            return false;
        }
        const QByteArray current = file.readAll();
        const QFileDevice::Permissions currentPermissions = file.permissions();
        if (file.error() != QFileDevice::NoError) {
            if (error)
                *error = QStringLiteral("cannot read it (%1)").arg(file.errorString());
            return false;
        }
        file.close();

        const QFileInfo after(path);
        const bool stable = !after.isSymLink() && after.isFile()
            && before.size() == after.size()
            && after.size() == current.size()
            && before.lastModified() == after.lastModified()
            && !current.trimmed().isEmpty();
        if (stable && havePrevious && current == previous) {
            if (content)
                *content = current;
            if (permissions)
                *permissions = currentPermissions;
            return true;
        }

        havePrevious = stable;
        previous = stable ? current : QByteArray();
        QThread::msleep(10);
    }

    if (error)
        *error = QStringLiteral("configuration changed while it was being read");
    return false;
}

//! Mirror the config parser's treatment of POST_DB closely enough to know
//! whether the copied config already preserves an explicit database path.
//! The last occurrence wins, just as it does in NgPost::_parseConfig().
ConfigHistorySetting historySetting(const QByteArray &content)
{
    ConfigHistorySetting setting;
    const QString text = QString::fromUtf8(content);
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char('/')))
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        if (line.left(equals).trimmed().compare(QStringLiteral("POST_DB"),
                                                Qt::CaseInsensitive) == 0) {
            setting.present = true;
            setting.value   = line.mid(equals + 1).trimmed();
        }
    }
    return setting;
}

//! Old saveConfig() files already contain an absolute POST_DB and are copied
//! byte for byte. A hand-written/older config may not: pin only the canonical
//! copy to the legacy database so the history stays visible without copying
//! or moving a potentially multi-gigabyte SQLite/WAL bundle.
QByteArray configForTarget(const QByteArray &legacyContent,
                           const QString &legacyHistory,
                           bool *addedHistoryPath)
{
    if (addedHistoryPath)
        *addedHistoryPath = false;
    const ConfigHistorySetting setting = historySetting(legacyContent);
    if (!QFileInfo::exists(legacyHistory)
        || (setting.present && !setting.value.isEmpty()))
        return legacyContent;

    QByteArray content = legacyContent;
    if (!content.isEmpty() && !content.endsWith('\n'))
        content += '\n';
    content += "\n# Kept in the previous folder by ngPost's configuration-folder migration.\n";
    content += "POST_DB = ";
    content += legacyHistory.toUtf8();
    content += '\n';
    if (addedHistoryPath)
        *addedHistoryPath = true;
    return content;
}

//! Records in configDir() that the one-shot adoption already happened. Its
//! presence — not the presence of a config file — is what makes the migration
//! strictly one-shot: deleting or replacing ngPost.conf afterwards must not
//! pull in a second, older install behind the user's back.
QString migrationStampPath(const QString &dir)
{
    return dir + QStringLiteral("/.ngPost_config_migration");
}

bool writeMigrationStamp(const QString &dir, const char *outcome,
                         const ConfigDirMigrationResult &r, QString *error)
{
    QString stamp;
    QTextStream s(&stamp);
    s << "# ngPost looked for an older, name-derived configuration folder\n"
      << "# once, and will not look again. Nothing in the old folder was\n"
      << "# deleted. Delete this file only if you want ngPost to run that\n"
      << "# one-time check again on the next start.\n"
      << "date=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "\n"
      << "outcome=" << QLatin1String(outcome) << "\n"
      << "from=" << r.legacyDir << "\n"
      << "saved=" << r.savedConfigPath << "\n"
      << "history=" << r.retainedHistoryPath << "\n"
      << "adopted=" << r.adopted.join(QLatin1Char(' ')) << "\n";
    for (const QString &other : r.otherLegacyDirs)
        s << "other=" << other << "\n";
    for (const QString &skipped : r.skipped)
        s << "left-behind=" << skipped << "\n";
    return writeFileAtomically(migrationStampPath(dir),
                               stamp.toUtf8(),
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner,
                               error);
}

//! An ngPost configuration folder is one that holds an ngPost.conf. Detecting
//! by marker rather than by name is what makes an update work: the folder to
//! adopt is named after the *previous* program file, which we cannot guess.
bool holdsNgPostConfig(const QString &dir)
{
    const QFileInfo config(dir + QStringLiteral("/ngPost.conf"));
    return !config.isSymLink() && config.isFile();
}

//! Sibling directories of configDir() that hold an ngPost.conf, best first:
//! an exact match on the name Qt derived for this very program is certain, so
//! it wins; otherwise the most recently configured install wins.
QStringList configDirCandidates(const QString &targetDir, const QString &preferredName)
{
    const QString root = genericConfigRoot();
    if (root.isEmpty())
        return {};

    struct Candidate
    {
        QString   dir;
        QDateTime configModified;
        bool      nameMatches = false;
    };
    QList<Candidate> candidates;

    const QFileInfoList dirs = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &dir : dirs) {
        const QString path = dir.absoluteFilePath();
        if (samePath(path, targetDir) || !holdsNgPostConfig(path))
            continue;

        Candidate c;
        c.dir            = path;
        c.configModified = QFileInfo(path + QStringLiteral("/ngPost.conf")).lastModified();
        c.nameMatches    = !preferredName.isEmpty() && dir.fileName() == preferredName;
        candidates << c;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  if (a.nameMatches != b.nameMatches)
                      return a.nameMatches;
                  return a.configModified > b.configModified;
              });

    QStringList paths;
    paths.reserve(candidates.size());
    for (const Candidate &c : candidates)
        paths << c.dir;
    return paths;
}

ConfigDirMigrationResult &migrationState()
{
    static ConfigDirMigrationResult state;
    return state;
}

//! Run-once guard. Keyed rather than a plain bool so a test can drive the
//! migration again from a fresh sandbox; in production both parts are
//! constant for the life of the process and it runs exactly once.
struct MigrationRun
{
    QString appName;
    QString targetDir;
    bool    done = false;
};

MigrationRun &migrationRun()
{
    static MigrationRun run;
    return run;
}

} // namespace

QString configDirPath()
{
#ifdef NGPOST_TESTING
    const QString testDir = testEnvPath("NGPOST_TEST_CONFIG_DIR");
    if (!testDir.isEmpty())
        return testDir;
#endif

    return genericConfigRoot() + QStringLiteral("/ngPost");
}

QString configDir()
{
    const QString d = configDirPath();
    QDir().mkpath(d);
    return d;
}

QString appNamedConfigDir(const QString &appName)
{
    if (appName.isEmpty())
        return QString();

    return genericConfigRoot() + QLatin1Char('/') + appName;
}

QString configFilePath()
{
    return configDir() + QStringLiteral("/ngPost.conf");
}

QString historyDbFileName()
{
    return QStringLiteral("ngPost_history.sqlite");
}

QString vpnDir()
{
    QString d = configDir() + QStringLiteral("/vpn");
    QDir().mkpath(d);
    return d;
}

QString vpnRuntimeDir()
{
    // No leading dot in the directory name: confirmed via live-reproduction
    // on a Windows VM that OpenVPNServiceInteractive's impersonation of the
    // calling user refuses to read an auth file from a directory whose name
    // starts with '.'. openvpn.exe then exits with code 1 immediately and
    // the service surfaces "0x20000000 OpenVPN exited with error". Using
    // "runtime" (no dot) works on Windows and is functionally equivalent on
    // Linux — security comes from the per-file permissions we set below,
    // not from the directory being marked hidden.
    QString d = vpnDir() + QStringLiteral("/runtime");
    QDir().mkpath(d);
    // Best-effort: tighten perms so other local users can't list our
    // ephemeral auth files. Qt's setPermissions is portable (no-op where
    // not meaningful).
    QFile::setPermissions(d, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);
    return d;
}

QString legacyConfigFilePath()
{
#ifdef NGPOST_TESTING
    const QString testLegacy = testEnvPath("NGPOST_TEST_LEGACY_CONFIG");
    if (!testLegacy.isEmpty())
        return testLegacy;
#endif

#if defined(Q_OS_WIN) || defined(WIN32) || defined(__MINGW64__)
#ifdef NGPOST_TESTING
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/ngPost.conf");
#endif
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ngPost.conf");
#else
#ifdef NGPOST_TESTING
    const QString testHome = testEnvPath("NGPOST_TEST_HOME");
    if (!testHome.isEmpty())
        return testHome + QStringLiteral("/.ngPost");
#endif
    return QDir::homePath() + QStringLiteral("/.ngPost");
#endif
}

QString backupPathFor(const QString &path)
{
    const QString simple = path + QStringLiteral(".bak");
    if (!QFile::exists(simple))
        return simple;

    const QFileInfo fi(path);
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = QStringLiteral("%1/%2.%3.bak")
                            .arg(fi.absolutePath(), fi.fileName(), stamp);
    if (!QFile::exists(candidate))
        return candidate;

    for (int i = 1; ; ++i) {
        candidate = QStringLiteral("%1/%2.%3.%4.bak")
                        .arg(fi.absolutePath(), fi.fileName(), stamp)
                        .arg(i);
        if (!QFile::exists(candidate))
            return candidate;
    }
}

ConfigMigrationResult migrateLegacyConfigIfNeeded(bool overwriteConfirmed)
{
    QString newPath    = configFilePath();
    QString legacyPath = legacyConfigFilePath();

    QFileInfo legacy(legacyPath);
    if (!legacy.exists() || !legacy.isFile()) {
        if (QFile::exists(newPath))
            return result(ConfigMigrationStatus::AlreadyMigrated, legacyPath, newPath);
        return result(ConfigMigrationStatus::NoLegacy, legacyPath, newPath);
    }

    if (samePath(legacyPath, newPath)) {
        if (!overwriteConfirmed)
            return result(ConfigMigrationStatus::NeedsOverwriteConfirmation,
                          legacyPath,
                          newPath);

        const QString backupPath = backupPathFor(newPath);
        if (!QFile::copy(newPath, backupPath))
            return result(ConfigMigrationStatus::BackupFailed,
                          legacyPath,
                          newPath,
                          backupPath,
                          QStringLiteral("could not create backup"));

        return result(ConfigMigrationStatus::AlreadyMigrated,
                      legacyPath,
                      newPath,
                      backupPath);
    }

    if (QFile::exists(newPath))
    {
        ConfigMigrationResult r = result(ConfigMigrationStatus::SkippedNewExists,
                                          legacyPath,
                                          newPath);
        r.legacyModifiedAfterMigration =
            legacy.lastModified() > QFileInfo(newPath).lastModified();
        return r;
    }

    // Make sure the parent dir exists (configDir() already does it via mkpath
    // when called above, but be explicit).
    QDir().mkpath(QFileInfo(newPath).absolutePath());

    if (!QFile::copy(legacyPath, newPath))
        return result(ConfigMigrationStatus::CopyFailed,
                      legacyPath,
                      newPath,
                      QString(),
                      QStringLiteral("could not copy legacy config"));

    return result(ConfigMigrationStatus::CopiedAndKeptLegacy,
                  legacyPath,
                  newPath);
}

const ConfigDirMigrationResult &migrateAppNamedConfigDirIfNeeded(const QString &legacyAppName)
{
    ConfigDirMigrationResult &state = migrationState();
    MigrationRun            &run   = migrationRun();

#ifdef NGPOST_TESTING
    // Check the sandbox BEFORE configDir() creates or scans anything. This is
    // deliberately stricter than a production guard: a forgotten HomeSandbox
    // must not even create a directory in the developer's real profile.
    //  - NGPOST_TEST_CONFIG_DIR pins configDir() outright, so there is no
    //    name-derived sibling to adopt in the first place;
    //  - no sandbox marker at all means a test forgot its HomeSandbox. Refuse
    //    before consulting QStandardPaths.
    if (!testEnvPath("NGPOST_TEST_CONFIG_DIR").isEmpty()
        || testEnvPath("NGPOST_TEST_HOME").isEmpty()) {
        state = ConfigDirMigrationResult();
        return state;
    }
#endif

    // configDir() creates the target directory. No user file is touched yet.
    const QString target = configDir();
    if (run.done && run.appName == legacyAppName && run.targetDir == target)
        return state;

    state           = ConfigDirMigrationResult();
    state.targetDir = target;
    run.appName     = legacyAppName;
    run.targetDir   = target;
    run.done        = true;

    const QStringList candidates = configDirCandidates(target, legacyAppName);
    if (candidates.isEmpty())
        return state; // NotNeeded — fresh install, or nothing left to adopt

    state.legacyDir       = candidates.first();
    state.otherLegacyDirs = candidates.mid(1);

    // Two renamed ngPost processes can start together (desktop double-click,
    // monitor restart, cron overlap). Only one may decide which old folder is
    // adopted. Never expire a live lock merely because a user kept a large
    // model or other asset in the old folder; a dead local process is still
    // detected by QLockFile's PID/host checks.
    QLockFile lock(target + QStringLiteral("/.ngPost_config_migration.lock"));
    lock.setStaleLockTime(0);
#ifdef NGPOST_TESTING
    constexpr int lockWaitMs = 50;
#else
    constexpr int lockWaitMs = 10000;
#endif
    if (!lock.tryLock(lockWaitMs)) {
        state.status = ConfigDirMigrationStatus::Failed;
        switch (lock.error()) {
        case QLockFile::PermissionError:
            state.error = QStringLiteral("cannot create the migration lock (permission denied)");
            break;
        case QLockFile::LockFailedError:
            state.error = QStringLiteral("another ngPost process is adopting the configuration");
            break;
        default:
            state.error = QStringLiteral("cannot acquire the configuration migration lock");
            break;
        }
        return state;
    }

    // One shot, durably. The stamp survives the user later deleting or
    // replacing ngPost.conf, which the "target is configured" test below would
    // not: without it, wiping a config would silently pull in an older install.
    if (QFileInfo::exists(migrationStampPath(target)))
        return state;

    // A configured install is never overwritten. A history bundle counts as
    // user data too even when no ngPost.conf exists (a CLI-only install can
    // legitimately create exactly that layout), so it also blocks adoption.
    const QString targetConf = target + QStringLiteral("/ngPost.conf");
    const QFileInfo targetConfInfo(targetConf);
    const bool targetConfigured = targetConfInfo.exists() || targetConfInfo.isSymLink()
        || hasHistoryBundle(target);

    if (targetConfigured) {
        state.status = ConfigDirMigrationStatus::SkippedTargetConfigured;
        if (!targetConfInfo.exists() && !targetConfInfo.isSymLink())
            state.skipped << QStringLiteral("history already present in the new folder");
        // Stamped too, so the "you have other config folders" notice is shown
        // once rather than at every single start for the rest of time.
        QString stampError;
        if (!writeMigrationStamp(target,
                                 "skipped-target-already-configured",
                                 state,
                                 &stampError))
            state.error = QStringLiteral("could not record the one-time check (%1)")
                              .arg(stampError);
        return state;
    }

    const QString legacy     = state.legacyDir;
    const QString legacyConf = legacy + QStringLiteral("/ngPost.conf");
    QByteArray legacyConfig;
    QFileDevice::Permissions configPermissions{};
    QString stableReadError;
    if (!readStableConfig(legacyConf,
                          &legacyConfig,
                          &configPermissions,
                          &stableReadError)) {
        state.status = ConfigDirMigrationStatus::Failed;
        state.error  = QStringLiteral("could not read '%1' (%2)")
                          .arg(legacyConf, stableReadError);
        return state;
    }

    // Step 1 — copy every non-database asset first, merging directories and
    // never overwriting a name already present. Publishing ngPost.conf last is
    // the recovery protocol: if the process stops here, the next start sees no
    // configured target, takes the lock, and safely fills in whatever remains.
    // SQLite, WAL and SHM files are deliberately not copied or moved. A live
    // database cannot be migrated safely as three unrelated filesystem files,
    // and it can be many gigabytes; the copied config keeps pointing to it.
    const QString       stampName = QFileInfo(migrationStampPath(legacy)).fileName();
    const QFileInfoList entries   = QDir(legacy).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (name == QStringLiteral("ngPost.conf") || isArchiveEntry(name)
            || name == stampName || isHistoryEntry(name))
            continue;

        if (entry.isSymLink()) {
            state.skipped << QStringLiteral("%1: symbolic link left untouched").arg(name);
            continue;
        }
        if (!entry.isDir() && !entry.isFile()) {
            state.skipped << QStringLiteral("%1: special file left untouched").arg(name);
            continue;
        }

        const QString targetEntry = target + QLatin1Char('/') + name;
        const QFileInfo targetInfo(targetEntry);
        if (targetInfo.isSymLink()
            || (targetInfo.exists() && !(entry.isDir() && targetInfo.isDir()))) {
            state.skipped
                << QStringLiteral("%1: already present in the new folder").arg(name);
            continue;
        }

        QString err;
        const bool ok = copyEntry(entry, target, &state.skipped, &err);
        if (ok)
            state.adopted << name;
        else
            state.skipped << QStringLiteral("%1: %2").arg(name, err);
    }

    // Step 2 — snapshot the source configuration as .save, and deliberately
    // leave the original file exactly where it is. The snapshot is helpful but
    // non-fatal: the original itself remains the authoritative rollback copy.
    //
    // Renaming it away would be tidier, and it is tempting: the old folder
    // would stop looking like a live install. It is also the one thing here
    // that can break a working setup. "-c <old folder>/ngPost.conf" is a
    // perfectly normal way to drive ngPost — scripts and cron jobs hold that
    // path — and moving the file out from under them turns an update into a
    // broken automation. Nothing in this function needs the rename: the stamp
    // written below is what keeps the migration one-shot. So the old folder is
    // left byte for byte as it was, plus a .save snapshot of the configuration
    // as it stood at migration time.
    QByteArray verifiedConfig;
    QFileDevice::Permissions verifiedPermissions{};
    stableReadError.clear();
    if (!readStableConfig(legacyConf,
                          &verifiedConfig,
                          &verifiedPermissions,
                          &stableReadError)
        || verifiedConfig != legacyConfig) {
        state.status = ConfigDirMigrationStatus::Failed;
        state.error = stableReadError.isEmpty()
            ? QStringLiteral("'%1' changed while its assets were being copied; retrying is safe")
                  .arg(legacyConf)
            : QStringLiteral("could not revalidate '%1' (%2)")
                  .arg(legacyConf, stableReadError);
        return state;
    }
    configPermissions = verifiedPermissions;

    const QString saved = savePathFor(legacyConf);
    QString saveError;
    if (writeFileAtomically(saved, legacyConfig, configPermissions, &saveError))
        state.savedConfigPath = saved;
    else
        state.skipped << QStringLiteral("ngPost.conf.save: %1").arg(saveError);

    // Step 3 — publish the configuration atomically and last. Existing 5.4.x
    // configs already carry their absolute POST_DB and therefore keep using
    // exactly the same SQLite file. For an older/hand-written config without
    // POST_DB, add the old database path to the canonical copy only.
    const QString legacyHistory = legacy + QLatin1Char('/') + historyDbFileName();
    bool historyPathAdded = false;
    const QByteArray targetConfig =
        configForTarget(legacyConfig, legacyHistory, &historyPathAdded);

    const ConfigHistorySetting dbSetting = historySetting(legacyConfig);
    if (QFileInfo::exists(legacyHistory)
        && (historyPathAdded
            || (dbSetting.present && !dbSetting.value.isEmpty()
                && samePath(dbSetting.value, legacyHistory))))
        state.retainedHistoryPath = legacyHistory;

    QString configError;
    if (!writeFileAtomically(targetConf,
                             targetConfig,
                             configPermissions,
                             &configError)) {
        state.status = ConfigDirMigrationStatus::Failed;
        state.error  = QStringLiteral("could not publish '%1' (%2)")
                          .arg(targetConf, configError);
        return state;
    }
    state.adopted.prepend(QStringLiteral("ngPost.conf"));

    state.status = ConfigDirMigrationStatus::Migrated;

    // Written last, once the outcome is known, so an interrupted migration is
    // retried on the next start rather than silently marked as done.
    QString stampError;
    if (!writeMigrationStamp(target, "migrated", state, &stampError)) {
        state.skipped << QStringLiteral("migration stamp: %1").arg(stampError);
        state.error = QStringLiteral("the one-time migration marker could not be written");
    }
    return state;
}

const ConfigDirMigrationResult &configDirMigrationResult()
{
    return migrationState();
}

} // namespace PathHelper
