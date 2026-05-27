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
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>

#define private public
#include "vpn/VpnManager.h"
#undef private

#include "hmi/MainWindow.h"
#include "hmi/SettingsDialog.h"
#include "hmi/VpnSettingsDialog.h"
#include "hmi/CheckBoxCenterWidget.h"
#include "utils/PathHelper.h"
#include "vpn/VpnProfile.h"
#include "NgPost.h"
#include "TestEnv.h"

using ngpost::tests::HomeSandbox;

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    //! The main window now shows a multi-server selection table, using the
    //! configured server labels/captions instead of declaring servers inline.
    void server_selection_lists_labels_and_vpn_status();

    //! Save Config persists the main-window server selection while preserving
    //! server declarations and labels from the Settings-owned table.
    void server_selection_save_persists_enabled_and_labels();

    //! Settings Apply pushes edited config to NgPost and disk; closing without
    //! Apply leaves in-memory values untouched.
    void settings_dialog_apply_persists_and_cancel_discards();

    //! The random-poster option stays on MainWindow, with the requested new
    //! French wording, and GROUP_POLICY is kept next to the groups field.
    void main_window_keeps_random_email_and_group_policy_controls();

    //! Long history details should scroll inside the history panel instead of
    //! changing the top-level window dimensions.
    void history_detail_text_does_not_resize_window();

    //! Save Config must persist the active GUI tab's RAR_MAX checkbox and
    //! PAR2_PCT spinbox, even when PAR2_ARGS is present.
    void save_config_persists_rar_max_and_par2_pct();

    //! The VPN actions dialog chooses the profile to connect without changing
    //! the Settings-owned default profile, then freezes it while connecting.
    void vpn_actions_profile_selector_is_independent_and_freezes();

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
void writeServerConfig(const QString &confPath, const QString &tmpDir)
{
    QFile conf(confPath);
    QVERIFY2(conf.open(QIODevice::WriteOnly | QIODevice::Text),
             qPrintable(QStringLiteral("Could not write test config: %1").arg(confPath)));
    QTextStream s(&conf);
    s << "GROUPS = alt.binaries.test, alt.binaries.test2\n"
      << "GROUP_POLICY = EACH_FILE\n"
      << "TMP_DIR = " << tmpDir << "\n"
      << "RAR_PATH = /bin/true\n"
      << "[server]\n"
      << "label = Primary\n"
      << "host = primary.example.test\n"
      << "port = 563\n"
      << "ssl = true\n"
      << "user = user1\n"
      << "pass = pass1\n"
      << "connection = 4\n"
      << "enabled = true\n"
      << "useVpn = true\n"
      << "[server]\n"
      << "caption = Backup\n"
      << "host = backup.example.test\n"
      << "port = 119\n"
      << "ssl = false\n"
      << "connection = 2\n"
      << "enabled = false\n";
}
} // namespace

void TestMainWindow::server_selection_lists_labels_and_vpn_status()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    writeServerConfig(confPath, sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *table = window->findChild<QTableWidget*>(QStringLiteral("serversTable"));
    QVERIFY2(table, "serversTable not found in MainWindow");
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Primary"));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("Backup"));
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
    QCOMPARE(table->item(1, 0)->checkState(), Qt::Unchecked);
    QVERIFY2(table->item(0, 2)->text().contains(QStringLiteral("VPN")),
             qPrintable(table->item(0, 2)->text()));
}

void TestMainWindow::server_selection_save_persists_enabled_and_labels()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    writeServerConfig(confPath, sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *table = window->findChild<QTableWidget*>(QStringLiteral("serversTable"));
    QVERIFY(table);
    table->item(0, 0)->setCheckState(Qt::Unchecked);
    table->item(1, 0)->setCheckState(Qt::Checked);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("label = Primary")), qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("label = Backup")), qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("host = primary.example.test\n"
                                             "port = 563\n"
                                             "ssl  = true\n"
                                             "user = user1\n"
                                             "pass = pass1\n"
                                             "connection = 4\n"
                                             "enabled = false")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("host = backup.example.test\n"
                                             "port = 119\n"
                                             "ssl  = false\n"
                                             "user = \n"
                                             "pass = \n"
                                             "connection = 2\n"
                                             "enabled = true")),
             qPrintable(content));
}

void TestMainWindow::settings_dialog_apply_persists_and_cancel_discards()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    writeServerConfig(confPath, sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    {
        SettingsDialog dialog(&ngPost, window);
        auto *from = dialog.findChild<QLineEdit*>(QStringLiteral("settingsFromEdit"));
        auto *saveFrom = dialog.findChild<QCheckBox*>(QStringLiteral("settingsSaveFromCB"));
        auto *servers = dialog.findChild<QTableWidget*>(QStringLiteral("settingsServersTable"));
        QVERIFY(from);
        QVERIFY(saveFrom);
        QVERIFY(servers);
        QCOMPARE(servers->rowCount(), 2);

        from->setText(QStringLiteral("poster@example.test"));
        saveFrom->setChecked(true);
        servers->item(0, 1)->setText(QStringLiteral("Edited Primary"));
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onApply", Qt::DirectConnection));
    }

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("FROM = poster@example.test")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("label = Edited Primary")),
             qPrintable(content));

    {
        SettingsDialog dialog(&ngPost, window);
        auto *from = dialog.findChild<QLineEdit*>(QStringLiteral("settingsFromEdit"));
        auto *servers = dialog.findChild<QTableWidget*>(QStringLiteral("settingsServersTable"));
        QVERIFY(from);
        QVERIFY(servers);
        from->setText(QStringLiteral("cancelled@example.test"));
        servers->item(0, 1)->setText(QStringLiteral("Cancelled"));
        dialog.reject();
    }

    saved.close();
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("FROM = poster@example.test")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("label = Edited Primary")),
             qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("cancelled@example.test")),
             qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("label = Cancelled")),
             qPrintable(content));
}

void TestMainWindow::main_window_keeps_random_email_and_group_policy_controls()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    writeServerConfig(confPath, sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *randomEmail = window->findChild<QCheckBox*>(QStringLiteral("uniqueFromCB"));
    QVERIFY(randomEmail);
    QCOMPARE(randomEmail->text(), QStringLiteral("Utiliser une adresse mail aléatoire pour poster"));

    auto *policy = window->findChild<QComboBox*>(QStringLiteral("groupPolicyCB"));
    QVERIFY2(policy, "groupPolicyCB not found");
    QCOMPARE(policy->currentText(), QStringLiteral("One group per file"));

    auto *groups = window->findChild<QTextEdit*>(QStringLiteral("groupsEdit"));
    QVERIFY(groups);
    randomEmail->setChecked(true);
    groups->setPlainText(QStringLiteral(" alt.binaries.one, ,alt.binaries.two "));
    policy->setCurrentIndex(1);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("GEN_FROM = true")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("GROUP_POLICY = EACH_POST")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("alt.binaries.one,alt.binaries.two")),
             qPrintable(content));
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

void TestMainWindow::vpn_actions_profile_selector_is_independent_and_freezes()
{
    HomeSandbox sandbox;

    VpnProfile primary;
    primary.name = QStringLiteral("Primary");
    primary.backend = VpnManager::Backend::OpenVPN;
    primary.configFileName = QStringLiteral("primary.ovpn");

    VpnProfile backup;
    backup.name = QStringLiteral("Backup");
    backup.backend = VpnManager::Backend::WireGuard;
    backup.configFileName = QStringLiteral("backup.conf");

    VpnManager manager;
    manager.setProfilesFromConfig({ primary, backup }, primary.name);

    VpnSettingsDialog dialog(&manager);
    auto *profile = dialog.findChild<QComboBox*>(QStringLiteral("profileCB"));
    QVERIFY2(profile, "profileCB not found in VPN actions dialog");
    QCOMPARE(profile->count(), 2);
    QCOMPARE(profile->currentData().toString(), primary.name);

    profile->setCurrentIndex(profile->findData(backup.name));
    QCOMPARE(profile->currentData().toString(), backup.name);
    QCOMPARE(manager.activeProfileName(), primary.name);

    manager._runtimeProfileName = backup.name;
    manager._state = VpnManager::State::Starting;
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onStateChanged", Qt::DirectConnection,
                                      Q_ARG(VpnManager::State, VpnManager::State::Starting)));
    QVERIFY(!profile->isEnabled());
    QCOMPARE(profile->currentData().toString(), backup.name);

    manager._runtimeProfileName.clear();
    manager._state = VpnManager::State::Disabled;
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onStateChanged", Qt::DirectConnection,
                                      Q_ARG(VpnManager::State, VpnManager::State::Disabled)));
    QVERIFY(profile->isEnabled());
    QCOMPARE(profile->currentData().toString(), backup.name);

    Q_UNUSED(sandbox);
}

QTEST_MAIN(TestMainWindow)
#include "tst_MainWindow.moc"
