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
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>

#include "history/PostHistoryStore.h"
#include "NgPost.h"
#include "NzbCheck.h"
#include "PostingJob.h"
#include "MockNntpServer.h"
#include "TestEnv.h"

#ifndef APP_VERSION
#  define APP_VERSION "0.0.0"
#endif

using ngpost::tests::HomeSandbox;
using ngpost::tests::locateNgPostBinary;
using ngpost::tests::MockNntpServer;

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

    //! A configuration written before "[server]" blocks existed puts HOST,
    //! USER and PASS at the top level. Those keys used to be written into a
    //! server object no block had created, which crashed the process.
    void server_keys_without_a_block_are_not_a_crash();

    //! A check-enabled server with zero configured connections cannot ever
    //! emit a disconnect signal; reject it before entering the event loop.
    void check_with_zero_connections_does_not_hang();

    //! Machine-readable check output remains exactly one JSON document even
    //! when verbose diagnostics were explicitly requested.
    void check_json_stdout_is_a_single_document();

    //! An unavailable Qt TLS backend is an inconclusive check, never a silent
    //! success without a JSON report.
    void check_json_without_tls_is_inconclusive();

    //! Articles announced by the subject but absent from the XML belong in
    //! the report total without being mistaken for network-checkable IDs.
    void check_json_total_includes_articles_absent_from_nzb();
    void check_recovery_is_certain_when_blocks_cover_the_loss();
    void check_recovery_is_impossible_when_the_loss_exceeds_the_blocks();
    void check_prorates_the_blocks_of_a_damaged_par2_volume();
    void check_loses_no_article_when_the_connection_drops();
    void check_stops_once_the_post_is_provably_beyond_repair();
    void check_full_verifies_everything_anyway();
    void check_detects_par2_when_the_subject_ends_on_the_extension();
    void check_withholds_redundancy_when_a_size_is_missing();
    void check_survives_a_server_that_sends_half_a_line();
    void check_never_declares_a_post_dead_on_an_inferred_slice_size();
    void check_does_not_write_off_a_volume_it_only_estimated_to_zero();
    void check_holds_the_data_back_until_the_par2_answers_are_in();
    void check_does_not_charge_a_short_last_article_as_a_full_one();
    void check_will_not_promise_certainty_without_intact_metadata();
    void check_gives_no_verdict_when_the_files_disagree_on_article_size();
    void check_knows_index_only_par2_cannot_mend_anything();
    void check_counts_losses_in_separate_files_as_separate_blocks();
    void check_stops_on_losses_the_nzb_itself_already_declared();

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

    //! A failed restoration must retain enough state for a later retry; the
    //! successful retry then removes the empty staging directory.
    void obfuscated_source_restore_is_retryable();

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
    void warns_when_c_points_at_the_adopted_legacy_config();

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
#if !defined(Q_OS_LINUX)
    // The sandbox cannot reach the binary this test spawns. genericConfigRoot()
    // honours NGPOST_TEST_HOME only under NGPOST_TESTING, which the production
    // binary is not built with, so it resolves its config directory through
    // QStandardPaths -- and that reads the real user profile on macOS and
    // Windows rather than HOME or LOCALAPPDATA. Measured on the runners: the
    // test planted its legacy folder in the sandbox while the binary used
    // /Users/runner/Library/Application Support/ngPost. Asserting here would
    // test the runner's own configuration, and would write into it.
    //
    // The adoption itself is covered in-process by tst_PathHelper, which runs
    // on all three platforms and is properly sandboxed -- it is what caught the
    // Windows publish bug fixed in 05cf6a4.
    QSKIP("config-folder adoption through a spawned binary can only be sandboxed on Linux");
#endif

    HomeSandbox sandbox;
    // One place decides where a spawned production binary keeps its config
    // directory on each platform; see HomeSandbox::configRootFor.
    const QString configRoot = sandbox.configRoot();
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

void TestCliParser::server_keys_without_a_block_are_not_a_crash()
{
    HomeSandbox sandbox;

    // Exactly what a very old ngPost.conf looks like: one server, spelled out
    // at the top level, with no "[server]" line anywhere.
    const QString confPath = sandbox.rootPath() + QStringLiteral("/legacy.conf");
    QFile         conf(confPath);
    QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
    conf.write("HOST = news.example.invalid\n"
               "PORT = 119\n"
               "USER = someone\n"
               "PASS = secret\n"
               "CONNECTION = 3\n");
    conf.close();

    const RunResult r = run(_bin, { "-c", confPath, "--history" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    // A crash is a signal, not an exit code: on Unix QProcess reports it as a
    // crash exit status, so assert the clean value rather than "not 139".
    QVERIFY2(r.exitCode == 0,
             qPrintable(QStringLiteral("a pre-[server] configuration must be read, not crash on; exit=%1 out=%2")
                            .arg(r.exitCode)
                            .arg(r.stdoutText + r.stderrText)));
}

void TestCliParser::check_with_zero_connections_does_not_hang()
{
    HomeSandbox sandbox;
    const QString confPath = sandbox.rootPath() + QStringLiteral("/zero-connections.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write("[server]\n"
                 "host = 127.0.0.1\n"
                 "port = 119\n"
                 "enabled = true\n"
                 "nzbCheck = true\n"
                 "connection = 0\n");
    config.close();

    const QString nzbPath = QStringLiteral(NGPOST_TESTS_ROOT)
                            + QStringLiteral("/fixtures/nzb/tiny_1seg.golden.nzb");
    const RunResult result = run(_bin,
                                 { "-c", confPath, "--check", nzbPath },
                                 sandbox.rootPath());

    QVERIFY2(!result.timedOut, "--check hung with a zero-connection server");
    QCOMPARE(result.exitCode,
             static_cast<int>(NzbCheck::CheckStatus::Inconclusive));
    QVERIFY2((result.stdoutText + result.stderrText)
                 .contains(QStringLiteral("INCONCLUSIVE"), Qt::CaseInsensitive),
             qPrintable(result.stdoutText + result.stderrText));
}

void TestCliParser::check_json_stdout_is_a_single_document()
{
    HomeSandbox sandbox;
    const QString confPath = sandbox.rootPath() + QStringLiteral("/json-check.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write("[server]\n"
                 "host = 127.0.0.1\n"
                 "port = 119\n"
                 "enabled = true\n"
                 "nzbCheck = true\n"
                 "connection = 0\n");
    config.close();

    const QString nzbPath = QStringLiteral(NGPOST_TESTS_ROOT)
                            + QStringLiteral("/fixtures/nzb/tiny_1seg.golden.nzb");
    const RunResult result = run(_bin,
                                 { "-c", confPath, "--debug", "--check_json",
                                   "--check", nzbPath },
                                 sandbox.rootPath());

    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));
    QCOMPARE(result.exitCode,
             static_cast<int>(NzbCheck::CheckStatus::Inconclusive));

    QJsonParseError parseError;
    const QJsonDocument report =
        QJsonDocument::fromJson(result.stdoutText.trimmed().toUtf8(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(report.isObject());
    QCOMPARE(report.object().value(QStringLiteral("status")).toString(),
             QStringLiteral("inconclusive"));
    QVERIFY2(result.stderrText.contains(QStringLiteral("INCONCLUSIVE"), Qt::CaseInsensitive),
             qPrintable(result.stderrText));
}

void TestCliParser::check_json_without_tls_is_inconclusive()
{
#if !defined(Q_OS_LINUX)
    QSKIP("The TLS-plugin mount isolation used by this end-to-end test is Linux-specific");
#else
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (bwrap.isEmpty())
        QSKIP("bwrap is unavailable; cannot isolate the Qt TLS plugins");
    const QString tlsPluginPath = QLibraryInfo::path(QLibraryInfo::PluginsPath)
                                  + QStringLiteral("/tls");
    if (!QFileInfo(tlsPluginPath).isDir())
        QSKIP("the Qt TLS plugin directory could not be located");

    HomeSandbox sandbox;
    const QString confPath = sandbox.rootPath() + QStringLiteral("/tls-check.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write("[server]\n"
                 "host = 127.0.0.1\n"
                 "port = 119\n"
                 "enabled = true\n"
                 "nzbCheck = true\n"
                 "connection = 1\n");
    config.close();

    const QString nzbPath = QStringLiteral(NGPOST_TESTS_ROOT)
                            + QStringLiteral("/fixtures/nzb/tiny_1seg.golden.nzb");
    const RunResult result =
        run(bwrap,
            { "--die-with-parent",
              "--ro-bind", "/", "/",
              "--tmpfs", tlsPluginPath,
              "--dev-bind", "/dev", "/dev",
              "--proc", "/proc",
              _bin, "-c", confPath, "--check_json", "--check", nzbPath },
            sandbox.rootPath());

    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));
    if (result.stderrText.startsWith(QStringLiteral("bwrap:"), Qt::CaseInsensitive))
        QSKIP("bwrap is installed but user namespaces are unavailable");
    QCOMPARE(result.exitCode,
             static_cast<int>(NzbCheck::CheckStatus::Inconclusive));

    QJsonParseError parseError;
    const QJsonDocument report =
        QJsonDocument::fromJson(result.stdoutText.trimmed().toUtf8(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(report.isObject());
    QCOMPARE(report.object().value(QStringLiteral("status")).toString(),
             QStringLiteral("inconclusive"));
    QVERIFY2(result.stderrText.contains(QStringLiteral("SSL"), Qt::CaseInsensitive),
             qPrintable(result.stderrText));
#endif
}

void TestCliParser::check_json_total_includes_articles_absent_from_nzb()
{
    MockNntpServer server;
    QVERIFY2(server.start(), "failed to start the mock NNTP server");

    HomeSandbox sandbox;
    const QString confPath = sandbox.rootPath() + QStringLiteral("/missing-in-nzb.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream conf(&config);
    conf << "[server]\n"
         << "host = 127.0.0.1\n"
         << "port = " << server.port() << "\n"
         << "enabled = true\n"
         << "nzbCheck = true\n"
         << "connection = 1\n";
    config.close();

    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/missing-segments.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"poster\" date=\"0\" "
              "subject=\"sample.bin yEnc (1/3) 128\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"128\" number=\"1\">"
              "MSGID-0</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult result = run(_bin,
                                 { "-c", confPath, "--check_json", "--check", nzbPath },
                                 sandbox.rootPath());
    QVERIFY2(!result.timedOut, qPrintable(result.stdoutText + result.stderrText));
    // Articles are missing and the nzb carries no PAR2 at all, so nothing can
    // rebuild them: that is exit code 2, not merely "articles are missing".
    QCOMPARE(result.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError parseError;
    const QJsonDocument report =
        QJsonDocument::fromJson(result.stdoutText.trimmed().toUtf8(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(report.isObject());
    QCOMPARE(report.object().value(QStringLiteral("status")).toString(),
             QStringLiteral("unrecoverable"));
    QCOMPARE(report.object().value(QStringLiteral("par2")).toObject()
                     .value(QStringLiteral("recovery")).toString(),
             QStringLiteral("noPar2AtAll"));
    const QJsonObject articles = report.object().value(QStringLiteral("articles")).toObject();
    // The nzb declares two articles it does not carry and offers no PAR2 to
    // rebuild them, so the answer is settled before a single STAT goes out.
    QVERIFY2(report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "a post that is already beyond repair should not be checked at all");
    QCOMPARE(articles.value(QStringLiteral("checked")).toInt(), 0);
    QCOMPARE(articles.value(QStringLiteral("missing")).toInt(), 2);
    QCOMPARE(articles.value(QStringLiteral("missingInNzb")).toInt(), 2);
    QCOMPARE(articles.value(QStringLiteral("total")).toInt(), 3);
}

namespace
{
//! Write an nzb with one data file and one PAR2 volume, using predictable
//! message-ids (d1..dN, p1..pM) so a test can name the ones to hide.
QString writeRecoveryNzb(const QString &dir,
                         const QString &name,
                         int            nbDataArticles,
                         qint64         articleSize,
                         int            par2Blocks,
                         int            nbPar2Articles)
{
    QString xml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n");

    xml += QStringLiteral("  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; "
                          "yEnc (1/%1) %2\">\n"
                          "    <groups><group>alt.binaries.test</group></groups>\n"
                          "    <segments>\n")
                   .arg(nbDataArticles)
                   .arg(articleSize * nbDataArticles);
    for (int i = 1; i <= nbDataArticles; ++i)
        xml += QStringLiteral("      <segment bytes=\"1\" number=\"%1\">d%1</segment>\n").arg(i);
    xml += QStringLiteral("    </segments>\n  </file>\n");

    xml += QStringLiteral("  <file poster=\"p\" date=\"0\" "
                          "subject=\"&quot;data.vol00+%1.par2&quot; yEnc (1/%2) %3\">\n"
                          "    <groups><group>alt.binaries.test</group></groups>\n"
                          "    <segments>\n")
                   .arg(par2Blocks)
                   .arg(nbPar2Articles)
                   .arg(articleSize * nbPar2Articles);
    for (int i = 1; i <= nbPar2Articles; ++i)
        xml += QStringLiteral("      <segment bytes=\"1\" number=\"%1\">p%1</segment>\n").arg(i);
    xml += QStringLiteral("    </segments>\n  </file>\n</nzb>\n");

    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(xml.toUtf8());
    return path;
}

QString writeCheckConf(const QString &dir, quint16 port)
{
    const QString path = dir + QStringLiteral("/recovery.conf");
    QFile config(path);
    if (config.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream conf(&config);
        conf << "[server]\n"
             << "host = 127.0.0.1\n"
             << "port = " << port << "\n"
             << "enabled = true\n"
             << "nzbCheck = true\n"
             << "connection = 1\n";
    }
    return path;
}

QString writeMissingIds(const QString &dir, const QStringList &ids)
{
    const QString path = dir + QStringLiteral("/missing-ids.txt");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write((ids.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
    return path;
}
} // namespace

//! One lost article against four recovery blocks: covered whatever the layout.
void TestCliParser::check_recovery_is_certain_when_blocks_cover_the_loss()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(), { QStringLiteral("d2") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("ok.nzb"),
                                         4, 716800, 4, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Missing));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(), QStringLiteral("certain"));
    QCOMPARE(par2.value(QStringLiteral("blocksUsable")).toInt(), 4);
    // Three, not two: the pessimistic end uses the largest article the subjects
    // still allow. Four articles covering 2867200 bytes pin the payload to
    // [716800, 955733] -- the last one is short, so the file size says less
    // than it looks -- and the worst case has to use the top of that.
    QCOMPARE(par2.value(QStringLiteral("damagedBlocksMax")).toInt(), 3);
}

//! Three lost articles against two blocks: beyond repair at any layout, and the
//! exit code says so rather than merely "articles are missing".
void TestCliParser::check_recovery_is_impossible_when_the_loss_exceeds_the_blocks()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(
            sandbox.rootPath(),
            { QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("dead.nzb"),
                                         8, 716800, 2, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QCOMPARE(report.object().value(QStringLiteral("status")).toString(),
             QStringLiteral("unrecoverable"));
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(), QStringLiteral("impossible"));
}

//! A PAR2 volume that lost one of its four articles is not worth zero blocks.
//! par2cmdline validates each packet on its own and uses what it can still
//! read, so the volume keeps its share: 40 blocks over 4 articles, one gone,
//! leaves 30 -- minus one for the packet the article boundary cuts in half,
//! since recovery packets do not stop politely on article boundaries.
void TestCliParser::check_prorates_the_blocks_of_a_damaged_par2_volume()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(), { QStringLiteral("p1") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("degraded.nzb"),
                                         2, 716800, 40, 4);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Missing));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("blocksTotal")).toInt(), 40);
    QCOMPARE(par2.value(QStringLiteral("blocksUsable")).toInt(), 29);
    // The data itself never left, so there is nothing to recover.
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(), QStringLiteral("notNeeded"));
}

//! A connection cut mid-check used to take its in-flight article with it: the
//! reconnection popped a fresh one and the answer for the old one was never
//! asked for again, so the run ended "incomplete" through its own doing.
void TestCliParser::check_loses_no_article_when_the_connection_drops()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    // Small enough that the server hangs up after a couple of commands, so the
    // check has to reconnect several times to get through the nzb.
    QVERIFY2(server.start({ QStringLiteral("--drop-after-bytes"), QStringLiteral("60") }),
             "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("flaky.nzb"),
                                         6, 716800, 4, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject articles = report.object().value(QStringLiteral("articles")).toObject();
    QCOMPARE(articles.value(QStringLiteral("checked")).toInt(), 8);
    QCOMPARE(articles.value(QStringLiteral("total")).toInt(), 8);
    QCOMPARE(articles.value(QStringLiteral("missing")).toInt(), 0);
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Complete));
    QVERIFY2(!r.stderrText.contains(QStringLiteral("INCOMPLETE")),
             qPrintable(QStringLiteral("the check reported itself incomplete:\n") + r.stderrText));
}

//! PAR2 first, then data, and give up as soon as the loss provably exceeds the
//! blocks: on a large dead post that is the difference between minutes and
//! seconds. The count it reports is then a floor, and says so.
void TestCliParser::check_stops_once_the_post_is_provably_beyond_repair()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(
            sandbox.rootPath(),
            { QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"),
              QStringLiteral("d4"), QStringLiteral("d5") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("dead.nzb"),
                                         12, 716800, 2, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject root = report.object();
    QVERIFY2(root.value(QStringLiteral("stoppedEarly")).toBool(), "the check did not stop early");

    const QJsonObject articles = root.value(QStringLiteral("articles")).toObject();
    QVERIFY2(articles.value(QStringLiteral("missingIsALowerBound")).toBool(),
             "a truncated run must not present its count as a total");
    QVERIFY2(articles.value(QStringLiteral("checked")).toInt()
                     < articles.value(QStringLiteral("total")).toInt(),
             "stopping early should leave articles unchecked");

    // The redundancy has to be known before any data article is weighed against
    // it, so the PAR2 articles must go out first.
    QFile log(server.logFile());
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList stats = QString::fromUtf8(log.readAll())
                                      .split(QLatin1Char('\n'))
                                      .filter(QStringLiteral("STAT <"));
    QVERIFY2(stats.size() >= 2, "no STAT reached the server");
    QVERIFY2(stats.at(0).contains(QStringLiteral("<p")) && stats.at(1).contains(QStringLiteral("<p")),
             qPrintable(QStringLiteral("data was checked before par2:\n") + stats.join('\n')));
}

//! The same post with --check_full: no shortcut, every article asked for.
void TestCliParser::check_full_verifies_everything_anyway()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(
            sandbox.rootPath(),
            { QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"),
              QStringLiteral("d4"), QStringLiteral("d5") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("dead.nzb"),
                                         12, 716800, 2, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--check_full",
                              "--par2_block_size", "716800", "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY2(!report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "--check_full must not stop early");

    const QJsonObject articles = report.object().value(QStringLiteral("articles")).toObject();
    QCOMPARE(articles.value(QStringLiteral("checked")).toInt(), 14);
    QCOMPARE(articles.value(QStringLiteral("missing")).toInt(), 5);
    QVERIFY2(!articles.value(QStringLiteral("missingIsALowerBound")).toBool(),
             "a complete sweep reports a total, not a floor");
}

//! A subject that stops right after ".par2" is still a PAR2 file. The detection
//! used to require a quote or a space behind the extension, so such a file was
//! counted as data: its blocks vanished from the recovery capacity and its
//! articles turned every loss into apparent data loss.
void TestCliParser::check_detects_par2_when_the_subject_ends_on_the_extension()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    QVERIFY2(server.start(), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/bare-par2.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; yEnc (1/2) 1433600\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d1</segment>"
              "<segment bytes=\"1\" number=\"2\">d2</segment></segments>\n"
              "  </file>\n"
              // no quote, no trailing space: the subject ends on the extension
              "  <file poster=\"p\" date=\"0\" subject=\"data.vol00+04.par2\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("volumes")).toInt(), 1);
    QCOMPARE(par2.value(QStringLiteral("blocksTotal")).toInt(), 4);

    const QJsonObject articles = report.object().value(QStringLiteral("articles")).toObject();
    QCOMPARE(articles.value(QStringLiteral("par2")).toInt(), 1);
    QCOMPARE(articles.value(QStringLiteral("data")).toInt(), 2);
}

//! Redundancy is a share of the data, so it needs the whole of the data. When
//! only some subjects announce a size, summing them would understate the
//! denominator and overstate the answer: the figure is withheld instead.
void TestCliParser::check_withholds_redundancy_when_a_size_is_missing()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    QVERIFY2(server.start(), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/mixed-sizes.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;a.bin&quot; yEnc (1/2) 1433600\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d1</segment>"
              "<segment bytes=\"1\" number=\"2\">d2</segment></segments>\n"
              "  </file>\n"
              // this one says nothing about its size
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;b.bin&quot; yEnc (1/2)\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d3</segment>"
              "<segment bytes=\"1\" number=\"2\">d4</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" "
              "subject=\"&quot;a.vol00+04.par2&quot; yEnc (1/1) 716800\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("blocksTotal")).toInt(), 4);
    QVERIFY2(!par2.contains(QStringLiteral("redundancyPercent")),
             "redundancy must not be computed from a partial data size");
}

//! readyRead also fires on half a line. Disarming the watchdog on the mere
//! arrival of bytes let a server send one fragment and then go quiet for ever,
//! which is the exact hang the watchdog was added to prevent.
void TestCliParser::check_survives_a_server_that_sends_half_a_line()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--partial-line") }), "mock server did not start");

    const QString confPath = sandbox.rootPath() + QStringLiteral("/partial.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream conf(&config);
    // SOCK_TIMEOUT is in seconds and floored at 5; one attempt is enough to
    // show the deadline works, so no reconnection budget.
    conf << "SOCK_TIMEOUT = 6\n"
         << "RETRY = 0\n"
         << "[server]\n"
         << "host = 127.0.0.1\n"
         << "port = " << server.port() << "\n"
         << "enabled = true\n"
         << "nzbCheck = true\n"
         << "connection = 1\n";
    config.close();

    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("stalled.nzb"),
                                         2, 716800, 4, 2);

    QElapsedTimer elapsed;
    elapsed.start();
    const RunResult r = run(_bin, { "-c", confPath, "--check", nzb }, sandbox.rootPath());
    QVERIFY2(!r.timedOut, "the check hung on a partial line");
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Inconclusive));
    QVERIFY2(elapsed.elapsed() > 4000,
             "the connection ended too fast to have gone through the watchdog");
    QVERIFY2(r.stderrText.contains(QStringLiteral("stopped answering")),
             qPrintable(QStringLiteral("no watchdog message:\n") + r.stderrText));
}

//! The slice size decides how much damage a lost article does. Inferred from
//! the nzb it is a guess, so the analysis may say "try the repair" but never
//! "this is dead": nothing should send someone off to re-post a whole set on
//! the strength of an estimate, and nothing should stop the check early either.
void TestCliParser::check_never_declares_a_post_dead_on_an_inferred_slice_size()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(
            sandbox.rootPath(),
            { QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("guessed.nzb"),
                                         8, 716800, 2, 2);

    // Same nzb, same losses, twice: once letting the check infer the slice
    // size, once telling it. Only the second may reach a verdict of death.
    const RunResult inferred = run(_bin,
                                   { "-c", conf, "--check_json", "--check", nzb },
                                   sandbox.rootPath());
    QVERIFY2(!inferred.timedOut, qPrintable(inferred.stdoutText + inferred.stderrText));
    QCOMPARE(inferred.exitCode, static_cast<int>(NzbCheck::CheckStatus::Missing));

    QJsonParseError err;
    QJsonDocument report = QJsonDocument::fromJson(inferred.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QVERIFY2(!par2.value(QStringLiteral("blockSizeMeasured")).toBool(),
             "the slice size should have been inferred here");
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(),
             QStringLiteral("layoutDependent"));
    QVERIFY2(!report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "a guess must not cut the check short");

    const RunResult measured = run(_bin,
                                   { "-c", conf, "--check_json", "--par2_block_size", "716800",
                                     "--check", nzb },
                                   sandbox.rootPath());
    QVERIFY2(!measured.timedOut, qPrintable(measured.stdoutText + measured.stderrText));
    QCOMPARE(measured.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));
    report = QJsonDocument::fromJson(measured.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    par2 = report.object().value(QStringLiteral("par2")).toObject();
    QVERIFY2(par2.value(QStringLiteral("blockSizeMeasured")).toBool(),
             "the slice size was given on the command line");
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(), QStringLiteral("impossible"));
}

//! PAR2 slices restart at every file boundary, so a half-block loss in each of
//! two files costs two blocks, not one. Summing the losses across the whole
//! post before a single ceil() made the optimistic bound too optimistic --
//! which is the bound the "beyond repair" verdict is measured against.
void TestCliParser::check_counts_losses_in_separate_files_as_separate_blocks()
{
    HomeSandbox sandbox;
    // One article gone from each of the two files.
    const QString ids = writeMissingIds(sandbox.rootPath(),
                                        { QStringLiteral("a2"), QStringLiteral("b2") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/two-files.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;a.bin&quot; yEnc (1/4) 2867200\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">a1</segment>"
              "<segment bytes=\"1\" number=\"2\">a2</segment>"
              "<segment bytes=\"1\" number=\"3\">a3</segment>"
              "<segment bytes=\"1\" number=\"4\">a4</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;b.bin&quot; yEnc (1/4) 2867200\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">b1</segment>"
              "<segment bytes=\"1\" number=\"2\">b2</segment>"
              "<segment bytes=\"1\" number=\"3\">b3</segment>"
              "<segment bytes=\"1\" number=\"4\">b4</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" "
              "subject=\"&quot;a.vol00+01.par2&quot; yEnc (1/1) 716800\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    // Blocks of 2 MB against articles of 700 kB: each loss on its own is well
    // under one block, so summing them first would round the pair down to a
    // single damaged block and call one recovery block enough.
    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "2000000",
                              "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("damagedBlocksMin")).toInt(), 2);
    QCOMPARE(par2.value(QStringLiteral("blocksUsable")).toInt(), 1);
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(), QStringLiteral("impossible"));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));
}

//! Articles the nzb never lists are counted at parse time, long before any
//! connection opens, so no "missing article" event ever fires for them. While
//! the decision to stop hung off that event alone, a post whose nzb was already
//! truncated beyond repair was checked from end to end for nothing.
void TestCliParser::check_stops_on_losses_the_nzb_itself_already_declared()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    QVERIFY2(server.start(), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/truncated.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    // The subject announces eight articles; three are listed. The five the nzb
    // does not carry are already more than two recovery blocks can rebuild.
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; yEnc (1/8) 5734400\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d1</segment>"
              "<segment bytes=\"1\" number=\"2\">d2</segment>"
              "<segment bytes=\"1\" number=\"3\">d3</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" "
              "subject=\"&quot;data.vol00+02.par2&quot; yEnc (1/2) 1433600\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment>"
              "<segment bytes=\"1\" number=\"2\">p2</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY2(report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "losses the nzb already declared should end the run as soon as the PAR2 "
             "phase closes");

    // The three data articles it does list were never worth asking about.
    const QJsonObject articles = report.object().value(QStringLiteral("articles")).toObject();
    QCOMPARE(articles.value(QStringLiteral("checked")).toInt(), 2);
    QCOMPARE(articles.value(QStringLiteral("missingInNzb")).toInt(), 5);
}

//! Losing one of a volume's two articles does not prove its single recovery
//! block is gone: PAR2 packets carry their own checksum and may sit anywhere
//! in the file, so the surviving article may hold it whole. The pro rata is a
//! prudent floor, and a floor of zero is not a measurement -- turning it into
//! "no redundancy left" and stopping the run on it was asserting a fact from
//! an estimate.
void TestCliParser::check_does_not_write_off_a_volume_it_only_estimated_to_zero()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(),
                                        { QStringLiteral("d2"), QStringLiteral("p1") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    // One recovery block spread over two articles, one of them gone.
    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("thin.nzb"),
                                         4, 716800, 1, 2);

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzb },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();

    QCOMPARE(par2.value(QStringLiteral("blocksUsable")).toInt(), 0);   // the prudent floor
    QCOMPARE(par2.value(QStringLiteral("blocksUsableMax")).toInt(), 1); // what may survive
    QVERIFY2(par2.value(QStringLiteral("recovery")).toString() != QStringLiteral("noUsableBlocks"),
             "a floor of zero is not proof the block is gone");
    QVERIFY2(!report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "an estimate must not cut the run short");
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Missing));
}

//! With more connections than PAR2 articles, the spare ones used to race
//! straight into the data while the PAR2 answers were still in flight -- so the
//! redundancy was still unknown when the first data losses came back, which is
//! the one thing checking PAR2 first exists to avoid.
void TestCliParser::check_holds_the_data_back_until_the_par2_answers_are_in()
{
    HomeSandbox sandbox;

    MockNntpServer server;
    // 150 ms before every reply: wide enough that a connection jumping the gun
    // is unmistakable in the timestamps.
    QVERIFY2(server.start({ QStringLiteral("--slow-mode-ms"), QStringLiteral("150") }),
             "mock server did not start");

    const QString confPath = sandbox.rootPath() + QStringLiteral("/many-cons.conf");
    QFile config(confPath);
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream conf(&config);
    conf << "[server]\n"
         << "host = 127.0.0.1\n"
         << "port = " << server.port() << "\n"
         << "enabled = true\n"
         << "nzbCheck = true\n"
         << "connection = 8\n";
    config.close();

    const QString nzb = writeRecoveryNzb(sandbox.rootPath(), QStringLiteral("wide.nzb"),
                                         16, 716800, 2, 2);
    const RunResult r = run(_bin, { "-c", confPath, "--check", nzb }, sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QFile log(server.logFile());
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'));

    // "[+   106ms] [127.0.0.1:x] STAT <p1> -> 223"
    const QRegularExpression stamp(QStringLiteral("^\\[\\+\\s*(\\d+)ms\\].*STAT <([^>]+)>"));
    qint64 lastPar2 = -1, firstData = -1;
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = stamp.match(line);
        if (!m.hasMatch())
            continue;
        const qint64 at = m.captured(1).toLongLong();
        if (m.captured(2).startsWith(QLatin1Char('p')))
            lastPar2 = qMax(lastPar2, at);
        else if (firstData < 0)
            firstData = at;
    }
    QVERIFY2(lastPar2 >= 0 && firstData >= 0, "the run did not reach both kinds of article");
    QVERIFY2(firstData - lastPar2 >= 100,
             qPrintable(QStringLiteral("data started %1 ms after the last par2 command, so it "
                                       "did not wait for its answer")
                                .arg(firstData - lastPar2)));
}

//! The last article of a file holds the remainder, which can be a single byte.
//! Charging it a full article's worth of blocks turned a one-block loss into
//! four and produced UNRECOVERABLE, plus an irreversible early stop, on a post
//! a repair would have fixed.
void TestCliParser::check_does_not_charge_a_short_last_article_as_a_full_one()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(), { QStringLiteral("d2") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/short-tail.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    // 716801 bytes in two articles: a full one and a one-byte remainder. d2 is
    // that remainder, and blocks are 100000 bytes.
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; yEnc (1/2) 716801\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d1</segment>"
              "<segment bytes=\"1\" number=\"2\">d2</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" "
              "subject=\"&quot;data.vol00+01.par2&quot; yEnc (1/1) 100000\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "100000",
                              "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("damagedBlocksMin")).toInt(), 1);
    QVERIFY2(par2.value(QStringLiteral("recovery")).toString() != QStringLiteral("impossible"),
             "a one-byte article cannot exhaust the redundancy on its own");
    QVERIFY2(!report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "and it certainly must not end the run");
}

//! Blocks are useless without the packets that say what to rebuild. Not finding
//! a wholly intact PAR2 file does not prove those are gone -- the format
//! repeats them in every file -- but it forbids promising they are there.
void TestCliParser::check_will_not_promise_certainty_without_intact_metadata()
{
    HomeSandbox sandbox;
    // One article gone from the data, and one from each PAR2 volume, so no PAR2
    // file is wholly intact while plenty of blocks remain.
    const QString ids = writeMissingIds(
            sandbox.rootPath(),
            { QStringLiteral("d1"), QStringLiteral("p1"), QStringLiteral("q1") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/no-intact-par2.nzb");
    QString xml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
            "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; yEnc (1/20) 14336000\">\n"
            "    <groups><group>alt.binaries.test</group></groups>\n    <segments>\n");
    for (int i = 1; i <= 20; ++i)
        xml += QStringLiteral("      <segment bytes=\"1\" number=\"%1\">d%1</segment>\n").arg(i);
    xml += QStringLiteral("    </segments>\n  </file>\n");
    for (const QString &tag : { QStringLiteral("p"), QStringLiteral("q") }) {
        xml += QStringLiteral("  <file poster=\"p\" date=\"0\" subject=\"&quot;data.vol%1+20.par2"
                              "&quot; yEnc (1/10) 7168000\">\n"
                              "    <groups><group>alt.binaries.test</group></groups>\n"
                              "    <segments>\n")
                       .arg(tag == QStringLiteral("p") ? QStringLiteral("00")
                                                       : QStringLiteral("20"));
        for (int i = 1; i <= 10; ++i) {
            // Built outside the format string on purpose: "%2%1" would be read
            // as the single marker %21, which silently collapses every id to
            // the tag alone and makes the whole fixture meaningless.
            const QString id = tag + QString::number(i);
            xml += QStringLiteral("      <segment bytes=\"1\" number=\"%1\">%2</segment>\n")
                           .arg(i)
                           .arg(id);
        }
        xml += QStringLiteral("    </segments>\n  </file>\n");
    }
    xml += QStringLiteral("</nzb>\n");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write(xml.toUtf8());
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "716800",
                              "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QVERIFY2(!par2.value(QStringLiteral("metadataAvailable")).toBool(),
             qPrintable(QStringLiteral("no PAR2 file should be intact; nzb was:\n%1\nreport:\n%2")
                                .arg(xml)
                                .arg(QString::fromUtf8(
                                        QJsonDocument(report.object()).toJson()))));
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(),
             QStringLiteral("layoutDependent"));
    // And the guaranteed floor is zero, because both volumes are damaged: what
    // a pro rata expects is not what the format promises.
    QCOMPARE(par2.value(QStringLiteral("blocksUsableGuaranteed")).toInt(), 0);
    QVERIFY2(par2.value(QStringLiteral("blocksUsable")).toInt() > 0,
             "the likely figure should still be reported");
}

//! Two files posted with different article sizes cannot both constrain one A.
//! Collapsing contradictory bounds to a point would turn a contradiction into
//! a certainty, so the analysis declines to bound the damage at all.
void TestCliParser::check_gives_no_verdict_when_the_files_disagree_on_article_size()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(), { QStringLiteral("a1") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/mixed-article-sizes.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    // 2 articles of ~716800 in one file, 4 articles of ~100000 in the other:
    // the bounds are [716800, ...] and [..., 133333], which cannot overlap.
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;a.bin&quot; yEnc (1/2) 1433600\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">a1</segment>"
              "<segment bytes=\"1\" number=\"2\">a2</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;b.bin&quot; yEnc (1/4) 400000\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">b1</segment>"
              "<segment bytes=\"1\" number=\"2\">b2</segment>"
              "<segment bytes=\"1\" number=\"3\">b3</segment>"
              "<segment bytes=\"1\" number=\"4\">b4</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" "
              "subject=\"&quot;a.vol00+01.par2&quot; yEnc (1/1) 100000\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin,
                            { "-c", conf, "--check_json", "--par2_block_size", "100000",
                              "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("damagedBlocksMax")).toInt(), -1); // unbounded
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(),
             QStringLiteral("layoutDependent"));
    QVERIFY2(!report.object().value(QStringLiteral("stoppedEarly")).toBool(),
             "nothing can be concluded, so nothing may be cut short");
}

//! A post carrying only the base .par2 has an index and no recovery block: it
//! can say what is broken, not mend it. That is a fact, and it used to be
//! reported as "every volume lost every one of its articles".
void TestCliParser::check_knows_index_only_par2_cannot_mend_anything()
{
    HomeSandbox sandbox;
    const QString ids = writeMissingIds(sandbox.rootPath(), { QStringLiteral("d1") });

    MockNntpServer server;
    QVERIFY2(server.start({ QStringLiteral("--missing-ids"), ids }), "mock server did not start");

    const QString conf = writeCheckConf(sandbox.rootPath(), server.port());
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/index-only.nzb");
    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    nzb.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.bin&quot; yEnc (1/2) 1433600\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">d1</segment>"
              "<segment bytes=\"1\" number=\"2\">d2</segment></segments>\n"
              "  </file>\n"
              "  <file poster=\"p\" date=\"0\" subject=\"&quot;data.par2&quot; yEnc (1/1) 40000\">\n"
              "    <groups><group>alt.binaries.test</group></groups>\n"
              "    <segments><segment bytes=\"1\" number=\"1\">p1</segment></segments>\n"
              "  </file>\n"
              "</nzb>\n");
    nzb.close();

    const RunResult r = run(_bin, { "-c", conf, "--check_json", "--check", nzbPath },
                            sandbox.rootPath());
    QVERIFY2(!r.timedOut, qPrintable(r.stdoutText + r.stderrText));
    QCOMPARE(r.exitCode, static_cast<int>(NzbCheck::CheckStatus::Unrecoverable));

    QJsonParseError err;
    const QJsonDocument report = QJsonDocument::fromJson(r.stdoutText.trimmed().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject par2 = report.object().value(QStringLiteral("par2")).toObject();
    QCOMPARE(par2.value(QStringLiteral("volumes")).toInt(), 1);
    QCOMPARE(par2.value(QStringLiteral("blocksTotal")).toInt(), 0);
    QCOMPARE(par2.value(QStringLiteral("recovery")).toString(),
             QStringLiteral("noRecoveryBlocks"));
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

void TestCliParser::obfuscated_source_restore_is_retryable()
{
    QTemporaryDir sandbox;
    QVERIFY(sandbox.isValid());

    const QString stagingPath = sandbox.filePath(QStringLiteral(".ngPost_src_test"));
    QVERIFY(QDir().mkpath(stagingPath));
    const QString stagedPath = stagingPath + QStringLiteral("/random-name.bin");
    const QString originalPath = sandbox.filePath(QStringLiteral("source.bin"));

    QFile staged(stagedPath);
    QVERIFY(staged.open(QIODevice::WriteOnly));
    QCOMPARE(staged.write("original payload"), qint64(16));
    staged.close();

    QFile occupiedDestination(originalPath);
    QVERIFY(occupiedDestination.open(QIODevice::WriteOnly));
    QCOMPARE(occupiedDestination.write("occupied"), qint64(8));
    occupiedDestination.close();

    QMap<QString, QString> mappings;
    mappings.insert(stagedPath, originalPath);
    QString retainedStagingPath = stagingPath;

    QVERIFY(!PostingJob::restoreObfuscatedPathsForTest(mappings, retainedStagingPath));
    QCOMPARE(mappings.value(stagedPath), originalPath);
    QCOMPARE(retainedStagingPath, stagingPath);
    QVERIFY(QFileInfo::exists(stagedPath));

    QVERIFY(QFile::remove(originalPath));
    QVERIFY(PostingJob::restoreObfuscatedPathsForTest(mappings, retainedStagingPath));
    QVERIFY(mappings.isEmpty());
    QVERIFY(retainedStagingPath.isEmpty());
    QVERIFY(!QFileInfo::exists(stagingPath));

    QFile restored(originalPath);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("original payload"));
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

void TestCliParser::warns_when_c_points_at_the_adopted_legacy_config()
{
#if !defined(Q_OS_LINUX)
    // The sandbox cannot reach the binary this test spawns. genericConfigRoot()
    // honours NGPOST_TEST_HOME only under NGPOST_TESTING, which the production
    // binary is not built with, so it resolves its config directory through
    // QStandardPaths -- and that reads the real user profile on macOS and
    // Windows rather than HOME or LOCALAPPDATA. Measured on the runners: the
    // test planted its legacy folder in the sandbox while the binary used
    // /Users/runner/Library/Application Support/ngPost. Asserting here would
    // test the runner's own configuration, and would write into it.
    //
    // The adoption itself is covered in-process by tst_PathHelper, which runs
    // on all three platforms and is properly sandboxed -- it is what caught the
    // Windows publish bug fixed in 05cf6a4.
    QSKIP("config-folder adoption through a spawned binary can only be sandboxed on Linux");
#endif

    QTemporaryDir home;
    QVERIFY(home.isValid());
    // Where the config directory lives depends on the platform, so ask rather
    // than assume: hardcoding ".config" adopted nothing on macOS and Windows.
    const QString legacyDir = HomeSandbox::configRootFor(home.path())
        + QStringLiteral("/ngPost-5.4.2-x86_64.AppImage");
    QVERIFY(QDir().mkpath(legacyDir));

    // A legacy install: a configuration with no POST_DB, and its database.
    const QString legacyConf = legacyDir + QStringLiteral("/ngPost.conf");
    {
        QFile f(legacyConf);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("GROUPS = alt.binaries.test\n");
    }
    const QString legacyDb = legacyDir + QStringLiteral("/ngPost_history.sqlite");
    {
        QString err;
        PostHistoryStore store(legacyDb, true);
        QVERIFY2(store.initialize(&err), qPrintable(err));
    }

    // First run adopts it.
    RunResult adopt = run(_bin, { QStringLiteral("--history") }, home.path());
    QVERIFY(!adopt.timedOut);
    // "Using default config file: ..." on stdout names the folder the spawned
    // binary actually resolved. Without it, a failure here cannot distinguish
    // "adoption is broken" from "the sandbox does not reach that binary".
    const QString where = QStringLiteral(
        "expected legacy dir: %1\nstdout:\n%2\nstderr:\n%3")
        .arg(legacyDir, adopt.stdoutText, adopt.stderrText);
    QVERIFY2(adopt.stderrText.contains(QStringLiteral("brought over"), Qt::CaseInsensitive),
             qPrintable(where));

    // A cron job still passing the old file reads settings that never had a
    // POST_DB line, so its posts would land in the new folder's database while
    // the adopted configuration keeps using the old one. Say so.
    RunResult viaLegacy = run(_bin,
                              { QStringLiteral("-c"), legacyConf, QStringLiteral("--history") },
                              home.path());
    QVERIFY(!viaLegacy.timedOut);
    QVERIFY2(viaLegacy.stderrText.contains(QStringLiteral("no POST_DB line")),
             qPrintable(viaLegacy.stderrText));
    QVERIFY2(viaLegacy.stderrText.contains(legacyDb), qPrintable(viaLegacy.stderrText));

    // The adopted configuration itself selects that database: nothing to warn.
    const QString adopted = HomeSandbox::configRootFor(home.path())
        + QStringLiteral("/ngPost/ngPost.conf");
    QVERIFY(QFileInfo::exists(adopted));
    RunResult viaAdopted = run(_bin,
                               { QStringLiteral("-c"), adopted, QStringLiteral("--history") },
                               home.path());
    QVERIFY(!viaAdopted.timedOut);
    QVERIFY2(!viaAdopted.stderrText.contains(QStringLiteral("no POST_DB line")),
             qPrintable(viaAdopted.stderrText));

    // And any unrelated configuration stays silent too.
    const QString other = home.filePath(QStringLiteral("autre.conf"));
    {
        QFile f(other);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("GROUPS = x\n");
    }
    RunResult viaOther = run(_bin,
                             { QStringLiteral("-c"), other, QStringLiteral("--history") },
                             home.path());
    QVERIFY(!viaOther.timedOut);
    QVERIFY2(!viaOther.stderrText.contains(QStringLiteral("no POST_DB line")),
             qPrintable(viaOther.stderrText));
}

QTEST_MAIN(TestCliParser)
#include "tst_CliParser.moc"
