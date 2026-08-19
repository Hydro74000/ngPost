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
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>

#include "hmi/MainWindow.h"
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

    //! The post metadata group really folds (children hidden), it does not
    //! merely grey its children out the way a checkable QGroupBox does, and a
    //! row exposes named widgets so a value can be typed and read back.
    void post_meta_table_folds_and_exposes_named_widgets();

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

void TestMainWindow::post_meta_table_folds_and_exposes_named_widgets()
{
    HomeSandbox sandbox;
    {
        QFile conf(PathHelper::configFilePath());
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n";
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

    auto *content = quickTab->findChild<QWidget *>(QStringLiteral("postMetaContent"));
    QVERIFY2(content, "post metadata section not found in the posting tab");
    auto *toggle = quickTab->findChild<QToolButton *>(QStringLiteral("postMetaToggle"));
    QVERIFY(toggle);

    // folded by default: it must not eat vertical space of the posting tab
    QVERIFY(content->isHidden());

    toggle->setChecked(true);
    QVERIFY(!content->isHidden());

    auto *table = quickTab->findChild<QTableWidget *>(QStringLiteral("postMetaTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1); // unfolding offers a first empty row

    for (const char *name : { "postMetaKeyEdit_0", "postMetaValueEdit_0",
                              "postMetaNzbCB_0", "postMetaDelButton_0" }) {
        QVERIFY2(quickTab->findChild<QWidget *>(QString::fromLatin1(name)),
                 qPrintable(QStringLiteral("widget not found: %1").arg(QString::fromLatin1(name))));
    }

    // private by default: publishing in the nzb is an explicit choice
    auto *nzbCB = quickTab->findChild<QCheckBox *>(QStringLiteral("postMetaNzbCB_0"));
    QVERIFY(nzbCB);
    QVERIFY(!nzbCB->isChecked());

    toggle->setChecked(false);
    QVERIFY(content->isHidden());
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
    QVERIFY2(rarMax, "rarMaxCB not found on Quick tab");
    QVERIFY2(redundancy, "redundancySB not found on Quick tab");
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
