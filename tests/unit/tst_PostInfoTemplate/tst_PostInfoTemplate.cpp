// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_PostInfoTemplate.cpp — post info file rendering: variable contracts,
// single pass substitution, output path safety, permissions.
//
//========================================================================

#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "FileUploader.h"
#include "PostCmdRunner.h"
#include "postinfo/PostInfoTemplate.h"

class TestPostInfoTemplate : public QObject
{
    Q_OBJECT

private slots:
    //! The variable table is the single source of truth: no duplicate name, no
    //! duplicate environment variable, never an empty description.
    void fields_table_is_consistent();
    void escaping_follows_the_declared_format();
    //! Rendering for stdout must be able to use the complete file pipeline
    //! without creating a destination file, even with hostile JSON values.
    void render_template_file_in_memory_uses_the_complete_pipeline();
    void escaping_never_touches_paths_or_commands();
    void format_falls_back_on_the_model_name();
    void a_secret_is_protected_even_once_escaped();
    void shipped_json_model_produces_valid_json();
    void template_lines_round_trip();
    void comment_lines_are_not_written();
    void tokens_in_lists_every_variable_of_a_model();

    //! Every fixed variable resolves, and the tricky contracts hold: par2Pct is
    //! empty when par2 is off, sizeInByte stays the legacy value.
    void fixed_variables_resolve();

    //! __date__ defaults to ISO, __date:fmt__ honours the Qt format.
    void date_default_and_custom_format();

    //! A missing metadata renders empty and is never an error.
    void meta_present_and_missing();

    //! An unknown variable stays verbatim (what NZB_POST_CMD always did) and is
    //! reported so the user can be told about it.
    void unknown_variable_kept_and_reported();

    //! Regression guard for the old replace() chain: a value that happens to
    //! contain __nzbPath__ must NOT be expanded a second time.
    void substitution_is_single_pass();

    //! The legacy %1 goes through the same protected pass, but ONLY for post
    //! commands: in a record sheet "50%1 off" is prose.
    void legacy_percent_one_is_substituted();

    //! Splitting first then substituting keeps a value with spaces as one
    //! argument, instead of letting it break the command line.
    void arguments_with_spaces_stay_one_argument();

    //! Secrets are replaced before anything reaches a log.
    void secrets_are_redacted();

    //! The fields a model asks for can be read off it, so an editor can offer
    //! exactly those instead of leaving the user to guess the names.
    void meta_names_are_read_off_a_template();

    //! Absolute as-is, ~ expanded, relative resolved against the given base.
    void template_path_resolution();

    //! Round trip through a real file, including UTF-8 and a missing folder.
    void render_to_file_creates_parent_folder_and_keeps_utf8();

    //! An unknown variable in the destination is fatal: writing to an
    //! approximate location is worse than not writing.
    void unknown_variable_in_output_path_is_fatal();

    //! A relative destination follows the configuration, not the directory the
    //! process happens to have been started from.
    void relative_output_resolves_against_the_given_base();

    //! Without the opt-in, the password is removed from the environment, not
    //! merely left out: ngPost may have inherited one of its own.
    void password_is_removed_from_an_inherited_environment();

    //! The nzb, the template itself and the source files are never overwritten,
    //! and a source folder protects everything below it.
    void refuses_to_overwrite_protected_paths();

    //! A password in a filename leaks through directory listings even when
    //! file permissions are private, so it is rejected outright.
    void refuses_a_secret_in_the_output_path();

    //! The protection follows symlinks, so writing "through" a link to a
    //! source is refused as well.
    void protection_resolves_symlinks();

    //! A post info file holding the archive password is owner only (Unix).
    void permissions_are_restricted_when_a_secret_is_written();

    //! The shipped Baselien template renders byte for byte as expected. This
    //! pins the promise made to that index.
    void baselien_template_golden();

    //! Hooks are globally FIFO, receive a self-describing JSON file, keep
    //! secrets out by default and drain chatty output without leaking it.
    void post_command_runner_fifo_json_secrets_and_bounded_output();

    //! A timeout and a command that cannot start both release the FIFO so the
    //! commands behind them still run.
    void post_command_runner_timeout_and_start_failure();

    //! Settings belong to the queued post: changing the runner afterwards
    //! must not alter its timeout, secret exposure or failure policy.
    void post_command_runner_snapshots_settings_per_task();

    //! QNetworkReply::abort may synchronously emit finished(). Releasing a
    //! timed-out upload must therefore be reentrancy-safe and close the NZB.
    void file_uploader_release_handles_synchronous_abort();
};

namespace
{

PostInfoData sampleData()
{
    PostInfoData d;
    d.originalPath  = QStringLiteral("/data/backup");
    d.sourcePath    = QStringLiteral("/data/backup/Rando-Mercantour-2026.mkv");
    d.originalName  = QStringLiteral("Rando-Mercantour-2026.mkv");
    d.nzbPath       = QStringLiteral("/data/nzb/Rando.nzb");
    d.nzbDir        = QStringLiteral("/data/nzb");
    d.nzbName       = QStringLiteral("Rando");
    d.nzbFileName   = QStringLiteral("Rando.nzb");
    d.rarName       = QStringLiteral("876EFID5Y22SH1CO5C7C");
    d.rarPass       = QStringLiteral("s3cr3t");
    d.groups        = QStringLiteral("alt.binaries.test,alt.binaries.misc");
    d.nzbPoster     = QStringLiteral("h0wsef7@8xw81s.r7");
    d.status        = QStringLiteral("success");
    d.avgSpeed      = QStringLiteral("12.5 MB/s");
    d.appVersion    = QStringLiteral("5.5.0");
    d.par2Pct       = 8;
    d.postSizeBytes = 2505484398ULL;
    d.legacySizeBytes = 2505484398ULL;
    d.nbFiles          = 42;
    d.nbArticles       = 3600;
    d.nbArticlesPosted = 3600;
    d.nbArticlesFailed = 0;
    d.durationSec      = 200;
    d.historyPostId    = 7;
    d.startedAt        = QDateTime(QDate(2026, 8, 15), QTime(21, 30, 0));
    d.finishedAt       = QDateTime(QDate(2026, 8, 15), QTime(22, 0, 0));
    d.meta.insert(QStringLiteral("titre"),
                  MetaValue(QStringLiteral("Randonnee au Mercantour, \"2026\""), MetaScope::Local));
    d.meta.insert(QStringLiteral("portail1"),
                  MetaValue(QStringLiteral("https://example.org/albums/view?id=326598&size=full"),
                            MetaScope::Local));
    d.meta.insert(QStringLiteral("categorie"), MetaValue(QStringLiteral("Video perso")));
    d.meta.insert(QStringLiteral("qualite"), MetaValue(QStringLiteral("1080p")));
    d.meta.insert(QStringLiteral("genre"), MetaValue(QStringLiteral("Nature")));
    return d;
}

QString repoTemplate(QString const &name)
{
    return QDir::cleanPath(QStringLiteral(NGPOST_TESTS_ROOT) + QStringLiteral("/../templates/")
                           + name);
}

bool writeFile(QString const &path, QString const &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    f.close();
    return true;
}

QString readFile(QString const &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QString quotedCommandArg(QString const &arg)
{
    // QProcess::splitCommand treats a triple quote as a literal quote. Test
    // paths do not normally contain one, but keeping the helper exact makes
    // the test portable to a user profile that does.
    QString escaped = arg;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString pythonExecutable()
{
    QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        python = QStandardPaths::findExecutable(QStringLiteral("python"));
    return python;
}

QString writeRunnerScript(QTemporaryDir const &dir)
{
    const QString path = dir.filePath(QStringLiteral("runner.py"));
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    script.write(
        "import json, os, sys, time\n"
        "mode, marker = sys.argv[1], sys.argv[2]\n"
        "if mode == 'first':\n"
        "    with open(os.environ['NGPOST_JSON'], encoding='utf-8') as f:\n"
        "        data = json.load(f)\n"
        "    assert data['jsonPath'] == os.environ['NGPOST_JSON']\n"
        "    assert 'rarPass' not in data\n"
        "    assert 'NGPOST_RAR_PASS' not in os.environ\n"
        "    with open(marker, 'a', encoding='utf-8') as f: f.write('1')\n"
        "    sys.stdout.write('A' * 100000)\n"
        "    sys.stderr.write('secret=' + sys.argv[3] + '\\n' + 'B' * 100000)\n"
        "elif mode == 'second':\n"
        "    with open(marker, 'a', encoding='utf-8') as f: f.write('2')\n"
        "elif mode == 'sleep':\n"
        "    time.sleep(5)\n"
        "elif mode == 'block':\n"
        "    time.sleep(1)\n"
        "    with open(marker, 'a', encoding='utf-8') as f: f.write('B')\n"
        "elif mode == 'settings':\n"
        "    with open(os.environ['NGPOST_JSON'], encoding='utf-8') as f:\n"
        "        data = json.load(f)\n"
        "    assert data['rarPass'] == sys.argv[3]\n"
        "    assert os.environ['NGPOST_RAR_PASS'] == sys.argv[3]\n"
        "    with open(marker, 'a', encoding='utf-8') as f: f.write('E')\n"
        "elif mode == 'sleep_then_write':\n"
        "    time.sleep(5)\n"
        "    with open(marker, 'a', encoding='utf-8') as f: f.write('T')\n"
        "elif mode == 'fail':\n"
        "    sys.stderr.write('expected settings failure\\n')\n"
        "    sys.exit(7)\n");
    script.close();
    return path;
}

class SynchronousAbortReply : public QNetworkReply
{
public:
    SynchronousAbortReply(QNetworkAccessManager::Operation operation,
                          QNetworkRequest const &request,
                          QObject *parent)
        : QNetworkReply(parent)
    {
        setOperation(operation);
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void abort() override
    {
        ++abortCalls;
        setError(OperationCanceledError, QStringLiteral("fixture abort"));
        setFinished(true);
        emit errorOccurred(OperationCanceledError);
        emit finished(); // deliberately direct, as permitted by QNetworkReply
    }

    int abortCalls = 0;

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class SynchronousAbortNetworkManager : public QNetworkAccessManager
{
public:
    QPointer<SynchronousAbortReply> reply;

protected:
    QNetworkReply *createRequest(Operation operation,
                                 QNetworkRequest const &request,
                                 QIODevice *) override
    {
        reply = new SynchronousAbortReply(operation, request, this);
        return reply;
    }
};

} // namespace

void TestPostInfoTemplate::fields_table_is_consistent()
{
    QSet<QString> placeholders;
    QSet<QString> envNames;
    for (PostInfoTemplate::FieldDoc const &field : PostInfoTemplate::fields()) {
        QString const placeholder = QString::fromLatin1(field.placeholder);
        QString const envName     = QString::fromLatin1(field.envName);

        QVERIFY2(placeholder.startsWith("__") && placeholder.endsWith("__"),
                 qPrintable(placeholder));
        QVERIFY2(envName.startsWith("NGPOST_"), qPrintable(envName));
        QVERIFY2(qstrlen(field.description) > 0, qPrintable(placeholder));
        QVERIFY2(!placeholders.contains(placeholder), qPrintable(placeholder));
        QVERIFY2(!envNames.contains(envName), qPrintable(envName));

        placeholders.insert(placeholder);
        envNames.insert(envName);
    }
    QVERIFY(placeholders.contains(PostInfoTemplate::passwordPlaceholder()));

    // The help panel is generated from this table, so every entry must be
    // describable: a variable added without a description would show a blank
    // line to the user instead of failing here.
    for (QString const &placeholder : placeholders) {
        PostInfoTemplate::Token token;
        token.raw  = placeholder;
        token.name = placeholder.mid(2, placeholder.size() - 4);
        QVERIFY2(!PostInfoTemplate::describe(token).isEmpty(), qPrintable(placeholder));
    }
}

void TestPostInfoTemplate::escaping_follows_the_declared_format()
{
    PostInfoData d = sampleData();
    d.meta.insert(QStringLiteral("titre"),
                  MetaValue(QStringLiteral("Mon \"super\" titre\\avec\nun saut & <balise>")));

    // No declaration: nothing is escaped. That is what lets any separator,
    // any wording and any prose work in a plain text sheet.
    QString const plain = QStringLiteral("titre =__meta:titre__\n");
    QCOMPARE(PostInfoTemplate::escapeModeIn(plain), PostInfoTemplate::Escape::None);
    QVERIFY(PostInfoTemplate::render(plain, d, false).contains(QStringLiteral("\"super\"")));

    // json: quotes, backslashes and the control characters
    QString const json = QStringLiteral("#!json\n{\"titre\": \"__meta:titre__\"}\n");
    QCOMPARE(PostInfoTemplate::escapeModeIn(json), PostInfoTemplate::Escape::Json);
    QString const rendered = PostInfoTemplate::render(
        PostInfoTemplate::stripComments(json), d, false,
        PostInfoTemplate::OnUnknown::KeepVerbatim, nullptr, false,
        PostInfoTemplate::Escape::Json);

    QJsonParseError err;
    QJsonDocument const doc = QJsonDocument::fromJson(rendered.toUtf8(), &err);
    QVERIFY2(err.error == QJsonParseError::NoError, qPrintable(err.errorString() + " | " + rendered));
    // and it survives the round trip unchanged, which is the whole point
    QCOMPARE(doc.object().value(QStringLiteral("titre")).toString(),
             d.meta.value(QStringLiteral("titre")).value);

    // xml
    QString const xml = QStringLiteral("#!xml\n<titre>__meta:titre__</titre>\n");
    QCOMPARE(PostInfoTemplate::escapeModeIn(xml), PostInfoTemplate::Escape::Xml);
    QString const xmlOut = PostInfoTemplate::render(
        PostInfoTemplate::stripComments(xml), d, false,
        PostInfoTemplate::OnUnknown::KeepVerbatim, nullptr, false,
        PostInfoTemplate::Escape::Xml);
    QVERIFY(xmlOut.contains(QStringLiteral("&amp; &lt;balise&gt;")));
    QVERIFY(!xmlOut.contains(QStringLiteral("<balise>")));

    // the declaration is a comment, so it never reaches the produced file
    QVERIFY(!PostInfoTemplate::stripComments(json).contains(QStringLiteral("#!json")));

    // an unknown format is reported, not guessed at
    QString unknown;
    QCOMPARE(PostInfoTemplate::escapeModeIn(QStringLiteral("#!yaml\na: __meta:titre__\n"), &unknown),
             PostInfoTemplate::Escape::None);
    QCOMPARE(unknown, QStringLiteral("yaml"));

    // "#!json" only counts as a directive on a comment line of its own
    QCOMPARE(PostInfoTemplate::escapeModeIn(QStringLiteral("titre =#!json\n")),
             PostInfoTemplate::Escape::None);
}

void TestPostInfoTemplate::render_template_file_in_memory_uses_the_complete_pipeline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tmplPath = dir.filePath(QStringLiteral("hostile.tpl"));
    QVERIFY(writeFile(tmplPath,
                      QStringLiteral("#!json\n"
                                     "# this comment is deliberately not valid JSON\n"
                                     "{\"title\":\"__meta:title__\","
                                     "\"unknown\":\"__futureField__\"}\n")));

    PostInfoData data = sampleData();
    const QString hostile = QStringLiteral("quote=\" slash=\\ newline=\n tab=\t control=")
                            + QChar(1) + QString::fromUtf8(" été 中文");
    data.meta.insert(QStringLiteral("title"), MetaValue(hostile));

    const PostInfoTemplate::RenderResult rendered =
        PostInfoTemplate::renderTemplateFile(tmplPath, data);
    QVERIFY2(rendered.ok, qPrintable(rendered.error));
    QCOMPARE(rendered.escape, PostInfoTemplate::Escape::Json);
    QVERIFY(!rendered.text.contains(QStringLiteral("#!json")));
    QVERIFY(!rendered.text.contains(QStringLiteral("not valid JSON")));
    QCOMPARE(rendered.warnings.size(), 1);
    QVERIFY(rendered.warnings.first().contains(QStringLiteral("__futureField__")));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rendered.text.toUtf8(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(parseError.errorString() + QStringLiteral(" | ") + rendered.text));
    QCOMPARE(document.object().value(QStringLiteral("title")).toString(), hostile);
    QCOMPARE(document.object().value(QStringLiteral("unknown")).toString(),
             QStringLiteral("__futureField__"));

    // Reading/rendering in memory is side-effect free: only the supplied model
    // exists until a caller explicitly chooses renderToFile().
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{ QStringLiteral("hostile.tpl") });

    const QString outPath = dir.filePath(QStringLiteral("hostile.json"));
    const PostInfoTemplate::Result written =
        PostInfoTemplate::renderToFile(tmplPath, outPath, data, QStringList());
    QVERIFY2(written.ok, qPrintable(written.error));
    QCOMPARE(readFile(written.outPath), rendered.text);
}

void TestPostInfoTemplate::shipped_json_model_produces_valid_json()
{
    // The models we ship must do what they promise, including for a post that
    // recorded nothing: an older post has no size and no par2 percentage, and
    // that is exactly when a hand written JSON model falls apart.
    // NGPOST_SOURCE_ROOT comes from qmake, which knows the repository as an
    // absolute path. __FILE__ does not: the compiler is handed a relative
    // source path on the CI runners, so the test ended up looking for the
    // model next to the working directory instead of in the sources.
    QString const path = QDir::cleanPath(
        QDir(QStringLiteral(NGPOST_SOURCE_ROOT))
            .absoluteFilePath(QStringLiteral("templates/post_info_json.txt")));

    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
    QString const tmpl = QString::fromUtf8(file.readAll());
    file.close();

    QCOMPARE(PostInfoTemplate::escapeModeIn(tmpl), PostInfoTemplate::Escape::Json);

    for (bool complete : { true, false }) {
        PostInfoData d;
        if (complete) {
            d = sampleData();
            d.meta.insert(QStringLiteral("title"),
                          MetaValue(QStringLiteral("A \"quoted\" title\nand a break")));
        }
        // else: everything empty, postSizeBytes at -1, par2Pct at -1

        QString const out = PostInfoTemplate::render(
            PostInfoTemplate::stripComments(tmpl), d, false,
            PostInfoTemplate::OnUnknown::KeepVerbatim, nullptr, false,
            PostInfoTemplate::Escape::Json);

        QJsonParseError err;
        QJsonDocument::fromJson(out.toUtf8(), &err);
        QVERIFY2(err.error == QJsonParseError::NoError,
                 qPrintable(QStringLiteral("%1 (complete=%2)\n%3")
                                .arg(err.errorString()).arg(complete).arg(out)));
    }
}

void TestPostInfoTemplate::a_secret_is_protected_even_once_escaped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A password the user chose, holding a character json has to escape.
    PostInfoData d = sampleData();
    d.rarPass = QStringLiteral("pa\"ss\\word");

    QString const tmplPath = dir.filePath(QStringLiteral("sheet.json"));
    {
        QFile f(tmplPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"pass\": \"__rarPass__\"}\n");
    }

    PostInfoTemplate::Result const res = PostInfoTemplate::renderToFile(
        tmplPath, dir.filePath(QStringLiteral("out.json")), d, QStringList());
    QVERIFY2(res.ok, qPrintable(res.error));

    // The sheet does hold the password, escaped. It must be protected all the
    // same: matching only the raw form would leave it world readable.
    QString const produced = readFile(res.outPath);
    QVERIFY2(produced.contains(QStringLiteral("pa\\\"ss\\\\word")), qPrintable(produced));

#ifdef Q_OS_UNIX
    QFile::Permissions const perms = QFile::permissions(res.outPath);
    QVERIFY2(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther)),
             qPrintable(QStringLiteral("permissions 0x%1 on %2")
                            .arg(static_cast<int>(perms), 0, 16)
                            .arg(res.outPath)));
#endif

    // Same for xml, where the escaped characters are different ones.
    PostInfoData x = sampleData();
    x.rarPass = QStringLiteral("pa&ss<word");
    QString const xmlTmpl = dir.filePath(QStringLiteral("sheet.xml"));
    {
        QFile f(xmlTmpl);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<pass>__rarPass__</pass>\n");
    }
    PostInfoTemplate::Result const xres = PostInfoTemplate::renderToFile(
        xmlTmpl, dir.filePath(QStringLiteral("out.xml")), x, QStringList());
    QVERIFY2(xres.ok, qPrintable(xres.error));
    QVERIFY(readFile(xres.outPath).contains(QStringLiteral("pa&amp;ss&lt;word")));
#ifdef Q_OS_UNIX
    QFile::Permissions const xperms = QFile::permissions(xres.outPath);
    QVERIFY(!(xperms & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif

    // A value with nothing to escape must come out byte for byte identical,
    // in every mode: escaping must never be a reformatting.
    for (auto mode : { PostInfoTemplate::Escape::None,
                       PostInfoTemplate::Escape::Json,
                       PostInfoTemplate::Escape::Xml }) {
        QCOMPARE(PostInfoTemplate::escapeValue(QStringLiteral("Rando Mercantour 2026 - 1080p"),
                                               mode),
                 QStringLiteral("Rando Mercantour 2026 - 1080p"));
        // accents and CJK go through as themselves, they are valid utf-8
        QCOMPARE(PostInfoTemplate::escapeValue(QStringLiteral("Été 中文"), mode),
                 QStringLiteral("Été 中文"));
    }
}

void TestPostInfoTemplate::format_falls_back_on_the_model_name()
{
    QString const plain = QStringLiteral("titre =__meta:titre__\n");

    // Nothing declared: the name of the model decides.
    QCOMPARE(PostInfoTemplate::escapeModeFor(plain, QStringLiteral("/m/sheet.json")),
             PostInfoTemplate::Escape::Json);
    QCOMPARE(PostInfoTemplate::escapeModeFor(plain, QStringLiteral("/m/sheet.XML")),
             PostInfoTemplate::Escape::Xml);
    QCOMPARE(PostInfoTemplate::escapeModeFor(plain, QStringLiteral("/m/sheet.txt")),
             PostInfoTemplate::Escape::None);
    QCOMPARE(PostInfoTemplate::escapeModeFor(plain, QString()),
             PostInfoTemplate::Escape::None);

    // yaml is not supported: quoting a yaml scalar depends on where it sits,
    // so escaping alone would change the TYPE of a value without saying so.
    QCOMPARE(PostInfoTemplate::escapeModeFor(plain, QStringLiteral("/m/sheet.yaml")),
             PostInfoTemplate::Escape::None);

    // A declaration always wins over the name, in both directions.
    QCOMPARE(PostInfoTemplate::escapeModeFor(QStringLiteral("#!xml\n") + plain,
                                             QStringLiteral("/m/sheet.json")),
             PostInfoTemplate::Escape::Xml);

    // Including when it is unrecognised: the author said something, so the
    // extension does not get to answer in their place. It is reported instead.
    QString unknown;
    QCOMPARE(PostInfoTemplate::escapeModeFor(QStringLiteral("#!yaml\n") + plain,
                                             QStringLiteral("/m/sheet.json"), &unknown),
             PostInfoTemplate::Escape::None);
    QCOMPARE(unknown, QStringLiteral("yaml"));

    // A word ngPost does not know, followed by one it does: the file IS
    // escaped, so warning about the first would point at a problem that is
    // not there.
    unknown.clear();
    QCOMPARE(PostInfoTemplate::escapeModeFor(QStringLiteral("#!yaml\n#!json\n") + plain,
                                             QString(), &unknown),
             PostInfoTemplate::Escape::Json);
    QVERIFY2(unknown.isEmpty(), qPrintable(unknown));
}

void TestPostInfoTemplate::escaping_never_touches_paths_or_commands()
{
    PostInfoData d = sampleData();
    d.rarName = QStringLiteral("my \"archive\" & co");

    // An output path is a file name, not a document: escaping it would create
    // a file literally called my &quot;archive&quot;.
    QCOMPARE(PostInfoTemplate::render(QStringLiteral("/tmp/__rarName__.txt"), d, false),
             QStringLiteral("/tmp/my \"archive\" & co.txt"));

    // Same for a post command argument: the shell is not xml either.
    QStringList const args = PostInfoTemplate::renderArguments(
        QStringList{ QStringLiteral("echo"), QStringLiteral("__rarName__") }, d, false);
    QCOMPARE(args.at(1), d.rarName);
}

void TestPostInfoTemplate::template_lines_round_trip()
{
    QString const tmpl = QStringLiteral("# a header, never written out\n"
                                        "\n"
                                        "date        =__date:dd/MM/yyyy__\n"
                                        "size human  =__postSizeHuman__\n"
                                        "url =http://x/y=z&a=b\n"
                                        " # written, it is indented\n"
                                        "----------\n"
                                        "titre =__meta:titre__\n");

    QVector<PostInfoTemplate::SheetLine> const lines = PostInfoTemplate::parseTemplate(tmpl);

    // Opened and saved again, a model is unchanged. That is what makes an
    // editor safe to point at somebody else's file.
    QCOMPARE(PostInfoTemplate::buildTemplate(lines), tmpl);

    QCOMPARE(lines.at(0).kind, PostInfoTemplate::SheetLine::Kind::Comment);
    QCOMPARE(lines.at(1).kind, PostInfoTemplate::SheetLine::Kind::Raw); // blank
    QCOMPARE(lines.at(2).kind, PostInfoTemplate::SheetLine::Kind::Field);
    QCOMPARE(lines.at(2).label, QStringLiteral("date"));
    QCOMPARE(lines.at(2).expression, QStringLiteral("__date:dd/MM/yyyy__"));

    // alignment is part of the file, so it survives
    QCOMPARE(lines.at(3).label, QStringLiteral("size human"));
    QCOMPARE(lines.at(3).separator, QStringLiteral("  ="));

    // split on the FIRST '=' only: an url keeps its own
    QCOMPARE(lines.at(4).label, QStringLiteral("url"));
    QCOMPARE(lines.at(4).expression, QStringLiteral("http://x/y=z&a=b"));

    // '#' is a comment in the first column only; one space and it is content
    QCOMPARE(lines.at(5).kind, PostInfoTemplate::SheetLine::Kind::Raw);
    QCOMPARE(lines.at(6).kind, PostInfoTemplate::SheetLine::Kind::Raw); // "----------"

    // a CRLF model comes back as a CRLF model
    QString const crlf = QStringLiteral("date =__date__\r\ntitre =__meta:titre__\r\n");
    QVERIFY(PostInfoTemplate::usesCrLf(crlf));
    QCOMPARE(PostInfoTemplate::buildTemplate(PostInfoTemplate::parseTemplate(crlf), true), crlf);
    QCOMPARE(PostInfoTemplate::parseTemplate(crlf).at(0).expression, QStringLiteral("__date__"));
}

void TestPostInfoTemplate::comment_lines_are_not_written()
{
    QString const tmpl = QStringLiteral("# mot de passe =__rarPass__\n"
                                        "date =__date:yyyy__\n"
                                        " # gardez cette ligne\n");

    QCOMPARE(PostInfoTemplate::stripComments(tmpl),
             QStringLiteral("date =__date:yyyy__\n # gardez cette ligne\n"));

    // A variable quoted in a comment asks for nothing: the editor must not
    // offer a field the sheet will never carry, and __rarPass__ in a comment
    // must not turn the file into a secret one.
    QCOMPARE(PostInfoTemplate::metaNamesIn(QStringLiteral("# titre =__meta:titre__\n")),
             QStringList());
    QVERIFY(PostInfoTemplate::tokensIn(QStringLiteral("# x =__rarPass__\n")).isEmpty());

    PostInfoData d = sampleData();
    QString const rendered = PostInfoTemplate::render(PostInfoTemplate::stripComments(tmpl), d, false);
    QVERIFY(!rendered.contains(QStringLiteral("mot de passe")));
    QVERIFY(!rendered.contains(d.rarPass));
    QVERIFY(rendered.contains(QStringLiteral("gardez cette ligne")));
}

void TestPostInfoTemplate::tokens_in_lists_every_variable_of_a_model()
{
    QString const tmpl = QStringLiteral("nom =__originalName__\n"
                                        "titre =__meta:titre__\n"
                                        "date =__date:dd/MM/yyyy__\n"
                                        "annee =__date:yyyy__\n"
                                        "encore =__originalName__\n"
                                        "libre =50%1 off\n"
                                        "inconnu =__nawak__\n");

    QVector<PostInfoTemplate::Token> const tokens = PostInfoTemplate::tokensIn(tmpl);

    QStringList raws;
    for (PostInfoTemplate::Token const &t : tokens)
        raws << t.raw;

    // in the order of the model, without duplicates, and the legacy "%1" is
    // not a variable so it is not listed
    QCOMPARE(raws,
             (QStringList{ QStringLiteral("__originalName__"),
                           QStringLiteral("__meta:titre__"),
                           QStringLiteral("__date:dd/MM/yyyy__"),
                           QStringLiteral("__date:yyyy__"),
                           QStringLiteral("__nawak__") }));

    // same variable, two formats: two lines of the sheet, two entries
    QCOMPARE(tokens.at(2).name, QStringLiteral("date"));
    QCOMPARE(tokens.at(2).arg, QStringLiteral("dd/MM/yyyy"));
    QCOMPARE(tokens.at(3).arg, QStringLiteral("yyyy"));

    QVERIFY(tokens.at(1).isMeta());
    QCOMPARE(tokens.at(1).arg, QStringLiteral("titre"));
    QVERIFY(!tokens.at(0).isMeta());

    // an unknown variable has no description, which is how the editor tells
    // the user it will be copied as it is
    QVERIFY(PostInfoTemplate::describe(tokens.at(4)).isEmpty());
    QVERIFY(!PostInfoTemplate::describe(tokens.at(0)).isEmpty());
    QVERIFY(!PostInfoTemplate::describe(tokens.at(2)).isEmpty());

    // metaNamesIn stays the subset the editor uses for the blanks
    QCOMPARE(PostInfoTemplate::metaNamesIn(tmpl), QStringList{ QStringLiteral("titre") });
}

void TestPostInfoTemplate::fixed_variables_resolve()
{
    PostInfoData d = sampleData();

    // every declared variable must resolve to something (possibly empty)
    for (PostInfoTemplate::FieldDoc const &field : PostInfoTemplate::fields()) {
        QStringList unknown;
        PostInfoTemplate::render(QString::fromLatin1(field.placeholder),
                                 d,
                                 false,
                                 PostInfoTemplate::OnUnknown::KeepVerbatim,
                                 &unknown);
        QVERIFY2(unknown.isEmpty(), field.placeholder);
    }

    QCOMPARE(PostInfoTemplate::render("__postSize__", d, false), QStringLiteral("2505484398"));
    QCOMPARE(PostInfoTemplate::render("__rarName__", d, false),
             QStringLiteral("876EFID5Y22SH1CO5C7C"));
    QCOMPARE(PostInfoTemplate::render("__par2Pct__", d, false), QStringLiteral("8"));

    // par2 disabled: empty, not "0", even if a global percentage is configured
    d.par2Pct = -1;
    QCOMPARE(PostInfoTemplate::render("__par2Pct__", d, false), QString());

    // a size that was never recorded renders empty, not as a very wrong zero:
    // an index importing the sheet would take 0 bytes at face value
    d.postSizeBytes = -1;
    QCOMPARE(PostInfoTemplate::render("__postSize__", d, false), QString());
    QCOMPARE(PostInfoTemplate::render("__postSizeHuman__", d, false), QString());
    d.postSizeBytes = 2505484398LL;

    // the legacy size keeps its own value, it is not aliased to postSize
    d.legacySizeBytes = 42;
    QCOMPARE(PostInfoTemplate::render("__sizeInByte__", d, false), QStringLiteral("42"));
    QCOMPARE(PostInfoTemplate::render("__postSize__", d, false), QStringLiteral("2505484398"));
}

void TestPostInfoTemplate::date_default_and_custom_format()
{
    PostInfoData const d = sampleData();
    QCOMPARE(PostInfoTemplate::render("__date__", d, false), QStringLiteral("2026-08-15"));
    QCOMPARE(PostInfoTemplate::render("__date:dd/MM/yyyy__", d, false),
             QStringLiteral("15/08/2026"));
    QCOMPARE(PostInfoTemplate::render("__date:yyyy-MM-dd HH:mm__", d, false),
             QStringLiteral("2026-08-15 22:00"));
    QCOMPARE(PostInfoTemplate::render("__dateStart:HH:mm__", d, false), QStringLiteral("21:30"));

    PostInfoData empty;
    QCOMPARE(PostInfoTemplate::render("__date__", empty, false), QString());
}

void TestPostInfoTemplate::meta_present_and_missing()
{
    PostInfoData const d = sampleData();
    QCOMPARE(PostInfoTemplate::render("__meta:genre__", d, false), QStringLiteral("Nature"));

    QStringList unknown;
    QCOMPARE(PostInfoTemplate::render("[__meta:absent__]",
                                      d,
                                      false,
                                      PostInfoTemplate::OnUnknown::KeepVerbatim,
                                      &unknown),
             QStringLiteral("[]"));
    QVERIFY(unknown.isEmpty()); // a blank metadata is normal, not a mistake
}

void TestPostInfoTemplate::unknown_variable_kept_and_reported()
{
    PostInfoData const d = sampleData();
    QStringList        unknown;
    QCOMPARE(PostInfoTemplate::render("a __nope__ b",
                                      d,
                                      false,
                                      PostInfoTemplate::OnUnknown::KeepVerbatim,
                                      &unknown),
             QStringLiteral("a __nope__ b"));
    QCOMPARE(unknown, QStringList{ QStringLiteral("__nope__") });
}

void TestPostInfoTemplate::substitution_is_single_pass()
{
    PostInfoData d = sampleData();
    d.rarName      = QStringLiteral("__nzbPath__");
    QCOMPARE(PostInfoTemplate::render("__rarName__", d, false), QStringLiteral("__nzbPath__"));
}

void TestPostInfoTemplate::legacy_percent_one_is_substituted()
{
    PostInfoData d = sampleData();

    // a post command line: %1 is the historical alias of the nzb path
    QCOMPARE(PostInfoTemplate::renderArguments({ QStringLiteral("%1") }, d, false),
             QStringList{ QStringLiteral("/data/nzb/Rando.nzb") });

    // a record sheet: it is text, and must be left alone
    QCOMPARE(PostInfoTemplate::render("remise =50%1 sur tout", d, false),
             QStringLiteral("remise =50%1 sur tout"));

    // and a value containing %1 is not re-expanded either
    d.rarName = QStringLiteral("%1");
    QCOMPARE(PostInfoTemplate::renderArguments({ QStringLiteral("__rarName__") }, d, false),
             QStringList{ QStringLiteral("%1") });
}

void TestPostInfoTemplate::arguments_with_spaces_stay_one_argument()
{
    PostInfoData d = sampleData();
    d.nzbPath      = QStringLiteral("/data/my nzb/Le Film \"2026\".nzb");

    QStringList const args     = { QStringLiteral("scp"),
                                   QStringLiteral("__nzbPath__"),
                                   QStringLiteral("box:") };
    QStringList const rendered = PostInfoTemplate::renderArguments(args, d, false);

    QCOMPARE(rendered.size(), 3);
    QCOMPARE(rendered.at(1), QStringLiteral("/data/my nzb/Le Film \"2026\".nzb"));
}

void TestPostInfoTemplate::secrets_are_redacted()
{
    PostInfoData const d      = sampleData();
    QString const      logged = PostInfoTemplate::redactSecrets(
        QStringLiteral("upload.sh /data/nzb/Fuze.nzb s3cr3t"), d);
    QVERIFY(!logged.contains(QStringLiteral("s3cr3t")));
    QVERIFY(logged.contains(QStringLiteral("****")));
}

void TestPostInfoTemplate::meta_names_are_read_off_a_template()
{
    QCOMPARE(PostInfoTemplate::metaNamesIn(QStringLiteral(
                 "titre =__meta:titre__\n"
                 "cat =__meta:categorie__\n"
                 "encore =__meta:titre__\n"   // a repeat is one field, not two
                 "taille =__postSize__\n"     // not a metadata
                 "date =__date:dd/MM/yyyy__\n")),
             QStringList({ QStringLiteral("titre"), QStringLiteral("categorie") }));

    QVERIFY(PostInfoTemplate::metaNamesIn(QStringLiteral("nothing here")).isEmpty());
    // an empty name is not a field
    QVERIFY(PostInfoTemplate::metaNamesIn(QStringLiteral("x =__meta:__")).isEmpty());
}

void TestPostInfoTemplate::template_path_resolution()
{
    QCOMPARE(PostInfoTemplate::resolveTemplatePath("/etc/ngPost/f.txt", "/base"),
             QStringLiteral("/etc/ngPost/f.txt"));
    QCOMPARE(PostInfoTemplate::resolveTemplatePath("f.txt", "/base"),
             QStringLiteral("/base/f.txt"));
    QCOMPARE(PostInfoTemplate::resolveTemplatePath("sub/../f.txt", "/base"),
             QStringLiteral("/base/f.txt"));
    QCOMPARE(PostInfoTemplate::resolveTemplatePath("~/f.txt", "/base"),
             QDir::homePath() + QStringLiteral("/f.txt"));
    QCOMPARE(PostInfoTemplate::resolveTemplatePath(QString(), "/base"), QString());
}

void TestPostInfoTemplate::render_to_file_creates_parent_folder_and_keeps_utf8()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString const tmplPath = dir.filePath("fiche.tpl");
    QVERIFY(writeFile(tmplPath,
                      QString::fromUtf8("titre =__meta:titre__\nete =Mon \xC3\x89t\xC3\xA9\n")));

    PostInfoData d = sampleData();
    d.meta.insert(QStringLiteral("titre"), MetaValue(QString::fromUtf8("L'\xC3\x89t\xC3\xA9 & Cie")));

    PostInfoTemplate::Result const res =
        PostInfoTemplate::renderToFile(tmplPath,
                                       dir.filePath("out/__meta:categorie__/__nzbName__.info.txt"),
                                       d,
                                       QStringList());

    QVERIFY2(res.ok, qPrintable(res.error));
    QCOMPARE(QFileInfo(res.outPath).fileName(), QStringLiteral("Rando.info.txt"));
    QCOMPARE(QFileInfo(res.outPath).absoluteDir().dirName(), QStringLiteral("Video perso"));
    QCOMPARE(readFile(res.outPath),
             QString::fromUtf8("titre =L'\xC3\x89t\xC3\xA9 & Cie\nete =Mon \xC3\x89t\xC3\xA9\n"));
}

void TestPostInfoTemplate::unknown_variable_in_output_path_is_fatal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const tmplPath = dir.filePath("fiche.tpl");
    QVERIFY(writeFile(tmplPath, QStringLiteral("x\n")));

    PostInfoTemplate::Result const res =
        PostInfoTemplate::renderToFile(tmplPath,
                                       dir.filePath("__oups__/f.txt"),
                                       sampleData(),
                                       QStringList());
    QVERIFY(!res.ok);
    QVERIFY(res.error.contains(QStringLiteral("__oups__")));
    QVERIFY(!QFileInfo::exists(dir.filePath("__oups__/f.txt")));
}

void TestPostInfoTemplate::relative_output_resolves_against_the_given_base()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath("conf")));

    QString const tmplPath = dir.filePath("sheet.tpl");
    QVERIFY(writeFile(tmplPath, QStringLiteral("x\n")));

    PostInfoData const d = sampleData();
    PostInfoTemplate::Result const res = PostInfoTemplate::renderToFile(
        tmplPath, QStringLiteral("out/sheet.txt"), d, QStringList(), dir.filePath("conf"));

    QVERIFY2(res.ok, qPrintable(res.error));
    QCOMPARE(res.outPath, QDir::cleanPath(dir.filePath("conf/out/sheet.txt")));
    QVERIFY(QFileInfo::exists(res.outPath));
}

void TestPostInfoTemplate::password_is_removed_from_an_inherited_environment()
{
    PostInfoData d = sampleData();
    d.rarPass = QStringLiteral("s3cr3t");

    QProcessEnvironment env;
    // ngPost itself was started with one, for another post entirely
    env.insert(QStringLiteral("NGPOST_RAR_PASS"), QStringLiteral("inherited-from-elsewhere"));

    PostInfoTemplate::applyEnvironment(env, d, /*exposeSecrets*/ false);
    QVERIFY2(!env.contains(QStringLiteral("NGPOST_RAR_PASS")),
             qPrintable(env.value(QStringLiteral("NGPOST_RAR_PASS"))));

    PostInfoTemplate::applyEnvironment(env, d, /*exposeSecrets*/ true);
    QCOMPARE(env.value(QStringLiteral("NGPOST_RAR_PASS")), QStringLiteral("s3cr3t"));
}

void TestPostInfoTemplate::refuses_to_overwrite_protected_paths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString const tmplPath = dir.filePath("fiche.tpl");
    QVERIFY(writeFile(tmplPath, QStringLiteral("x\n")));
    QVERIFY(QDir(dir.path()).mkpath("src/season"));
    QString const nzbPath    = dir.filePath("out.nzb");
    QString const sourceDir  = dir.filePath("src");
    QStringList const guards = { nzbPath, tmplPath, sourceDir };

    PostInfoData d = sampleData();

    // the nzb itself
    QVERIFY(!PostInfoTemplate::renderToFile(tmplPath, nzbPath, d, guards).ok);
    // the template itself
    QVERIFY(!PostInfoTemplate::renderToFile(tmplPath, tmplPath, d, guards).ok);
    // a file below a source folder, not just the folder path
    QVERIFY(!PostInfoTemplate::renderToFile(tmplPath, sourceDir + "/season/ep.mkv", d, guards).ok);
    // and something outside is still fine
    QVERIFY(PostInfoTemplate::renderToFile(tmplPath, dir.filePath("ok.txt"), d, guards).ok);
}

void TestPostInfoTemplate::refuses_a_secret_in_the_output_path()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tmplPath = dir.filePath(QStringLiteral("sheet.tpl"));
    QVERIFY(writeFile(tmplPath, QStringLiteral("name=__nzbName__\n")));

    PostInfoData data = sampleData();
    data.rarPass = QStringLiteral("visible-secret");
    const PostInfoTemplate::Result result = PostInfoTemplate::renderToFile(
        tmplPath,
        dir.filePath(QStringLiteral("__rarPass__.txt")),
        data,
        {});
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("secret")), qPrintable(result.error));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("visible-secret.txt"))));
}

void TestPostInfoTemplate::protection_resolves_symlinks()
{
#ifndef Q_OS_UNIX
    QSKIP("symlinks need a unix filesystem here");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString const tmplPath = dir.filePath("sheet.tpl");
    QVERIFY(writeFile(tmplPath, QStringLiteral("x\n")));

    // a source, and a symlink pointing at it
    QString const source = dir.filePath("source.bin");
    QVERIFY(writeFile(source, QStringLiteral("precious\n")));
    QString const link = dir.filePath("link-to-source.bin");
    QVERIFY(QFile::link(source, link));

    PostInfoData d = sampleData();

    // protecting the real file must also refuse writing through the link
    QVERIFY(!PostInfoTemplate::renderToFile(tmplPath, link, d, { source }).ok);
    // and protecting the link must refuse writing on the real file
    QVERIFY(!PostInfoTemplate::renderToFile(tmplPath, source, d, { link }).ok);

    QCOMPARE(readFile(source), QStringLiteral("precious\n"));
#endif
}

void TestPostInfoTemplate::permissions_are_restricted_when_a_secret_is_written()
{
#ifndef Q_OS_UNIX
    QSKIP("owner only permissions are a firm guarantee on Unix only");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString const withPass    = dir.filePath("with.tpl");
    QString const withoutPass = dir.filePath("without.tpl");
    QVERIFY(writeFile(withPass, QStringLiteral("pass =__rarPass__\n")));
    QVERIFY(writeFile(withoutPass, QStringLiteral("name =__rarName__\n")));

    PostInfoData const d = sampleData();

    PostInfoTemplate::Result const secret =
        PostInfoTemplate::renderToFile(withPass, dir.filePath("secret.txt"), d, QStringList());
    QVERIFY2(secret.ok, qPrintable(secret.error));
    // Permissions are applied to the temporary file before the first write, so
    // the password is never readable by others, not even in between.
    // Qt reports the same Unix bits twice (Owner = the file owner, User = the
    // current user), so the contract is expressed as "owner yes, nobody else".
    QFileDevice::Permissions const perms = QFile::permissions(secret.outPath);
    QVERIFY(perms & QFileDevice::ReadOwner);
    QVERIFY(perms & QFileDevice::WriteOwner);
    QVERIFY(!(perms
              & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                 | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    // A post info file without any secret keeps the usual permissions, whatever
    // the umask of the machine says: compare against a plainly created file.
    PostInfoTemplate::Result const plain =
        PostInfoTemplate::renderToFile(withoutPass, dir.filePath("plain.txt"), d, QStringList());
    QVERIFY2(plain.ok, qPrintable(plain.error));
    QVERIFY(writeFile(dir.filePath("reference.txt"), QStringLiteral("x")));
    QCOMPARE(QFile::permissions(plain.outPath), QFile::permissions(dir.filePath("reference.txt")));
#endif
}

void TestPostInfoTemplate::baselien_template_golden()
{
    QString const tmplPath = repoTemplate(QStringLiteral("post_info_baselien.txt"));
    QVERIFY2(QFileInfo::exists(tmplPath), qPrintable(tmplPath));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostInfoTemplate::Result const res =
        PostInfoTemplate::renderToFile(tmplPath, dir.filePath("fiche.txt"), sampleData(), QStringList());
    QVERIFY2(res.ok, qPrintable(res.error));

    QString const expected = QString::fromUtf8(
        "date =15/08/2026\n"
        "nom du post =Rando-Mercantour-2026.mkv\n"
        "taille post =2505484398\n"
        "mot de passe =s3cr3t\n"
        "nom a rechercher =876EFID5Y22SH1CO5C7C\n"
        "posteur =h0wsef7@8xw81s.r7\n"
        "groupe =alt.binaries.test,alt.binaries.misc\n"
        "pourcent =8\n"
        "portail1 =https://example.org/albums/view?id=326598&size=full\n"
        "titre =Randonnee au Mercantour, \"2026\"\n"
        "categorie =Video perso\n"
        "qualite =1080p\n"
        "genre =Nature\n");
    // Git may check the template out with CRLF on Windows. The renderer
    // deliberately preserves the model's line endings, so compare text after
    // canonicalising the platform representation.
    QString actual = readFile(res.outPath);
    actual.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QCOMPARE(actual, expected);
}

void TestPostInfoTemplate::post_command_runner_fifo_json_secrets_and_bounded_output()
{
    const QString python = pythonExecutable();
    if (python.isEmpty())
        QSKIP("Python is required for the cross-platform hook fixture");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = writeRunnerScript(dir);
    QVERIFY(!script.isEmpty());
    const QString marker = dir.filePath(QStringLiteral("order.txt"));

    PostInfoData data = sampleData();
    data.historyPostId = 4711;
    data.rarPass = QStringLiteral("runner-secret-4711");

    const QString first = QStringLiteral("%1 %2 first %3 %4")
                              .arg(quotedCommandArg(python),
                                   quotedCommandArg(script),
                                   quotedCommandArg(marker),
                                   quotedCommandArg(data.rarPass));
    const QString second = QStringLiteral("%1 %2 second %3")
                               .arg(quotedCommandArg(python),
                                    quotedCommandArg(script),
                                    quotedCommandArg(marker));

    QNetworkAccessManager network;
    PostCmdRunner runner(network);
    PostCmdRunner::Settings settings;
    settings.cmdTimeoutSec = 10;
    settings.exposeSecrets = false;
    runner.setSettings(settings);

    QSignalSpy idleSpy(&runner, &PostCmdRunner::idle);
    QSignalSpy logSpy(&runner, &PostCmdRunner::log);
    QSignalSpy errorSpy(&runner, &PostCmdRunner::error);

    runner.enqueue(data, { first }, QUrl());
    runner.enqueue(data, { second }, QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(runner.isIdle(), 15000);
    QVERIFY(idleSpy.count() >= 1);
    QCOMPARE(readFile(marker), QStringLiteral("12"));
    QCOMPARE(errorSpy.count(), 0);

    QString logs;
    for (const QList<QVariant> &row : logSpy)
        logs += row.at(0).toString() + QLatin1Char('\n');
    QVERIFY2(!logs.contains(data.rarPass), qPrintable(logs));
    QVERIFY2(logs.contains(QStringLiteral("****")), qPrintable(logs));
    QVERIFY2(logs.contains(QStringLiteral("bytes omitted")), qPrintable(logs));
}

void TestPostInfoTemplate::post_command_runner_timeout_and_start_failure()
{
    const QString python = pythonExecutable();
    if (python.isEmpty())
        QSKIP("Python is required for the cross-platform hook fixture");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = writeRunnerScript(dir);
    QVERIFY(!script.isEmpty());
    const QString marker = dir.filePath(QStringLiteral("after-errors.txt"));

    const QString sleeper = QStringLiteral("%1 %2 sleep %3")
                                .arg(quotedCommandArg(python),
                                     quotedCommandArg(script),
                                     quotedCommandArg(marker));
    const QString after = QStringLiteral("%1 %2 second %3")
                              .arg(quotedCommandArg(python),
                                   quotedCommandArg(script),
                                   quotedCommandArg(marker));

    PostInfoData data = sampleData();
    data.historyPostId = 4712;

    QNetworkAccessManager network;
    PostCmdRunner runner(network);
    PostCmdRunner::Settings settings;
    settings.cmdTimeoutSec = 1;
    settings.failIsError = false;
    runner.setSettings(settings);

    QSignalSpy logSpy(&runner, &PostCmdRunner::log);
    runner.enqueue(data,
                   { sleeper,
                     QStringLiteral("ngpost-command-that-does-not-exist-4712"),
                     after },
                   QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(runner.isIdle(), 10000);
    QCOMPARE(readFile(marker), QStringLiteral("2"));

    QString logs;
    for (const QList<QVariant> &row : logSpy)
        logs += row.at(0).toString() + QLatin1Char('\n');
    QVERIFY2(logs.contains(QStringLiteral("timed out"), Qt::CaseInsensitive), qPrintable(logs));
    QVERIFY2(logs.contains(QStringLiteral("could not run"), Qt::CaseInsensitive), qPrintable(logs));
}

void TestPostInfoTemplate::post_command_runner_snapshots_settings_per_task()
{
    const QString python = pythonExecutable();
    if (python.isEmpty())
        QSKIP("Python is required for the cross-platform hook fixture");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = writeRunnerScript(dir);
    QVERIFY(!script.isEmpty());
    const QString marker = dir.filePath(QStringLiteral("settings-order.txt"));

    auto command = [&](QString const &mode, QString const &extra = QString()) {
        QString cmd = QStringLiteral("%1 %2 %3 %4")
                          .arg(quotedCommandArg(python),
                               quotedCommandArg(script),
                               mode,
                               quotedCommandArg(marker));
        if (!extra.isEmpty())
            cmd += QLatin1Char(' ') + quotedCommandArg(extra);
        return cmd;
    };

    QNetworkAccessManager network;
    PostCmdRunner runner(network);
    QSignalSpy logSpy(&runner, &PostCmdRunner::log);
    QSignalSpy errorSpy(&runner, &PostCmdRunner::error);

    PostInfoData data = sampleData();
    data.rarPass = QStringLiteral("snapshot-secret-4810");

    // Keep the first task alive while all the following tasks and their
    // distinct settings are queued.
    PostCmdRunner::Settings settings;
    settings.cmdTimeoutSec = 10;
    runner.setSettings(settings);
    data.historyPostId = 4809;
    runner.enqueue(data, { command(QStringLiteral("block")) }, QUrl());

    settings.exposeSecrets = true;
    settings.failIsError = true;
    runner.setSettings(settings);
    data.historyPostId = 4810;
    runner.enqueue(data,
                   { command(QStringLiteral("settings"), data.rarPass) },
                   QUrl());

    settings.cmdTimeoutSec = 1;
    settings.exposeSecrets = false;
    settings.failIsError = false;
    runner.setSettings(settings);
    data.historyPostId = 4811;
    runner.enqueue(data, { command(QStringLiteral("sleep_then_write")) }, QUrl());

    settings.cmdTimeoutSec = 10;
    settings.failIsError = true;
    runner.setSettings(settings);
    data.historyPostId = 4812;
    runner.enqueue(data, { command(QStringLiteral("fail")) }, QUrl());

    // This configuration must affect only tasks queued from now on.
    settings.cmdTimeoutSec = 0;
    settings.exposeSecrets = false;
    settings.failIsError = false;
    runner.setSettings(settings);

    QTRY_VERIFY_WITH_TIMEOUT(runner.isIdle(), 10000);
    QCOMPARE(readFile(marker), QStringLiteral("BE"));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("exit code 7")),
             qPrintable(errorSpy.at(0).at(0).toString()));

    QString logs;
    for (const QList<QVariant> &row : logSpy)
        logs += row.at(0).toString() + QLatin1Char('\n');
    QVERIFY2(logs.contains(QStringLiteral("timed out after 1s"), Qt::CaseInsensitive),
             qPrintable(logs));
}

void TestPostInfoTemplate::file_uploader_release_handles_synchronous_abort()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString nzbPath = dir.filePath(QStringLiteral("post.nzb"));
    QVERIFY(writeFile(nzbPath, QStringLiteral("fixture")));

    SynchronousAbortNetworkManager network;
    FileUploader uploader(network, nzbPath);
    // This is the same direct re-entry shape as PostCmdRunner::onUploadDone():
    // finished -> readyToDie -> release while the original release is in abort.
    connect(&uploader, &FileUploader::readyToDie,
            &uploader, &FileUploader::release, Qt::DirectConnection);
    QSignalSpy readySpy(&uploader, &FileUploader::readyToDie);

    uploader.startUpload(QUrl(QStringLiteral("https://example.invalid/upload")));
    QVERIFY(network.reply);
    SynchronousAbortReply *reply = network.reply;
    uploader.release();

    QCOMPARE(reply->abortCalls, 1);
    QCOMPARE(readySpy.count(), 0); // release detached finished before aborting
    QVERIFY(QFile::rename(nzbPath, dir.filePath(QStringLiteral("moved.nzb"))));
}

QTEST_MAIN(TestPostInfoTemplate)
#include "tst_PostInfoTemplate.moc"
