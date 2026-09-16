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
#include <QTextBlock>
#include <QTextBrowser>
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
#include <QPlainTextEdit>
#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QDateTime>
#include <QTranslator>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QDirIterator>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTextStream>

#include "hmi/MainWindow.h"
#include "hmi/PostingWidget.h"
#include "hmi/StartupTabBar.h"
#include "hmi/AutoPostWidget.h"
#include "hmi/CheckBoxCenterWidget.h"
#include "hmi/CompressionSettingsDialog.h"
#include "hmi/ExternalToolPathWidget.h"
#include "hmi/Par2SettingsDialog.h"
#include "MockNntpServer.h"
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QMessageBox>
#include <QPointer>
#include "utils/PathHelper.h"
#include "NgPost.h"
#include "PostingJob.h"
#include "PostingJobOptions.h"
#include "TestEnv.h"

#include <QBoxLayout>

using ngpost::tests::HomeSandbox;

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void legacy_bundle_paths_become_automatic();
    void tool_paths_keep_custom_choices_and_report_missing_tools();
    //! A PAR2_PATH or RAR_PATH written before *_SOURCE existed, and gone since,
    //! used to abort every post: it falls back to automatic discovery, loudly.
    void legacy_missing_tool_paths_fall_back_with_a_warning_data();
    void legacy_missing_tool_paths_fall_back_with_a_warning();
    void explicit_missing_parity_engine_is_reported();
    void automatic_archiver_ignores_the_legacy_path();
    void failed_compressor_reports_unrestored_sources();
    //! Without PAR2_ARGS, the engine an old PAR2_PATH names -- vanished, or next
    //! to ngPost in a bundle that no longer ships it -- is only adopted when it is
    //! installed: auto detection builds arguments for whichever engine it finds.
    void legacy_parity_path_adopts_its_engine_only_when_usable_data();
    void legacy_parity_path_adopts_its_engine_only_when_usable();
    //! "Show path" disappears with the detected tool; the path it showed must too.
    void tool_path_details_hide_with_an_unavailable_tool();
    //! A path next to *_SOURCE = auto, and RAR_TOOL = rar next to a 7-Zip
    //! executable, cannot do what the file says: reported, and the engine follows
    //! the executable.
    void explicit_tool_lines_that_cannot_apply_are_reported();
    //! RAR_TOOL is read whatever its case, a tool line that means nothing
    //! (*_TOOL, *_SOURCE) is reported without refusing the configuration, and
    //! a missing executable is only reported when the configuration uses it.
    void tool_lines_are_lenient_and_missing_tools_reported_only_when_used();
    //! Typing in a custom path keeps the cursor and the undo history, and a
    //! 7-Zip executable selects the 7-Zip engine without losing the path.
    void custom_archiver_path_keeps_the_cursor_and_selects_the_engine();
    void log_timestamps_cover_debug_errors_and_fragments();
    void log_file_keeps_timestamped_debug_fragments();
    void post_all_tabs_submits_only_prepared_posts();
    void par2_dialog_defaults_overrides_and_cancel();
    void par2_dialog_preserves_exact_volume_bytes();
    void par2_dialog_detects_real_gpu();
    void par2_dialog_refuses_gpu_without_opencl();
    void par2_dialog_checks_opencl_data();
    void par2_dialog_checks_opencl();
    void par2_dialog_rechecks_changed_opencl_tool();
    //! Every prefix of a path being typed may be another executable: none is
    //! run until the edit is finished.
    void par2_dialog_runs_a_typed_path_only_once_typing_ends();
    void par2_dialog_multipar_clears_inexact_check_hint();
    void sizing_dialogs_translations_fit_data();
    void sizing_dialogs_translations_fit();
    void rar_limit_value_persists_and_zero_is_rejected();
    void queued_post_keeps_rar_and_par2_settings();
    void queued_post_keeps_rar_and_par2_settings_data();
    void post_all_continues_after_overwrite_declined_and_auto_close();
    //! The log pane must not grow for the life of the process. It is fed from
    //! the posting threads, several lines per article at debug 2, and nothing
    //! used to drop a line ever -- 120 000 of them cost 877 MB.
    void log_pane_stays_within_its_budget();

    //! Trimming happens a slice at a time, not a line at a time, so the pane
    //! is allowed to run over the cap before it is cut back. What must never
    //! happen is that the newest line is the one thrown away.
    void log_pane_keeps_the_newest_lines();

    //! Compression progress in the normal GUI mode is written as repeated
    //! "*" fragments with newline=false. Split it before QTextDocument ever
    //! has to lay out one pathological block.
    void log_pane_bounds_fragments_without_newlines();

    //! readAllStandardOutput() can return a large accumulated chunk at once;
    //! that input must be split before insertion too, not repaired afterwards.
    void log_pane_bounds_one_large_fragment_without_newline();

    //! Splitting must preserve natural line separators and never bisect one
    //! non-BMP character's UTF-16 surrogate pair.
    void log_pane_fragment_splitting_preserves_text_boundaries();

    //! append() takes its whole argument as one block however long it is, so
    //! the newline-terminated path and logError() used to bypass the per-block
    //! cap entirely -- the single very long line the cap exists to split.
    void log_pane_bounds_whole_lines_and_errors();

    //! The pane inserts text, it never renders markup, so a log message
    //! carrying an HTML fragment reaches the user as literal tags -- and the
    //! same string is written verbatim into the log file.
    void log_pane_shows_markup_as_plain_text();

    //! ...so no log call may pass one. Guards the whole family at the source,
    //! which is where "<h3>Start Post #1: ...</h3>" escaped review.
    void no_log_call_passes_html_markup();

    //! Every VPN affordance in the main window must agree with
    //! VpnManager::vpnPlatformSupported(). On a platform with no VPN
    //! integration the user must see nothing about it at all -- not a settings
    //! button offering to install a helper that cannot exist, not a state
    //! label stuck on "disabled", not a per-server column they cannot act on.
    void vpn_affordances_follow_platform_support();
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

    //! `obfuscate` names what to obfuscate, and it is a list. File name
    //! obfuscation was reachable only from the GUI checkbox and saved nowhere,
    //! so it was lost on every restart; the config has to carry it like the
    //! rest. A bare `obfuscate = article`, the only spelling older ngPost
    //! understood, must keep meaning exactly what it used to.
    void obfuscate_config_key_carries_both_kinds();
    void obfuscate_config_key_survives_a_save();

    //! Every new post info / post command key survives a full round trip:
    //! written in a conf, parsed, saved back by saveConfig, parsed again.
    void save_config_round_trips_post_info_keys();

    //! If the atomic replacement cannot be staged, Save Config must leave the
    //! existing file intact instead of truncating it in place.
    void save_config_preserves_existing_file_when_atomic_open_fails();

    //! LOG_IN_FILE wrote to $HOME/ngPost.log -- /ngPost.log with HOME unset --
    //! and, on Windows, to whatever the working directory was.
    void log_in_file_is_written_in_the_config_folder();

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
    //! The sheet can go somewhere else than the configuration says, per post.
    void post_info_dialog_offers_a_destination();

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

    //! A posting tab reads top to bottom: what the post produces, the files,
    //! then how they are packed. Each switch sits on the line above the
    //! settings it commands, which is what makes a greyed one legible.
    void posting_tab_lines_are_in_the_new_order();

    //! The compression paths, the volume size and the default password are
    //! configuration, not per post choices: they live in one dialog and no
    //! longer in every tab, where each copy overwrote the others.
    void compression_settings_dialog_owns_the_configuration_widgets();

    //! Validating that dialog writes ngPost.conf straight away: a setting typed
    //! in a dialog must not also require finding the Save button.
    void compression_settings_dialog_persists_on_validation();

    //! Reported bug: "Limit RAR Number" and "Keep Archives" could not be
    //! modified any more. They are greyed on purpose — without compression
    //! ngPost builds nothing to limit or to keep — but a greyed control with
    //! no explanation reads as a broken one, so each says what it waits for.
    void greyed_controls_say_what_they_need();

    //! retranslateUi() resets tooltips to the plain .ui text. The requirement
    //! must come back with the new language instead of being silently lost.
    void state_tooltips_survive_a_language_change();

    //! The same setting must not follow two different rules depending on the
    //! tab it is shown in.
    void auto_post_tab_greys_the_same_controls_as_a_posting_tab();

    //! The default password moved out of the head into the dialog; it must
    //! still reach the posting tabs, which is the whole point of it.
    void default_archive_password_still_reaches_the_posting_tabs();

    //! "Keep the archives" is a default held by the dialog: a new tab starts
    //! with it, and the choice a single post makes must not rewrite it.
    void keep_archives_default_is_owned_by_the_dialog();

    //! The history columns belong to the user: every one of them answers to the
    //! mouse -- the name column was a Stretch section the header sized itself,
    //! which is why it could not be narrowed -- and a width set by hand is not
    //! undone by the next refresh.
    void history_columns_can_be_resized_by_hand();

    //! A width set by hand is kept for the next run, and the header's "Reset
    //! column widths" gives the columns back to ngPost -- in the settings too,
    //! or the old widths would come back at the next start.
    void history_column_widths_survive_a_restart();

    //! Nothing pinned: ngPost opens on the quick post tab, no title is bold,
    //! and the tab context menu offers the option unticked -- on the three
    //! fixed tabs only. Picking it pins the tab and writes the setting at
    //! once; picking it again on the same tab takes the setting away.
    void startup_tab_is_the_quick_post_until_one_is_pinned();

    //! What was pinned is what opens on the next start, bold and ticked. A
    //! value that no longer points at a fixed tab is ignored rather than
    //! opening on nothing.
    void pinned_startup_tab_opens_on_the_next_start();

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
          << "NZB_UPLOAD_TIMEOUT = 45\n"
          << "PAR2_BLOCK_SIZE = 5242880\n";
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
        // Read but never written back, this one used to vanish on the first
        // save, and --check then quietly went back to inferring a slice size.
        QCOMPARE(ngPost.par2BlockSizeForTest(), Q_INT64_C(5242880));
    }
}

void TestMainWindow::log_in_file_is_written_in_the_config_folder()
{
    HomeSandbox sandbox;
    {
        QFile conf(PathHelper::configFilePath());
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&conf) << "LOG_IN_FILE = true\n"
                           << "[server]\n"
                           << "host = news.example.invalid\n"
                           << "port = 563\n"
                           << "ssl = true\n"
                           << "connection = 1\n"
                           << "enabled = true\n";
    }
    const QString logPath = PathHelper::configDir() + QStringLiteral("/ngPost.log");
    const QString homeLog = QDir(sandbox.rootPath()).filePath(QStringLiteral("ngPost.log"));

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    {
        NgPost ngPost(argc, argv);
        const QString parseErr = ngPost.parseDefaultConfig();
        QVERIFY2(QFile::exists(logPath),
                 qPrintable(
                     QStringLiteral("no log at %1 (config errors: %2)").arg(logPath, parseErr)));
    }
    QVERIFY2(!QFile::exists(homeLog), qPrintable(homeLog));
    // closed and flushed with ngPost: the start line must have reached the file
    QVERIFY(QFileInfo(logPath).size() > 0);
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

void TestMainWindow::post_info_dialog_offers_a_destination()
{
    HomeSandbox sandbox;
    const QString tmplPath = sandbox.rootPath() + QStringLiteral("/sheet.tpl");
    {
        QFile tmpl(tmplPath);
        QVERIFY(tmpl.open(QIODevice::WriteOnly));
        tmpl.write("titre =__meta:titre__\n");
    }

    PostInfoData preview;
    preview.nzbDir  = sandbox.rootPath() + QStringLiteral("/nzb");
    preview.nzbName = QStringLiteral("mon-post");

    const QString configured = QStringLiteral("__nzbDir__/__nzbName__.info.txt");
    PostInfoDialog dlg(tmplPath, QString(), QMap<QString, MetaValue>(), QStringList(),
                       preview, configured);

    auto *out = dlg.findChild<QLineEdit *>(QStringLiteral("postInfoOutput"));
    auto *hint = dlg.findChild<QLabel *>(QStringLiteral("postInfoOutputHint"));
    QVERIFY(out);
    QVERIFY(hint);

    // Empty by default: the post follows the configuration, and the field
    // shows that rather than pretending the post chose it.
    QVERIFY(out->text().isEmpty());
    QCOMPARE(out->placeholderText(), configured);
    QVERIFY(dlg.outputOverride().isEmpty());

    // ...and the hint resolves the configured pattern, so the user sees the
    // actual path instead of the variables.
    QVERIFY2(hint->text().contains(QStringLiteral("mon-post.info.txt")), qPrintable(hint->text()));

    // Typing a destination makes it this post's own.
    const QString mine = sandbox.rootPath() + QStringLiteral("/ailleurs/__nzbName__.json");
    out->setText(mine);
    emit out->textEdited(mine);
    QCOMPARE(dlg.outputOverride(), mine);
    QVERIFY2(hint->text().contains(QStringLiteral("ailleurs")), qPrintable(hint->text()));
    QVERIFY2(hint->text().contains(QStringLiteral("mon-post.json")), qPrintable(hint->text()));

    // Typing back exactly what the configuration says is not an override: the
    // post keeps following it.
    out->setText(configured);
    emit out->textEdited(configured);
    QVERIFY(dlg.outputOverride().isEmpty());

    // A variable ngPost does not know is reported rather than written to.
    out->setText(QStringLiteral("/tmp/__nawak__.txt"));
    emit out->textEdited(QStringLiteral("/tmp/__nawak__.txt"));
    QVERIFY2(hint->text().contains(QStringLiteral("__nawak__")), qPrintable(hint->text()));
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

    auto *redundancy = quickTab->findChild<QSpinBox*>(QStringLiteral("redundancySB"));
    auto *compress = quickTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    auto *par2 = quickTab->findChild<QCheckBox*>(QStringLiteral("par2CB"));
    QVERIFY2(redundancy, "redundancySB not found on Quick tab");
    QVERIFY(compress);
    QVERIFY(par2);

    // The redundancy percentage follows PAR2, which itself follows the
    // compression of this post (see greyed_controls_say_what_they_need).
    compress->setChecked(false);
    par2->setChecked(false);
    QVERIFY(!redundancy->isEnabled());

    compress->setChecked(true);
    par2->setChecked(true);
    QVERIFY(redundancy->isEnabled());

    // The volume limit is a configuration value now: it is set in the dialog,
    // which writes the file itself. Per-post PAR2 overrides never replace the default.
    {
        CompressionSettingsDialog dlg(&ngPost, window);
        auto *rarMax = dlg.findChild<QCheckBox*>(QStringLiteral("rarMaxCB"));
        QVERIFY2(rarMax, "rarMaxCB not found in the Compression settings dialog");
        QVERIFY2(rarMax->isChecked(), "the dialog did not load RAR_MAX from the config");
        rarMax->setChecked(false);
        dlg.accept();
    }
    redundancy->setValue(17);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\n#RAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("\nPAR2_PCT = 8\n")),
             qPrintable(content));

    saved.close();
    {
        CompressionSettingsDialog dlg(&ngPost, window);
        auto *rarMax = dlg.findChild<QCheckBox*>(QStringLiteral("rarMaxCB"));
        QVERIFY(rarMax);
        QVERIFY2(!rarMax->isChecked(), "the dialog did not reload the state it just wrote");
        rarMax->setChecked(true);
        dlg.accept();
    }
    redundancy->setValue(23);
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\nRAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("\n#RAR_MAX = 99\n")),
             qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("\nPAR2_PCT = 8\n")),
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

namespace {

//! Boots an NgPost on a sandboxed HOME with the given configuration and returns
//! its initialised MainWindow. Every test below needs the same six lines.
MainWindow *bootWindow(NgPost &ngPost, const QString &confBody, QString *error)
{
    const QString confPath = PathHelper::configFilePath();
    {
        QFile conf(confPath);
        if (!conf.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            *error = QStringLiteral("Could not write test config: %1").arg(confPath);
            return nullptr;
        }
        QTextStream s(&conf);
        s << confBody;
    }

    *error = ngPost.parseDefaultConfig();
    if (!error->isEmpty())
        return nullptr;

    MainWindow *window = ngPost.mainWindowForTest();
    if (!window)
    {
        *error = QStringLiteral("NgPost did not create a GUI MainWindow for the test");
        return nullptr;
    }
    window->init(&ngPost);
    return window;
}

//! objectName of the layout or of the widget held by one item of a box layout,
//! so a test can state the order of the lines rather than their contents.
QString itemName(QLayoutItem *item)
{
    if (!item)
        return QString();
    if (QLayout *l = item->layout())
        return l->objectName();
    if (QWidget *w = item->widget())
        return w->objectName();
    return QStringLiteral("<spacer>");
}

} // namespace

void TestMainWindow::posting_tab_lines_are_in_the_new_order()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY2(tabs, "postTabWidget not found");
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);

    auto *column = quickTab->findChild<QVBoxLayout*>(QStringLiteral("verticalLayout"));
    QVERIFY2(column, "verticalLayout not found on the quick tab");

    QStringList order;
    for (int i = 0; i < column->count(); ++i)
        order << itemName(column->itemAt(i));

    const QStringList expected{
        QStringLiteral("horizontalLayout_9"),  // line A: nzb, nfo, post info sheet
        QStringLiteral("filesList"),
        QStringLiteral("horizontalLayout_4"),  // the Select Files / Folder buttons
        QStringLiteral("packingLayout"),       // one line: how this post is packed
        QStringLiteral("postLayout"),
    };
    QCOMPARE(order, expected);

    // The configuration widgets left the tab for the dialog; a leftover copy
    // here would silently overwrite what the dialog wrote.
    QVERIFY(!quickTab->findChild<QLineEdit*>(QStringLiteral("compressPathEdit")));
    QVERIFY(!quickTab->findChild<QLineEdit*>(QStringLiteral("rarEdit")));
    QVERIFY(!quickTab->findChild<QLineEdit*>(QStringLiteral("rarSizeEdit")));

    // The check box labels the field it commands, so there is no separate label.
    QVERIFY(!quickTab->findChild<QLabel*>(QStringLiteral("compressNameLbl")));
    auto *compressBox = quickTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    QVERIFY(compressBox);
    QVERIFY(compressBox->text().endsWith(QLatin1Char(':')));

    // The volume limit followed the volume size into the dialog.
    QVERIFY(!quickTab->findChild<QCheckBox*>(QStringLiteral("rarMaxCB")));
}

void TestMainWindow::compression_settings_dialog_owns_the_configuration_widgets()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    QVERIFY2(window->findChild<QPushButton*>(QStringLiteral("compressionSettingsBtn")),
             "the head does not offer the Compression settings button");
    // The fixed password left the head with everything that served it.
    QVERIFY(!window->findChild<QCheckBox*>(QStringLiteral("rarPassCB")));
    QVERIFY(!window->findChild<QLineEdit*>(QStringLiteral("rarPassEdit")));

    CompressionSettingsDialog dlg(&ngPost, window);
    QVERIFY(dlg.findChild<QLineEdit*>(QStringLiteral("compressPathEdit")));
    QVERIFY(dlg.findChild<QLineEdit*>(QStringLiteral("rarEdit")));
    QVERIFY(dlg.findChild<QLineEdit*>(QStringLiteral("rarSizeEdit")));
    QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("rarMaxCB")));
    QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("keepRarDefaultCB")));
    QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("rarPassCB")));
    QVERIFY(dlg.findChild<QLineEdit*>(QStringLiteral("rarPassEdit")));
    QVERIFY(dlg.findChild<QSpinBox*>(QStringLiteral("rarLengthSB")));
}

void TestMainWindow::compression_settings_dialog_persists_on_validation()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost,
                                    QStringLiteral("GROUPS = alt.binaries.test\n"
                                                   "RAR_SIZE = 50\n"),
                                    &err);
    QVERIFY2(window, qPrintable(err));

    CompressionSettingsDialog dlg(&ngPost, window);
    auto *rarSize  = dlg.findChild<QLineEdit*>(QStringLiteral("rarSizeEdit"));
    auto *passCB   = dlg.findChild<QCheckBox*>(QStringLiteral("rarPassCB"));
    auto *passEdit = dlg.findChild<QLineEdit*>(QStringLiteral("rarPassEdit"));
    QVERIFY(rarSize && passCB && passEdit);

    // What the dialog loaded is what the configuration said.
    QCOMPARE(rarSize->text(), QStringLiteral("50"));
    QVERIFY(!passCB->isChecked());

    rarSize->setText(QStringLiteral("42"));
    passCB->setChecked(true);
    passEdit->setText(QStringLiteral("s3cret"));
    // Note we deliberately never call onSaveConfig.
    dlg.accept();

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\nRAR_SIZE = 42\n")), qPrintable(content));
    QVERIFY2(content.contains(QStringLiteral("\nRAR_PASS = s3cret\n")), qPrintable(content));
}

void TestMainWindow::greyed_controls_say_what_they_need()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);

    auto *compress   = quickTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    auto *par2       = quickTab->findChild<QCheckBox*>(QStringLiteral("par2CB"));
    auto *keepRar    = quickTab->findChild<QCheckBox*>(QStringLiteral("keepRarCB"));
    auto *nzbPass    = quickTab->findChild<QCheckBox*>(QStringLiteral("nzbPassCB"));
    auto *nzbPassEd  = quickTab->findChild<QLineEdit*>(QStringLiteral("nzbPassEdit"));
    auto *redundancy = quickTab->findChild<QSpinBox*>(QStringLiteral("redundancySB"));
    QVERIFY(compress && par2 && keepRar && nzbPass && nzbPassEd && redundancy);

    compress->setChecked(false);

    // Nothing on the packing line means anything while this post is not
    // compressed, so the whole line follows that one box.
    QVERIFY(!keepRar->isEnabled());
    QVERIFY(!nzbPass->isEnabled());
    QVERIFY(!nzbPassEd->isEnabled());
    QVERIFY(!par2->isEnabled());
    QVERIFY(!redundancy->isEnabled());

    // The point of the change: a greyed control names its switch, so the user
    // is not left with a dead box and no way to find out why.
    QVERIFY2(keepRar->toolTip().contains(compress->text()), qPrintable(keepRar->toolTip()));
    QVERIFY2(nzbPass->toolTip().contains(compress->text()), qPrintable(nzbPass->toolTip()));
    QVERIFY2(par2->toolTip().contains(compress->text()), qPrintable(par2->toolTip()));
    QVERIFY2(redundancy->toolTip().contains(compress->text()), qPrintable(redundancy->toolTip()));
    // and it keeps the help it already carried.
    QVERIFY2(keepRar->toolTip().contains(QStringLiteral("deleted uppon post success")),
             qPrintable(keepRar->toolTip()));

    compress->setChecked(true);

    QVERIFY(keepRar->isEnabled());
    QVERIFY(nzbPass->isEnabled());
    QVERIFY(par2->isEnabled());
    // Once reachable, the requirement is gone: it would be noise.
    QVERIFY2(!keepRar->toolTip().contains(compress->text()), qPrintable(keepRar->toolTip()));
    QVERIFY2(keepRar->toolTip().contains(QStringLiteral("deleted uppon post success")),
             qPrintable(keepRar->toolTip()));

    // Two of them keep a second switch of their own, and name that one instead.
    nzbPass->setChecked(false);
    QVERIFY(!nzbPassEd->isEnabled());
    QVERIFY2(nzbPassEd->toolTip().contains(nzbPass->text()), qPrintable(nzbPassEd->toolTip()));
    nzbPass->setChecked(true);
    QVERIFY(nzbPassEd->isEnabled());

    par2->setChecked(false);
    QVERIFY(!redundancy->isEnabled());
    QVERIFY2(redundancy->toolTip().contains(par2->text()), qPrintable(redundancy->toolTip()));
    par2->setChecked(true);
    QVERIFY(redundancy->isEnabled());
}

void TestMainWindow::state_tooltips_survive_a_language_change()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    auto *quickTab = qobject_cast<PostingWidget*>(tabs->widget(0));
    QVERIFY(quickTab);

    auto *compress = quickTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    auto *keepRar  = quickTab->findChild<QCheckBox*>(QStringLiteral("keepRarCB"));
    QVERIFY(compress && keepRar);

    compress->setChecked(false);
    QVERIFY(!keepRar->toolTip().isEmpty());

    // retranslateUi() resets every tooltip to the plain .ui text. Before the
    // fix, the requirement was written once and never put back.
    quickTab->retranslate();

    QVERIFY(!keepRar->isEnabled());
    QVERIFY2(keepRar->toolTip().contains(compress->text()), qPrintable(keepRar->toolTip()));
}

void TestMainWindow::auto_post_tab_greys_the_same_controls_as_a_posting_tab()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    QWidget *autoTab = tabs->widget(1);
    QVERIFY(autoTab);

    // The paths and the volume size are gone from here too: one dialog owns them.
    QVERIFY(!autoTab->findChild<QLineEdit*>(QStringLiteral("compressPathEdit")));
    QVERIFY(!autoTab->findChild<QLineEdit*>(QStringLiteral("rarEdit")));
    QVERIFY(!autoTab->findChild<QLineEdit*>(QStringLiteral("rarSizeEdit")));

    QVERIFY(!autoTab->findChild<QCheckBox*>(QStringLiteral("rarMaxCB")));

    auto *compress   = autoTab->findChild<QCheckBox*>(QStringLiteral("compressCB"));
    auto *par2       = autoTab->findChild<QCheckBox*>(QStringLiteral("par2CB"));
    auto *keepRar    = autoTab->findChild<QCheckBox*>(QStringLiteral("keepRarCB"));
    auto *redundancy = autoTab->findChild<QSpinBox*>(QStringLiteral("redundancySB"));
    QVERIFY(compress && par2 && keepRar && redundancy);

    // This tab forces PAR2 on when compression goes off (it cannot post folders
    // without one of the two), so PAR2 is asserted separately from compression.
    compress->setChecked(false);
    QVERIFY(!keepRar->isEnabled());
    QVERIFY2(keepRar->toolTip().contains(compress->text()), qPrintable(keepRar->toolTip()));

    compress->setChecked(true);
    QVERIFY(keepRar->isEnabled());

    par2->setChecked(false);
    QVERIFY(!redundancy->isEnabled());
    QVERIFY2(redundancy->toolTip().contains(par2->text()), qPrintable(redundancy->toolTip()));
}

void TestMainWindow::default_archive_password_still_reaches_the_posting_tabs()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);
    auto *nzbPassEdit = quickTab->findChild<QLineEdit*>(QStringLiteral("nzbPassEdit"));
    QVERIFY(nzbPassEdit);
    QVERIFY(nzbPassEdit->text().isEmpty());

    CompressionSettingsDialog dlg(&ngPost, window);
    auto *passCB   = dlg.findChild<QCheckBox*>(QStringLiteral("rarPassCB"));
    auto *passEdit = dlg.findChild<QLineEdit*>(QStringLiteral("rarPassEdit"));
    QVERIFY(passCB && passEdit);
    passCB->setChecked(true);
    passEdit->setText(QStringLiteral("hunter2"));
    dlg.accept();

    // What the head used to do live on every keystroke now happens once, when
    // the dialog is validated.
    QVERIFY(QMetaObject::invokeMethod(window, "onRarPassUpdated", Qt::DirectConnection,
                                      Q_ARG(QString, dlg.fixedPassword())));
    QCOMPARE(nzbPassEdit->text(), QStringLiteral("hunter2"));
    QVERIFY(window->useFixedPassword());
    QCOMPARE(window->fixedArchivePassword(), QStringLiteral("hunter2"));
}

void TestMainWindow::keep_archives_default_is_owned_by_the_dialog()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &err);
    QVERIFY2(window, qPrintable(err));

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    QWidget *quickTab = tabs->widget(0);
    QVERIFY(quickTab);
    auto *keepRar = quickTab->findChild<QCheckBox*>(QStringLiteral("keepRarCB"));
    QVERIFY(keepRar);
    QVERIFY(!keepRar->isChecked());

    {
        CompressionSettingsDialog dlg(&ngPost, window);
        auto *def = dlg.findChild<QCheckBox*>(QStringLiteral("keepRarDefaultCB"));
        QVERIFY2(def, "keepRarDefaultCB not found in the Compression settings dialog");
        QVERIFY(!def->isChecked());
        def->setChecked(true);
        dlg.accept();
    }

    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(saved.readAll());
    saved.close();
    QVERIFY2(content.contains(QStringLiteral("\nKEEP_RAR = true\n")), qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("\n#KEEP_RAR = true\n")), qPrintable(content));

    // A tab opened afterwards starts from that default.
    PostingWidget *fresh = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(fresh);
    fresh->init();
    auto *freshKeep = fresh->findChild<QCheckBox*>(QStringLiteral("keepRarCB"));
    QVERIFY(freshKeep);
    QVERIFY2(freshKeep->isChecked(), "a new tab did not pick up the configured default");

    // But what one post decides stays with that post: saving from it must not
    // silently turn the default off for every post to come.
    freshKeep->setChecked(false);
    tabs->setCurrentIndex(tabs->indexOf(fresh));
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));

    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("\nKEEP_RAR = true\n")), qPrintable(content));
    QVERIFY2(!content.contains(QStringLiteral("\n#KEEP_RAR = true\n")), qPrintable(content));
}

namespace
{
//! The startup tab lives in the GUI preferences INI, next to the window
//! geometry -- not in ngPost.conf, which is the posting configuration.
QString guiSettingsPath()
{
    return PathHelper::configDir() + QStringLiteral("/ngPost_gui.ini");
}

//! Minimal configuration: these tests only need NgPost to parse and build its
//! window.
void writeMinimalConf(const QString &tmpDir)
{
    QFile conf(PathHelper::configFilePath());
    QVERIFY2(conf.open(QIODevice::WriteOnly | QIODevice::Text), "could not write the test config");
    QTextStream s(&conf);
    s << "GROUPS = alt.binaries.test\n"
      << "TMP_DIR = " << tmpDir << "\n";
}

//! Pick "Open this tab on startup" in the context menu of \a tabIndex, the
//! way a right click on that tab then a click on the entry would.
void pickStartupEntry(MainWindow *window, int tabIndex)
{
    QMenu menu;
    window->fillTabContextMenuForTest(menu, tabIndex);
    QAction *startup = menu.actions().value(0);
    QVERIFY2(startup && startup->isCheckable(),
             qPrintable(QStringLiteral("no startup entry on tab %1").arg(tabIndex)));
    startup->trigger();
}

//! Whether that entry is ticked in the context menu of \a tabIndex -- false
//! too when the entry is not there at all, which no caller expects.
bool startupEntryIsTicked(MainWindow *window, int tabIndex)
{
    QMenu menu;
    window->fillTabContextMenuForTest(menu, tabIndex);
    QAction *startup = menu.actions().value(0);
    return startup && startup->isCheckable() && startup->isChecked();
}
} // namespace

void TestMainWindow::history_columns_can_be_resized_by_hand()
{
    HomeSandbox sandbox;
    writeMinimalConf(sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *table = window->findChild<QTableWidget*>(QStringLiteral("historyTable"));
    QVERIFY2(table, "historyTable not found");
    QHeaderView *header = table->horizontalHeader();

    // Not one column is sized by the header: they all answer to the mouse.
    for (int col = 0; col < table->columnCount(); ++col)
        QCOMPARE(header->sectionResizeMode(col), QHeaderView::Interactive);

    // Until the user resizes one, ngPost still fits them to what it displays --
    // this is what every refresh calls.
    const int nameColumn = 1;
    window->fitHistoryColumnsForTest(true);
    QVERIFY(header->sectionSize(nameColumn) > 0);

    // Narrowing the name column is precisely what a Stretch section refused.
    header->resizeSection(nameColumn, 120);
    QCOMPARE(header->sectionSize(nameColumn), 120);

    // And it stays: a refresh no longer resizes the columns behind the user.
    window->fitHistoryColumnsForTest(true);
    QCOMPARE(header->sectionSize(nameColumn), 120);
}

void TestMainWindow::history_column_widths_survive_a_restart()
{
    HomeSandbox sandbox;
    writeMinimalConf(sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    const QString columnsKey = QStringLiteral("MainWindow/historyColumns");
    const int     nameColumn = 1;

    {
        NgPost ngPost(argc, argv);
        const QString parseError = ngPost.parseDefaultConfig();
        QVERIFY2(parseError.isEmpty(), qPrintable(parseError));
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);

        auto *table = window->findChild<QTableWidget*>(QStringLiteral("historyTable"));
        QVERIFY2(table, "historyTable not found");

        // Nothing is written as long as ngPost owns the widths.
        {
            QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
            QVERIFY2(!guiSettings.contains(columnsKey), "widths saved before the user set any");
        }

        // What a drag does. The write is debounced, so it lands shortly after.
        table->horizontalHeader()->resizeSection(nameColumn, 137);
        QTRY_VERIFY(QSettings(guiSettingsPath(), QSettings::IniFormat).contains(columnsKey));
    }

    // Next run: the columns come back as they were left, and ngPost still does
    // not size them.
    {
        NgPost ngPost(argc, argv);
        const QString parseError = ngPost.parseDefaultConfig();
        QVERIFY2(parseError.isEmpty(), qPrintable(parseError));
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);

        auto *table = window->findChild<QTableWidget*>(QStringLiteral("historyTable"));
        QVERIFY2(table, "historyTable not found");
        QCOMPARE(table->horizontalHeader()->sectionSize(nameColumn), 137);

        window->fitHistoryColumnsForTest(true);
        QCOMPARE(table->horizontalHeader()->sectionSize(nameColumn), 137);

        // "Reset column widths" hands them back, and takes the setting away so
        // the next run starts from ngPost's own layout again.
        QVERIFY(QMetaObject::invokeMethod(window, "_resetHistoryColumns", Qt::DirectConnection));
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        QVERIFY2(!guiSettings.contains(columnsKey), "the reset left the widths behind");
    }
}

void TestMainWindow::startup_tab_is_the_quick_post_until_one_is_pinned()
{
    HomeSandbox sandbox;
    writeMinimalConf(sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString parseError = ngPost.parseDefaultConfig();
    QVERIFY2(parseError.isEmpty(), qPrintable(parseError));

    MainWindow *window = ngPost.mainWindowForTest();
    QVERIFY(window);
    window->init(&ngPost);

    auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
    QVERIFY2(tabs, "postTabWidget not found");
    auto *tabBar = qobject_cast<StartupTabBar*>(tabs->tabBar());
    QVERIFY2(tabBar, "the post tab widget does not carry a StartupTabBar");

    // Nothing pinned: the quick post tab, and no title in bold.
    QCOMPARE(window->startupTabForTest(), -1);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(tabBar->startupTab(), -1);

    // The three fixed tabs offer the option, unticked...
    for (int tabIndex = 0; tabIndex < 3; ++tabIndex)
    {
        QMenu menu;
        window->fillTabContextMenuForTest(menu, tabIndex);
        QAction *startup = menu.actions().value(0);
        QVERIFY2(startup && startup->isCheckable(),
                 qPrintable(QStringLiteral("tab %1 has no startup entry").arg(tabIndex)));
        QVERIFY(!startup->isChecked());
    }
    // ... the "New" tab does not: its menu starts with "Close All finished Tabs".
    {
        QMenu menu;
        window->fillTabContextMenuForTest(menu, tabs->count() - 1);
        QAction *first = menu.actions().value(0);
        QVERIFY(first && !first->isCheckable());
    }

    // Pin the history tab: bold right away, saved right away, and the user is
    // left on the tab they were reading.
    pickStartupEntry(window, 2);
    QCOMPARE(window->startupTabForTest(), 2);
    QCOMPARE(tabBar->startupTab(), 2);
    QCOMPARE(tabs->currentIndex(), 0);
    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        QCOMPARE(guiSettings.value(QStringLiteral("MainWindow/startupTab")).toInt(), 2);
    }

    // The tick follows the pinned tab, and only it.
    QVERIFY(startupEntryIsTicked(window, 2));
    QVERIFY(!startupEntryIsTicked(window, 1));

    // Pinning another tab moves the setting rather than adding one.
    pickStartupEntry(window, 1);
    QCOMPARE(tabBar->startupTab(), 1);
    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        QCOMPARE(guiSettings.value(QStringLiteral("MainWindow/startupTab")).toInt(), 1);
    }

    // Picking it again on the tab that is already the startup one clears the
    // setting: back to unspecified, and the key leaves the file.
    pickStartupEntry(window, 1);
    QCOMPARE(window->startupTabForTest(), -1);
    QCOMPARE(tabBar->startupTab(), -1);
    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        QVERIFY2(!guiSettings.contains(QStringLiteral("MainWindow/startupTab")),
                 "unpinning left the setting behind");
    }
}

void TestMainWindow::pinned_startup_tab_opens_on_the_next_start()
{
    HomeSandbox sandbox;
    writeMinimalConf(sandbox.rootPath());

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };

    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        guiSettings.setValue(QStringLiteral("MainWindow/startupTab"), 1);
    }
    {
        NgPost ngPost(argc, argv);
        const QString parseError = ngPost.parseDefaultConfig();
        QVERIFY2(parseError.isEmpty(), qPrintable(parseError));
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);

        auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
        QVERIFY(tabs);
        auto *tabBar = qobject_cast<StartupTabBar*>(tabs->tabBar());
        QVERIFY(tabBar);

        QCOMPARE(tabs->currentIndex(), 1);
        QCOMPARE(tabBar->startupTab(), 1);
        QVERIFY(startupEntryIsTicked(window, 1));
    }

    // A value pointing at no fixed tab (an old setting, a hand edited file)
    // is ignored: the quick post tab, with nothing in bold.
    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        guiSettings.setValue(QStringLiteral("MainWindow/startupTab"), 7);
    }
    {
        NgPost ngPost(argc, argv);
        const QString parseError = ngPost.parseDefaultConfig();
        QVERIFY2(parseError.isEmpty(), qPrintable(parseError));
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);

        auto *tabs = window->findChild<QTabWidget*>(QStringLiteral("postTabWidget"));
        QVERIFY(tabs);
        auto *tabBar = qobject_cast<StartupTabBar*>(tabs->tabBar());
        QVERIFY(tabBar);

        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(tabBar->startupTab(), -1);
    }
}

void TestMainWindow::obfuscate_config_key_carries_both_kinds()
{
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };

    struct Case
    {
        const char *value;
        bool        articles;
        bool        fileName;
    };
    // "file_name" and "file name" are accepted too: the GUI has always called
    // this "File Name Obfuscation", and a user copying that wording into the
    // config should not be met with silence.
    const QVector<Case> cases = {
        { "article", true, false },
        { "filename", false, true },
        { "article, filename", true, true },
        { "filename, article", true, true },
        { "file_name", false, true },
        { "FileName", false, true },
    };

    for (Case const &c : cases)
    {
        HomeSandbox sandbox;
        const QString confPath = PathHelper::configFilePath();
        {
            QFile conf(confPath);
            QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream s(&conf);
            s << "GROUPS = alt.binaries.test\n"
              << "obfuscate = " << c.value << "\n";
        }

        NgPost ngPost(argc, argv);
        QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), c.value);
        QVERIFY2(ngPost.obfuscateArticlesForTest() == c.articles, c.value);
        QVERIFY2(ngPost.obfuscateFileNameForTest() == c.fileName, c.value);
    }
}

void TestMainWindow::obfuscate_config_key_survives_a_save()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n"
          << "obfuscate = article, filename\n";
    }

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };

    {
        NgPost ngPost(argc, argv);
        QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), "first parse failed");
        QVERIFY(ngPost.obfuscateArticlesForTest());
        QVERIFY(ngPost.obfuscateFileNameForTest());
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);
        ngPost.saveConfig();
    }

    {
        NgPost ngPost(argc, argv);
        QVERIFY2(ngPost.parseDefaultConfig().isEmpty(), "reparse of the saved config failed");
        QVERIFY2(ngPost.obfuscateArticlesForTest(), "article obfuscation lost on save");
        QVERIFY2(ngPost.obfuscateFileNameForTest(), "file name obfuscation lost on save");
    }

    // Nothing enabled must be written commented out, and must still name a
    // value the user can simply uncomment.
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "GROUPS = alt.binaries.test\n";
    }
    {
        NgPost ngPost(argc, argv);
        QVERIFY(ngPost.parseDefaultConfig().isEmpty());
        MainWindow *window = ngPost.mainWindowForTest();
        QVERIFY(window);
        window->init(&ngPost);
        ngPost.saveConfig();
    }
    QFile saved(confPath);
    QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(saved.readAll());
    QVERIFY2(content.contains(QStringLiteral("#obfuscate = article")),
             "a disabled obfuscation must stay a commented, uncommentable example");
    {
        NgPost ngPost(argc, argv);
        QVERIFY(ngPost.parseDefaultConfig().isEmpty());
        QVERIFY2(!ngPost.obfuscateArticlesForTest(), "commented key must stay off");
        QVERIFY2(!ngPost.obfuscateFileNameForTest(), "commented key must stay off");
    }
}

void TestMainWindow::log_pane_stays_within_its_budget()
{
    MainWindow win;
    win.setLogBlockCapForTest(200);

    for (int i = 0; i < 5000; ++i)
        win.log(QStringLiteral("[builder #1] article %1 encoded").arg(i));

    // Cap plus one slice is the contract: the pane is cut back once it runs a
    // whole slice over, so that is the ceiling, not the cap itself.
    QVERIFY2(win.logBlockCountForTest() <= 200 + 1000 + 1,
             qPrintable(QStringLiteral("log pane held %1 blocks for a cap of 200")
                                .arg(win.logBlockCountForTest())));
    QVERIFY(win.logBlockCountForTest() >= 200);
}

void TestMainWindow::log_pane_keeps_the_newest_lines()
{
    MainWindow win;
    win.setLogBlockCapForTest(50);

    for (int i = 0; i < 3000; ++i)
        win.log(QStringLiteral("line %1").arg(i));

    const QString kept = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"))
                                 ->toPlainText();
    QVERIFY2(kept.contains(QStringLiteral("line 2999")), "the newest line was trimmed away");
    QVERIFY2(!kept.contains(QStringLiteral("line 0\n")), "the oldest line survived the trim");
}

//! The foreground colour actually applied to the last character of the pane.
//! logError() used to emit an HTML fragment; it now sets a QTextCharFormat, so
//! the "errors are red" contract needs an assertion of its own.
static QColor lastLogColour(const QTextDocument *document)
{
    QTextBlock const last = document->lastBlock();
    QColor colour;
    for (QTextBlock::iterator it = last.begin(); !it.atEnd(); ++it) {
        QTextFragment const fragment = it.fragment();
        if (fragment.isValid())
            colour = fragment.charFormat().foreground().color();
    }
    return colour;
}

static int longestLogBlock(const QTextDocument *document)
{
    int longest = 0;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next())
        longest = qMax(longest, block.length() - 1); // exclude the block separator
    return longest;
}

void TestMainWindow::log_pane_bounds_fragments_without_newlines()
{
    MainWindow win;
    const int blockCharacterCap = win.logMaxBlockCharactersForTest();

    win.log(QStringLiteral("old-marker"), false);
    const QString fragment(100, QLatin1Char('*'));
    for (int i = 0; i < 2000; ++i)
        win.log(fragment, false);
    win.log(QStringLiteral("new-marker"), false);

    QTextBrowser *browser = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"));
    QVERIFY(browser);
    QVERIFY2(win.logBlockCountForTest() > 1, "the fragment stream remained one block");
    QCOMPARE(longestLogBlock(browser->document()), blockCharacterCap);

    QString kept = browser->toPlainText();
    kept.remove(QRegularExpression("\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
    kept.remove(QLatin1Char('\n')); // only the deliberate safety boundaries
    QCOMPARE(kept,
             QStringLiteral("old-marker") + QString(200000, QLatin1Char('*'))
                 + QStringLiteral("new-marker"));
}

void TestMainWindow::log_pane_bounds_one_large_fragment_without_newline()
{
    MainWindow win;
    const QString payload(200021, QLatin1Char('x'));
    win.log(payload, false);

    QTextBrowser *browser = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"));
    QVERIFY(browser);
    QVERIFY(win.logBlockCountForTest() > 1);
    QCOMPARE(longestLogBlock(browser->document()), win.logMaxBlockCharactersForTest());

    QString kept = browser->toPlainText();
    kept.remove(QRegularExpression("\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
    kept.remove(QLatin1Char('\n'));
    QCOMPARE(kept, payload);
}

void TestMainWindow::log_pane_fragment_splitting_preserves_text_boundaries()
{
    MainWindow win;
    const int cap = win.logMaxBlockCharactersForTest();
    const QString emoji = QString::fromUtf8("\xF0\x9F\x99\x82");
    const QString payload = QString(cap - 1, QLatin1Char('a')) + emoji
        + QStringLiteral("b\r\nc\rd\ne") + QChar(0x2028) + QStringLiteral("f")
        + QChar(0x2029) + QString(cap + 3, QLatin1Char('z'));
    win.log(payload, false);

    QTextBrowser *browser = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"));
    QVERIFY(browser);
    QVERIFY(longestLogBlock(browser->document()) <= cap);
    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        QVERIFY2(text.isEmpty() || !text.front().isLowSurrogate(),
                 "a block starts with the low half of a surrogate pair");
        QVERIFY2(text.isEmpty() || !text.back().isHighSurrogate(),
                 "a block ends with the high half of a surrogate pair");
    }

    QString flattened;
    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next())
        flattened += block.text();
    QString expected = payload;
    expected.remove(QLatin1Char('\r'));
    expected.remove(QLatin1Char('\n'));
    expected.remove(QChar(0x2028));
    expected.remove(QChar(0x2029));
    flattened.remove(QRegularExpression("\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
    QCOMPARE(flattened, expected);
}

void TestMainWindow::vpn_affordances_follow_platform_support()
{
    MainWindow win;
    const bool supported = VpnManager::vpnPlatformSupported();

    QWidget *settingsBtn = win.findChild<QWidget *>(QStringLiteral("vpnSettingsBtn"));
    QWidget *stateLbl    = win.findChild<QWidget *>(QStringLiteral("vpnStateLbl"));
    QVERIFY(settingsBtn);
    QVERIFY(stateLbl);
    // isVisibleTo(), not isVisible(): the window is never shown here, and we
    // are asking whether it *would* appear, which is the user-visible claim.
    QCOMPARE(settingsBtn->isVisibleTo(&win), supported);
    QCOMPARE(stateLbl->isVisibleTo(&win), supported);

    QTableWidget *servers = win.findChild<QTableWidget *>(QStringLiteral("serversTable"));
    QVERIFY(servers);
    // The table starts empty: its columns only come into being when a row is
    // added, which is also when the implementation gets its chance to hide the
    // VPN one.
    addServer(&win);
    QVERIFY(MainWindow::kServerUseVpnColumn < servers->columnCount());
    QCOMPARE(!servers->isColumnHidden(MainWindow::kServerUseVpnColumn), supported);
}

void TestMainWindow::log_pane_bounds_whole_lines_and_errors()
{
    MainWindow win;
    const int cap = win.logMaxBlockCharactersForTest();
    QTextBrowser *browser = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"));
    QVERIFY(browser);

    // A newline-terminated line far longer than one block.
    win.log(QString(cap * 4 + 17, QLatin1Char('a')), true);
    QVERIFY2(win.logBlockCountForTest() > 1, "a long whole line stayed in one block");
    QCOMPARE(longestLogBlock(browser->document()), cap);

    // The same through logError(), which used to build an HTML fragment.
    const QString marker = QStringLiteral("ERR-MARKER");
    win.logError(QString(cap * 3, QLatin1Char('b')) + marker);
    QCOMPARE(longestLogBlock(browser->document()), cap);
    QVERIFY2(browser->toPlainText().contains(marker),
             "the error text did not survive the bounded insertion");
    QCOMPARE(lastLogColour(browser->document()), QColor(Qt::red));

    // And an ordinary line that follows must not inherit the error colour.
    win.log(QStringLiteral("back to normal"), true);
    QVERIFY2(lastLogColour(browser->document()) != QColor(Qt::red),
             "a plain log line was rendered with the error colour");
}

void TestMainWindow::log_pane_shows_markup_as_plain_text()
{
    MainWindow win;
    QTextBrowser *browser = win.findChild<QTextBrowser *>(QStringLiteral("logBrowser"));
    QVERIFY(browser);

    // Pinned deliberately: rendering markup here would also mean rendering it
    // in the file names the log is full of, which is the caller's text and not
    // ngPost's. Emphasis belongs in a QTextCharFormat -- see logError().
    win.log(QStringLiteral("<h3>Start Post #1: movie.nzb</h3>"), true);
    QVERIFY2(browser->document()->toPlainText().contains(
                     QStringLiteral("<h3>Start Post #1: movie.nzb</h3>")),
             qPrintable(browser->document()->toPlainText()));
}

void TestMainWindow::no_log_call_passes_html_markup()
{
    // Only the tags a log line would plausibly carry, so a message that merely
    // says "a < b" is not an offender.
    static QRegularExpression const markupInLogCall(
            QStringLiteral(R"(_log\s*\(\s*(?:tr|QStringLiteral|QString)\s*\()"
                           R"("[^"]*<\s*/?\s*(?:h[1-6]|b|i|u|p|br|div|span|font|a|em|strong|pre|code)\b)"));

    QStringList  offenders;
    QDirIterator it(QStringLiteral(NGPOST_SOURCE_ROOT "/src"),
                    QStringList{ QStringLiteral("*.cpp") },
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString const   path = it.next();
        QFileInfo const info(path);
        // Generated and build-tree copies are not sources anyone edits.
        if (info.fileName().startsWith(QStringLiteral("moc_"))
            || path.contains(QStringLiteral("/build/")))
            continue;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        QRegularExpressionMatchIterator m =
                markupInLogCall.globalMatch(QString::fromUtf8(file.readAll()));
        while (m.hasNext())
            offenders << QStringLiteral("%1: %2").arg(info.fileName(),
                                                      m.next().captured().simplified());
    }

    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("log messages carrying markup:\n%1")
                                .arg(offenders.join(QLatin1Char('\n')))));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto helperName = QFileInfo(app.applicationFilePath()).fileName();
    if (helperName.startsWith("ngpost-opencl-")) {
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly)) return 2;
        if (!app.arguments().contains("--opencl-list")) {
            output.write("ParPar test helper\n");
            return 0;
        }
        if (helperName.contains("slow")) QThread::msleep(400);
        if (helperName.contains("failed")) {
            output.write("Error: Could not load OpenCL runtime\n");
            return 1;
        }
        if (helperName.contains("invalid")) {
            output.write("not a device listing\n");
            return 0;
        }
        if (helperName.contains("none")) output.write("{\"platforms\":[]}");
        else if (helperName.contains("offline"))
            output.write(R"({"platforms":[{"devices":[{"name":"Offline","type":"GPU","available":false,"supported":true}]}]})");
        else
            output.write(R"({"platforms":[{"devices":[{"name":"Microsoft Basic Render Driver","type":"CPU","available":true,"supported":true}]}]})");
        return 0;
    }
    if (QFileInfo(app.applicationFilePath()).fileName().startsWith("ngpost-recording-")) {
        const auto args = app.arguments().mid(1);
        if (args.isEmpty() || args.contains("--help")) return 0;
        QFile recorded(app.applicationFilePath() + ".args");
        if (!recorded.open(QIODevice::WriteOnly)) return 1;
        recorded.write(args.join('\n').toUtf8());
        recorded.close();
        for (const auto &arg : args) {
            if (!arg.endsWith(".rar") && !arg.endsWith(".par2")) continue;
            QFile output(arg);
            if (!output.open(QIODevice::WriteOnly)) return 2;
            return output.write(QByteArray(8192, 'x')) == 8192 ? 0 : 3;
        }
        return 4;
    }
    TestMainWindow test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_MainWindow.moc"

void TestMainWindow::post_all_tabs_submits_only_prepared_posts()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "15"}));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    const QString conf = QString("GROUPS = alt.binaries.test\nthread = 1\nTMP_DIR = %1\nnzbPath = %1\n[server]\nhost = 127.0.0.1\nport = %2\nssl = false\nconnection = 1\n")
                             .arg(sandbox.rootPath()).arg(mock.port());
    auto *window = bootWindow(ngPost, conf, &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *button = window->findChild<QPushButton *>("postAllTabsButton");
    QVERIFY(button && tabs);
    QVERIFY(!button->isEnabled());
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(0));
    auto *second = window->addNewQuickTab(tabs->count() - 1);
    auto *third = window->addNewQuickTab(tabs->count() - 1);
    auto *missing = window->addNewQuickTab(tabs->count() - 1);
    window->addNewQuickTab(tabs->count() - 1); // empty, deliberately ignored
    QStringList completionOrder;
    const QList<PostingWidget *> posts{first, second, third, missing};
    for (int i = 0; i < posts.size(); ++i) {
        const QString path = sandbox.rootPath() + QString("/input%1.bin").arg(i);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(160000, char('a' + i)));
        f.close();
        posts[i]->addPath(path, 0);
        connect(posts[i], &PostingWidget::submissionEligibilityChanged, window, [&, i] {
            if (posts[i]->isPostingFinished() && !completionOrder.contains(QString::number(i)))
                completionOrder << QString::number(i);
        });
        if (posts[i] == missing) QVERIFY(QFile::remove(path));
    }
    // The visual order, not the job number or creation order, controls submission.
    tabs->tabBar()->moveTab(tabs->indexOf(third), tabs->indexOf(second));
    QVERIFY(button->isEnabled());
    button->click();
    QVERIFY(first->isPosting());
    QVERIFY(second->isPosting());
    QVERIFY(third->isPosting());
    QVERIFY(!missing->isPosting());
    button->click(); // the missing-file tab cannot duplicate or stop queued jobs
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished() && second->isPostingFinished() && third->isPostingFinished(), 20000);
    QCOMPARE(completionOrder, QStringList({"0", "2", "1"}));
    QCOMPARE(mock.receivedArticles().size(), 3);
    QVERIFY(!missing->isPostingFinished());
    window->closeTab(missing);
    QVERIFY(!button->isEnabled());
    // A retained finished tab can be cleared and used for a new post.
    QVERIFY(QMetaObject::invokeMethod(first, "onClearFilesClicked", Qt::DirectConnection));
    const QString path = sandbox.rootPath() + "/again.bin";
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly)); f.write("again"); f.close();
    first->addPath(path, 0);
    QVERIFY(button->isEnabled());
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished(), 15000);
    QCOMPARE(mock.receivedArticles().size(), 4);
}

void TestMainWindow::par2_dialog_defaults_overrides_and_cancel()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    // This executable accepts --help on all test platforms; no PAR2 generation is needed here.
    const auto conf = QString("GROUPS = alt.binaries.test\nPAR2_PCT = 8\nPAR2_TOOL = par2cmdline\nPAR2_PATH = %1\nPAR2_ARGS = c -r8 -s65536 -l\n")
                          .arg(QCoreApplication::applicationFilePath());
    auto *window = bootWindow(ngPost, conf, &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(0));
    auto *second = window->addNewQuickTab(tabs->count() - 1);
    auto *firstPct = first->findChild<QSpinBox *>("redundancySB");
    auto *secondPct = second->findChild<QSpinBox *>("redundancySB");
    auto *autoPct = window->findChild<AutoPostWidget *>()->findChild<QSpinBox *>("redundancySB");
    QCOMPARE(autoPct->value(), -1);
    QCOMPARE(firstPct->value(), -1);
    firstPct->setValue(23);
    {
        Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
        QVERIFY(dialog.findChild<QLabel *>("par2Estimate")->text().contains("Prepare a post"));
        dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(49);
        dialog.reject();
    }
    QCOMPARE(ngPost.par2DefaultPercentage(), 8u);
    QFile sample(sandbox.rootPath() + "/preview.bin");
    QVERIFY(sample.open(QIODevice::WriteOnly)); sample.write(QByteArray(1048576, 'p')); sample.close();
    {
        Par2SettingsDialog dialog(&ngPost, {QFileInfo(sample)}, true, true, window);
        dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(15);
        QTRY_VERIFY(dialog.findChild<QLabel *>("par2Estimate")->text().contains("source blocks"));
        QVERIFY(dialog.findChild<QLabel *>("par2Estimate")->text().contains("override"));
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QCOMPARE(ngPost.par2DefaultPercentage(), 15u);
    QCOMPARE(firstPct->value(), 23);
    QCOMPARE(secondPct->value(), -1);
    QVERIFY(secondPct->text().contains("15"));
    QVERIFY(autoPct->text().contains("15"));
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto saved = config.readAll(); config.close();
    QVERIFY(saved.contains("PAR2_PCT = 15\n"));
    QVERIFY(saved.contains("PAR2_TOOL = par2cmdline\n"));
    {
        Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
        dialog.accept(); // no change must not rewrite arguments or configuration
    }
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(config.readAll(), saved);
    config.close();
    autoPct->setValue(31);
    {
        Par2SettingsDialog dialog(&ngPost, {QFileInfo(sandbox.rootPath() + "/missing")}, false, false, window);
        QTRY_VERIFY(dialog.findChild<QLabel *>("par2Estimate")->text().contains("could not be read"));
        dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(19);
        dialog.accept();
    }
    QCOMPARE(autoPct->value(), 31);
    QCOMPARE(firstPct->value(), 23);
    QVERIFY(secondPct->text().contains("19"));
}

void TestMainWindow::par2_dialog_preserves_exact_volume_bytes()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    const auto conf = QString("GROUPS = alt.binaries.test\nPAR2_PCT = 8\nPAR2_TOOL = parpar\n"
                              "PAR2_PATH = %1\nPAR2_ARGS = -r8% -s32B --max-input-slices=32B -p<32B\n")
                          .arg(QCoreApplication::applicationFilePath());
    auto *window = bootWindow(ngPost, conf, &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(12);
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(config.readAll().contains("PAR2_ARGS = -r12% -s32B --max-input-slices=32B -p<32B\n"));
}

void TestMainWindow::par2_dialog_detects_real_gpu()
{
    const auto deviceId = qEnvironmentVariable("NGPOST_TEST_OPENCL_DEVICE");
    if (deviceId.isEmpty()) QSKIP("Set NGPOST_TEST_OPENCL_DEVICE to test a real OpenCL GPU.");
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    const auto executable = par2::findExecutable(par2::Tool::ParPar);
    QVERIFY(!executable.isEmpty());
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\nPAR2_PATH = %1\n"
                                             "PAR2_PCT = 10\nPAR2_ARGS = -s1M --auto-slice-size -r10%\n").arg(executable), &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    dialog.findChild<QCheckBox *>("par2Gpu")->setChecked(true);
    auto *devices = dialog.findChild<QComboBox *>("par2GpuDevice");
    dialog.findChild<QPushButton *>("par2FindGpus")->click();
    QTRY_VERIFY_WITH_TIMEOUT(devices->findData(deviceId) >= 0, 10000);
    devices->setCurrentIndex(devices->findData(deviceId));
    QVERIFY(!devices->currentText().isEmpty());
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto saved = config.readAll();
    QVERIFY(saved.contains("--opencl-process=100%"));
    QVERIFY(saved.contains("--opencl-device " + deviceId.toUtf8()));
}

void TestMainWindow::par2_dialog_refuses_gpu_without_opencl()
{
#ifdef Q_OS_WIN
    QSKIP("The stub tool below is a POSIX shell script.");
#else
    HomeSandbox sandbox;
    QTemporaryDir stubDir;
    QVERIFY(stubDir.isValid());
    // ParPar on a machine without any OpenCL driver: "--opencl-list" succeeds
    // and lists nothing, and a real run would then stop with "Unable to obtain
    // OpenCL device info" after writing no par2 at all, aborting the post.
    const QString stub = stubDir.filePath(QStringLiteral("parpar"));
    QFile file(stub);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("#!/bin/sh\n"
               "case \"$1\" in\n"
               "--opencl-list) echo '{\"type\":\"opencl_list\",\"platforms\":[]}' ;;\n"
               "*) echo 'ParPar v0.0.0-stub' ;;\n"
               "esac\n");
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\nPAR2_PATH = %1\n"
                                             "PAR2_PCT = 10\nPAR2_ARGS = -s1M --auto-slice-size -r10%\n").arg(stub), &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
    auto *status = dialog.findChild<QLabel *>("par2Status");
    QVERIFY(save->isEnabled());
    dialog.findChild<QCheckBox *>("par2Gpu")->setChecked(true);
    // The listing is asynchronous: Save has to close once the answer arrives.
    QTRY_VERIFY_WITH_TIMEOUT(!save->isEnabled(), 10000);
    QVERIFY(status->text().contains(QStringLiteral("OpenCL")));
    dialog.accept();
    QCOMPARE(dialog.result(), 0); // still open, and nothing written
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(!config.readAll().contains("--opencl-process"));
    // The choice is the user's to undo, and Save comes back with it.
    dialog.findChild<QCheckBox *>("par2Gpu")->setChecked(false);
    QVERIFY(save->isEnabled());
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
#endif
}

static QString openClHelper(const QString &directory, const QString &mode)
{
    auto path = directory + "/ngpost-opencl-" + mode;
#ifdef Q_OS_WIN
    path += ".exe";
#endif
    return QFile::copy(QCoreApplication::applicationFilePath(), path) ? path : QString();
}

void TestMainWindow::par2_dialog_checks_opencl_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("usable");
    for (const auto *mode : {"none", "failed", "invalid", "offline", "slow-cpu"})
        QTest::newRow(mode) << QString::fromLatin1(mode) << (QString::fromLatin1(mode) == "slow-cpu");
}

void TestMainWindow::par2_dialog_checks_opencl()
{
    QFETCH(QString, mode);
    QFETCH(bool, usable);
    HomeSandbox sandbox;
    const auto stub = openClHelper(sandbox.rootPath(), mode);
    QVERIFY(!stub.isEmpty());
    int argc = 1; QByteArray arg0("tst_MainWindow"); char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\nPAR2_PATH = %1\n"
                                             "PAR2_PCT = 10\nPAR2_ARGS = -s1M --auto-slice-size -r10% --opencl-process=100%\n").arg(stub), &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
    auto *devices = dialog.findChild<QComboBox *>("par2GpuDevice");
    QVERIFY(!save->isEnabled()); // the asynchronous probe must finish first
    dialog.accept();
    QCOMPARE(dialog.result(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!devices->toolTip().isEmpty(), 10000);
    QCOMPARE(save->isEnabled(), usable);
    if (usable) {
        QVERIFY(devices->findData("0:0") >= 0); // CPU OpenCL is a usable choice
        devices->setCurrentIndex(devices->findData("0:0"));
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QFile config(PathHelper::configFilePath());
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(config.readAll().contains("--opencl-device 0:0"));
    } else {
        dialog.accept();
        QCOMPARE(dialog.result(), 0);
        dialog.findChild<QCheckBox *>("par2Gpu")->setChecked(false);
        QVERIFY(save->isEnabled());
    }
}

void TestMainWindow::par2_dialog_rechecks_changed_opencl_tool()
{
    HomeSandbox sandbox;
    const auto failed = openClHelper(sandbox.rootPath(), "failed");
    const auto cpu = openClHelper(sandbox.rootPath(), "slow-cpu");
    QVERIFY(!failed.isEmpty() && !cpu.isEmpty());
    int argc = 1; QByteArray arg0("tst_MainWindow"); char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\nPAR2_PATH = %1\n"
                                             "PAR2_ARGS = -s1M --auto-slice-size --opencl-process=100%\n").arg(failed), &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    auto *path = dialog.findChild<QLineEdit *>("par2Path");
    auto *devices = dialog.findChild<QComboBox *>("par2GpuDevice");
    auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
    QTRY_VERIFY_WITH_TIMEOUT(!devices->toolTip().isEmpty(), 10000);
    QVERIFY(!save->isEnabled());
    path->setText(cpu);
    QVERIFY(devices->toolTip().isEmpty());
    QVERIFY(!save->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 10000);
    QVERIFY(devices->findData("0:0") >= 0);
    path->setText(failed);
    QVERIFY(devices->findData("0:0") < 0); // previous executable's list was discarded
    QVERIFY(!save->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!devices->toolTip().isEmpty(), 10000);
    QVERIFY(!save->isEnabled());
    // An obsolete slow result must not override the new executable's failure.
    path->setText(cpu);
    QTest::qWait(100);
    path->setText(failed);
    QTRY_VERIFY_WITH_TIMEOUT(!devices->toolTip().isEmpty(), 10000);
    QTest::qWait(500);
    QVERIFY(!save->isEnabled());
    QVERIFY(devices->findData("0:0") < 0);
}

void TestMainWindow::par2_dialog_runs_a_typed_path_only_once_typing_ends()
{
#ifdef Q_OS_WIN
    QSKIP("The stub tool below is a POSIX shell script.");
#else
    HomeSandbox sandbox;
    const QString calls = sandbox.rootPath() + "/calls.log";
    const QString prefix = sandbox.rootPath() + "/par2";
    const QString typed = prefix + "-stub";
    for (const QString &stub : { prefix, typed }) {
        QFile file(stub);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(QString("#!/bin/sh\necho \"$0\" >> '%1'\necho 'par2cmdline stub'\n")
                       .arg(calls)
                       .toUtf8());
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              QStringLiteral(
                                  "GROUPS = alt.binaries.test\nPAR2_TOOL = par2cmdline\n"),
                              &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    auto *path = dialog.findChild<QLineEdit *>("par2Path");
    dialog.findChild<QComboBox *>("par2PathMode")->setCurrentIndex(1);
    QVERIFY(path->text().isEmpty());
    for (const QChar c : typed)
        path->insert(QString(c));
    QCOMPARE(path->text(), typed);
    QTest::qWait(500);
    QVERIFY2(!QFile::exists(calls), "a path was run while it was still being typed");

    emit path->editingFinished();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(calls), 5000);
    QFile log(calls);
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(log.readAll()), typed + "\n");
#endif
}

void TestMainWindow::par2_dialog_multipar_clears_inexact_check_hint()
{
    HomeSandbox sandbox;
    int argc = 1; QByteArray arg0("tst_MainWindow"); char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nPAR2_TOOL = multipar\nPAR2_PATH = %1\n"
                                             "PAR2_PCT = 10\nPAR2_ARGS = c /ss1048576 /rr10\nPAR2_BLOCK_SIZE = 1048576\n")
                                             .arg(QCoreApplication::applicationFilePath()), &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
    dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(12);
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const auto saved = config.readAll();
    QVERIFY(!QRegularExpression("(?m)^PAR2_BLOCK_SIZE\\s*=\\s*1048576").match(QString::fromUtf8(saved)).hasMatch());
    QVERIFY(saved.contains("/ss1048576"));
}

void TestMainWindow::sizing_dialogs_translations_fit_data()
{
    QTest::addColumn<QString>("language");
    for (const auto *language : {"en", "fr", "de", "es", "nl", "pt", "zh"})
        QTest::newRow(language) << QString::fromLatin1(language);
}

void TestMainWindow::sizing_dialogs_translations_fit()
{
    QFETCH(QString, language);
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QString("GROUPS = alt.binaries.test\nRAR_SIZE = 250\nRAR_MAX = 99\n"
                                             "PAR2_TOOL = par2cmdline\nPAR2_PATH = %1\nPAR2_PCT = 10\n")
                                         .arg(QCoreApplication::applicationFilePath()), &error);
    QVERIFY2(window, qPrintable(error));
    QTranslator translator;
    QVERIFY(translator.load(":/lang/ngPost_" + language + ".qm"));
    qApp->installTranslator(&translator);
    auto inspect = [&](QDialog &dialog, const QString &name) {
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        if (name.startsWith("par2")) {
            const auto available = dialog.screen()->availableGeometry().size();
            QTRY_VERIFY(dialog.frameGeometry().height() <= available.height());
            QTRY_VERIFY(dialog.frameGeometry().width() <= available.width());
            for (auto *field : dialog.findChildren<QWidget *>()) {
                if (!field->isVisible()) continue;
                // Embedded editors use their parent's frame and deliberately
                // have less height than a standalone QLineEdit's size hint.
                if (qobject_cast<QAbstractSpinBox *>(field->parentWidget())
                    || qobject_cast<QComboBox *>(field->parentWidget())) continue;
                if (qobject_cast<QComboBox *>(field) || qobject_cast<QAbstractSpinBox *>(field)
                    || qobject_cast<QLineEdit *>(field) || qobject_cast<QPlainTextEdit *>(field)) {
                    QTRY_VERIFY2(field->height() >= field->minimumSizeHint().height(), qPrintable(field->objectName()));
                    auto *scroll = dialog.findChild<QScrollArea *>("par2SettingsScroll");
                    if (scroll && scroll->widget()->isAncestorOf(field))
                        QTRY_VERIFY2(field->mapTo(scroll->widget(), QPoint()).x() + field->width()
                                     <= scroll->widget()->width(), qPrintable(field->objectName()));
                }
            }
        }
        for (auto *label : dialog.findChildren<QLabel *>()) {
            if (!label->wordWrap() || !label->isVisible()) continue;
            QTRY_VERIFY2(label->height() >= label->heightForWidth(label->width()), qPrintable(label->text()));
        }
        for (auto *box : dialog.findChildren<QDialogButtonBox *>())
            for (auto *button : box->buttons()) {
                QVERIFY(button->isVisible());
                QVERIFY(dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint()), button->size())));
            }
        const auto directory = qEnvironmentVariable("NGPOST_TEST_SCREENSHOT_DIR");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(dialog.grab().save(directory + '/' + name + '-' + language + ".png"));
        }
    };
    CompressionSettingsDialog rarDialog(&ngPost, window);
    inspect(rarDialog, "rar");
    rarDialog.reject();
    Par2SettingsDialog parDialog(&ngPost, {}, false, false, window);
    inspect(parDialog, "par2");
    parDialog.findChild<QToolButton *>()->setChecked(true);
    inspect(parDialog, "par2-advanced");
    auto *scroll = parDialog.findChild<QScrollArea *>("par2SettingsScroll");
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    inspect(parDialog, "par2-advanced-bottom");
    // Exercise the GPU rows too: par2cmdline hides them. They must fit in the
    // basic form, including the editable device selector in every translation.
    parDialog.findChild<QToolButton *>()->setChecked(false);
    auto *tool = parDialog.findChild<QComboBox *>("par2Tool");
    tool->setCurrentIndex(tool->findData(int(par2::Tool::ParPar)));
    parDialog.findChild<QCheckBox *>("par2Gpu")->setChecked(true);
    scroll->ensureWidgetVisible(parDialog.findChild<QPushButton *>("par2FindGpus"));
    inspect(parDialog, "par2-parpar-gpu");
    tool->setCurrentIndex(tool->findData(int(par2::Tool::MultiPar)));
    scroll->ensureWidgetVisible(parDialog.findChild<QCheckBox *>("par2Gpu"));
    inspect(parDialog, "par2-multipar-gpu");
    parDialog.reject();
}

void TestMainWindow::rar_limit_value_persists_and_zero_is_rejected()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    {
        NgPost ngPost(argc, argv);
        QString err;
        auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\nRAR_SIZE = 250\nRAR_MAX = 99\n", &err);
        QVERIFY2(window, qPrintable(err));
        CompressionSettingsDialog dialog(&ngPost, window);
        auto *maximum = dialog.findChild<QSpinBox *>("rarMaxSB");
        auto *enabled = dialog.findChild<QCheckBox *>("rarMaxCB");
        QVERIFY(maximum && enabled);
        QCOMPARE(maximum->minimum(), 1);
        maximum->setValue(42);
        QVERIFY(dialog.findChild<QLabel *>("volumeHelpLabel")->text().contains("priority"));
        dialog.show();
        auto *help = dialog.findChild<QLabel *>("volumeHelpLabel");
        QTRY_VERIFY(help->height() >= help->heightForWidth(help->width()));
        dialog.accept();
        CompressionSettingsDialog again(&ngPost, window);
        QCOMPARE(again.findChild<QSpinBox *>("rarMaxSB")->value(), 42);
        again.findChild<QCheckBox *>("rarMaxCB")->setChecked(false);
        QVERIFY(!again.findChild<QSpinBox *>("rarMaxSB")->isEnabled());
        again.accept();
        QFile file(PathHelper::configFilePath());
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(file.readAll().contains("#RAR_MAX = 42\n"));
    }
    {
        NgPost restarted(argc, argv);
        QVERIFY(restarted.parseDefaultConfig().isEmpty());
        CompressionSettingsDialog dialog(&restarted);
        QCOMPARE(dialog.findChild<QSpinBox *>("rarMaxSB")->value(), 42);
        QVERIFY(!dialog.findChild<QCheckBox *>("rarMaxCB")->isChecked());
    }
    QFile file(PathHelper::configFilePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("RAR_MAX = 0\n"); file.close();
    NgPost invalid(argc, argv);
    QVERIFY(invalid.parseDefaultConfig().contains("RAR_MAX"));
}

void TestMainWindow::queued_post_keeps_rar_and_par2_settings_data()
{
    QTest::addColumn<int>("requestedSize");
    QTest::addColumn<bool>("limited");
    QTest::addColumn<int>("maximum");
    QTest::addColumn<qint64>("sourceSize");
    QTest::addColumn<QString>("volumeArgument");
    QTest::addColumn<int>("increasedSize");
    QTest::newRow("fixed250") << 250 << false << 99 << 12LL * 1048576 << QString("-v250m") << 0;
    QTest::newRow("under-limit") << 250 << true << 99 << 12LL * 1048576 << QString("-v250m") << 0;
    QTest::newRow("increased") << 1 << true << 2 << 12LL * 1048576 << QString("-v7m") << 7;
    QTest::newRow("unsplit") << 0 << false << 2 << 12LL * 1048576 << QString() << 0;
    QTest::newRow("automatic") << 0 << true << 2 << 12LL * 1048576 << QString("-v7m") << 7;
    QTest::newRow("rounding-below") << 1 << true << 2 << 3LL * 1048576 - 1 << QString("-v1m") << 0;
    QTest::newRow("rounding-above") << 1 << true << 2 << 3LL * 1048576 << QString("-v2m") << 2;
}

void TestMainWindow::queued_post_keeps_rar_and_par2_settings()
{
    QFETCH(int, requestedSize);
    QFETCH(bool, limited);
    QFETCH(int, maximum);
    QFETCH(qint64, sourceSize);
    QFETCH(QString, volumeArgument);
    QFETCH(int, increasedSize);
    HomeSandbox sandbox;
    const auto root = sandbox.rootPath();
    auto helper = [&](const QString &name) {
        auto path = root + "/ngpost-recording-" + name;
#ifdef Q_OS_WIN
        path += ".exe";
#endif
        // A native executable exercises QProcess quoting and Unicode paths on
        // Windows as well as Unix. Its small recording mode lives in main().
        if (!QFile::copy(QCoreApplication::applicationFilePath(), path)) return QString();
        return path;
    };
    const auto rar = helper(QString::fromUtf8("archive helper é"));
    const auto par = helper(QString::fromUtf8("recovery helper é"));
    QVERIFY(!rar.isEmpty() && !par.isEmpty());
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "15"}));
    int argc = 1; QByteArray arg0("tst_MainWindow"); char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    const auto conf = QString("GROUPS = alt.binaries.test\nthread = 1\nTMP_DIR = %1\nnzbPath = %1\n"
                              "RAR_PATH = %2\nRAR_SIZE = %5\n%6RAR_MAX = %7\nPAR2_PATH = %3\nPAR2_TOOL = par2cmdline\n"
                              "PAR2_PCT = 8\nPAR2_ARGS = c -r8 -s4096\n[server]\nhost = 127.0.0.1\nport = %4\nssl = false\nconnection = 1\n")
                          .arg(root, rar, par).arg(mock.port()).arg(requestedSize).arg(limited ? "" : "#").arg(maximum);
    auto *window = bootWindow(ngPost, conf, &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(0));
    auto *second = window->addNewQuickTab(tabs->count() - 1);
    QFile firstFile(root + "/first.bin"), secondFile(root + "/second.bin");
    QVERIFY(firstFile.open(QIODevice::WriteOnly)); firstFile.write(QByteArray(500000, 'a')); firstFile.close();
    QVERIFY(secondFile.open(QIODevice::WriteOnly)); QVERIFY(secondFile.resize(sourceSize)); secondFile.close();
    first->addPath(firstFile.fileName(), 0);
    second->addPath(secondFile.fileName(), 0);
    second->findChild<QCheckBox *>("compressCB")->setChecked(true);
    second->findChild<QCheckBox *>("par2CB")->setChecked(true);
    second->setPar2PercentageOverride(17);
    window->findChild<QPushButton *>("postAllTabsButton")->click();
    QVERIFY(first->isPosting() && second->isPosting());
    QVERIFY(!QFileInfo::exists(rar + ".args")); // the second job is still waiting
    {
        CompressionSettingsDialog dialog(&ngPost, window);
        dialog.findChild<QSpinBox *>("rarMaxSB")->setValue(20);
        dialog.accept();
        Par2SettingsDialog parDialog(&ngPost, {}, false, false, window);
        auto *tool = parDialog.findChild<QComboBox *>("par2Tool");
        tool->setCurrentIndex(tool->findData(int(par2::Tool::ParPar)));
        parDialog.findChild<QLineEdit *>("par2Path")->setText(QCoreApplication::applicationFilePath());
        parDialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(50);
        parDialog.accept();
        QCOMPARE(parDialog.result(), int(QDialog::Accepted));
    }
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished() && second->isPostingFinished(), 20000);
    QFile rarArgs(rar + ".args"), parArgs(par + ".args");
    QVERIFY(rarArgs.open(QIODevice::ReadOnly));
    const auto recordedRar = QString::fromUtf8(rarArgs.readAll()).split('\n').filter(QRegularExpression("^-v"));
    QCOMPARE(recordedRar, volumeArgument.isEmpty() ? QStringList{} : QStringList{volumeArgument});
    QVERIFY(parArgs.open(QIODevice::ReadOnly));
    const auto recorded = parArgs.readAll().split('\n');
    QVERIFY(recorded.contains("-r17"));
    QVERIFY(recorded.contains("-s4096"));
    QVERIFY(!recorded.contains("-r50%"));
    QCOMPARE(mock.receivedArticles().size(), 3);
    bool adjustmentLogged = false;
    for (auto *log : window->findChildren<QTextBrowser *>())
        adjustmentLogged |= log->toPlainText().contains(QString("increased from %1 MiB to %2 MiB").arg(requestedSize).arg(increasedSize));
    QCOMPARE(adjustmentLogged, increasedSize > 0);
}

void TestMainWindow::post_all_continues_after_overwrite_declined_and_auto_close()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1; QByteArray arg0("tst_MainWindow"); char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    const auto conf = QString("GROUPS = alt.binaries.test\nthread = 1\nAUTO_CLOSE_TABS = true\nnzbPath = %1\n"
                              "[server]\nhost = 127.0.0.1\nport = %2\nssl = false\nconnection = 1\n")
                          .arg(sandbox.rootPath()).arg(mock.port());
    auto *window = bootWindow(ngPost, conf, &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(0));
    QPointer<PostingWidget> second = window->addNewQuickTab(tabs->count() - 1);
    for (auto *post : {first, second.data()}) {
        const QString path = sandbox.rootPath() + (post == first ? "/existing.bin" : "/fresh.bin");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("sample"); file.close();
        post->addPath(path, 0);
    }
    const auto nzbPath = first->findChild<QLineEdit *>("nzbFileEdit")->text();
    QFile existing(nzbPath.endsWith(".nzb") ? nzbPath : nzbPath + ".nzb");
    QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("original nzb"); existing.close();
    bool declined = false;
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *question = qobject_cast<QMessageBox *>(widget)) {
                declined = true;
                question->done(QMessageBox::No);
            }
    });
    dismiss.start(10);
    window->findChild<QPushButton *>("postAllTabsButton")->click();
    dismiss.stop();
    QVERIFY(declined);
    QVERIFY(first->canSubmit());
    QTRY_VERIFY_WITH_TIMEOUT(second.isNull(), 15000);
    QCOMPARE(mock.receivedArticles().size(), 1);
    QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("original nzb"));
    QVERIFY(!window->findChild<QPushButton *>("postAllTabsButton")->isEnabled());
}

void TestMainWindow::legacy_bundle_paths_become_automatic()
{
#ifndef Q_OS_LINUX
    QSKIP("AppImage migration is Linux-specific");
#else
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              QStringLiteral(
                                  "GROUPS = alt.binaries.test\nPAR2_TOOL = auto\nPAR2_PCT = 10\n"
                                  "PAR2_ARGS = -s1M --auto-slice-size -r1n*0.6 -q\n"
                                  "PAR2_PATH = /tmp/.mount_ngpostOLD/usr/bin/parpar\n"
                                  "RAR_PATH = /tmp/.mount_ngpostOLD/usr/bin/rar\n"),
                              &error);
    QVERIFY2(window, qPrintable(error));
    Par2SettingsDialog parity(&ngPost, {}, false, false, window);
    QCOMPARE(parity.findChild<QComboBox *>("par2PathMode")->currentData().toInt(), 0);
    QCOMPARE(parity.findChild<QComboBox *>("par2Tool")->currentData().toInt(),
             int(par2::Tool::ParPar));
    CompressionSettingsDialog compression(&ngPost, window);
    QCOMPARE(compression.findChild<QComboBox *>("rarPathMode")->currentData().toInt(), 0);
    ngPost.saveConfig();
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const auto saved = config.readAll();
    QVERIFY(saved.contains("PAR2_SOURCE = auto\n"));
    QVERIFY(saved.contains("PAR2_TOOL = parpar\n"));
    QVERIFY(saved.contains("RAR_SOURCE = auto\n"));
    QVERIFY(!saved.contains(".mount_ngpostOLD"));
    QVERIFY(
        !QRegularExpression("(?m)^(PAR2|RAR)_PATH =").match(QString::fromUtf8(saved)).hasMatch());
    const auto log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY(!log.contains("not an executable"));
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    Par2SettingsDialog reloaded(&ngPost, {}, false, false, window);
    QCOMPARE(reloaded.findChild<QComboBox *>("par2PathMode")->currentData().toInt(), 0);
#endif
}

void TestMainWindow::tool_paths_keep_custom_choices_and_report_missing_tools()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    const QString custom = QCoreApplication::applicationFilePath();
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_TOOL = "
                                      "par2cmdline\nPAR2_PCT = 8\nPAR2_PATH = %1\nRAR_PATH = %1\n")
                                  .arg(custom),
                              &error);
    QVERIFY2(window, qPrintable(error));
    {
        Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
        auto *mode = dialog.findChild<QComboBox *>("par2PathMode");
        QCOMPARE(mode->currentData().toInt(), 1);
        QCOMPARE(dialog.findChild<QLineEdit *>("par2Path")->text(), custom);
        dialog.findChild<QSpinBox *>("par2DefaultPct")->setValue(9);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    auto saved = config.readAll();
    config.close();
    QVERIFY(saved.contains("PAR2_SOURCE = custom\n"));
    QVERIFY(saved.contains(("PAR2_PATH = " + custom + "\n").toUtf8()));
    {
        Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
        dialog.findChild<QLineEdit *>("par2Path")->setText(sandbox.rootPath() + "/missing-parpar");
        QVERIFY(
            !dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("par2PathStatus")->text().contains("unavailable"));
        // An explicit engine choice must not persist an automatically resolved path.
        auto *tool = dialog.findChild<QComboBox *>("par2Tool");
        tool->setCurrentIndex(tool->findData(int(par2::Tool::ParPar)));
        QCOMPARE(dialog.findChild<QComboBox *>("par2PathMode")->currentData().toInt(), 0);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QVERIFY(config.open(QIODevice::ReadOnly));
    saved = config.readAll();
    config.close();
    QVERIFY(saved.contains("PAR2_SOURCE = auto\n"));
    QVERIFY(!QRegularExpression("(?m)^PAR2_PATH =").match(QString::fromUtf8(saved)).hasMatch());
    {
        CompressionSettingsDialog dialog(&ngPost, window);
        QCOMPARE(dialog.findChild<QComboBox *>("rarPathMode")->currentData().toInt(), 1);
        dialog.findChild<QLineEdit *>("rarEdit")->setText(sandbox.rootPath() + "/missing-rar");
        QVERIFY(
            !dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
        dialog.findChild<QComboBox *>("rarPathMode")->setCurrentIndex(0);
        QVERIFY(
            dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QVERIFY(config.open(QIODevice::ReadOnly));
    saved = config.readAll();
    QVERIFY(saved.contains("RAR_SOURCE = auto\n"));
    QVERIFY(!QRegularExpression("(?m)^RAR_PATH =").match(QString::fromUtf8(saved)).hasMatch());
}

void TestMainWindow::log_timestamps_cover_debug_errors_and_fragments()
{
    MainWindow window;
    window.log("Normal entry");
    window.log("[Poster #1] debug details\nsecond debug line");
    window.log("process fragment", false);
    window.log(" completed\r", false);
    window.log("\nnext process line", false);
    window.logError("failure\nreason");
    window.log("[12:34:56.789] existing timestamp");
    const QString text = window.findChild<QTextBrowser *>("logBrowser")->toPlainText();
    const auto lines = text.split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 8);
    const QRegularExpression stamp("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] ");
    for (const auto &line : lines)
        QVERIFY2(stamp.match(line).hasMatch(), qPrintable(line));
    QVERIFY(text.contains("process fragment completed\n"));
    QVERIFY(text.contains("[12:34:56.789] existing timestamp"));
    QVERIFY(!text.contains("] [12:34:56.789]"));
}

void TestMainWindow::log_file_keeps_timestamped_debug_fragments()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              QStringLiteral("GROUPS = alt.binaries.test\nLOG_IN_FILE = true\n"),
                              &error);
    QVERIFY2(window, qPrintable(error));
    emit ngPost.log("first debug fragment", false);
    emit ngPost.log(" completed\r", false);
    emit ngPost.log("\nsecond debug line", false);
    emit ngPost.log("\n3%\r4%\r", false);
    emit ngPost.log("normal entry", true);
    emit ngPost.error("test error");
    QCoreApplication::sendPostedEvents(&ngPost, QEvent::MetaCall);
    QFile log(PathHelper::configDir() + "/ngPost.log");
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(log.readAll());
    const QRegularExpression stamp("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] ");
    QVERIFY2(text.contains("first debug fragment completed\n["), qPrintable(text));
    QVERIFY2(text.contains("second debug line\n["), qPrintable(text));
    // Progress rewrites its line in the file, as on a terminal.
    QVERIFY2(text.contains("3%\r[") && text.contains("4%\n["), qPrintable(text));
    QVERIFY(text.contains("ERR: test error"));
    for (const auto &line : text.split('\n', Qt::SkipEmptyParts))
        QVERIFY2(stamp.match(line).hasMatch(), qPrintable(line));
    const QString pane = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(pane.contains("first debug fragment completed\n["), qPrintable(pane));
    QVERIFY(!QRegularExpression("\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] \\[\\d{2}:")
                 .match(pane)
                 .hasMatch());
}

namespace
{
//! An empty file Qt reports as executable, named as given (.exe added on Windows).
QString makeFakeExecutable(const QString &dir, const QString &name)
{
    QDir().mkpath(dir);
    QString path = QDir(dir).filePath(name);
#ifdef Q_OS_WIN
    path += QStringLiteral(".exe");
#endif
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}
} // namespace

void TestMainWindow::legacy_missing_tool_paths_fall_back_with_a_warning_data()
{
    QTest::addColumn<bool>("used");
    QTest::newRow("unused") << false;
    QTest::newRow("used") << true;
}

void TestMainWindow::legacy_missing_tool_paths_fall_back_with_a_warning()
{
    QFETCH(bool, used);
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString par2Path = sandbox.rootPath() + "/uninstalled/parpar";
    const QString rarPath = sandbox.rootPath() + "/uninstalled/7z";
    QString error;
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_PATH = %1\n"
                                      "PAR2_ARGS = -s1M --auto-slice-size -r1n*0.6 -q\n"
                                      "RAR_PATH = %2\n%3")
                                  .arg(par2Path,
                                       rarPath,
                                       used ? "PACK = COMPRESS, GEN_PAR2\n" : ""),
                              &error);
    QVERIFY2(window, qPrintable(error));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("PAR2_PATH = " + par2Path + " is not an executable file") == used,
             qPrintable(log));
    QVERIFY2(log.contains("RAR_PATH = " + rarPath + " is not an executable file") == used,
             qPrintable(log));

    // The file name still tells which engine PAR2_ARGS and the archive switches were written
    // for: those arguments keep ParPar even on a machine where it is not installed.
    Par2SettingsDialog parity(&ngPost, {}, false, false, window);
    QCOMPARE(parity.findChild<QComboBox *>("par2PathMode")->currentData().toInt(), 0);
    QCOMPARE(parity.findChild<QComboBox *>("par2Tool")->currentData().toInt(),
             int(par2::Tool::ParPar));
    CompressionSettingsDialog compression(&ngPost, window);
    QCOMPARE(compression.findChild<QComboBox *>("rarPathMode")->currentData().toInt(), 0);
    QCOMPARE(compression.findChild<QComboBox *>("rarTool")->currentData().toString(),
             QString("7zip"));

    ngPost.saveConfig();
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(config.readAll());
    QVERIFY(saved.contains("PAR2_TOOL = parpar\n"));
    QVERIFY(saved.contains("RAR_TOOL = 7zip\n"));
    QVERIFY(saved.contains("PAR2_PATH = " + par2Path + "\n"));
    QVERIFY(saved.contains("RAR_PATH = " + rarPath + "\n"));
    QVERIFY(!QRegularExpression("(?m)^(PAR2|RAR)_SOURCE =").match(saved).hasMatch());
    config.close();
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    ngPost.saveConfig();
    QVERIFY(config.open(QIODevice::ReadOnly));
    const auto reloaded = config.readAll();
    QVERIFY(reloaded.contains(("PAR2_PATH = " + par2Path + "\n").toUtf8()));
    QVERIFY(reloaded.contains(("RAR_PATH = " + rarPath + "\n").toUtf8()));
}

void TestMainWindow::legacy_parity_path_adopts_its_engine_only_when_usable_data()
{
    QTest::addColumn<bool>("bundled");
    QTest::addColumn<bool>("installed");
    QTest::newRow("vanished path, ParPar installed") << false << true;
    QTest::newRow("vanished path, ParPar missing") << false << false;
    QTest::newRow("bundled path, ParPar installed") << true << true;
    QTest::newRow("bundled path, ParPar missing") << true << false;
}

void TestMainWindow::legacy_parity_path_adopts_its_engine_only_when_usable()
{
#ifndef Q_OS_UNIX
    QSKIP("On Windows, automatic discovery also searches Program Files, beyond the test's PATH");
#else
    QFETCH(bool, bundled);
    QFETCH(bool, installed);
    HomeSandbox sandbox;
    const QString bin = sandbox.rootPath() + "/bin";
    QVERIFY(QDir().mkpath(bin));
    if (installed)
        QVERIFY(!makeFakeExecutable(bin, "parpar").isEmpty());
    // Only this folder is searched, whatever the machine running the test has installed.
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", bin.toLocal8Bit());
    const auto restorePath = qScopeGuard([&savedPath] { qputenv("PATH", savedPath); });

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    // A bundled path names the test binary's folder, which holds no ParPar: the
    // bundle a Windows installation without its optional ParPar leaves behind.
    const QString par2Path = bundled ? QCoreApplication::applicationDirPath() + "/parpar"
                                     : sandbox.rootPath() + "/uninstalled/parpar";
    QVERIFY(!QFile::exists(par2Path));
    QString error;
    auto *window = bootWindow(
        ngPost,
        QString("GROUPS = alt.binaries.test\nPAR2_PCT = 10\nPAR2_PATH = %1\n").arg(par2Path),
        &error);
    QVERIFY2(window, qPrintable(error));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    const QString unusable = "PAR2_PATH = " + par2Path + " is not an executable file";
    if (bundled) {
        // A bundle path is a migration, not a mistake of the user's: said nowhere.
        QVERIFY2(!log.contains(unusable), qPrintable(log));
    } else {
        const QString outcome = installed ? "; ngPost uses " + bin + "/parpar"
                                          : QString(". Posts that need this tool will stop");
        QVERIFY2(log.contains(unusable + outcome), qPrintable(log));
    }

    ngPost.saveConfig();
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(config.readAll());
    QVERIFY2(saved.contains(installed ? "PAR2_TOOL = parpar\n" : "PAR2_TOOL = auto\n"),
             qPrintable(saved));
    QCOMPARE(saved.contains("PAR2_SOURCE = auto\n"), bundled);
    QCOMPARE(QRegularExpression("(?m)^PAR2_PATH =").match(saved).hasMatch(), !bundled);
#endif
}

void TestMainWindow::tool_path_details_hide_with_an_unavailable_tool()
{
#ifndef Q_OS_UNIX
    QSKIP("On Windows, automatic discovery also searches Program Files, beyond the test's PATH");
#else
    HomeSandbox sandbox;
    const QString bin = sandbox.rootPath() + "/bin";
    QVERIFY(!makeFakeExecutable(bin, "parpar").isEmpty());
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", bin.toLocal8Bit());
    const auto restorePath = qScopeGuard([&savedPath] { qputenv("PATH", savedPath); });

    ExternalToolPathWidget widget(QStringLiteral("parpar"),
                                  externaltool::PathMode::Automatic,
                                  {},
                                  QStringLiteral("par2"),
                                  nullptr);
    auto *details = widget.findChild<QToolButton *>("par2PathDetails");
    auto *path = widget.findChild<QLineEdit *>("par2Path");
    QVERIFY(details && path);
    QVERIFY(!details->isHidden());
    QVERIFY(path->isHidden());
    details->setChecked(true);
    QVERIFY(!path->isHidden());

    // No par2 in PATH: nothing detected, nothing to show, and no button left to hide it with.
    widget.selectTool(QStringLiteral("par2cmdline"));
    QVERIFY(details->isHidden());
    QVERIFY(path->isHidden());

    widget.selectTool(QStringLiteral("parpar"));
    QVERIFY(!details->isHidden());
    QVERIFY(!path->isHidden());
#endif
}

void TestMainWindow::explicit_tool_lines_that_cannot_apply_are_reported()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString sevenZip = makeFakeExecutable(sandbox.rootPath() + "/bin", "7z");
    QVERIFY(!sevenZip.isEmpty());
    QString error;
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_SOURCE = auto\n"
                                      "PAR2_PATH = %1/par2\nRAR_TOOL = rar\nRAR_SOURCE = custom\n"
                                      "RAR_PATH = %2\n")
                                  .arg(sandbox.rootPath(), sevenZip),
                              &error);
    QVERIFY2(window, qPrintable(error));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("PAR2_PATH is ignored because PAR2_SOURCE = auto"), qPrintable(log));
    QVERIFY2(log.contains("RAR_TOOL = rar does not match RAR_PATH = " + sevenZip), qPrintable(log));

    CompressionSettingsDialog compression(&ngPost, window);
    QCOMPARE(compression.findChild<QComboBox *>("rarPathMode")->currentData().toInt(), 1);
    QCOMPARE(compression.findChild<QComboBox *>("rarTool")->currentData().toString(),
             QString("7zip"));
    QCOMPARE(compression.findChild<QLineEdit *>("rarEdit")->text(), sevenZip);

    ngPost.saveConfig();
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(config.readAll());
    QVERIFY(saved.contains("RAR_TOOL = 7zip\n"));
    QVERIFY(saved.contains("RAR_SOURCE = custom\n"));
    QVERIFY(saved.contains("RAR_PATH = " + sevenZip + "\n"));
    QVERIFY(!QRegularExpression("(?m)^PAR2_PATH =").match(saved).hasMatch());
}

void TestMainWindow::tool_lines_are_lenient_and_missing_tools_reported_only_when_used()
{
    for (const bool used : { false, true }) {
        HomeSandbox sandbox;
        int argc = 1;
        QByteArray arg0("tst_MainWindow");
        char *argv[] = { arg0.data(), nullptr };
        NgPost ngPost(argc, argv);
        const QString missing = sandbox.rootPath() + "/missing";
        QString error;
        auto *window = bootWindow(ngPost,
                                  QString("GROUPS = alt.binaries.test\nRAR_TOOL = 7Zip\n"
                                          "RAR_SOURCE = Custom\nRAR_PATH = %1/7z\n"
                                          "PAR2_TOOL = par3\nPAR2_SOURCE = sometimes\n"
                                          "PAR2_PATH = %1/par2\n%2")
                                      .arg(missing, used ? "PACK = COMPRESS, GEN_PAR2\n" : ""),
                                  &error);
        QVERIFY2(window, qPrintable(error));
        const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
        QVERIFY2(!log.contains("RAR_TOOL must be"), qPrintable(log));
        QVERIFY2(log.contains("PAR2_SOURCE must be auto or custom."), qPrintable(log));
        QVERIFY2(log.contains("PAR2_TOOL must be auto, parpar, par2cmdline or multipar."),
                 qPrintable(log));
        QVERIFY2(log.contains("RAR_PATH = " + missing + "/7z is not an executable file") == used,
                 qPrintable(log));
        QVERIFY2(log.contains("PAR2_PATH = " + missing + "/par2 is not an executable file") == used,
                 qPrintable(log));

        CompressionSettingsDialog compression(&ngPost, window);
        QCOMPARE(compression.findChild<QComboBox *>("rarTool")->currentData().toString(),
                 QString("7zip"));
    }
}

void TestMainWindow::custom_archiver_path_keeps_the_cursor_and_selects_the_engine()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, QStringLiteral("GROUPS = alt.binaries.test\n"), &error);
    QVERIFY2(window, qPrintable(error));
    CompressionSettingsDialog dialog(&ngPost, window);
    auto *path = dialog.findChild<QLineEdit *>("rarEdit");
    auto *mode = dialog.findChild<QComboBox *>("rarPathMode");
    auto *tool = dialog.findChild<QComboBox *>("rarTool");
    tool->setCurrentIndex(tool->findData(QString("rar")));

    path->setText(sandbox.rootPath() + "/archiver");
    QCOMPARE(mode->currentData().toInt(), 1);
    const int middle = sandbox.rootPath().size() + 1;
    path->setCursorPosition(middle);
    path->insert(QStringLiteral("x"));
    QCOMPARE(path->text(), sandbox.rootPath() + "/xarchiver");
    QCOMPARE(path->cursorPosition(), middle + 1);
    QVERIFY(path->isUndoAvailable());
    QCOMPARE(tool->currentData().toString(), QString("rar"));

    const QString sevenZip = sandbox.rootPath() + "/bin/7zz";
    path->setText(sevenZip);
    QCOMPARE(tool->currentData().toString(), QString("7zip"));
    QCOMPARE(mode->currentData().toInt(), 1);
    QCOMPARE(path->text(), sevenZip);

    // An engine picked by hand still starts over with automatic discovery.
    tool->setCurrentIndex(tool->findData(QString("rar")));
    QCOMPARE(mode->currentData().toInt(), 0);

    // Typing goes through prefixes: "/7z" of "/7z-tools/winrar-cli" must not
    // leave 7-Zip selected once the whole name says neither archiver.
    mode->setCurrentIndex(1);
    QVERIFY(path->text().isEmpty());
    const QString typed = sandbox.rootPath() + "/7z-tools/winrar-cli";
    bool sevenZipOnTheWay = false;
    for (const QChar c : typed) {
        path->insert(QString(c));
        sevenZipOnTheWay |= tool->currentData().toString() == QLatin1String("7zip");
    }
    QVERIFY(sevenZipOnTheWay);
    QCOMPARE(path->text(), typed);
    QCOMPARE(tool->currentData().toString(), QString("rar"));
}

void TestMainWindow::explicit_missing_parity_engine_is_reported()
{
#ifndef Q_OS_UNIX
    QSKIP("The test isolates automatic discovery using PATH");
#else
    HomeSandbox sandbox;
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", sandbox.rootPath().toLocal8Bit());
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", savedPath); });
    QVERIFY(!QFile::exists(QCoreApplication::applicationDirPath() + "/parpar"));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\nPAR2_PCT = 10\n",
                              &error);
    QVERIFY2(window, qPrintable(error));
    const auto log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("PAR2_TOOL = parpar: no executable was found"), qPrintable(log));
    QVERIFY(log.contains("PAR2_SOURCE = custom"));
#endif
}

void TestMainWindow::automatic_archiver_ignores_the_legacy_path()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(
        ngPost,
        "GROUPS = alt.binaries.test\nRAR_SOURCE = auto\nRAR_PATH = /ignored/7z\n",
        &error);
    QVERIFY2(window, qPrintable(error));
    CompressionSettingsDialog dialog(&ngPost, window);
    QCOMPARE(dialog.findChild<QComboBox *>("rarTool")->currentData().toString(), QString("rar"));
    QVERIFY(window->findChild<QTextBrowser *>("logBrowser")
                ->toPlainText()
                .contains("RAR_PATH is ignored because RAR_SOURCE = auto"));
}

void TestMainWindow::failed_compressor_reports_unrestored_sources()
{
#ifndef Q_OS_UNIX
    QSKIP("Uses an executable script with a missing interpreter");
#else
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    const QString source = sandbox.rootPath() + "/original.bin";
    QFile input(source);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("original contents");
    input.close();
    const auto compressor = makeFakeExecutable(sandbox.rootPath(), "rar");
    QFile script(compressor);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/ngpost-test/missing-interpreter\n");
    script.close();
    PostingJobOptions options;
    options.files = { QFileInfo(source) };
    options.inputPaths = { source };
    options.nzbFilePath = sandbox.rootPath() + "/test.nzb";
    options.tmpPath = sandbox.rootPath();
    options.rarPath = compressor;
    options.rarName = "archive";
    options.doCompress = true;
    options.obfuscateFileName = true;
    QSignalSpy errors(&ngPost, &NgPost::error);
    {
        PostingJob job(&ngPost, options);
        // This job belongs to the test, not to NgPost's managed queue.
        disconnect(&job, &PostingJob::postingFinished, &ngPost, nullptr);
        QSignalSpy finished(&job, &PostingJob::postingFinished);
        QVERIFY(QMetaObject::invokeMethod(&job,
                                          "onStartPosting",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, false)));
        QVERIFY(!QFile::exists(source));
        // Occupy the original name before the queued FailedToStart handler runs.
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("replacement");
        input.close();
        QTRY_COMPARE(finished.count(), 1);
        QString messages;
        for (const auto &row : errors)
            messages += row.first().toString() + '\n';
        QVERIFY2(messages.contains("Couldn't restore") && messages.contains(source),
                 qPrintable(messages));
        QVERIFY2(messages.contains("Some source files are still under their obfuscated name"),
                 qPrintable(messages));
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(input.readAll(), QByteArray("replacement"));
        input.close();
        QVERIFY(input.remove()); // destructor can now retry the restoration
    }
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCOMPARE(input.readAll(), QByteArray("original contents"));
#endif
}
