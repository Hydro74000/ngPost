// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_PathHelper.cpp — XDG / appdata directory resolution, vpn dir layout,
// non-destructive legacy config migration.
//
//========================================================================

#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#  include <sys/stat.h>
#endif

#include "TestEnv.h"
#include "utils/PathHelper.h"

using ngpost::tests::HomeSandbox;

namespace
{

//! HomeSandbox pins NGPOST_TEST_CONFIG_DIR, which makes configDir() return
//! that path verbatim and disables the config-folder migration by design. The
//! migration tests need the real XDG-derived resolution, so they drop the
//! override for their lifetime — HOME and XDG_CONFIG_HOME stay sandboxed, and
//! ~HomeSandbox restores everything.
class XdgSandbox
{
public:
    XdgSandbox() { qunsetenv("NGPOST_TEST_CONFIG_DIR"); }

    //! The generic config root, i.e. the parent every ngPost config folder
    //! (canonical or name-derived) sits in.
    QString configRoot() const { return _sandbox.xdgConfigHome(); }

private:
    HomeSandbox _sandbox;
};

//! Build a plausible ngPost config folder: a config, a history database, a
//! post-info template and a vpn profile. Returns the folder path.
QString makeConfigFolder(const QString &root, const QString &name,
                         const QString &fromAddr = QStringLiteral("old@example.com"),
                         bool includePostDb = true,
                         const QString &postDb = QString())
{
    const QString dir = root + QLatin1Char('/') + name;
    QDir().mkpath(dir + QStringLiteral("/vpn"));

    auto write = [](const QString &path, const QByteArray &content) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(content);
    };
    QString config = QStringLiteral("FROM = %1\nPOST_INFO_TEMPLATE = mine.txt\n").arg(fromAddr);
    if (includePostDb) {
        const QString db = postDb.isEmpty()
            ? dir + QStringLiteral("/ngPost_history.sqlite")
            : postDb;
        config += QStringLiteral("POST_DB = %1\n").arg(db);
    }
    write(dir + QStringLiteral("/ngPost.conf"), config.toUtf8());
    write(dir + QStringLiteral("/ngPost_history.sqlite"), "not really sqlite, but a file\n");
    write(dir + QStringLiteral("/mine.txt"), "Title: __nzbName__\n");
    write(dir + QStringLiteral("/ngPost_gui.ini"), "[General]\ngeom=42\n");
    write(dir + QStringLiteral("/vpn/Profile.ovpn"), "client\n");
    return dir;
}

//! Force a config file's mtime so candidate ordering doesn't depend on the
//! filesystem's timestamp resolution.
void setConfigAge(const QString &dir, int secondsAgo)
{
    QFile f(dir + QStringLiteral("/ngPost.conf"));
    if (f.open(QIODevice::ReadWrite))
        f.setFileTime(QDateTime::currentDateTime().addSecs(-secondsAgo),
                      QFileDevice::FileModificationTime);
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestPathHelper : public QObject
{
    Q_OBJECT

private slots:
    //! configDir() must not depend on QCoreApplication::applicationName(): an
    //! AppImage is renamed on install and on every update, and the config
    //! folder must not follow it. Regression guard for the split-config bug.
    void configDir_is_independent_of_the_application_name();

    //! The name-derived folder an older ngPost used sits next to configDir().
    void appNamedConfigDir_is_a_sibling_of_configDir();

    //! The update case: the folder to adopt is named after the *previous*
    //! program file, so it is found by marker (ngPost.conf), not by name.
    void migrate_adopts_a_folder_whose_name_no_longer_matches();

    //! Configuration assets come over, while SQLite history remains at the
    //! exact path already recorded in POST_DB.
    void migrate_keeps_history_and_takes_templates_and_vpn();

    //! A hand-written old config without POST_DB is pinned to its old default
    //! database in the canonical copy, without modifying the source config.
    void migrate_adds_legacy_history_path_when_post_db_is_absent();

    //! An explicitly empty POST_DB has the same effective meaning as an
    //! absent value and must not hide the old default history.
    void migrate_adds_legacy_history_path_when_post_db_is_empty();

    //! A custom POST_DB is copied verbatim; an unrelated default DB is never
    //! mistaken for the active history.
    void migrate_preserves_a_custom_post_db();

    //! The previous configuration is never destroyed nor moved: a snapshot is
    //! saved next to it as ngPost.conf.save.
    void migrate_keeps_the_previous_config_as_save();

    //! A configured install is never overwritten — both folders stay byte for
    //! byte as they were.
    void migrate_never_touches_an_already_configured_target();

    //! Fresh install: nothing to adopt, nothing reported.
    void migrate_does_nothing_on_a_fresh_install();

    //! A correctly named install already sits on configDir(): no candidate,
    //! no migration, no stamp.
    void migrate_does_nothing_for_a_correctly_named_install();

    //! Running it again must not migrate a second time.
    void migrate_is_idempotent_across_restarts();

    //! The stamp is what makes it strictly one-shot: deleting the adopted
    //! config afterwards must not pull in another old install.
    void migrate_runs_only_once_even_if_the_config_is_deleted();

    //! Several old folders: the one matching the current program name is
    //! certain, so it wins.
    void migrate_prefers_the_folder_matching_the_program_name();

    //! Otherwise the most recently configured install wins, and the others
    //! are reported untouched.
    void migrate_prefers_the_most_recent_config_and_reports_the_others();

    //! Backups stay behind: they are the archive, not the live config.
    void migrate_leaves_backups_in_the_old_folder();

    //! An entry already present in the target is never overwritten; it is
    //! reported as left behind instead.
    void migrate_never_overwrites_an_entry_already_in_the_target();

    //! Any target history is user data, not an assumed placeholder. Its
    //! presence blocks automatic adoption and neither history is touched.
    void migrate_never_replaces_an_existing_target_history();

    //! Symbolic links are not followed into arbitrary trees or cycles.
    void migrate_leaves_symbolic_links_untouched();

    //! A symlink masquerading as the source config is not a migration
    //! candidate, and a broken symlink at the target is never replaced.
    void migrate_rejects_config_symlinks();

    //! FIFOs/devices/sockets cannot be read as ordinary assets (a FIFO could
    //! otherwise block startup forever).
    void migrate_leaves_special_files_untouched();

    //! A transient lock failure changes no authoritative file and the next
    //! process can safely retry and complete the adoption.
    void migrate_retries_after_a_transient_lock_failure();

    //! A test build with no sandbox at all must refuse to run: the migration
    //! moves files around a home directory and must never reach a real one.
    void migrate_refuses_to_run_outside_a_test_sandbox();

    //! With the test harness override active the migration must not run at
    //! all — no test may touch a real ~/.config.
    void migrate_is_a_no_op_under_the_test_config_dir_override();
    void marker_remembers_where_the_config_came_from();

    //! configDir() must live under the sandboxed HOME and exist after the call.
    void configDir_under_sandbox_and_created();

    //! configFilePath() ends with ngPost.conf and is a child of configDir().
    void configFilePath_inside_configDir();

    //! vpnDir() is "<configDir>/vpn" and is created on demand.
    void vpnDir_subdir_of_configDir_and_created();

    //! vpnRuntimeDir() must NOT have a leading dot in its final segment
    //! (regression guard for the Windows openvpn breakage fixed in 31c4a8c).
    void vpnRuntimeDir_no_leading_dot_in_basename();

    //! migrateLegacyConfigIfNeeded() copies the legacy config into the modern
    //! location and keeps the legacy file for ngPost 4.16.
    void migrateLegacyConfigIfNeeded_copies_and_keeps_legacy();

    //! Called twice in a row: the second call must be a no-op (no exception,
    //! no overwrite of the migrated file).
    void migrateLegacyConfigIfNeeded_is_idempotent();

    //! With no legacy file and no new file present, the migrate call should
    //! create nothing — it must not invent an empty ngPost.conf.
    void migrateLegacyConfigIfNeeded_no_legacy_no_op();

    //! If the new config already exists, the legacy file (if any) is left
    //! alone — we do not overwrite a configured user.
    void migrateLegacyConfigIfNeeded_keeps_new_when_both_present();

    //! backupPathFor() uses .bak first, then an available timestamped path.
    void backupPathFor_prefers_simple_then_timestamped();

    //! If source and destination resolve to the same path, migration requires
    //! explicit confirmation and leaves the file untouched otherwise.
    void migrateLegacyConfigIfNeeded_same_path_requires_confirmation();

    //! A confirmed same-path migration creates a backup before continuing.
    void migrateLegacyConfigIfNeeded_same_path_confirmed_creates_backup();

    //! Test override lets us simulate the Windows legacy ngPost.conf path.
    void legacyConfigFilePath_honors_test_override();

    //! If both configs exist and the legacy one (e.g. next to ngPost.exe)
    //! was modified more recently than the active one, migration must flag
    //! it: it's the "user is hand-editing a file we no longer read" case.
    void migrateLegacyConfigIfNeeded_flags_legacy_edited_after_migration();

    //! The common case: the legacy file predates the active config (it's
    //! just what it was copied from originally) — no warning warranted.
    void migrateLegacyConfigIfNeeded_does_not_flag_untouched_legacy();
};

void TestPathHelper::configDir_under_sandbox_and_created()
{
    HomeSandbox sandbox;
    const QString d = PathHelper::configDir();

    QVERIFY(QFileInfo(d).exists());
    QVERIFY(QFileInfo(d).isDir());
    QVERIFY2(d.startsWith(sandbox.rootPath()),
             qPrintable(QStringLiteral("configDir not under sandbox: %1").arg(d)));
}

void TestPathHelper::configFilePath_inside_configDir()
{
    HomeSandbox sandbox;
    const QString f = PathHelper::configFilePath();
    QVERIFY(f.endsWith(QStringLiteral("/ngPost.conf")));
    QCOMPARE(QFileInfo(f).absolutePath(), PathHelper::configDir());
}

void TestPathHelper::vpnDir_subdir_of_configDir_and_created()
{
    HomeSandbox sandbox;
    const QString v = PathHelper::vpnDir();

    QVERIFY(QFileInfo(v).exists());
    QVERIFY(QFileInfo(v).isDir());
    QCOMPARE(QFileInfo(v).absolutePath(), PathHelper::configDir());
    QCOMPARE(QFileInfo(v).fileName(), QStringLiteral("vpn"));
}

void TestPathHelper::vpnRuntimeDir_no_leading_dot_in_basename()
{
    HomeSandbox sandbox;
    const QString r = PathHelper::vpnRuntimeDir();
    const QString basename = QFileInfo(r).fileName();

    QVERIFY2(!basename.startsWith('.'),
             qPrintable(QStringLiteral(
                 "vpnRuntimeDir basename starts with '.', breaks OpenVPNServiceInteractive on Windows. Got: %1")
                            .arg(basename)));
    QCOMPARE(basename, QStringLiteral("runtime"));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_copies_and_keeps_legacy()
{
    HomeSandbox sandbox;
    const QString legacy = PathHelper::legacyConfigFilePath();

    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("lang = EN\nhost = legacy.example.com\n");
    f.close();
    QVERIFY(QFile::exists(legacy));

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);
    QCOMPARE(r.legacyPath, legacy);

    const QString newConf = PathHelper::configFilePath();
    QVERIFY2(QFile::exists(newConf), "new config not created");
    QVERIFY2(QFile::exists(legacy), "legacy config should be kept for ngPost 4.16");

    QFile g(newConf);
    QVERIFY(g.open(QIODevice::ReadOnly));
    const QByteArray content = g.readAll();
    QVERIFY(content.contains("host = legacy.example.com"));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_is_idempotent()
{
    HomeSandbox sandbox;
    const QString legacy = PathHelper::legacyConfigFilePath();
    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("lang = FR\n");
    f.close();

    PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);

    const QString newConf = PathHelper::configFilePath();
    QVERIFY(QFile::exists(newConf));
    const qint64 sizeAfterFirst = QFileInfo(newConf).size();

    // Second call should not modify anything.
    r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);

    QVERIFY(QFile::exists(newConf));
    QCOMPARE(QFileInfo(newConf).size(), sizeAfterFirst);
    QVERIFY(QFile::exists(legacy));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_no_legacy_no_op()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QVERIFY(!QFile::exists(legacy));
    QVERIFY(!QFile::exists(newConf));

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::NoLegacy);

    QVERIFY2(!QFile::exists(newConf),
             "migrate created an empty config when no legacy existed");
}

void TestPathHelper::migrateLegacyConfigIfNeeded_keeps_new_when_both_present()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QDir().mkpath(QFileInfo(newConf).absolutePath());
    {
        QFile n(newConf);
        QVERIFY(n.open(QIODevice::WriteOnly));
        n.write("from_new = true\n");
    }
    {
        QFile l(legacy);
        QVERIFY(l.open(QIODevice::WriteOnly));
        l.write("from_legacy = true\n");
    }

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);

    QFile n(newConf);
    QVERIFY(n.open(QIODevice::ReadOnly));
    const QByteArray content = n.readAll();
    QVERIFY2(content.contains("from_new"),
             "new config was overwritten by the migration");
    QVERIFY2(!content.contains("from_legacy"),
             "legacy content leaked into pre-existing new config");
    QVERIFY2(QFile::exists(legacy), "legacy config should be kept");
}

void TestPathHelper::backupPathFor_prefers_simple_then_timestamped()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    QDir().mkpath(QFileInfo(conf).absolutePath());

    QCOMPARE(PathHelper::backupPathFor(conf), conf + QStringLiteral(".bak"));

    QFile bak(conf + QStringLiteral(".bak"));
    QVERIFY(bak.open(QIODevice::WriteOnly));
    bak.write("existing backup\n");
    bak.close();

    const QString timestamped = PathHelper::backupPathFor(conf);
    QVERIFY(timestamped != conf + QStringLiteral(".bak"));
    QVERIFY(timestamped.endsWith(QStringLiteral(".bak")));
    QVERIFY(timestamped.contains(QFileInfo(conf).fileName() + QStringLiteral(".")));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_same_path_requires_confirmation()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    qputenv("NGPOST_TEST_LEGACY_CONFIG", conf.toLocal8Bit());

    QDir().mkpath(QFileInfo(conf).absolutePath());
    QFile f(conf);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("same_path = true\n");
    f.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::NeedsOverwriteConfirmation);
    QVERIFY(QFile::exists(conf));
    QVERIFY(!QFile::exists(conf + QStringLiteral(".bak")));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_same_path_confirmed_creates_backup()
{
    HomeSandbox sandbox;
    const QString conf = PathHelper::configFilePath();
    qputenv("NGPOST_TEST_LEGACY_CONFIG", conf.toLocal8Bit());

    QDir().mkpath(QFileInfo(conf).absolutePath());
    QFile f(conf);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("same_path = true\n");
    f.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded(true);
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::AlreadyMigrated);
    QVERIFY(!r.backupPath.isEmpty());
    QVERIFY(QFile::exists(r.backupPath));

    QFile backup(r.backupPath);
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QVERIFY(backup.readAll().contains("same_path = true"));
}

void TestPathHelper::legacyConfigFilePath_honors_test_override()
{
    HomeSandbox sandbox;
    const QString injected = sandbox.rootPath() + QStringLiteral("/app/ngPost.conf");
    qputenv("NGPOST_TEST_LEGACY_CONFIG", injected.toLocal8Bit());

    QCOMPARE(PathHelper::legacyConfigFilePath(), injected);

    QDir().mkpath(QFileInfo(injected).absolutePath());
    QFile legacy(injected);
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write("windows_legacy = true\n");
    legacy.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::CopiedAndKeptLegacy);
    QVERIFY(QFile::exists(injected));
    QVERIFY(QFile::exists(PathHelper::configFilePath()));
}

void TestPathHelper::migrateLegacyConfigIfNeeded_flags_legacy_edited_after_migration()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QDir().mkpath(QFileInfo(newConf).absolutePath());
    {
        QFile n(newConf);
        QVERIFY(n.open(QIODevice::WriteOnly));
        n.write("from_new = true\n");
    }
    {
        QFile l(legacy);
        QVERIFY(l.open(QIODevice::WriteOnly));
        l.write("from_legacy = true\n");
    }

    // Force the legacy file's mtime strictly after the active config's, as
    // if the user had just hand-edited it (filesystem mtime resolution is
    // too coarse to rely on write ordering alone).
    const QDateTime newerThanNew = QFileInfo(newConf).lastModified().addSecs(60);
    QFile legacyFile(legacy);
    QVERIFY(legacyFile.open(QIODevice::ReadWrite));
    QVERIFY(legacyFile.setFileTime(newerThanNew, QFileDevice::FileModificationTime));
    legacyFile.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);
    QVERIFY2(r.legacyModifiedAfterMigration,
             "legacy file modified after the active config was not flagged");
}

void TestPathHelper::migrateLegacyConfigIfNeeded_does_not_flag_untouched_legacy()
{
    HomeSandbox sandbox;
    const QString newConf = PathHelper::configFilePath();
    const QString legacy  = PathHelper::legacyConfigFilePath();

    QDir().mkpath(QFileInfo(newConf).absolutePath());
    {
        QFile l(legacy);
        QVERIFY(l.open(QIODevice::WriteOnly));
        l.write("from_legacy = true\n");
    }
    {
        QFile n(newConf);
        QVERIFY(n.open(QIODevice::WriteOnly));
        n.write("from_new = true\n");
    }
    // Force the active config's mtime strictly after the legacy one, so the
    // assertion doesn't depend on filesystem mtime resolution.
    const QDateTime newerThanLegacy = QFileInfo(legacy).lastModified().addSecs(60);
    QFile newFile(newConf);
    QVERIFY(newFile.open(QIODevice::ReadWrite));
    QVERIFY(newFile.setFileTime(newerThanLegacy, QFileDevice::FileModificationTime));
    newFile.close();

    const PathHelper::ConfigMigrationResult r = PathHelper::migrateLegacyConfigIfNeeded();
    QVERIFY(r.status == PathHelper::ConfigMigrationStatus::SkippedNewExists);
    QVERIFY2(!r.legacyModifiedAfterMigration,
             "untouched legacy file should not be flagged");
}


// ============================================================================
// Config folder pinning + one-shot adoption of a name-derived folder.
// ============================================================================

void TestPathHelper::configDir_is_independent_of_the_application_name()
{
    XdgSandbox sandbox;

    const QString previousName = QCoreApplication::applicationName();
    const QString before = PathHelper::configDir();
    QCoreApplication::setApplicationName(QStringLiteral("ngPost-9.9-x86_64.AppImage"));
    const QString after = PathHelper::configDir();
    QCoreApplication::setApplicationName(previousName);

    QCOMPARE(after, before);
    QCOMPARE(QFileInfo(after).fileName(), QStringLiteral("ngPost"));
    QCOMPARE(after, sandbox.configRoot() + QStringLiteral("/ngPost"));
}

void TestPathHelper::appNamedConfigDir_is_a_sibling_of_configDir()
{
    XdgSandbox sandbox;

    const QString named = PathHelper::appNamedConfigDir(QStringLiteral("weird.AppImage"));
    QCOMPARE(named, sandbox.configRoot() + QStringLiteral("/weird.AppImage"));
    QCOMPARE(QFileInfo(named).absolutePath(), QFileInfo(PathHelper::configDir()).absolutePath());
    QVERIFY(PathHelper::appNamedConfigDir(QString()).isEmpty());
}

void TestPathHelper::migrate_adopts_a_folder_whose_name_no_longer_matches()
{
    XdgSandbox sandbox;
    // The AppImage was renamed by the update: the folder to adopt carries the
    // *previous* name, so matching on the current one would find nothing.
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("ngPost-5.4.AppImage"));

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("ngPost-5.5.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(QFileInfo(r.legacyDir).fileName(), QStringLiteral("ngPost-5.4.AppImage"));
    QVERIFY(readAll(PathHelper::configFilePath()).contains("old@example.com"));
}

void TestPathHelper::migrate_keeps_history_and_takes_templates_and_vpn()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    {
        QFile wal(old + QStringLiteral("/ngPost_history.sqlite-wal"));
        QFile shm(old + QStringLiteral("/ngPost_history.sqlite-shm"));
        QVERIFY(wal.open(QIODevice::WriteOnly));
        QVERIFY(shm.open(QIODevice::WriteOnly));
        wal.write("live wal\n");
        shm.write("live shm\n");
    }

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));
    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);

    const QString dir = PathHelper::configDir();
    QVERIFY2(QFileInfo(dir + "/ngPost.conf").isFile(), "config not adopted");
    QVERIFY2(QFileInfo(dir + "/mine.txt").isFile(), "post-info template not adopted");
    QVERIFY2(QFileInfo(dir + "/ngPost_gui.ini").isFile(), "GUI settings not adopted");
    QVERIFY2(QFileInfo(dir + "/vpn/Profile.ovpn").isFile(), "vpn profile not adopted");
    QVERIFY2(!QFileInfo::exists(dir + "/" + PathHelper::historyDbFileName()),
             "history database was copied into the canonical folder");
    QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/ngPost_history.sqlite-wal")));
    QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/ngPost_history.sqlite-shm")));

    // Copied, not moved: a "-c <old folder>/ngPost.conf" setup must keep
    // resolving its template next to the config file it names.
    QVERIFY2(QFileInfo(old + "/ngPost.conf").isFile(), "old config was taken away");
    QVERIFY2(QFileInfo(old + "/mine.txt").isFile(), "old template was taken away");
    QVERIFY2(QFileInfo(old + "/vpn/Profile.ovpn").isFile(), "old vpn profile was taken away");
    QVERIFY2(QFileInfo(old + "/ngPost_gui.ini").isFile(), "old GUI settings were taken away");

    // SQLite is deliberately retained in place. The copied v5.4.x config
    // already carries its absolute path, so both the canonical config and an
    // existing -c script continue to select the same database.
    const QString oldDb = old + QLatin1Char('/') + PathHelper::historyDbFileName();
    QVERIFY2(QFileInfo(oldDb).isFile(), "history database was moved");
    QVERIFY(QFileInfo::exists(oldDb + QStringLiteral("-wal")));
    QVERIFY(QFileInfo::exists(oldDb + QStringLiteral("-shm")));
    QVERIFY(readAll(PathHelper::configFilePath()).contains(oldDb.toUtf8()));
    QCOMPARE(r.retainedHistoryPath, oldDb);
}

void TestPathHelper::migrate_adds_legacy_history_path_when_post_db_is_absent()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(),
                                         QStringLiteral("old.AppImage"),
                                         QStringLiteral("old@example.com"),
                                         false);
    const QString oldDb = old + QLatin1Char('/') + PathHelper::historyDbFileName();

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QVERIFY(!readAll(old + "/ngPost.conf").contains("POST_DB"));
    const QByteArray adopted = readAll(PathHelper::configFilePath());
    QVERIFY(adopted.contains(QStringLiteral("POST_DB = %1").arg(oldDb).toUtf8()));
    QCOMPARE(r.retainedHistoryPath, oldDb);
    QVERIFY(QFileInfo(oldDb).isFile());
}

void TestPathHelper::migrate_adds_legacy_history_path_when_post_db_is_empty()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(),
                                         QStringLiteral("empty-db.AppImage"),
                                         QStringLiteral("old@example.com"),
                                         false);
    {
        QFile config(old + QStringLiteral("/ngPost.conf"));
        QVERIFY(config.open(QIODevice::Append | QIODevice::Text));
        config.write("POST_DB =   \n");
    }
    const QString oldDb = old + QLatin1Char('/') + PathHelper::historyDbFileName();

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("empty-db.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    const QByteArray adopted = readAll(PathHelper::configFilePath());
    QVERIFY(adopted.contains(QStringLiteral("POST_DB = %1").arg(oldDb).toUtf8()));
    QCOMPARE(r.retainedHistoryPath, oldDb);
    QVERIFY(QFileInfo(oldDb).isFile());
}

void TestPathHelper::migrate_preserves_a_custom_post_db()
{
    XdgSandbox sandbox;
    const QString customDb = sandbox.configRoot() + QStringLiteral("/external/history.sqlite");
    const QString old = makeConfigFolder(sandbox.configRoot(),
                                         QStringLiteral("old.AppImage"),
                                         QStringLiteral("old@example.com"),
                                         true,
                                         customDb);
    const QByteArray sourceConfig = readAll(old + "/ngPost.conf");

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(readAll(PathHelper::configFilePath()), sourceConfig);
    QVERIFY(r.retainedHistoryPath.isEmpty());
    QVERIFY(QFileInfo(old + "/" + PathHelper::historyDbFileName()).isFile());
}

void TestPathHelper::migrate_keeps_the_previous_config_as_save()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QByteArray original = readAll(old + "/ngPost.conf");

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(r.savedConfigPath, old + QStringLiteral("/ngPost.conf.save"));
    QCOMPARE(readAll(r.savedConfigPath), original);
    QCOMPARE(readAll(old + "/ngPost.conf"), original);
}

void TestPathHelper::migrate_never_touches_an_already_configured_target()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QString dir = PathHelper::configDir();
    {
        QFile f(dir + "/ngPost.conf");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("FROM = current@example.com\n");
    }
    const QByteArray target = readAll(dir + "/ngPost.conf");
    const QByteArray source = readAll(old + "/ngPost.conf");

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::SkippedTargetConfigured);
    QCOMPARE(readAll(dir + "/ngPost.conf"), target);
    QCOMPARE(readAll(old + "/ngPost.conf"), source);
    QVERIFY2(QFileInfo(old + "/" + PathHelper::historyDbFileName()).isFile(),
             "history database taken from a folder that should not have been touched");
    QVERIFY2(!QFileInfo::exists(dir + "/mine.txt"), "target folder was written to");
}

void TestPathHelper::migrate_does_nothing_on_a_fresh_install()
{
    XdgSandbox sandbox;
    Q_UNUSED(sandbox)

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("ngPost-5.5.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
    QVERIFY(r.legacyDir.isEmpty());
    // No stamp: a user who restores an old folder later must still get it.
    QVERIFY(!QFileInfo::exists(PathHelper::configDir() + "/.ngPost_config_migration"));
}

void TestPathHelper::migrate_does_nothing_for_a_correctly_named_install()
{
    XdgSandbox sandbox;
    Q_UNUSED(sandbox)
    // Qt derived "ngPost": the name-derived folder *is* configDir().
    makeConfigFolder(QFileInfo(PathHelper::configDir()).absolutePath(),
                     QStringLiteral("ngPost"));

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("ngPost"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
    QVERIFY(!QFileInfo::exists(PathHelper::configDir() + "/.ngPost_config_migration"));
}

void TestPathHelper::migrate_is_idempotent_across_restarts()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));

    QVERIFY(PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage")).status
            == PathHelper::ConfigDirMigrationStatus::Migrated);

    // A different derived name defeats the per-process cache, the way a second
    // launch of a renamed AppImage would.
    const PathHelper::ConfigDirMigrationResult again =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("other.AppImage"));

    QVERIFY(again.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
    // Exactly one snapshot, not one per start.
    QCOMPARE(QDir(old).entryList({QStringLiteral("*.save")}, QDir::Files).size(), 1);
}

void TestPathHelper::migrate_runs_only_once_even_if_the_config_is_deleted()
{
    XdgSandbox sandbox;
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    QVERIFY(PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage")).status
            == PathHelper::ConfigDirMigrationStatus::Migrated);

    // The user wipes the adopted config and another old install shows up.
    QVERIFY(QFile::remove(PathHelper::configFilePath()));
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("older.AppImage"),
                     QStringLiteral("older@example.com"));

    const PathHelper::ConfigDirMigrationResult again =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("older.AppImage"));

    QVERIFY2(again.status == PathHelper::ConfigDirMigrationStatus::NotNeeded,
             "the one-shot stamp did not hold");
    QVERIFY2(!QFileInfo::exists(PathHelper::configFilePath()),
             "a second config was pulled in behind the user's back");
}

void TestPathHelper::migrate_prefers_the_folder_matching_the_program_name()
{
    XdgSandbox sandbox;
    // The stale one is the most recent, but the name match is certain.
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("stale.AppImage"),
                     QStringLiteral("stale@example.com"));
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("ngPost-5.5.AppImage"),
                     QStringLiteral("named@example.com"));
    setConfigAge(sandbox.configRoot() + "/ngPost-5.5.AppImage", 3600);

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("ngPost-5.5.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(QFileInfo(r.legacyDir).fileName(), QStringLiteral("ngPost-5.5.AppImage"));
    QVERIFY(readAll(PathHelper::configFilePath()).contains("named@example.com"));
}

void TestPathHelper::migrate_prefers_the_most_recent_config_and_reports_the_others()
{
    XdgSandbox sandbox;
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("veryold.AppImage"),
                     QStringLiteral("veryold@example.com"));
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("recent.AppImage"),
                     QStringLiteral("recent@example.com"));
    setConfigAge(sandbox.configRoot() + "/veryold.AppImage", 86400);
    setConfigAge(sandbox.configRoot() + "/recent.AppImage", 60);

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("nomatch.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(QFileInfo(r.legacyDir).fileName(), QStringLiteral("recent.AppImage"));
    QCOMPARE(r.otherLegacyDirs.size(), 1);
    QVERIFY(r.otherLegacyDirs.first().endsWith(QStringLiteral("veryold.AppImage")));
    QVERIFY(readAll(PathHelper::configFilePath()).contains("recent@example.com"));
    // Reported, never touched.
    QVERIFY(readAll(sandbox.configRoot() + "/veryold.AppImage/ngPost.conf")
                .contains("veryold@example.com"));
}

void TestPathHelper::migrate_leaves_backups_in_the_old_folder()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    {
        QFile f(old + "/ngPost.conf.bak-20260101-120000");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("FROM = ancient@example.com\n");
    }

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QVERIFY(QFileInfo(old + "/ngPost.conf.bak-20260101-120000").isFile());
    QVERIFY2(!QFileInfo::exists(PathHelper::configDir() + "/ngPost.conf.bak-20260101-120000"),
             "a backup was dragged into the new folder");
}

void TestPathHelper::migrate_never_overwrites_an_entry_already_in_the_target()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QString dir = PathHelper::configDir();
    {   // a template of the same name already sits in the new folder
        QFile f(dir + "/mine.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("KEEP ME\n");
    }

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QCOMPARE(readAll(dir + "/mine.txt"), QByteArray("KEEP ME\n"));
    QVERIFY(QFileInfo(old + "/mine.txt").isFile());
    QVERIFY2(r.skipped.filter(QStringLiteral("mine.txt")).size() == 1,
             "the collision was not reported to the user");
}

void TestPathHelper::migrate_never_replaces_an_existing_target_history()
{
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QString dir = PathHelper::configDir();
    const QString db  = dir + QLatin1Char('/') + PathHelper::historyDbFileName();
    {
        QFile f(db);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("canonical history that must stay selected\n");
    }
    const QByteArray targetHistory = readAll(db);
    const QByteArray oldHistory = readAll(old + QLatin1Char('/') + PathHelper::historyDbFileName());

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::SkippedTargetConfigured);
    QCOMPARE(readAll(db), targetHistory);
    QCOMPARE(readAll(old + QLatin1Char('/') + PathHelper::historyDbFileName()), oldHistory);
    QVERIFY(!QFileInfo::exists(PathHelper::configFilePath()));
    QVERIFY(!QFileInfo::exists(db + ".save"));
}

void TestPathHelper::migrate_leaves_symbolic_links_untouched()
{
#if defined(Q_OS_WIN)
    QSKIP("Creating symlinks requires privileges on some Windows runners");
#else
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QString outside = sandbox.configRoot() + QStringLiteral("/outside");
    QDir().mkpath(outside);
    {
        QFile f(outside + QStringLiteral("/secret.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("do not copy\n");
    }
    QVERIFY(QFile::link(outside, old + QStringLiteral("/linked-tree")));

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QVERIFY(!QFileInfo::exists(PathHelper::configDir() + QStringLiteral("/linked-tree")));
    QVERIFY(!r.skipped.filter(QStringLiteral("linked-tree")).isEmpty());
#endif
}

void TestPathHelper::migrate_rejects_config_symlinks()
{
#if defined(Q_OS_WIN)
    QSKIP("Creating symlinks requires privileges on some Windows runners");
#else
    {
        XdgSandbox sandbox;
        const QString outside = sandbox.configRoot() + QStringLiteral("/outside.conf");
        {
            QFile f(outside);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("FROM = outside@example.com\n");
        }
        const QString old = sandbox.configRoot() + QStringLiteral("/linked.AppImage");
        QVERIFY(QDir().mkpath(old));
        QVERIFY(QFile::link(outside, old + QStringLiteral("/ngPost.conf")));

        const PathHelper::ConfigDirMigrationResult r =
            PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("linked.AppImage"));
        QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
        QVERIFY(!QFileInfo::exists(PathHelper::configFilePath()));
    }

    {
        XdgSandbox sandbox;
        makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
        const QString target = PathHelper::configDir() + QStringLiteral("/ngPost.conf");
        QVERIFY(QFile::link(sandbox.configRoot() + QStringLiteral("/missing.conf"), target));
        QVERIFY(QFileInfo(target).isSymLink());
        QVERIFY(!QFileInfo(target).exists());

        const PathHelper::ConfigDirMigrationResult r =
            PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));
        QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::SkippedTargetConfigured);
        QVERIFY(QFileInfo(target).isSymLink());
    }

    {
        XdgSandbox sandbox;
        makeConfigFolder(sandbox.configRoot(), QStringLiteral("history-link.AppImage"));
        const QString target = PathHelper::configDir()
            + QLatin1Char('/') + PathHelper::historyDbFileName();
        QVERIFY(QFile::link(sandbox.configRoot() + QStringLiteral("/missing.sqlite"), target));
        QVERIFY(QFileInfo(target).isSymLink());
        QVERIFY(!QFileInfo(target).exists());

        const PathHelper::ConfigDirMigrationResult r =
            PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("history-link.AppImage"));
        QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::SkippedTargetConfigured);
        QVERIFY(QFileInfo(target).isSymLink());
    }
#endif
}

void TestPathHelper::migrate_leaves_special_files_untouched()
{
#if defined(Q_OS_UNIX)
    XdgSandbox sandbox;
    const QString old = makeConfigFolder(sandbox.configRoot(), QStringLiteral("fifo.AppImage"));
    const QString fifo = old + QStringLiteral("/notifications.pipe");
    const QByteArray native = QFile::encodeName(fifo);
    QCOMPARE(::mkfifo(native.constData(), 0600), 0);

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("fifo.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QVERIFY(!QFileInfo::exists(PathHelper::configDir() + QStringLiteral("/notifications.pipe")));
    QVERIFY(!r.skipped.filter(QStringLiteral("notifications.pipe")).isEmpty());
#else
    QSKIP("FIFO test is Unix-specific");
#endif
}

void TestPathHelper::migrate_retries_after_a_transient_lock_failure()
{
    XdgSandbox sandbox;
    makeConfigFolder(sandbox.configRoot(), QStringLiteral("old.AppImage"));
    const QString target = PathHelper::configDir();
    QLockFile blocker(target + QStringLiteral("/.ngPost_config_migration.lock"));
    blocker.setStaleLockTime(0);
    QVERIFY(blocker.tryLock(0));

    const PathHelper::ConfigDirMigrationResult first =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));
    QVERIFY(first.status == PathHelper::ConfigDirMigrationStatus::Failed);
    QVERIFY(!QFileInfo::exists(target + QStringLiteral("/ngPost.conf")));
    QVERIFY(!QFileInfo::exists(target + QStringLiteral("/.ngPost_config_migration")));

    blocker.unlock();
    // A different name simulates the next process and bypasses only the
    // deliberate per-process result cache.
    const PathHelper::ConfigDirMigrationResult retry =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("retry.AppImage"));
    QVERIFY(retry.status == PathHelper::ConfigDirMigrationStatus::Migrated);
    QVERIFY(QFileInfo::exists(target + QStringLiteral("/ngPost.conf")));
}

void TestPathHelper::migrate_refuses_to_run_outside_a_test_sandbox()
{
    // No HomeSandbox on purpose: this is the "a test forgot to sandbox itself"
    // case. Nothing may happen, and in particular nothing may be looked for in
    // the developer's real config directory.
    const bool hadHome = qEnvironmentVariableIsSet("NGPOST_TEST_HOME");
    const QByteArray prevHome = qgetenv("NGPOST_TEST_HOME");
    const bool hadDir = qEnvironmentVariableIsSet("NGPOST_TEST_CONFIG_DIR");
    const QByteArray prevDir = qgetenv("NGPOST_TEST_CONFIG_DIR");
    qunsetenv("NGPOST_TEST_HOME");
    qunsetenv("NGPOST_TEST_CONFIG_DIR");

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    if (hadHome) qputenv("NGPOST_TEST_HOME", prevHome);
    if (hadDir)  qputenv("NGPOST_TEST_CONFIG_DIR", prevDir);

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
    QVERIFY(r.legacyDir.isEmpty());
}

void TestPathHelper::migrate_is_a_no_op_under_the_test_config_dir_override()
{
    HomeSandbox sandbox; // keeps NGPOST_TEST_CONFIG_DIR set
    makeConfigFolder(sandbox.xdgConfigHome(), QStringLiteral("old.AppImage"));

    const PathHelper::ConfigDirMigrationResult r =
        PathHelper::migrateAppNamedConfigDirIfNeeded(QStringLiteral("old.AppImage"));

    QVERIFY(r.status == PathHelper::ConfigDirMigrationStatus::NotNeeded);
    QVERIFY(r.legacyDir.isEmpty());
}

void TestPathHelper::marker_remembers_where_the_config_came_from()
{
    // Long after the adoption, the marker is the only thing that still knows
    // which folder the settings came from and which database was left there.
    // Reading it back is what lets ngPost recognise a script still pointing at
    // the old file instead of silently keeping two histories.
    HomeSandbox sandbox;
    const QString cfg = PathHelper::configDirPath();
    QVERIFY(QDir().mkpath(cfg));

    QVERIFY(PathHelper::adoptedLegacyConfigDir().isEmpty());
    QVERIFY(PathHelper::adoptedLegacyHistoryPath().isEmpty());

    const QString legacy = sandbox.xdgConfigHome() + QStringLiteral("/ngPost-old.AppImage");
    const QString db     = legacy + QStringLiteral("/ngPost_history.sqlite");
    const QString marker = cfg + QStringLiteral("/.ngPost_config_migration");
    {
        QFile f(marker);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "# a comment line\n"
            << "date=2026-08-24T10:00:00Z\n"
            << "outcome=migrated\n"
            << "from=" << legacy << "\n"
            << "history=" << db << "\n"
            << "adopted=ngPost.conf vpn\n";
    }
    QCOMPARE(PathHelper::adoptedLegacyConfigDir(), QDir::cleanPath(legacy));
    QCOMPARE(PathHelper::adoptedLegacyHistoryPath(), QDir::cleanPath(db));

    // A marker without a retained database: nothing to diverge from, so the
    // caller must get an empty answer rather than a made up path.
    {
        QFile f(marker);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "from=" << legacy << "\n" << "history=\n";
    }
    QCOMPARE(PathHelper::adoptedLegacyConfigDir(), QDir::cleanPath(legacy));
    QVERIFY(PathHelper::adoptedLegacyHistoryPath().isEmpty());
}

QTEST_APPLESS_MAIN(TestPathHelper)
#include "tst_PathHelper.moc"
