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
