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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

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

RunResult run(const QString &bin, const QStringList &args, const QString &sandboxHome)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", sandboxHome);
    env.insert("XDG_CONFIG_HOME", sandboxHome + QStringLiteral("/.config"));
    env.insert("APPDATA", sandboxHome);
    env.insert("USERPROFILE", sandboxHome);
    p.setProcessEnvironment(env);
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

    // the case that used to be dropped without a word: an Allocine URL
    QVERIFY(NgPost::splitMetaPair(
        QStringLiteral("portail1=https://www.allocine.fr/film/fichefilm_gen_cfilm=326598.html"),
        &key,
        &value));
    QCOMPARE(key, QStringLiteral("portail1"));
    QCOMPARE(value,
             QStringLiteral("https://www.allocine.fr/film/fichefilm_gen_cfilm=326598.html"));

    // an empty value is a legitimate way to blank a field of a record sheet
    QVERIFY(NgPost::splitMetaPair(QStringLiteral("portail2="), &key, &value));
    QCOMPARE(key, QStringLiteral("portail2"));
    QVERIFY(value.isEmpty());

    // spaces around the name are forgiven, the value is kept verbatim
    QVERIFY(NgPost::splitMetaPair(QStringLiteral(" titre = Mon Film "), &key, &value));
    QCOMPARE(key, QStringLiteral("titre"));
    QCOMPARE(value, QStringLiteral(" Mon Film "));

    QVERIFY(!NgPost::splitMetaPair(QStringLiteral("noEqualSignHere"), &key, &value));
    QVERIFY(!NgPost::splitMetaPair(QStringLiteral("=orphanValue"), &key, &value));
}

void TestCliParser::meta_key_cannot_be_public_and_private_at_once()
{
    HomeSandbox sandbox;
    const RunResult r =
        runWithMeta(_bin, { "--meta", "titre=Public", "--post_meta", "titre=Private" }, sandbox);

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, qPrintable(QStringLiteral("expected a failure, got %1").arg(r.exitCode)));
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains(QStringLiteral("titre")), qPrintable(out));
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

    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    QFile tmpl(tmplPath);
    QVERIFY(tmpl.open(QIODevice::WriteOnly));
    tmpl.write("name =__nzbName__\nid =__postId__\n");
    tmpl.close();

    const QString outPath = sandbox.rootPath() + QStringLiteral("/sheet.txt");
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
    QVERIFY2(content.contains(QStringLiteral("id =%1").arg(postId)), qPrintable(content));

    const RunResult toStdout = run(_bin,
                                   { "--export-post-info", QString::number(postId),
                                     "--post_info_template", tmplPath, "--post_db", dbPath },
                                   sandbox.rootPath());
    QVERIFY2(!toStdout.timedOut, "export to stdout timed out");
    QCOMPARE(toStdout.exitCode, 0);
    // stdout carries the record sheet and nothing else: a caller pipes it
    QCOMPARE(toStdout.stdoutText, content);

    const RunResult unknown = run(_bin,
                                  { "--export_post_info", "999999",
                                    "--post_info_template", tmplPath, "--post_db", dbPath },
                                  sandbox.rootPath());
    QVERIFY2(!unknown.timedOut, "export of an unknown id timed out");
    QVERIFY2(unknown.exitCode != 0,
             qPrintable(QStringLiteral("an unknown id should fail, got %1").arg(unknown.exitCode)));
    QVERIFY(unknown.stdoutText.isEmpty());
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

QTEST_MAIN(TestCliParser)
#include "tst_CliParser.moc"
