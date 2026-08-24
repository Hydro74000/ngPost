// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_MainWindow.cpp — offscreen GUI tests for the MainWindow's server
// table, focused on the dynamic server-row widgets that Phase 1d
// retrofitted with objectName()s.
//
// These tests do NOT call MainWindow::init(NgPost*) — that would require
// a real NgPost, which pulls in network/config/VPN state. Instead they
// exercise the parts of the window that are usable without a backend:
// the "Add server" button, the per-row widget creation in _addServer(),
// the per-row "Use VPN" checkbox signal, and the delete button.
//
// Run headless:  QT_QPA_PLATFORM=offscreen ./tst_MainWindow
//
//========================================================================

#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QToolButton>

#include "hmi/CheckBoxCenterWidget.h"
#include "hmi/PostInfoDialog.h"

#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QDateTime>
#include <QTableWidget>
#include <QTextStream>

#include "hmi/MainWindow.h"
#include "hmi/PostingWidget.h"
#include "hmi/CheckBoxCenterWidget.h"
#include "utils/PathHelper.h"
#include "NgPost.h"
#include "TestEnv.h"

using ngpost::tests::HomeSandbox;

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    //! Clicking the "Add Server" button adds a row to the servers table,
    //! and every per-row widget retrofitted with an objectName is findable
    //! from the window root.
    void add_server_row_creates_named_widgets();

    //! Two consecutive Adds produce two rows with distinct objectName
    //! suffixes (`_0` and `_1`).
    void add_two_servers_yields_unique_object_names();

    //! Toggling the per-row "Use VPN" checkbox via the inner QCheckBox
    //! programmatically fires CheckBoxCenterWidget::toggled.
    void vpn_checkbox_toggled_emits_signal();

    //! Long history details should scroll inside the history panel instead of
    //! changing the top-level window dimensions.
    void history_detail_text_does_not_resize_window();

    //! Save Config must persist the active GUI tab's RAR_MAX checkbox and
    //! PAR2_PCT spinbox, even when PAR2_ARGS is present.
    void save_config_persists_rar_max_and_par2_pct();

    //! Upgrading: a configuration written by an older ngPost must survive a
    //! GUI save, which rewrites the whole file. Losing a user setting on the
    //! first save after an update would be the worst kind of regression.
    void save_config_preserves_an_older_configuration();

    //! Every new post info / post command key survives a full round trip:
    //! written in a conf, parsed, saved back by saveConfig, parsed again.
    void save_config_round_trips_post_info_keys();

    //! If the atomic replacement cannot be staged, Save Config must leave the
    //! existing file intact instead of truncating it in place.
    void save_config_preserves_existing_file_when_atomic_open_fails();

    //! A posting tab carries one discreet checkbox; the button that opens the
    //! editor follows it.
    void post_info_row_exposes_a_checkbox_and_its_button();

    //! Picking a model offers exactly the fields it asks for, and what is
    //! typed comes back with its scope.
    void post_info_stays_on_across_tabs_and_posts();
    void post_info_dialog_offers_the_fields_of_the_model();
    //! JSON/XML models use raw lines rather than `label = expression`; their
    //! metadata fields must still be discovered from the complete model.
    void post_info_dialog_offers_the_fields_of_a_json_model();

    //! The model from the configuration is the default: it is marked as such,
    //! selected, and selecting it is not an override.
    //! The preview column is the sheet: known values, blanks for what only
    //! exists after the post, and free text mixed with variables.
    void post_info_dialog_previews_every_line();

    //! Lines and fields can be added and removed to compose a model.
    void post_info_dialog_edits_and_saves_a_model();
    void post_info_dialog_preview_follows_the_declared_format();

    void post_info_dialog_marks_the_configured_model_as_default();

    //! A model opened during the session is offered again to the next posts,
    //! and the small cross drops it from the list.
    void post_info_dialog_keeps_the_models_opened_this_session();

    //! Auto posting carries the same choice, once, for its whole run.
    void auto_post_tab_carries_one_post_info_choice();

    //! Regression test for the reported bug: filling in a newly-added
    //! server row and leaving the fields (editingFinished) must persist to
    //! ngPost.conf on its own — the user should never have to find and
    //! click the separate "Save" button just to keep a server they added.
    void add_server_and_edit_fields_persists_without_save_button();

    //! Phase 4 follow-up: a click-driven "delete row" test belongs here but
    //! requires the row's QPushButton to receive a real mouse event;
    //! offscreen QPA + nested-cell widgets do not deliver those reliably.
    //! Refactoring `MainWindow::onDelServer` to take the button as a
    //! parameter (rather than reading `sender()`) would let a test drive
    //! the deletion path in-process. Tracked separately so this binary
    //! stays useful in CI.
};

namespace
{
//! MainWindow::onAddServer is a private slot that's only connected to the
//! addServerButton inside init(NgPost*) — which we don't call in these
//! tests (it would pull in NgPost / VpnManager / config). Invoking via
//! QMetaObject mirrors what the connect would dispatch.
void addServer(QObject *window)
{
    QVERIFY(QMetaObject::invokeMethod(window, "onAddServer", Qt::DirectConnection));
}
} // namespace

void TestMainWindow::add_server_row_creates_named_widgets()
{
    MainWindow window;

    auto *table = window.findChild<QTableWidget*>(QStringLiteral("serversTable"));
    QVERIFY2(table, "serversTable not found in MainWindow");

    const int before = table->rowCount();
    addServer(&window);
    QCOMPARE(table->rowCount(), before + 1);

    // The Phase 1d retrofit gives each dynamic widget a `<Role>_<row>` name.
    // First row should be suffixed _0.
    for (const char *name : { "serverEnabledCb_0", "serverHostEdit_0",
                              "serverPortEdit_0",  "serverSslCb_0",
                              "serverUseVpnCb_0",  "serverNbConsEdit_0",
                              "serverUserEdit_0",  "serverPassEdit_0",
                              "serverDelButton_0" }) {
        QVERIFY2(window.findChild<QWidget*>(QString::fromLatin1(name)),
                 qPrintable(QStringLiteral("widget not found: %1").arg(QString::fromLatin1(name))));
    }

    // Specifically check the default port is the documented 563 (NNTP/SSL).
    auto *portEdit = window.findChild<QLineEdit*>(QStringLiteral("serverPortEdit_0"));
    QVERIFY(portEdit);
    QCOMPARE(portEdit->text(), QStringLiteral("563"));
}

void TestMainWindow::save_config_preserves_an_older_configuration()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    const QString root = sandbox.rootPath();
    QVERIFY(QDir().mkpath(root + QStringLiteral("/nzb")));
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        // A plausible ngPost 5.4.2 configuration, written before this feature
        // existed: none of these keys may be dropped by a save.
        s << "nzbPath = " << root << "/nzb\n"
          << "POST_HISTORY = " << root << "/history.csv\n"
          << "FIELD_SEPARATOR = |\n"
          << "GROUPS = alt.binaries.test,alt.binaries.other\n"
          << "FROM = old@user.local\n"
          << "GROUP_POLICY = EACH_FILE\n"
          << "article_size = 512000\n"
          << "retry = 7\n"
          << "NZB_RM_ACCENTS = true\n"
          << "PREPARE_PACKING = true\n"
          << "TMP_DIR = " << root << "\n"
          << "RAR_PATH = /bin/true\n"
          << "RAR_SIZE = 42\n"
          << "RAR_MAX = 99\n"
          << "PAR2_PCT = 8\n"
          << "LENGTH_NAME = 22\n"
          << "LENGTH_PASS = 15\n"
          << "KEEP_NFO_EXTENSION = true\n"
          << "NZB_POST_CMD = /bin/echo first \"__nzbPath__\"\n"
          << "NZB_POST_CMD = /bin/echo second %1\n"
          << "[server]\n"
          << "host = news.example.invalid\n"
          << "port = 563\n"
          << "ssl = true\n"
          << "user = someone\n"
          << "pass = secret\n"
          << "connection = 8\n"
          << "enabled = true\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    {
        NgPost ngPost(argc, argv);
        const QString parseErr = ngPost.parseDefaultConfig();
        QVERIFY2(parseErr.isEmpty(), qPrintable(parseErr));
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);
        ngPost.saveConfig(); // what the GUI does on "Save"
    }

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());

    // every setting must still be there, with its value
    const QVector<QPair<QString, QString>> expected = {
        { QStringLiteral("FIELD_SEPARATOR"), QStringLiteral("|") },
        { QStringLiteral("GROUP_POLICY"), QStringLiteral("EACH_FILE") }, // saved upper case, parsed case insensitively
        { QStringLiteral("article_size"), QStringLiteral("512000") },
        { QStringLiteral("retry"), QStringLiteral("7") },
        { QStringLiteral("RAR_SIZE"), QStringLiteral("42") },
        { QStringLiteral("RAR_MAX"), QStringLiteral("99") },
        { QStringLiteral("PAR2_PCT"), QStringLiteral("8") },
        { QStringLiteral("LENGTH_NAME"), QStringLiteral("22") },
        { QStringLiteral("LENGTH_PASS"), QStringLiteral("15") },
        { QStringLiteral("host"), QStringLiteral("news.example.invalid") },
        { QStringLiteral("connection"), QStringLiteral("8") },
    };
    for (const auto &kv : expected) {
        const QRegularExpression re(QStringLiteral("^\\s*%1\\s*=\\s*%2\\s*$")
                                        .arg(QRegularExpression::escape(kv.first),
                                             QRegularExpression::escape(kv.second)),
                                    QRegularExpression::MultilineOption);
        QVERIFY2(re.match(content).hasMatch(),
                 qPrintable(QStringLiteral("lost after save: %1 = %2").arg(kv.first, kv.second)));
    }

    // the booleans that were on
    QVERIFY2(content.contains(QRegularExpression(QStringLiteral("^NZB_RM_ACCENTS\\s*=\\s*true"),
                                                 QRegularExpression::MultilineOption)),
             "NZB_RM_ACCENTS lost");
    QVERIFY2(content.contains(QRegularExpression(QStringLiteral("^PREPARE_PACKING\\s*=\\s*true"),
                                                 QRegularExpression::MultilineOption)),
             "PREPARE_PACKING lost");
    QVERIFY2(content.contains(QRegularExpression(QStringLiteral("^KEEP_NFO_EXTENSION\\s*=\\s*true"),
                                                 QRegularExpression::MultilineOption)),
             "KEEP_NFO_EXTENSION lost");

    // both post commands, verbatim, %1 included
    QVERIFY2(content.contains(QStringLiteral("NZB_POST_CMD = /bin/echo first \"__nzbPath__\"")),
             qPrintable(QStringLiteral("first NZB_POST_CMD lost:\n%1").arg(content)));
    QVERIFY2(content.contains(QStringLiteral("NZB_POST_CMD = /bin/echo second %1")),
             "second NZB_POST_CMD lost");

    // the history path, and the groups
    QVERIFY2(content.contains(root + QStringLiteral("/history.csv")), "POST_HISTORY lost");
    QVERIFY2(content.contains(QStringLiteral("alt.binaries.other")), "GROUPS lost");
    QVERIFY2(content.contains(QStringLiteral("old@user.local")), "FROM lost");

    // and it must parse back without error
    {
        NgPost reloaded(argc, argv);
        QVERIFY2(reloaded.parseDefaultConfig().isEmpty(), "the saved config does not parse back");
    }
}

void TestMainWindow::save_config_round_trips_post_info_keys()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    const QString outPattern = QStringLiteral("/data/sheets/__nzbName__.txt");
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "POST_INFO_TEMPLATE = " << tmplPath << "\n"
          << "POST_INFO_OUTPUT = " << outPattern << "\n"
          << "POST_INFO_ONLY_ON_SUCCESS = false\n"
          << "POST_CMD_TIMEOUT = 120\n"
          << "POST_CMD_FAIL_IS_ERROR = true\n"
          << "POST_CMD_EXPOSE_PASSWORD = true\n"
          << "NZB_UPLOAD_TIMEOUT = 45\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };

    // 1st pass: parse, then let saveConfig rewrite the whole file
    {
        NgPost ngPost(argc, argv);
        QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), "first parse failed");
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);
        ngPost.saveConfig();
    }

    // 2nd pass: what was written must parse back to the same values
    {
        NgPost ngPost(argc, argv);
        QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), "reparse of the saved config failed");
        QCOMPARE(ngPost.postInfoTemplatePath(), QDir::cleanPath(tmplPath));
        QCOMPARE(ngPost.postInfoOutputForTest(), outPattern);
        QCOMPARE(ngPost.postInfoOnlyOnSuccessForTest(), false);
        QCOMPARE(ngPost.postCmdTimeoutSecForTest(), 120);
        QCOMPARE(ngPost.postCmdFailIsErrorForTest(), true);
        QCOMPARE(ngPost.postCmdExposePasswordForTest(), true);
        QCOMPARE(ngPost.nzbUploadTimeoutSecForTest(), 45);
    }
}

void TestMainWindow::save_config_preserves_existing_file_when_atomic_open_fails()
{
#ifndef Q_OS_UNIX
    QSKIP("This test relies on Unix directory write permissions");
#else
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    const QByteArray original("GROUPS = alt.binaries.test\n# must survive a failed save\n");
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(conf.write(original), qint64(original.size()));
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), "test configuration did not parse");

    const QString configDir = QFileInfo(confPath).absolutePath();
    const QFile::Permissions originalDirPermissions = QFile::permissions(configDir);
    const QFile::Permissions readOnlyDirPermissions =
        QFileDevice::ReadOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!QFile::setPermissions(configDir, readOnlyDirPermissions))
        QSKIP("Could not make the test configuration directory read-only");

    QFile permissionProbe(configDir + QStringLiteral("/write-probe"));
    if (permissionProbe.open(QIODevice::WriteOnly)) {
        permissionProbe.close();
        permissionProbe.remove();
        QFile::setPermissions(configDir, originalDirPermissions);
        QSKIP("The test process can bypass directory permissions");
    }

    // QSaveFile must create a sibling temporary file before publishing it.
    // QFile opened the writable target directly here and truncated it even
    // though its parent directory was read-only.
    ngPost.saveConfig();
    const bool restored = QFile::setPermissions(configDir, originalDirPermissions);

    QFile saved(confPath);
    const bool opened = saved.open(QIODevice::ReadOnly);
    const QByteArray content = opened ? saved.readAll() : QByteArray();
    QVERIFY2(restored, "Could not restore test directory permissions");
    QVERIFY2(opened, qPrintable(saved.errorString()));
    QCOMPARE(content, original);
#endif
}

void TestMainWindow::post_info_row_exposes_a_checkbox_and_its_button()
{
    HomeSandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootPath() + QStringLiteral("/nzb")));
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\ncat =__meta:categorie__\n");
    }
    {
        QFile conf(PathHelper::configFilePath());
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "nzbPath = " << sandbox.rootPath() << "/nzb\n"
          << "POST_INFO_TEMPLATE = " << tmplPath << "\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);

    // One discreet checkbox on the tab, everything else behind a button
    auto *cb = quickTab->findChild<QCheckBox *>(QStringLiteral("postInfoCB"));
    auto *btn = quickTab->findChild<QPushButton *>(QStringLiteral("postInfoButton"));
    QVERIFY2(cb, "the post info checkbox is not on the posting tab");
    QVERIFY(btn);

    // a model is configured, so the box starts ticked and the button is usable
    QVERIFY(cb->isChecked());
    QVERIFY(btn->isEnabled());

    cb->setChecked(false);
    QVERIFY2(!btn->isEnabled(), "the button must follow the checkbox");
    cb->setChecked(true);
    QVERIFY(btn->isEnabled());
}

void TestMainWindow::post_info_stays_on_across_tabs_and_posts()
{
    // The daily case: POST_INFO_TEMPLATE is set once in the configuration, and
    // no box should ever have to be ticked again.
    HomeSandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootPath() + QStringLiteral("/nzb")));
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }
    {
        QFile conf(PathHelper::configFilePath());
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "nzbPath = " << sandbox.rootPath() << "/nzb\n"
          << "POST_INFO_TEMPLATE = " << tmplPath << "\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);

    auto boxOf = [](QWidget *tab) {
        return tab ? tab->findChild<QCheckBox *>(QStringLiteral("postInfoCB")) : nullptr;
    };

    // the first tab
    auto *first = boxOf(tabs->widget(0));
    QVERIFY(first);
    QVERIFY2(first->isChecked(), "a configured model should tick the box on its own");

    // a tab opened later, as one does between two posts
    PostingWidget *second = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(second);
    QVERIFY2(boxOf(second) && boxOf(second)->isChecked(),
             "a tab opened later must start ticked too");

    // Auto Post carries the same default for its whole run
    auto *autoBox = window->findChild<QCheckBox *>(QStringLiteral("autoPostInfoCB"));
    QVERIFY(autoBox);
    QVERIFY(autoBox->isChecked());

    // Emptying a tab to queue the next post clears what described the previous
    // one, but must NOT turn the feature off.
    QVERIFY(QMetaObject::invokeMethod(second, "onClearFilesClicked", Qt::DirectConnection));
    QVERIFY2(boxOf(second)->isChecked(),
             "clearing the files must not untick the post info box");
}

void TestMainWindow::post_info_dialog_offers_the_fields_of_the_model()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("# une note\n"
                   "titre =__meta:titre__\n"
                   "cat =__meta:categorie__\n"
                   "taille =__postSize__\n");
    }

    PostInfoData preview;
    preview.rarName = QStringLiteral("my-archive");
    PostInfoDialog dlg(tmplPath, QString(), QMap<QString, MetaValue>(), QStringList(), preview);

    // The model table is the file: every line of it, comment included, in order.
    auto *model = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoModelTable"));
    QVERIFY(model);
    QCOMPARE(model->rowCount(), 5); // four lines plus the final empty one
    auto *comment = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelRaw_0"));
    QVERIFY(comment);
    QCOMPARE(comment->text(), QStringLiteral("# une note"));
    auto *label = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelLabel_1"));
    QVERIFY(label);
    QCOMPARE(label->text(), QStringLiteral("titre"));
    auto *expr = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelExpr_1"));
    QVERIFY(expr);
    QCOMPARE(expr->text(), QStringLiteral("__meta:titre__"));

    // The values table holds only what the user has to answer: the two metas,
    // never the comment nor the size ngPost works out by itself.
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoFieldsTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);

    auto *firstName = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoFieldName_0"));
    QVERIFY(firstName);
    QCOMPARE(firstName->text(), QStringLiteral("titre"));

    // private by default: publishing in the nzb stays an explicit choice
    auto *firstNzb = dlg.findChild<CheckBoxCenterWidget *>(QStringLiteral("postInfoFieldNzb_0"));
    QVERIFY(firstNzb);
    QVERIFY(!firstNzb->isChecked());

    // and what the user types comes back out, with its scope
    auto *firstValue = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoFieldValue_0"));
    QVERIFY(firstValue);
    firstValue->setText(QStringLiteral("Mercantour"));
    firstNzb->setChecked(true);

    QString duplicate;
    const QMap<QString, MetaValue> meta = dlg.meta(&duplicate);
    QVERIFY(duplicate.isEmpty());
    QCOMPARE(meta.value(QStringLiteral("titre")).value, QStringLiteral("Mercantour"));
    QCOMPARE(meta.value(QStringLiteral("titre")).scope, MetaScope::Nzb);
    QCOMPARE(meta.value(QStringLiteral("categorie")).scope, MetaScope::Local);
}

void TestMainWindow::post_info_dialog_offers_the_fields_of_a_json_model()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.json");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("#!json\n"
                   "{\n"
                   "  \"title\": \"__meta:title__\",\n"
                   "  \"details\": {\"genre\": \"__meta:genre__\"}\n"
                   "}\n");
    }

    PostInfoDialog dlg(tmplPath, QString(), QMap<QString, MetaValue>());
    auto *fields = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoFieldsTable"));
    QVERIFY(fields);
    QCOMPARE(fields->rowCount(), 2);

    auto *title = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoFieldName_0"));
    auto *genre = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoFieldName_1"));
    QVERIFY(title);
    QVERIFY(genre);
    QCOMPARE(title->text(), QStringLiteral("title"));
    QCOMPARE(genre->text(), QStringLiteral("genre"));
}

void TestMainWindow::post_info_dialog_previews_every_line()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        // a mixed line: free text and a variable on the same line
        tmpl.write("# ignoree\n"
                   "nom =__rarName__\n"
                   "pass =__rarPass__\n"
                   "statut =__status__\n"
                   "commentaire =moi __originalName__ et la suite\n"
                   "titre =__meta:titre__\n");
    }

    PostInfoData preview;
    preview.rarName      = QStringLiteral("my-archive");
    preview.rarPass      = QStringLiteral("qwerty42");
    preview.originalName = QStringLiteral("rando.mkv");

    PostInfoDialog dlg(tmplPath, QString(), QMap<QString, MetaValue>(), QStringList(), preview);

    // a comment produces nothing
    auto *p0 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_0"));
    QVERIFY(p0);
    QVERIFY(p0->text().isEmpty());

    // a date is knowable while preparing the post: it previews as today, so
    // the chosen format can be checked before posting
    {
        HomeSandbox dateBox;
        const QString datePath = dateBox.rootPath() + QStringLiteral("/date.tpl");
        QFile dateTmpl(datePath);
        QVERIFY(dateTmpl.open(QIODevice::WriteOnly));
        dateTmpl.write("date =__date:dd/MM/yyyy__\n");
        dateTmpl.close();

        PostInfoDialog dateDlg(datePath, QString(), QMap<QString, MetaValue>());
        auto *dp = dateDlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_0"));
        QVERIFY(dp);
        QCOMPARE(dp->text(),
                 QDateTime::currentDateTime().toString(QStringLiteral("dd/MM/yyyy")));
    }

    // a value already known while preparing the post is shown as it will be
    auto *p1 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_1"));
    QVERIFY(p1);
    QCOMPARE(p1->text(), QStringLiteral("my-archive"));

    // the password is rendered like everything else: this column is the sheet,
    // and the sheet will hold it
    auto *p2 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_2"));
    QVERIFY(p2);
    QCOMPARE(p2->text(), QStringLiteral("qwerty42"));

    // a value that only exists after the post stays blank, and says so
    auto *p3 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_3"));
    QVERIFY(p3);
    QVERIFY(p3->text().isEmpty());
    QVERIFY(!p3->placeholderText().isEmpty());

    // text and variables mix freely on one line
    auto *p4 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_4"));
    QVERIFY(p4);
    QCOMPARE(p4->text(), QStringLiteral("moi rando.mkv et la suite"));

    // and typing a value updates the preview of the line that uses it
    auto *value = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoFieldValue_0"));
    QVERIFY(value);
    value->setText(QStringLiteral("Mercantour"));
    emit value->textEdited(QStringLiteral("Mercantour"));
    auto *p5 = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_5"));
    QVERIFY(p5);
    QCOMPARE(p5->text(), QStringLiteral("Mercantour"));
}

void TestMainWindow::post_info_dialog_preview_follows_the_declared_format()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }

    QMap<QString, MetaValue> meta;
    meta.insert(QStringLiteral("titre"), MetaValue(QStringLiteral("un \"titre\" & co")));

    PostInfoDialog dlg(tmplPath, QString(), meta);
    auto *preview = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelPreview_0"));
    QVERIFY(preview);
    // plain text: nothing is escaped
    QCOMPARE(preview->text(), QStringLiteral("un \"titre\" & co"));

    // Declaring the format inside the editor must change the preview at once:
    // the column claims to show what the file will hold.
    auto *raw = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoModelRaw_1"));
    QVERIFY2(raw, "the trailing empty line of the model should be editable");
    raw->setText(QStringLiteral("#!xml"));
    emit raw->textEdited(QStringLiteral("#!xml"));

    QCOMPARE(preview->text(), QStringLiteral("un &quot;titre&quot; &amp; co"));
}

void TestMainWindow::post_info_dialog_edits_and_saves_a_model()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("# entete\ndate        =__date:yyyy__\ntitre =__meta:titre__\n");
    }

    PostInfoDialog dlg(tmplPath, QString(), QMap<QString, MetaValue>());
    auto *model = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoModelTable"));
    QVERIFY(model);
    QCOMPARE(model->rowCount(), 4); // three lines plus the final empty one

    // adding a field adds both the value row and the line that writes it
    auto *fields = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoFieldsTable"));
    QVERIFY(fields);
    const int fieldsBefore = fields->rowCount();
    auto *addField = dlg.findChild<QPushButton *>(QStringLiteral("postInfoAddFieldButton"));
    QVERIFY(addField);
    addField->click();
    QCOMPARE(fields->rowCount(), fieldsBefore + 1);
    QCOMPARE(model->rowCount(), 5);

    // a line can be dropped from the model
    auto *del = dlg.findChild<QPushButton *>(QStringLiteral("postInfoModelDel_0"));
    QVERIFY(del);
    del->click();
    QCOMPARE(model->rowCount(), 4);
}

void TestMainWindow::post_info_dialog_marks_the_configured_model_as_default()
{
    HomeSandbox sandbox;
    const QString configured = sandbox.rootPath() + QStringLiteral("/default.tpl");
    {
        QFile tmpl(configured);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }

    PostInfoDialog dlg(configured, QString(), QMap<QString, MetaValue>());
    auto *list = dlg.findChild<QComboBox *>(QStringLiteral("postInfoTemplateList"));
    QVERIFY(list);

    // the configured one comes first, says it is the default, and is selected
    QCOMPARE(list->currentIndex(), 0);
    QVERIFY2(list->itemText(0).contains(QStringLiteral("default")), qPrintable(list->itemText(0)));
    QCOMPARE(list->itemData(0).toString(), configured);

    // and keeping it is NOT an override: the post follows the configuration
    QVERIFY(dlg.templateOverride().isEmpty());
    QVERIFY(!dlg.setAsDefault()); // nothing asked for

    // its fields were offered without having to press anything
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("postInfoFieldsTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);

    // ticking the box asks for the selection to become the configured model
    auto *asDefault = dlg.findChild<QCheckBox *>(QStringLiteral("postInfoSetAsDefault"));
    QVERIFY(asDefault);
    asDefault->setChecked(true);
    QVERIFY(dlg.setAsDefault());
}

void TestMainWindow::post_info_dialog_keeps_the_models_opened_this_session()
{
    HomeSandbox sandbox;
    const QString configured = sandbox.rootPath() + QStringLiteral("/default.tpl");
    const QString other      = sandbox.rootPath() + QStringLiteral("/baselien.tpl");
    for (const QString &path : { configured, other })
    {
        QFile tmpl(path);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }

    // a model opened during an earlier post is offered again, without browsing
    PostInfoDialog dlg(configured, QString(), QMap<QString, MetaValue>(), QStringList{ other });
    auto *list = dlg.findChild<QComboBox *>(QStringLiteral("postInfoTemplateList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 3); // default, the session one, "Choose a file..."
    QCOMPARE(list->itemData(1).toString(), other);
    QCOMPARE(list->currentIndex(), 0); // the default stays selected

    auto *forget = dlg.findChild<QPushButton *>(QStringLiteral("postInfoForgetButton"));
    QVERIFY(forget);
    QVERIFY(!forget->isEnabled()); // the configured model is not ours to drop

    // selecting it enables the cross, and does not reopen anything
    list->setCurrentIndex(1);
    QCOMPARE(dlg.templateOverride(), other);
    QVERIFY(forget->isEnabled());

    // the cross removes it from the list and falls back on the default
    forget->click();
    QCOMPARE(list->count(), 2);
    QVERIFY(dlg.sessionTemplates().isEmpty());
    QVERIFY(dlg.templateOverride().isEmpty());
    QVERIFY(!forget->isEnabled());
}

void TestMainWindow::auto_post_tab_carries_one_post_info_choice()
{
    HomeSandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootPath() + QStringLiteral("/nzb")));
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }
    {
        QFile conf(PathHelper::configFilePath());
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "nzbPath = " << sandbox.rootPath() << "/nzb\n"
          << "POST_INFO_TEMPLATE = " << tmplPath << "\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    // the auto posting tab has its own pair, and says the choice is global
    auto *cb = window->findChild<QCheckBox *>(QStringLiteral("autoPostInfoCB"));
    auto *btn = window->findChild<QPushButton *>(QStringLiteral("autoPostInfoButton"));
    QVERIFY2(cb, "the auto post tab has no post info checkbox");
    QVERIFY(btn);
    QVERIFY2(cb->toolTip().contains(QStringLiteral("every post")), qPrintable(cb->toolTip()));

    QVERIFY(cb->isChecked()); // a model is configured
    cb->setChecked(false);
    QVERIFY(!btn->isEnabled());
}

void TestMainWindow::add_two_servers_yields_unique_object_names()
{
    MainWindow window;
    auto *table = window.findChild<QTableWidget*>(QStringLiteral("serversTable"));

    addServer(&window);
    addServer(&window);
    QCOMPARE(table->rowCount(), 2);

    QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("serverHostEdit_0")));
    QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("serverHostEdit_1")));
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("serverDelButton_0")));
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("serverDelButton_1")));
}

void TestMainWindow::vpn_checkbox_toggled_emits_signal()
{
    MainWindow window;
    addServer(&window);

    auto *vpnCb = window.findChild<CheckBoxCenterWidget*>(QStringLiteral("serverUseVpnCb_0"));
    QVERIFY2(vpnCb, "serverUseVpnCb_0 not found");

    // _addServer wires the checkbox's toggled() signal to
    // MainWindow::_onUseVpnToggled, which dereferences _ngPost — null in
    // this test. Disconnect the handler so the test only exercises the
    // signal-emission path, not the downstream side effect on NgPost.
    QObject::disconnect(vpnCb, &CheckBoxCenterWidget::toggled, &window, nullptr);

    QSignalSpy spy(vpnCb, &CheckBoxCenterWidget::toggled);

    const bool initial = vpnCb->isChecked();
    vpnCb->setChecked(!initial);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(vpnCb->isChecked(), !initial);
}

void TestMainWindow::history_detail_text_does_not_resize_window()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY2(tabs, "postTabWidget not found in MainWindow");

    QWidget *historyTab = window.buildHistoryTabForTest();
    tabs->addTab(historyTab, QStringLiteral("History"));
    tabs->setCurrentWidget(historyTab);

    window.resize(900, 600);
    window.show();
    QTest::qWait(50);
    const QSize before = window.size();

    auto *detail = window.findChild<QLabel*>(QStringLiteral("historyDetailInfo"));
    QVERIFY2(detail, "historyDetailInfo not found");

    QString rows;
    for (int i = 0; i < 200; ++i) {
        rows += QStringLiteral("<tr><td>file_%1_with_a_long_name.bin</td>"
                               "<td align='right'>4 MB</td>"
                               "<td align='center'>posted</td></tr>").arg(i);
    }
    detail->setText(QStringLiteral("<table>%1</table>").arg(rows));
    QApplication::processEvents();

    QCOMPARE(window.size(), before);
}

void TestMainWindow::save_config_persists_rar_max_and_par2_pct()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    {
        QFile conf(confPath);
        QVERIFY2(conf.open(QIODevice::WriteOnly | QIODevice::Text),
                 qPrintable(QStringLiteral("Could not write test config: %1").arg(confPath)));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "TMP_DIR = " << sandbox.rootPath() << "\n"
          << "RAR_PATH = /bin/true\n"
          << "RAR_MAX = 99\n"
          << "PAR2_PCT = 8\n"
          << "PAR2_ARGS = -s1M --auto-slice-size -r1n*0.6 -m2048M --progress stdout -q\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY2(window, "NgPost did not create a GUI MainWindow for the test");
    window->init(&ngPost);

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY2(tabs, "postTabWidget not found");
    tabs->setCurrentIndex(0);
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);

    auto *rarMax = quickTab->findChild<QCheckBox*>(QStringLiteral("rarMaxCB"));
    auto *redundancy = quickTab->findChild<QSpinBox*>(QStringLiteral("redundancySB"));
    auto *compress = quickTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    auto *par2 = quickTab->findChild<QCheckBox*>(QStringLiteral("par2CB"));
    QVERIFY2(rarMax, "rarMaxCB not found on Quick tab");
    QVERIFY2(redundancy, "redundancySB not found on Quick tab");
    QVERIFY(compress);
    QVERIFY(par2);

    // Each setting now follows its own switch: the volume limit means nothing
    // without compression, and a redundancy percentage means nothing without
    // PAR2. They are greyed until the box above them is ticked.
    compress->setChecked(false);
    par2->setChecked(false);
    QVERIFY(!rarMax->isEnabled());
    QVERIFY(!redundancy->isEnabled());

    compress->setChecked(true);
    par2->setChecked(true);
    QVERIFY(rarMax->isEnabled());
    QVERIFY(redundancy->isEnabled());

    rarMax->setChecked(false);
    redundancy->setValue(17);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\n#RAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("\nPAR2_PCT = 17\n")),
             qPrintable(content));

    saved.close();
    rarMax->setChecked(true);
    redundancy->setValue(23);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\nRAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("\n#RAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("\nPAR2_PCT = 23\n")),
             qPrintable(content));
}

void TestMainWindow::add_server_and_edit_fields_persists_without_save_button()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    {
        QFile conf(confPath);
        QVERIFY2(conf.open(QIODevice::WriteOnly | QIODevice::Text),
                 qPrintable(QStringLiteral("Could not write test config: %1").arg(confPath)));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY2(window, "NgPost did not create a GUI MainWindow for the test");
    window->init(&ngPost);

    QVERIFY(QMetaObject::invokeMethod(window, "onAddServer", Qt::DirectConnection));

    auto *hostEdit = window->findChild<QLineEdit*>(QStringLiteral("serverHostEdit_0"));
    auto *userEdit = window->findChild<QLineEdit*>(QStringLiteral("serverUserEdit_0"));
    QVERIFY2(hostEdit, "serverHostEdit_0 not found");
    QVERIFY2(userEdit, "serverUserEdit_0 not found");

    // This is exactly what happens interactively: the user types into the
    // field then moves focus away (Tab / click elsewhere), which fires
    // editingFinished. Note we deliberately never call onSaveConfig.
    hostEdit->setText(QStringLiteral("news.example.com"));
    emit hostEdit->editingFinished();
    userEdit->setText(QStringLiteral("bob"));
    emit userEdit->editingFinished();

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("host = news.example.com")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("user = bob")),
             qPrintable(content));
}

QTEST_MAIN(TestMainWindow)
#include "tst_MainWindow.moc"
