// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_CliParser.cpp — CLI argument parsing surface, exercised by spawning
// the real ngPost binary as a subprocess.
//
// We use QProcess rather than constructing NgPost in-process because
// `NgPost::parseCommandLine` reads `QCoreApplication::arguments()`, which is
// fixed at the QCoreApplication's construction. Varying argv across test
// methods would otherwise require recreating QCoreApplication, which Qt
// doesn't reliably support.
//
// The ngPost binary path is resolved from $NGPOST_BIN, falling back to the
// canonical build location at <repo>/src/ngPost.
//
//========================================================================

#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>

#include "history/PostHistoryStore.h"
#include "NgPost.h"
#include "PostingJob.h"
#include "TestEnv.h"

#ifndef APP_VERSION
#  define APP_VERSION "0.0.0"
#endif

using ngpost::tests::HomeSandbox;
using ngpost::tests::locateNgPostBinary;

namespace
{

struct RunResult
{
    int     exitCode;
    QString stdoutText;
    QString stderrText;
    bool    timedOut;
};

RunResult run(const QString &bin,
              const QStringList &args,
              const QString &sandboxHome,
              const QString &workingDirectory = QString())
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", sandboxHome);
    env.insert("XDG_CONFIG_HOME", sandboxHome + QStringLiteral("/.config"));
    env.insert("APPDATA", sandboxHome);
    env.insert("LOCALAPPDATA", sandboxHome);
    env.insert("USERPROFILE", sandboxHome);
    p.setProcessEnvironment(env);
    if (!workingDirectory.isEmpty())
        p.setWorkingDirectory(workingDirectory);
    p.start(bin, args);

    RunResult r{ -1, QString(), QString(), false };
    if (!p.waitForFinished(10000)) {
        r.timedOut = true;
        p.kill();
        p.waitForFinished(2000);
        return r;
    }
    r.exitCode   = p.exitCode();
    r.stdoutText = QString::fromLocal8Bit(p.readAllStandardOutput());
    r.stderrText = QString::fromLocal8Bit(p.readAllStandardError());
    return r;
}

qint64 createResumablePost(const QString &dbPath, const QString &sourcePath, QString *error)
{
    PostHistoryStore store(dbPath, true);
    if (!store.initialize(error))
        return 0;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("cli-resume.nzb");
    post.nzbPath = QFileInfo(sourcePath).absolutePath() + QStringLiteral("/cli-resume.nzb");
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, error);
    if (!postId)
        return 0;

    QFileInfo sourceInfo(sourcePath);
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = sourceInfo.absoluteFilePath();
    file.postedName = sourceInfo.fileName();
    file.sizeBytes = sourceInfo.size();
    file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
    file.totalArticles = 1;
    file.groups = post.groups;
    if (!store.upsertFile(file, error))
        return 0;

    return postId;
}

} // namespace

class TestCliParser : public QObject
{
    Q_OBJECT

private:
    QString _bin;

private slots:
    void initTestCase();

    //! `--version` prints the version banner and exits 0.
    void version_flag_succeeds();

    //! `--help` lists the user-facing flags (sanity-check a few well-known ones).
    void help_lists_major_flags();

    //! Read-only CLI commands and an explicit -c never adopt a default
    //! executable-named configuration folder as a side effect.
    void inspection_and_explicit_config_do_not_adopt();

    //! An unknown flag fails with a non-zero exit code. Currently
    //! ERR_WRONG_ARG = 3 but tests assert "non-zero" for resilience to enum
    //! reordering.
    void unknown_flag_rejected();

    //! `--vpn --no_vpn` together is contradictory and must be rejected with a
    //! "mutually exclusive" message.
    void vpn_and_no_vpn_mutually_exclusive();

    //! `--vpn_profile <name>` referring to a profile that does not exist in
    //! the config is rejected with a clear "does not match any profile"
    //! message.
    void vpn_profile_unknown_rejected();

    //! `--auto <dir>` without `--compress` must error with ERR_AUTO_NO_COMPRESS.
    void auto_dir_without_compress_rejected();

    //! Resume commands should work through dash/underscore aliases and dry-run
    //! without requiring normal posting input.
    void resume_commands_accept_aliases_and_dry_run();

    //! GUI PAR2_PCT must override PAR2_ARGS redundancy while preserving
    //! ParPar's percentage syntax.
    void par2_args_redundancy_override_for_parpar();

    //! ParPar's built-in default must let ParPar choose a slice size to stay
    //! under its hard 32768-slice limit on large posts.
    void parpar_default_args_use_auto_slice_size();

    //! Older bundled/default configs used a fixed ParPar slice size; normalize
    //! those known values so existing configs stop tripping the slice cap.
    void parpar_legacy_slice_size_uses_auto_slice_size();

    //! Repair configs that were temporarily written with --auto-slice-size
    //! but without ParPar's required --input-slices value.
    void parpar_auto_slice_size_without_input_slices_gets_default();

    //! GUI PAR2_PCT must override PAR2_ARGS redundancy for par2cmdline too.
    void par2_args_redundancy_override_for_par2cmdline();

    //! MultiPar (par2j) defaults must not leak par2cmdline-only flags (-l/-m).
    void multipar_default_args_use_only_slash_switches();
    //! GUI PAR2_PCT must override PAR2_ARGS redundancy for MultiPar (/rr) too.
    void par2_args_redundancy_override_for_multipar();

    //! A file that could not be read makes the post partial, not successful:
    //! such a file never produces a failed article, it is simply set aside.
    void unreadable_file_makes_the_post_not_successful();

    //! Failing before a connection/NZB transfer exists must still close the
    //! constructor-created history row as failed, never leave it "posting".
    void pre_transfer_failure_finalizes_history();

    //! A per-server VPN requirement can be rejected synchronously, before the
    //! application event loop starts. The CLI must still exit and must not
    //! leave a constructor-created phantom history row.
    void blocked_vpn_admission_exits_without_hanging_or_history_ghost();

    //! A metadata value very often holds a URL with its own '=' signs; the
    //! pair must be split on the first one only.
    void meta_value_may_contain_equal_signs();

    //! Claiming the same key as public and private at once is refused instead
    //! of silently resolved one way or the other.
    void meta_key_cannot_be_public_and_private_at_once();

    //! A pair without '=' is reported rather than silently dropped.
    void malformed_meta_is_reported();

    //! --export_post_info writes the record sheet of an old post: to a file,
    //! or to stdout with nothing else mixed in. An unknown id fails.
    void export_post_info_writes_to_file_or_stdout();

    //! Every post automation setting is reachable from the command line, in
    //! both directions for the booleans, so ngPost can be driven without a
    //! configuration file at all.
    void post_automation_settings_are_reachable_from_the_cli();

    //! A relative automatic output explicitly supplied by the CLI belongs to
    //! the caller's current directory, not beside a separately loaded config.
    void relative_post_info_output_from_cli_uses_cwd();

    //! A timeout must be a number of seconds, and a bad one is refused rather
    //! than silently ignored.
    void post_cmd_timeout_rejects_a_non_numeric_value();

    //! Zero or negative article sizes cannot form valid byte ranges and are
    //! rejected equally from CLI and configuration.
    void article_size_must_be_positive();

    //! Only ftp, http and https can receive an nzb.
    void nzb_upload_url_rejects_an_unsupported_scheme();
};

void TestCliParser::initTestCase()
{
    _bin = locateNgPostBinary();
    if (_bin.isEmpty()) {
        QSKIP("ngPost binary not found. Build it first or set NGPOST_BIN.");
    }
    qInfo() << "Using binary:" << _bin;
}

void TestCliParser::version_flag_succeeds()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--version" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "ngPost --version timed out");
    QCOMPARE(r.exitCode, 0);
    QVERIFY2(r.stdoutText.contains(QLatin1String(APP_VERSION)),
             qPrintable(QStringLiteral("stdout did not mention version: %1").arg(r.stdoutText)));
}

void TestCliParser::help_lists_major_flags()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--help" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "ngPost --help timed out");
    QCOMPARE(r.exitCode, 0);

    const QString out = r.stdoutText + r.stderrText;
    for (const char *flag : { "--vpn", "--vpn_profile", "--auto", "--monitor",
                              "--history", "--history-show", "--history_show",
                              "--resume-list", "--resume_list",
                              "--resume-check", "--resume_check",
                              "--resume-post", "--resume_post",
                              "--resume-all", "--resume_all",
                              "--resume-abandon", "--resume_abandon",
                              "--resume-purge", "--resume_purge",
                              "--regenerate-nzb", "--regenerate_nzb" }) {
        QVERIFY2(out.contains(QString::fromLatin1(flag)),
                 qPrintable(QStringLiteral("help output did not mention '%1'").arg(QString::fromLatin1(flag))));
    }
}

void TestCliParser::inspection_and_explicit_config_do_not_adopt()
{
    HomeSandbox sandbox;
#if defined(Q_OS_MAC)
    const QString configRoot = sandbox.rootPath() + QStringLiteral("/Library/Application Support");
#elif defined(Q_OS_WIN)
    const QString configRoot = sandbox.rootPath();
#else
    const QString configRoot = sandbox.xdgConfigHome();
#endif
    const QString oldDir = configRoot + QStringLiteral("/old.AppImage");
    QVERIFY(QDir().mkpath(oldDir));
    const QString oldConf = oldDir + QStringLiteral("/ngPost.conf");
    const QString oldDb = oldDir + QStringLiteral("/ngPost_history.sqlite");
    const QString sourcePath = sandbox.rootPath() + QStringLiteral("/existing-post.bin");
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("history payload");
    }
    QString historyError;
    const qint64 existingPostId = createResumablePost(oldDb, sourcePath, &historyError);
    QVERIFY2(existingPostId > 0, qPrintable(historyError));
    {
        QFile file(oldConf);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("FROM = old@example.invalid\nPOST_DB = %1\n")
                       .arg(oldDb)
                       .toUtf8());
    }
    const QByteArray original = [] (const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }(oldConf);
    const QString canonicalDir = configRoot + QStringLiteral("/ngPost");
    const QString adoptedConf = canonicalDir + QStringLiteral("/ngPost.conf");
    // HomeSandbox pre-creates its in-process NGPOST_TEST_CONFIG_DIR. The
    // spawned production binary must start with no canonical folder so this
    // test can detect even an otherwise-empty write by inspection commands.
    if (QFileInfo::exists(canonicalDir))
        QVERIFY(QDir().rmdir(canonicalDir));

    for (const QStringList &args : { QStringList{ "--version" },
                                    QStringList{ "--help" },
                                    QStringList{ "--definitely-invalid-option" },
                                    QStringList{ "-c", oldConf } }) {
        const RunResult r = run(_bin, args, sandbox.rootPath());
        QVERIFY2(!r.timedOut, qPrintable(args.join(QLatin1Char(' '))));
        QVERIFY2(!QFileInfo::exists(adoptedConf),
                 qPrintable(QStringLiteral("read-only invocation adopted config: %1")
                                .arg(args.join(QLatin1Char(' ')))));
        QVERIFY2(!QFileInfo::exists(canonicalDir),
                 qPrintable(QStringLiteral("read-only invocation created the config folder: %1")
                                .arg(args.join(QLatin1Char(' ')))));
        QFile source(oldConf);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), original);
    }

    // A legacy 4.x file newer than the adopted config triggers a warning in
    // parseDefaultConfig(). That warning is diagnostic stderr too: it must not
    // be able to corrupt a JSON history stream.
    const QString legacy4Config = sandbox.rootPath() + QStringLiteral("/.ngPost");
    {
        QFile legacy(legacy4Config);
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write("FROM = legacy4@example.invalid\n");
        QVERIFY(legacy.setFileTime(QDateTime::currentDateTime().addSecs(3600),
                                   QFileDevice::FileModificationTime));
    }

    // A real default-config command does adopt it, but its one-time notice is
    // diagnostic stderr: machine-readable stdout remains valid JSON.
    const RunResult migrated =
        run(_bin, { "--history", "--json", "--quiet", "--lang", "fr" },
            sandbox.rootPath());
    QVERIFY2(!migrated.timedOut, "history command timed out");
    QCOMPARE(migrated.exitCode, 0);
    QJsonParseError jsonError;
    const QJsonDocument json =
        QJsonDocument::fromJson(migrated.stdoutText.trimmed().toUtf8(), &jsonError);
    QCOMPARE(jsonError.error, QJsonParseError::NoError);
    QVERIFY(json.isArray());
    QCOMPARE(json.array().size(), 1);
    QCOMPARE(json.array().first().toObject().value(QStringLiteral("id")).toInteger(),
             existingPostId);
    QVERIFY(QFileInfo::exists(adoptedConf));
    QVERIFY2(QFileInfo::exists(oldDb), "the existing history database was moved");
    QVERIFY2(!QFileInfo::exists(canonicalDir + QStringLiteral("/ngPost_history.sqlite")),
             "a shadow history database was created in the canonical folder");
    QVERIFY2(!migrated.stderrText.isEmpty(), "migration was not reported on stderr");
}

void TestCliParser::unknown_flag_rejected()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--bogus-flag-does-not-exist" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0,
             qPrintable(QStringLiteral("unknown flag should fail; got exit=%1").arg(r.exitCode)));
    // Error path uses _error() which writes to stderr.
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("syntax", Qt::CaseInsensitive)
                 || out.contains("Unknown option", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected a syntax error message, got: %1").arg(out)));
}

void TestCliParser::vpn_and_no_vpn_mutually_exclusive()
{
    HomeSandbox sandbox;

    // Need at least one input source to bypass the earlier ERR_NO_INPUT check
    // and reach the VPN mutex check.
    const QString stub = sandbox.rootPath() + QStringLiteral("/in.bin");
    QFile f(stub);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();

    const RunResult r = run(_bin, { "--vpn", "--no_vpn", "-i", stub }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for --vpn + --no_vpn");
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("mutually exclusive", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected 'mutually exclusive' in output, got: %1").arg(out)));
}

void TestCliParser::vpn_profile_unknown_rejected()
{
    HomeSandbox sandbox;

    const QString stub = sandbox.rootPath() + QStringLiteral("/in.bin");
    QFile f(stub);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();

    const RunResult r = run(_bin,
                            { "--vpn_profile", "NotInTheConfig", "-i", stub },
                            sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for unknown vpn profile");
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("does not match any profile", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected profile-mismatch error, got: %1").arg(out)));
}

void TestCliParser::auto_dir_without_compress_rejected()
{
    HomeSandbox sandbox;
    const QString autoDir = sandbox.rootPath() + QStringLiteral("/autoDir");
    QDir().mkpath(autoDir);

    const RunResult r = run(_bin, { "--auto", autoDir }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for --auto without --compress");
}

void TestCliParser::resume_commands_accept_aliases_and_dry_run()
{
    HomeSandbox sandbox;

    const QString sourcePath = sandbox.rootPath() + QStringLiteral("/resume.bin");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("resume me");
    source.close();

    const QString dbPath = sandbox.rootPath() + QStringLiteral("/history.sqlite");
    QString err;
    const qint64 postId = createResumablePost(dbPath, sourcePath, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    const RunResult listDash = run(_bin, { "--resume-list", "--post_db", dbPath },
                                   sandbox.rootPath());
    QVERIFY2(!listDash.timedOut, "resume-list timed out");
    QCOMPARE(listDash.exitCode, 0);
    QVERIFY2(listDash.stdoutText.contains(QString::number(postId)),
             qPrintable(QStringLiteral("resume-list did not include post %1:\n%2")
                            .arg(postId).arg(listDash.stdoutText + listDash.stderrText)));

    const RunResult listUnderscore = run(_bin, { "--resume_list", "--post_db", dbPath },
                                         sandbox.rootPath());
    QVERIFY2(!listUnderscore.timedOut, "resume_list timed out");
    QCOMPARE(listUnderscore.exitCode, 0);
    QVERIFY2(listUnderscore.stdoutText.contains(QString::number(postId)),
             qPrintable(QStringLiteral("resume_list did not include post %1:\n%2")
                            .arg(postId).arg(listUnderscore.stdoutText + listUnderscore.stderrText)));

    const RunResult check = run(_bin, { "--resume-check", QString::number(postId),
                                        "--post_db", dbPath },
                                sandbox.rootPath());
    QVERIFY2(!check.timedOut, "resume-check timed out");
    QCOMPARE(check.exitCode, 0);
    QVERIFY2(check.stdoutText.contains(QStringLiteral("state: resumable")),
             qPrintable(QStringLiteral("resume-check output was:\n%1")
                            .arg(check.stdoutText + check.stderrText)));

    const RunResult postDryRun = run(_bin, { "--resume-post", QString::number(postId),
                                             "--dry-run", "--post_db", dbPath },
                                     sandbox.rootPath());
    QVERIFY2(!postDryRun.timedOut, "resume-post dry-run timed out");
    QCOMPARE(postDryRun.exitCode, 0);
    QVERIFY2(postDryRun.stdoutText.contains(QStringLiteral("pending: 1")),
             qPrintable(QStringLiteral("resume-post dry-run output was:\n%1")
                            .arg(postDryRun.stdoutText + postDryRun.stderrText)));

    const RunResult allDryRun = run(_bin, { "--resume-all", "--dry-run", "--post_db", dbPath },
                                    sandbox.rootPath());
    QVERIFY2(!allDryRun.timedOut, "resume-all dry-run timed out");
    QCOMPARE(allDryRun.exitCode, 0);
    QVERIFY2(allDryRun.stdoutText.contains(QString::number(postId)),
             qPrintable(QStringLiteral("resume-all dry-run output was:\n%1")
                            .arg(allDryRun.stdoutText + allDryRun.stderrText)));
}

void TestCliParser::par2_args_redundancy_override_for_parpar()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QStringLiteral("-s1M --auto-slice-size -r1n*0.6 -m2048M -p1l --progress stdout -q"),
        true,
        false,
        8);

    QCOMPARE(args, QStringList({
        QStringLiteral("-s1M"),
        QStringLiteral("--auto-slice-size"),
        QStringLiteral("-r8%"),
        QStringLiteral("-m2048M"),
        QStringLiteral("-p1l"),
        QStringLiteral("--progress"),
        QStringLiteral("stdout"),
        QStringLiteral("-q"),
    }));
}

void TestCliParser::parpar_default_args_use_auto_slice_size()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QString(),
        true,
        false,
        8);

    QCOMPARE(args, QStringList({
        QStringLiteral("-s1M"),
        QStringLiteral("--auto-slice-size"),
        QStringLiteral("-m1024M"),
        QStringLiteral("-r8%"),
    }));
}

void TestCliParser::parpar_legacy_slice_size_uses_auto_slice_size()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QStringLiteral("-s1M -r1n*0.6 -m2048M -p1l --progress stdout -q"),
        true,
        false,
        8);

    QCOMPARE(args, QStringList({
        QStringLiteral("-s1M"),
        QStringLiteral("--auto-slice-size"),
        QStringLiteral("-r8%"),
        QStringLiteral("-m2048M"),
        QStringLiteral("-p1l"),
        QStringLiteral("--progress"),
        QStringLiteral("stdout"),
        QStringLiteral("-q"),
    }));
}

void TestCliParser::parpar_auto_slice_size_without_input_slices_gets_default()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QStringLiteral("--auto-slice-size -r1n*0.6 -m2048M -p1l --progress stdout -q"),
        true,
        false,
        8);

    QCOMPARE(args, QStringList({
        QStringLiteral("-s1M"),
        QStringLiteral("--auto-slice-size"),
        QStringLiteral("-r8%"),
        QStringLiteral("-m2048M"),
        QStringLiteral("-p1l"),
        QStringLiteral("--progress"),
        QStringLiteral("stdout"),
        QStringLiteral("-q"),
    }));
}

void TestCliParser::par2_args_redundancy_override_for_par2cmdline()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QStringLiteral("c -l -m1024 -r8 -s768000"),
        false,
        false,
        12);

    QCOMPARE(args, QStringList({
        QStringLiteral("c"),
        QStringLiteral("-l"),
        QStringLiteral("-m1024"),
        QStringLiteral("-r12"),
        QStringLiteral("-s768000"),
    }));
}

void TestCliParser::multipar_default_args_use_only_slash_switches()
{
    // useMultiPar=true. par2j rejects par2cmdline's -l / -m1024, so the
    // default must be just the create verb + the /rr redundancy switch.
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QString(),
        false,
        true,
        8);

    QCOMPARE(args, QStringList({
        QStringLiteral("c"),
        QStringLiteral("/rr8"),
    }));
}

void TestCliParser::par2_args_redundancy_override_for_multipar()
{
    const QStringList args = PostingJob::buildPar2ArgsForTest(
        QStringLiteral("c /rr8 /lc4"),
        false,
        true,
        12);

    QCOMPARE(args, QStringList({
        QStringLiteral("c"),
        QStringLiteral("/rr12"),
        QStringLiteral("/lc4"),
    }));
}

namespace
{
//! Runs ngPost with the given metadata options on a stub input file, without
//! any server configured: the run fails later on, what matters here is whether
//! the metadata itself was accepted.
RunResult runWithMeta(const QString &bin, const QStringList &metaArgs, HomeSandbox &sandbox)
{
    const QString stub = sandbox.rootPath() + QStringLiteral("/in.bin");
    QFile f(stub);
    f.open(QIODevice::WriteOnly);
    f.write("hello");
    f.close();

    QStringList args{ "-i", stub };
    args += metaArgs;
    return run(bin, args, sandbox.rootPath());
}
} // namespace

void TestCliParser::meta_value_may_contain_equal_signs()
{
    QString key, value;

    // the case that used to be dropped without a word: a URL of its own
    QVERIFY(NgPost::splitMetaPair(
        QStringLiteral("gallery=https://example.org/albums/view?id=326598&size=full"),
        &key,
        &value));
    QCOMPARE(key, QStringLiteral("gallery"));
    QCOMPARE(value, QStringLiteral("https://example.org/albums/view?id=326598&size=full"));

    // an empty value is a legitimate way to blank a field of a record sheet
    QVERIFY(NgPost::splitMetaPair(QStringLiteral("comment="), &key, &value));
    QCOMPARE(key, QStringLiteral("comment"));
    QVERIFY(value.isEmpty());

    // spaces around the name are forgiven, the value is kept verbatim
    QVERIFY(NgPost::splitMetaPair(QStringLiteral(" album = Mes photos "), &key, &value));
    QCOMPARE(key, QStringLiteral("album"));
    QCOMPARE(value, QStringLiteral(" Mes photos "));

    QVERIFY(!NgPost::splitMetaPair(QStringLiteral("noEqualSignHere"), &key, &value));
    QVERIFY(!NgPost::splitMetaPair(QStringLiteral("=orphanValue"), &key, &value));
}

void TestCliParser::meta_key_cannot_be_public_and_private_at_once()
{
    HomeSandbox sandbox;
    const RunResult r =
        runWithMeta(_bin, { "--meta", "album=Public", "--post_meta", "album=Private" }, sandbox);

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, qPrintable(QStringLiteral("expected a failure, got %1").arg(r.exitCode)));
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains(QStringLiteral("album")), qPrintable(out));
}

void TestCliParser::malformed_meta_is_reported()
{
    HomeSandbox sandbox;
    const RunResult r = runWithMeta(_bin, { "--meta", "noEqualSignHere" }, sandbox);

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, qPrintable(QStringLiteral("expected a failure, got %1").arg(r.exitCode)));
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains(QStringLiteral("noEqualSignHere")), qPrintable(out));
}

void TestCliParser::export_post_info_writes_to_file_or_stdout()
{
    HomeSandbox sandbox;

    const QString sourcePath = sandbox.rootPath() + QStringLiteral("/export.bin");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("export me");
    source.close();

    const QString dbPath = sandbox.rootPath() + QStringLiteral("/history.sqlite");
    QString err;
    const qint64 postId = createResumablePost(dbPath, sourcePath, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    // Deliberately hostile document content: the stdout path used to bypass
    // directives, comment stripping and escaping, making this invalid JSON
    // while the file path was correct.
    const QString hostile = QStringLiteral("quote=\" slash=\\ newline=\n tab=\t control=")
                            + QChar(1) + QStringLiteral(" end");
    {
        PostHistoryStore store(dbPath, true);
        QMap<QString, MetaValue> meta;
        meta.insert(QStringLiteral("title"), MetaValue(hostile, MetaScope::Local));
        QVERIFY2(store.setPostMeta(postId, meta, &err), qPrintable(err));
    }

    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    QFile tmpl(tmplPath);
    QVERIFY(tmpl.open(QIODevice::WriteOnly));
    tmpl.write("#!json\n"
               "# deliberately not valid JSON if this comment reaches stdout\n"
               "{\"name\":\"__nzbName__\","
               "\"id\":\"__postId__\","
               "\"title\":\"__meta:title__\"}\n");
    tmpl.close();

    const QString outPath = sandbox.rootPath() + QStringLiteral("/sheet.json");
    const RunResult toFile = run(_bin,
                                 { "--export_post_info", QString::number(postId),
                                   "--post_info_template", tmplPath, "-o", outPath,
                                   "--post_db", dbPath },
                                 sandbox.rootPath());
    QVERIFY2(!toFile.timedOut, "export to file timed out");
    QCOMPARE(toFile.exitCode, 0);
    QVERIFY2(QFileInfo::exists(outPath),
             qPrintable(toFile.stdoutText + toFile.stderrText));

    QFile written(outPath);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(written.readAll());
    QJsonParseError fileParseError;
    const QJsonDocument fileDocument =
        QJsonDocument::fromJson(content.toUtf8(), &fileParseError);
    QVERIFY2(fileParseError.error == QJsonParseError::NoError,
             qPrintable(fileParseError.errorString() + QStringLiteral(" | ") + content));
    QCOMPARE(fileDocument.object().value(QStringLiteral("id")).toString(),
             QString::number(postId));
    QCOMPARE(fileDocument.object().value(QStringLiteral("title")).toString(), hostile);

    const RunResult toStdout = run(_bin,
                                   { "--export-post-info", QString::number(postId),
                                     "--post_info_template", tmplPath, "--post_db", dbPath },
                                   sandbox.rootPath());
    QVERIFY2(!toStdout.timedOut, "export to stdout timed out");
    QCOMPARE(toStdout.exitCode, 0);
    // stdout carries the record sheet and nothing else: a caller pipes it
    QString stdoutContent = toStdout.stdoutText;
    stdoutContent.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QString fileContent = content;
    fileContent.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QCOMPARE(stdoutContent, fileContent);
    QJsonParseError stdoutParseError;
    const QJsonDocument stdoutDocument =
        QJsonDocument::fromJson(stdoutContent.toUtf8(), &stdoutParseError);
    QVERIFY2(stdoutParseError.error == QJsonParseError::NoError,
             qPrintable(stdoutParseError.errorString() + QStringLiteral(" | ") + stdoutContent));
    QCOMPARE(stdoutDocument.object().value(QStringLiteral("title")).toString(), hostile);
    QVERIFY(!stdoutContent.contains(QStringLiteral("#!json")));
    QVERIFY(!stdoutContent.contains(QStringLiteral("not valid JSON")));

    const RunResult overwriteDb = run(_bin,
                                      { "--export_post_info", QString::number(postId),
                                        "--post_info_template", tmplPath, "-o", dbPath,
                                        "--post_db", dbPath },
                                      sandbox.rootPath());
    QVERIFY2(!overwriteDb.timedOut, "database overwrite guard timed out");
    QVERIFY2(overwriteDb.exitCode != 0,
             qPrintable(overwriteDb.stdoutText + overwriteDb.stderrText));

    const RunResult regenerateOverDb = run(_bin,
                                           { "--regenerate_nzb", QString::number(postId),
                                             "-o", dbPath, "--post_db", dbPath },
                                           sandbox.rootPath());
    QVERIFY2(!regenerateOverDb.timedOut, "NZB database overwrite guard timed out");
    QVERIFY2(regenerateOverDb.exitCode != 0,
             qPrintable(regenerateOverDb.stdoutText + regenerateOverDb.stderrText));

    PostHistoryStore stillThere(dbPath, true);
    const QList<PostHistoryStore::PostSummary> surviving =
        stillThere.listPosts(QString(), QString(), false, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(surviving.size(), 1);
    QCOMPARE(surviving.first().id, postId);

    const QString csvPath = sandbox.rootPath() + QStringLiteral("/legacy-history.csv");
    const QByteArray csvBefore(
        "date;nzb name;size;avg. speed;archive name;archive pass;groups;from\n"
        "2026/08/24 12:00:00;old.nzb;42;1 MB/s;old;;alt.test;poster@test\n");
    {
        QFile csv(csvPath);
        QVERIFY(csv.open(QIODevice::WriteOnly));
        QCOMPARE(csv.write(csvBefore), csvBefore.size());
    }
    const RunResult overwriteCsv = run(_bin,
                                       { "--export_post_info", QString::number(postId),
                                         "--post_info_template", tmplPath, "-o", csvPath,
                                         "--post_db", dbPath, "--post_history", csvPath },
                                       sandbox.rootPath());
    QVERIFY2(!overwriteCsv.timedOut, "CSV overwrite guard timed out");
    QVERIFY2(overwriteCsv.exitCode != 0,
             qPrintable(overwriteCsv.stdoutText + overwriteCsv.stderrText));
    QFile csvAfter(csvPath);
    QVERIFY(csvAfter.open(QIODevice::ReadOnly));
    QCOMPARE(csvAfter.readAll(), csvBefore);

    const RunResult unknown = run(_bin,
                                  { "--export_post_info", "999999",
                                    "--post_info_template", tmplPath, "--post_db", dbPath },
                                  sandbox.rootPath());
    QVERIFY2(!unknown.timedOut, "export of an unknown id timed out");
    QVERIFY2(unknown.exitCode != 0,
             qPrintable(QStringLiteral("an unknown id should fail, got %1").arg(unknown.exitCode)));
    QVERIFY(unknown.stdoutText.isEmpty());
}

void TestCliParser::post_automation_settings_are_reachable_from_the_cli()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--help" }, sandbox.rootPath());
    QVERIFY2(!r.timedOut, "ngPost --help timed out");

    const QString out = r.stdoutText + r.stderrText;
    for (const char *flag : { "--post_info_only_on_success", "--no_post_info_only_on_success",
                              "--nzb_post_cmd", "--post_cmd_timeout",
                              "--post_cmd_fail_is_error", "--no_post_cmd_fail_is_error",
                              "--post_cmd_expose_password", "--no_post_cmd_expose_password",
                              "--nzb_upload_url", "--nzb_upload_timeout", "--post_history",
                              "--no_post_info" }) {
        QVERIFY2(out.contains(QString::fromLatin1(flag)),
                 qPrintable(QStringLiteral("help does not mention '%1'").arg(QString::fromLatin1(flag))));
    }

    // and they are accepted together on a real command line
    const RunResult accepted = runWithMeta(_bin,
                                           { "--no_post_info",
                                             "--no_post_info_only_on_success",
                                             "--post_cmd_timeout", "30",
                                             "--post_cmd_fail_is_error",
                                             "--no_post_cmd_expose_password",
                                             "--nzb_upload_timeout", "60",
                                             "--nzb_post_cmd", "/bin/true" },
                                           sandbox);
    QVERIFY2(!accepted.timedOut, "process timed out");
    const QString acceptedOut = accepted.stdoutText + accepted.stderrText;
    QVERIFY2(!acceptedOut.contains(QStringLiteral("Unknown option")), qPrintable(acceptedOut));
}

void TestCliParser::relative_post_info_output_from_cli_uses_cwd()
{
    HomeSandbox sandbox;
    const QString configDir = sandbox.rootPath() + QStringLiteral("/conf");
    const QString workDir = sandbox.rootPath() + QStringLiteral("/work");
    QVERIFY(QDir().mkpath(configDir));
    QVERIFY(QDir().mkpath(workDir));

    const QString confPath = configDir + QStringLiteral("/ngPost.conf");
    QFile conf(confPath);
    QVERIFY(conf.open(QIODevice::WriteOnly));
    conf.write("NO_RESUME_AUTO = true\n");
    conf.close();

    const QString inputPath = sandbox.rootPath() + QStringLiteral("/payload.bin");
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("payload");
    input.close();

    const QString templatePath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    QFile model(templatePath);
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.write("status=__status__\n");
    model.close();

    // Reserve a definitely local port, then release it: connecting to it is a
    // fast, deterministic failure. The NZB has already been opened at that
    // point, so the explicitly allowed failure sheet is still generated.
    QTcpServer portPicker;
    QVERIFY(portPicker.listen(QHostAddress::LocalHost, 0));
    const quint16 unusedPort = portPicker.serverPort();
    portPicker.close();

    const QString relativeOutput = QStringLiteral("post-sheet.txt");
    const QString nzbPath = workDir + QStringLiteral("/failed-post.nzb");
    const RunResult result =
        run(_bin,
            { "-c", confPath,
              "-i", inputPath,
              "-h", "127.0.0.1",
              "-P", QString::number(unusedPort),
              "-n", "1",
              "-o", nzbPath,
              "--post_info_template", templatePath,
              "--post_info_output", relativeOutput,
              "--no_post_info_only_on_success" },
            sandbox.rootPath(),
            workDir);
    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));
    QVERIFY2(QFileInfo::exists(workDir + QLatin1Char('/') + relativeOutput),
             qPrintable(result.stdoutText + result.stderrText));
    QVERIFY2(!QFileInfo::exists(configDir + QLatin1Char('/') + relativeOutput),
             "the CLI-relative output was resolved beside ngPost.conf");
}

void TestCliParser::post_cmd_timeout_rejects_a_non_numeric_value()
{
    HomeSandbox sandbox;
    const RunResult r = runWithMeta(_bin, { "--post_cmd_timeout", "soon" }, sandbox);

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, qPrintable(QStringLiteral("expected a failure, got %1").arg(r.exitCode)));
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains(QStringLiteral("post_cmd_timeout")), qPrintable(out));
}

void TestCliParser::article_size_must_be_positive()
{
    HomeSandbox cliSandbox;
    const RunResult cli = runWithMeta(_bin, { "--article_size", "0" }, cliSandbox);
    QVERIFY2(!cli.timedOut, "process timed out");
    QVERIFY2(cli.exitCode != 0, qPrintable(cli.stdoutText + cli.stderrText));
    QVERIFY2((cli.stdoutText + cli.stderrText).contains(QStringLiteral("positive integer")),
             qPrintable(cli.stdoutText + cli.stderrText));

    HomeSandbox confSandbox;
    const QString confPath = confSandbox.rootPath() + QStringLiteral("/invalid.conf");
    QFile conf(confPath);
    QVERIFY(conf.open(QIODevice::WriteOnly));
    conf.write("ARTICLE_SIZE = -1\n");
    conf.close();

    const QString stub = confSandbox.rootPath() + QStringLiteral("/in.bin");
    QFile input(stub);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("hello");
    input.close();

    const RunResult fromConf =
        run(_bin, { "-c", confPath, "-i", stub }, confSandbox.rootPath());
    QVERIFY2(!fromConf.timedOut, "process timed out");
    QVERIFY2(fromConf.exitCode != 0,
             qPrintable(fromConf.stdoutText + fromConf.stderrText));
    QVERIFY2((fromConf.stdoutText + fromConf.stderrText)
                 .contains(QStringLiteral("positive integer")),
             qPrintable(fromConf.stdoutText + fromConf.stderrText));
}

void TestCliParser::nzb_upload_url_rejects_an_unsupported_scheme()
{
    HomeSandbox sandbox;
    const RunResult r = runWithMeta(_bin, { "--nzb_upload_url", "sftp://box/nzbs" }, sandbox);

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, qPrintable(QStringLiteral("expected a failure, got %1").arg(r.exitCode)));
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains(QStringLiteral("ftp")), qPrintable(out));
}

void TestCliParser::unreadable_file_makes_the_post_not_successful()
{
    // every article went through and nothing was set aside
    QVERIFY(PostingJob::postSucceeded(true, 0, false));

    // a whole file could not be read: no article failed, yet the post is not complete
    QVERIFY(!PostingJob::postSucceeded(true, 0, true));

    // the usual failure modes still count
    QVERIFY(!PostingJob::postSucceeded(true, 3, false));
    QVERIFY(!PostingJob::postSucceeded(false, 0, false));
}

void TestCliParser::pre_transfer_failure_finalizes_history()
{
    HomeSandbox sandbox;
    const QString inputPath = sandbox.rootPath() + QStringLiteral("/payload.bin");
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("payload");
    input.close();

    const QString dbPath = sandbox.rootPath() + QStringLiteral("/history.sqlite");
    const RunResult result =
        run(_bin, { "-i", inputPath, "--post_db", dbPath }, sandbox.rootPath());
    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));

    PostHistoryStore store(dbPath, true);
    QString error;
    const QList<PostHistoryStore::PostSummary> posts =
        store.listPosts(QString(), QString(), false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(posts.size(), 1);
    QCOMPARE(posts.first().status, QStringLiteral("failed"));
}

void TestCliParser::blocked_vpn_admission_exits_without_hanging_or_history_ghost()
{
    HomeSandbox sandbox;
    const QString inputPath = sandbox.rootPath() + QStringLiteral("/vpn-payload.bin");
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("payload");
    input.close();

    const QString dbPath = sandbox.rootPath() + QStringLiteral("/vpn-history.sqlite");
    const QString confPath = sandbox.rootPath() + QStringLiteral("/vpn-blocked.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream conf(&config);
    conf << "POST_DB = " << dbPath << "\n"
         << "[server]\n"
         << "host = news.example.invalid\n"
         << "port = 119\n"
         << "enabled = true\n"
         << "useVpn = true\n"
         << "connection = 1\n";
    config.close();

    const RunResult result = run(_bin, { "-c", confPath, "-i", inputPath }, sandbox.rootPath());
    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));
    QVERIFY2(result.exitCode != 0, qPrintable(result.stdoutText + result.stderrText));
    QVERIFY2((result.stdoutText + result.stderrText).contains(QStringLiteral("VPN")),
             qPrintable(result.stdoutText + result.stderrText));

    PostHistoryStore store(dbPath, true);
    QString error;
    const QList<PostHistoryStore::PostSummary> posts =
        store.listPosts(QString(), QString(), false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(posts.isEmpty());
}

QTEST_MAIN(TestCliParser)
#include "tst_CliParser.moc"
