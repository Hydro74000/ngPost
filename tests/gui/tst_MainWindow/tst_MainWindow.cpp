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
#include <csignal>
#include <QTextBlock>
#include <QTextBrowser>
#include <QApplication>
#include <QCheckBox>
#include <QToolButton>

#include "hmi/CheckBoxCenterWidget.h"
#include "hmi/PostInfoDialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
#include "history/PostHistoryService.h"
#include "vpn/VpnManager.h"
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

    //! An engine that is not installed must not stop every post that needs par2:
    //! ngPost runs the one that is, without rewriting the configured choice.
    void parity_engine_falls_back_to_an_installed_one();

    //! Issue #15: a PAR2_ARGS_CUSTOM line is the user's, and neither a save nor
    //! the settings dialog may replace it. Commented out, it does not apply.
    void custom_parity_arguments_are_never_rewritten();

    //! A save writes what ngPost holds in memory: an edit made in the file
    //! meanwhile must be kept aside instead of vanishing.
    void an_external_edit_is_merged_into_the_next_save();
    void an_external_edit_ngPost_cannot_take_now_stays_in_the_file();
    //! The parser reads a setting wherever it stands, below the [server] blocks
    //! included -- where appending a line puts it. The merge must read it there too.
    void a_setting_appended_below_the_servers_is_merged();
    //! The tab in front writes the name length back on every save, so an edit
    //! of it waits for a restart; the password length is the dialog's default,
    //! which no tab writes, so an edit of it is taken. Neither may be reverted.
    void lengths_edited_in_the_file_are_not_reverted_by_the_tabs();
    //! A redundancy or arguments taken from the file show in the posting tabs,
    //! as they do when the PAR2 Settings window changes them.
    void parity_settings_taken_from_the_file_reach_the_posting_tabs();
    //! Saving the PAR2 Settings window while this run falls back to another
    //! engine must neither empty the path nor keep a fallback the user replaced.
    void saving_the_parity_dialog_keeps_the_engine_fallback_right();

    //! Servers and VPN profiles are written from ngPost's own state, never
    //! merged: a block edited by hand is discarded, and that must be said.
    void a_hand_edited_server_block_is_reported();

    //! A conflict names the line it drops, and the log panel ends up in bug
    //! reports: the value of a credential must never be one of those names.
    void a_conflict_on_a_password_does_not_print_it();
    //! RAR_EXTRA carries -hp<password> as easily as -m0: its conflicts are
    //! named, never printed, like the archiver command line SecretMasker hides.
    void a_conflict_on_archiver_switches_does_not_print_them();

    //! A commented example is meant to be uncommented as it is. One ending in
    //! "(for parpar)" hands that note to the tool, which fails on it.
    void commented_examples_survive_being_uncommented();

    //! Switches of one parity engine are refused by another: say it while the
    //! configuration is read, not hours later at the par2 step.
    void custom_parity_arguments_for_another_tool_are_reported();
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
    void global_cancel_preserves_history_and_resume_data();
    void global_cancel_preserves_history_and_resume();
    void global_post_controls_translations();
    void global_cancel_during_confirmation();
    void global_pause_holds_pending_and_new_posts_data();
    void global_pause_holds_pending_and_new_posts();
    void quick_post_numbers_icons_and_palette();
    void quick_post_numbering_lifecycle_and_reset();
    void quick_post_numbering_with_backend_queue_data();
    void quick_post_numbering_with_backend_queue();
    void quick_post_numbering_from_new_and_auto_tabs();
    void auto_posts_can_retry_preparation_failures_data();
    void auto_posts_can_retry_preparation_failures();
    void preparation_retry_restores_sources_after_packing();
    void posting_controls_do_not_overlap_tab_scrollers();
    void progress_label_tracks_the_running_post_data();
    void progress_label_tracks_the_running_post();
    void global_cancel_external_tool_data();
    void global_cancel_external_tool();
    void canceled_job_never_starts();
    void global_post_controls_pause_resume_and_cancel_data();
    void global_post_controls_pause_resume_and_cancel();
    void shutdown_waits_for_every_post_data();
    void shutdown_waits_for_every_post();
    void shutdown_requires_new_completed_post_data();
    void shutdown_requires_new_completed_post();
    void shutdown_waits_during_vpn_confirmation_data();
    void shutdown_waits_during_vpn_confirmation();
    void shutdown_waits_for_posts_added_after_completion();
    void shutdown_requires_an_actual_transfer();
    void shutdown_waits_during_input_dialogs_data();
    void shutdown_waits_during_input_dialogs();
    void shutdown_test_command_must_match();
    void shutdown_ignores_requested_cancellation_data();
    void shutdown_ignores_requested_cancellation();
    void shutdown_handles_terminal_connection_loss_data();
    void shutdown_handles_terminal_connection_loss();
    void shutdown_ignores_completion_queued_before_arming_data();
    void shutdown_ignores_completion_queued_before_arming();
    //! Post All sets no flag of its own: a post that finishes while its
    //! overwrite question is open still finds that tab submittable.
    void shutdown_waits_during_post_all_overwrite_data();
    void shutdown_waits_during_post_all_overwrite();
    //! UI changes re-evaluate only an armed shutdown, once per burst.
    void shutdown_rechecks_are_coalesced_and_only_when_armed();
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
    //! Same for the password length: the save the dialog triggers used to read
    //! the tab in front back into ngPost and write its length instead.
    void password_length_default_is_owned_by_the_dialog();

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
    QWidget *quickTab = tabs->widget(2);
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
    auto *first = boxOf(tabs->widget(2));
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
    QWidget *quickTab = tabs->widget(2);
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

    // Compare the parsed command with the fixture before any test can arm it.
    // The test build also refuses execution unless this check succeeded.
    for (const QString &line : confBody.split('\n')) {
        if (line.startsWith("SHUTDOWN_CMD = ")) {
            const QString expected = line.mid(QString("SHUTDOWN_CMD = ").size());
            const QString helper = QString("\"%1\" --ngpost-test-shutdown ")
                                       .arg(QCoreApplication::applicationFilePath());
            if (!expected.startsWith(helper) || !ngPost.allowShutdownCommandForTest(expected)) {
                *error = "The harmless test shutdown command was not applied";
                return nullptr;
            }
        }
    }

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
    QWidget *quickTab = tabs->widget(2);
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
    QWidget *quickTab = tabs->widget(2);
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
    auto *quickTab = qobject_cast<PostingWidget *>(tabs->widget(2));
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
    QWidget *quickTab = tabs->widget(2);
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
    QWidget *quickTab = tabs->widget(2);
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

void TestMainWindow::password_length_default_is_owned_by_the_dialog()
{
    HomeSandbox sandbox;
    const QString confPath = PathHelper::configFilePath();
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString err;
    MainWindow *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\nLENGTH_PASS = 15\n", &err);
    QVERIFY2(window, qPrintable(err));
    auto configText = [&confPath]() {
        QFile file(confPath);
        return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                                : QString();
    };

    // The quick post tab in front still shows 15 when the dialog saves.
    {
        CompressionSettingsDialog dlg(&ngPost, window);
        auto *length = dlg.findChild<QSpinBox *>(QStringLiteral("rarLengthSB"));
        QVERIFY(length);
        QCOMPARE(length->value(), 15);
        length->setValue(24);
        dlg.accept();
    }
    QVERIFY2(configText().contains(QStringLiteral("\nLENGTH_PASS = 24\n")),
             qPrintable(configText()));

    // A tab opened afterwards starts from that default...
    auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("postTabWidget"));
    QVERIFY(tabs);
    PostingWidget *fresh = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(fresh);
    fresh->init();
    auto *freshLength = fresh->findChild<QSpinBox *>(QStringLiteral("passLengthSB"));
    QVERIFY(freshLength);
    QCOMPARE(freshLength->value(), 24);

    // ...and the length one post uses stays with that post.
    freshLength->setValue(8);
    tabs->setCurrentIndex(tabs->indexOf(fresh));
    QVERIFY(QMetaObject::invokeMethod(window, "onSaveConfig", Qt::DirectConnection));
    QVERIFY2(configText().contains(QStringLiteral("\nLENGTH_PASS = 24\n")),
             qPrintable(configText()));
    CompressionSettingsDialog again(&ngPost, window);
    QCOMPARE(again.findChild<QSpinBox *>(QStringLiteral("rarLengthSB"))->value(), 24);
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
    QCOMPARE(tabs->currentIndex(), 2);
    QCOMPARE(tabBar->startupTab(), -1);

    QCOMPARE(tabs->count(), 4);
    QVERIFY(tabs->widget(0)->findChild<QTableWidget *>("historyTable"));
    QVERIFY(qobject_cast<AutoPostWidget *>(tabs->widget(1)));
    auto *quick = qobject_cast<PostingWidget *>(tabs->widget(2));
    QVERIFY(quick);
    QCOMPARE(quick->jobNumber(), 1u);
    for (const QString &language : ngPost.languages()) {
        ngPost.changeLanguage(language);
        QCoreApplication::processEvents();
        QCOMPARE(tabs->tabText(0), QCoreApplication::translate("MainWindow", "History"));
        QCOMPARE(tabs->tabText(1), ngPost.folderMonitoringName());
        QCOMPARE(tabs->tabText(2), QString("%1 #1").arg(ngPost.quickJobName()));
        QCOMPARE(tabs->currentWidget(), quick);
    }
    // Fixed tabs must stay open, including Quick Post now at index 2.
    for (int index = 0; index < 3; ++index)
        QVERIFY(QMetaObject::invokeMethod(tabBar, "tabCloseRequested", Q_ARG(int, index)));
    QCOMPARE(tabs->count(), 4);
    QCOMPARE(tabs->currentWidget(), quick);

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
    pickStartupEntry(window, 0);
    QCOMPARE(window->startupTabForTest(), 0);
    QCOMPARE(tabBar->startupTab(), 0);
    QCOMPARE(tabs->currentIndex(), 2);
    {
        QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
        QCOMPARE(guiSettings.value(QStringLiteral("MainWindow/startupTab")).toInt(), 2);
    }

    // The tick follows the pinned tab, and only it.
    QVERIFY(startupEntryIsTicked(window, 0));
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

    // Existing preference IDs retain their meanings across the visual reorder.
    for (int savedId = 0; savedId < 3; ++savedId) {
        {
            QSettings guiSettings(guiSettingsPath(), QSettings::IniFormat);
            guiSettings.setValue(QStringLiteral("MainWindow/startupTab"), savedId);
        }
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

        const int expectedIndex = 2 - savedId;
        QCOMPARE(tabs->currentIndex(), expectedIndex);
        QCOMPARE(tabBar->startupTab(), expectedIndex);
        QVERIFY(startupEntryIsTicked(window, expectedIndex));
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

        QCOMPARE(tabs->currentIndex(), 2);
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
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--ngpost-test-shutdown") {
        QFile marker(app.arguments().at(2));
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Append))
            return 1;
        // Leave an observable empty file before writing: existence is not completion.
        QThread::msleep(100);
        marker.write("shutdown\n");
        return 0;
    }
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
    if (helperName.startsWith("ngpost-controlled-")) {
        if (app.arguments().contains("--help")) return 0;
        std::signal(SIGTERM, SIG_IGN);
        QFile started(app.applicationFilePath() + ".started");
        if (!started.open(QIODevice::WriteOnly)) return 2;
        started.close();
        QElapsedTimer deadline;
        deadline.start();
        while (!QFile::exists(app.applicationFilePath() + ".release") && deadline.elapsed() < 15000)
            QThread::msleep(10);
        for (const QString &arg : app.arguments()) {
            if (!arg.endsWith(".rar") && !arg.endsWith(".par2")) continue;
            QFile output(arg);
            if (!output.open(QIODevice::WriteOnly)) return 2;
            return output.write(QByteArray(8192, 'x')) == 8192 ? 0 : 3;
        }
        return 4;
    }
    if (QFileInfo(app.applicationFilePath()).fileName().startsWith("ngpost-recording-")) {
        const auto args = app.arguments().mid(1);
        if (args.isEmpty() || args.contains("--help")) return 0;
        if (helperName.contains("fail"))
            return 9;
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
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
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
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
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
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
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
    QCOMPARE(adjustmentLogged, requestedSize > 0 && increasedSize > 0);
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
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    // Written in text mode (CRLF on Windows). Not read in text mode: that would
    // also drop the bare CR a progress update rewrites its line with.
    const QString text = QString::fromUtf8(log.readAll()).replace("\r\n", "\n");
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString saved = QString::fromUtf8(config.readAll());
    QVERIFY(saved.contains("PAR2_TOOL = parpar\n"));
    QVERIFY(saved.contains("RAR_TOOL = 7zip\n"));
    QVERIFY(saved.contains("PAR2_PATH = " + par2Path + "\n"));
    QVERIFY(saved.contains("RAR_PATH = " + rarPath + "\n"));
    QVERIFY(!QRegularExpression("(?m)^(PAR2|RAR)_SOURCE =").match(saved).hasMatch());
    config.close();
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    ngPost.saveConfig();
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
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

void TestMainWindow::parity_engine_falls_back_to_an_installed_one()
{
#ifndef Q_OS_UNIX
    QSKIP("The test isolates automatic discovery using PATH");
#else
    HomeSandbox sandbox;
    const QString binDir = sandbox.rootPath() + "/bin";
    QVERIFY(QDir().mkpath(binDir));
    const QString installed = binDir + "/par2";
    {
        QFile fake(installed);
        QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(fake.write("#!/bin/sh\nexit 0\n") > 0);
        fake.close();
        QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));
    }
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", binDir.toLocal8Bit());
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", savedPath); });
    QVERIFY(!QFile::exists(QCoreApplication::applicationDirPath() + "/parpar"));

    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const QString arguments = "-s1M --auto-slice-size -r1n*0.6 --progress stdout -q";
    QString error;
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_TOOL = parpar\n"
                                      "PAR2_PCT = 10\nPAR2_ARGS = %1\n")
                                  .arg(arguments),
                              &error);
    QVERIFY2(window, qPrintable(error));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("PAR2_TOOL = parpar is not installed here"), qPrintable(log));
    QVERIFY2(log.contains(installed), qPrintable(log));
    QVERIFY2(log.contains("default arguments"), qPrintable(log));

    // This run uses the engine that exists, with its own defaults: ParPar
    // switches would make par2 fail on every post.
    QCOMPARE(ngPost.par2ToolInUse(), par2::Tool::Par2cmdline);
    QVERIFY(!ngPost.useParPar());

    // The configuration keeps the choice and the arguments: a reinstall of
    // ParPar, or the Windows installer option, makes them valid again.
    ngPost.saveConfig();
    QFile config(PathHelper::configFilePath());
    QVERIFY(config.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString saved = QString::fromUtf8(config.readAll());
    QVERIFY2(saved.contains("PAR2_TOOL = parpar\n"), "the fallback must not be persisted");
    QVERIFY(saved.contains("PAR2_ARGS = " + arguments + "\n"));
#endif
}

void TestMainWindow::custom_parity_arguments_for_another_tool_are_reported()
{
    HomeSandbox sandbox;
    const QString par2j = sandbox.rootPath() + "/par2j64";
    {
        QFile fake(par2j);
        QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(fake.write("#!/bin/sh\nexit 0\n") > 0);
        fake.close();
        QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));
    }
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    // MultiPar reads /switches; these are ParPar's.
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_PCT = 10\n"
                                      "PAR2_TOOL = multipar\nPAR2_SOURCE = custom\nPAR2_PATH = %1\n"
                                      "PAR2_ARGS_CUSTOM = -s1M --auto-slice-size -m1024M\n")
                                  .arg(par2j),
                              &error);
    QVERIFY2(window, qPrintable(error));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("PAR2_ARGS_CUSTOM is written for another tool than multipar"),
             qPrintable(log));
}

void TestMainWindow::an_external_edit_is_merged_into_the_next_save()
{
    HomeSandbox sandbox;
    // MultiPar at a path of its own: found wherever the test runs. Left to
    // automatic discovery it is missing off Windows, and an engine installed on
    // the machine -- the macOS runner puts ParPar on PATH -- would replace it
    // with its default arguments, the ones this test then reads back.
#ifdef Q_OS_WIN
    const QString par2j = sandbox.rootPath() + "/par2j64.exe";
#else
    const QString par2j = sandbox.rootPath() + "/par2j64";
#endif
    {
        QFile fake(par2j);
        QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(fake.write("#!/bin/sh\nexit 0\n") > 0);
        fake.close();
        QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));
    }
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              QString("GROUPS = alt.binaries.test\nPAR2_TOOL = multipar\n"
                                      "PAR2_SOURCE = custom\nPAR2_PATH = %1\nPAR2_PCT = 10\n")
                                  .arg(par2j),
                              &error);
    QVERIFY2(window, qPrintable(error));
    QCOMPARE(ngPost.par2ToolInUse(), par2::Tool::MultiPar);

    // What issue #15 describes: the file is edited by hand while ngPost runs.
    // The save that follows reads those lines back instead of erasing them.
    const QString mine = "c /rr10 /sn3000 /rd3 /ls2 /lr260000000";
    const QString conf = PathHelper::configFilePath();
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(
            file.write(
                QString("PAR2_ARGS_CUSTOM = %1\nMY_OWN_NOTE = keep-me\nMY_OTHER_NOTE = me-too\n")
                    .arg(mine)
                    .toUtf8())
            > 0);
    }
    ngPost.saveConfig();

    // Taken into memory, so this run already posts with those arguments.
    QCOMPARE(ngPost.par2ArgsInUse(), mine);
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("taken from your file: par2_args_custom"), qPrintable(log));
    QVERIFY2(log.contains("unused by ngPost: my_other_note, my_own_note"), qPrintable(log));

    auto configText = [&conf]() {
        QFile file(conf);
        return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                                : QString();
    };
    QString saved = configText();
    QCOMPARE(saved.count("PAR2_ARGS_CUSTOM = " + mine + "\n"), 1);
    QVERIFY2(saved.contains("MY_OWN_NOTE = keep-me\n"), qPrintable(saved));
    // A line ngPost does not write joins the other settings, above the
    // sections, where a reader of the file looks for it -- all of them under
    // one header, however many there are.
    QVERIFY(saved.indexOf("MY_OWN_NOTE") < saved.indexOf("[server]"));
    QVERIFY2(saved.contains("MY_OTHER_NOTE = me-too\n"), qPrintable(saved));
    QCOMPARE(saved.count("## Added to your configuration file, kept here"), 1);

    // No copy aside, and saving again neither duplicates nor drops anything.
    const QDir folder(QFileInfo(conf).absolutePath());
    QVERIFY(
        folder.entryInfoList({ QStringLiteral("ngPost.conf.edited-*") }, QDir::Files).isEmpty());
    ngPost.saveConfig();
    saved = configText();
    QCOMPARE(saved.count("PAR2_ARGS_CUSTOM = " + mine + "\n"), 1);
    QCOMPARE(saved.count("MY_OWN_NOTE = keep-me\n"), 1);
    QCOMPARE(saved.count("MY_OTHER_NOTE = me-too\n"), 1);
    QCOMPARE(saved.count("## Added to your configuration file, kept here"), 1);
    // Said once, when the line appeared: the saves that follow keep it silently.
    QCOMPARE(
        window->findChild<QTextBrowser *>("logBrowser")->toPlainText().count("unused by ngPost"),
        1);

    // And the file still reads back to the same state.
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    QCOMPARE(ngPost.par2ArgsInUse(), mine);

    // Commenting the line out is how the configuration turns the setting off,
    // and doing it under ngPost must not bring the line back active.
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        text.replace("\nPAR2_ARGS_CUSTOM = ", "\n#PAR2_ARGS_CUSTOM = ");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(text.toUtf8()) > 0);
    }
    ngPost.saveConfig();
    QVERIFY2(ngPost.par2ArgsInUse() != mine, "a commented out line must stop being used");
    saved = configText();
    QVERIFY2(!saved.contains("\nPAR2_ARGS_CUSTOM = "), qPrintable(saved));
    QVERIFY2(window->findChild<QTextBrowser *>("logBrowser")
                 ->toPlainText()
                 .contains("so it stops using them: par2_args_custom"),
             "the log must say the line was turned off");
}

void TestMainWindow::an_external_edit_ngPost_cannot_take_now_stays_in_the_file()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nPAR2_TOOL = multipar\nPAR2_PCT = 10\n"
                              "ARTICLE_SIZE = 716800\n",
                              &error);
    QVERIFY2(window, qPrintable(error));
    const QString conf = PathHelper::configFilePath();

    // ARTICLE_SIZE is read once, when the posters are built, and PAR2_TOOL
    // picks an executable at startup: taking either now would leave the running
    // job disagreeing with itself. PAR2_PCT is edited on both sides.
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(file.write("PAR2_TOOL = par2\nARTICLE_SIZE = 1433600\nPAR2_PCT = 33\n"
                           "HOST = news.example.com\n")
                > 0);
    }
    Par2SettingsDialog parity(&ngPost, {}, false, false, window);
    auto *percentage = parity.findChild<QSpinBox *>("par2DefaultPct");
    QVERIFY(percentage);
    percentage->setValue(12);
    parity.accept();
    ngPost.saveConfig();

    QFile file(conf);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString saved = QString::fromUtf8(file.readAll());
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();

    // Kept in the file for the next start, while this run keeps its own values.
    QVERIFY2(saved.contains("PAR2_TOOL = par2\n"), qPrintable(saved));
    QVERIFY2(saved.contains("article_size = 1433600\n"), qPrintable(saved));
    QCOMPARE(NgPost::articleSize(), Q_INT64_C(716800));
    QVERIFY2(log.contains("used only after a restart: article_size"), qPrintable(log));

    // Edited on both sides: the window the user just validated wins, and the
    // value it drops is named so nothing disappears silently.
    QVERIFY2(saved.contains("PAR2_PCT = 12\n"), qPrintable(saved));
    QVERIFY2(log.contains("drops yours: par2_pct = 33"), qPrintable(log));

    // A [server] key written at the top level is the one thing not merged back:
    // it would add a second server on the next start.
    QVERIFY2(!saved.contains("HOST = news.example.com"), qPrintable(saved));
    QVERIFY2(log.contains("[server] section holds them now: host"), qPrintable(log));

    // Every save patches the two lines again, and says so only the first time.
    ngPost.saveConfig();
    file.close();
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString again = QString::fromUtf8(file.readAll());
    QVERIFY2(again.contains("PAR2_TOOL = par2\n"), qPrintable(again));
    QVERIFY2(again.contains("article_size = 1433600\n"), qPrintable(again));
    QCOMPARE(window->findChild<QTextBrowser *>("logBrowser")
                 ->toPlainText()
                 .count("used only after a restart"),
             1);
}

void TestMainWindow::a_hand_edited_server_block_is_reported()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\nPAR2_PCT = 10\n", &error);
    QVERIFY2(window, qPrintable(error));

    const QString conf = PathHelper::configFilePath();
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(file.write("\n[server]\nhost = news.hand.example\nport = 563\n"
                           "connection = 10\nenabled = true\n")
                > 0);
    }
    ngPost.saveConfig();

    QFile file(conf);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY2(!QString::fromUtf8(file.readAll()).contains("news.hand.example"),
             "a hand written block is not merged: that is the documented limit");
    auto log = [window]() {
        return window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    };
    QVERIFY2(log().contains("were edited by hand"), qPrintable(log()));

    // Said for the edit, not for every save that follows.
    ngPost.saveConfig();
    QCOMPARE(log().count("were edited by hand"), 1);
}

void TestMainWindow::a_conflict_on_a_password_does_not_print_it()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nRAR_PASS = as-ngPost-read-it\n",
                              &error);
    QVERIFY2(window, qPrintable(error));

    const QString conf = PathHelper::configFilePath();
    const QString onlyOnDisk = QStringLiteral("edited-behind-ngPost-back");
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        text.replace("RAR_PASS = as-ngPost-read-it", "RAR_PASS = " + onlyOnDisk);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(text.toUtf8()) > 0);
    }

    // The window changes it too, which makes it a conflict: ngPost keeps its
    // own value and says which line it dropped.
    CompressionSettingsDialog dialog(&ngPost, window);
    auto *box = dialog.findChild<QCheckBox *>("rarPassCB");
    auto *edit = dialog.findChild<QLineEdit *>("rarPassEdit");
    QVERIFY(box && edit);
    box->setChecked(true);
    edit->setText("typed-in-the-window");
    dialog.accept();

    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("rar_pass"), qPrintable(log));
    QVERIFY2(!log.contains(onlyOnDisk), "a credential must never reach the log");
}

void TestMainWindow::a_conflict_on_archiver_switches_does_not_print_them()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nRAR_TOOL = rar\nRAR_EXTRA = -m0\n",
                              &error);
    QVERIFY2(window, qPrintable(error));

    // A password glued to the switches in the file...
    const QString conf = PathHelper::configFilePath();
    const QString onlyOnDisk = QStringLiteral("edited-behind-ngPost-back");
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY2(text.contains("RAR_EXTRA = -m0"), qPrintable(text));
        text.replace("RAR_EXTRA = -m0", "RAR_EXTRA = -m0 -hp" + onlyOnDisk);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(text.toUtf8()) > 0);
    }

    // ...while the window switches archiver, which drops ngPost's own switches:
    // changed on both sides, so ngPost keeps its value and names the line.
    CompressionSettingsDialog dialog(&ngPost, window);
    auto *tool = dialog.findChild<QComboBox *>("rarTool");
    QVERIFY(tool);
    tool->setCurrentIndex(tool->findData(QStringLiteral("7zip")));
    dialog.accept();

    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("drops yours: rar_extra"), qPrintable(log));
    QVERIFY2(!log.contains(onlyOnDisk), "an archive password must never reach the log");
}

void TestMainWindow::commented_examples_survive_being_uncommented()
{
    // Everything after the = goes to the tool or the shell, so a note in
    // parentheses at the end of an example becomes an argument.
    const QRegularExpression example("^#([A-Za-z_0-9]+)\\s*=\\s*(.+)$");
    const QRegularExpression note("\\([^()]*\\)\\s*$");
    auto check = [&](QString const &text, QString const &what) {
        for (QString const &line : text.split(QLatin1Char('\n'))) {
            const auto m = example.match(line.trimmed());
            if (!m.hasMatch())
                continue;
            // A command may legitimately hold parentheses inside its own
            // quotes; only a note tacked on at the end is the trap.
            if (m.captured(2).contains(QLatin1Char('"')))
                continue;
            QVERIFY2(!note.match(m.captured(2)).hasMatch(),
                     qPrintable(what + ": " + line.trimmed()));
        }
    };

    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    ngPost.saveConfig();
    QFile written(PathHelper::configFilePath());
    QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
    check(QString::fromUtf8(written.readAll()), "the configuration ngPost writes");

    for (QString const &name :
         { QStringLiteral("ngPost.conf.example"), QStringLiteral("ngPost_fr.conf") }) {
        QFile shipped(QStringLiteral(NGPOST_SOURCE_ROOT) + QLatin1Char('/') + name);
        QVERIFY2(shipped.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(name));
        check(QString::fromUtf8(shipped.readAll()), name);
    }
}

void TestMainWindow::custom_parity_arguments_are_never_rewritten()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    // The line of issue #15, next to the one the PAR2 Settings dialog maintains.
    const QString mine = "c /rr10 /sn3000 /rd3 /ls2 /lr260000000 /lc288";
    const QString ngPostLine = "c /rr10 /ss1048576 /rd3";
    const QString config = QString("GROUPS = alt.binaries.test\nPAR2_TOOL = multipar\n"
                                   "PAR2_PCT = 10\nPAR2_ARGS = %1\n%2PAR2_ARGS_CUSTOM = %3\n");

    {
        // Commented out, the custom line does not exist for ngPost.
        NgPost ngPost(argc, argv);
        QString error;
        auto *window = bootWindow(ngPost, config.arg(ngPostLine, "#", mine), &error);
        QVERIFY2(window, qPrintable(error));
        Par2SettingsDialog parity(&ngPost, {}, false, false, window);
        QVERIFY(!parity.findChild<QCheckBox *>("par2Custom")->isChecked());
        QCOMPARE(parity.findChild<QPlainTextEdit *>("par2Arguments")->toPlainText(), ngPostLine);
    }

    // Uncommented, it is what runs, and nothing may rewrite it.
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, config.arg(ngPostLine, "", mine), &error);
    QVERIFY2(window, qPrintable(error));

    Par2SettingsDialog parity(&ngPost, {}, false, false, window);
    auto *custom = parity.findChild<QCheckBox *>("par2Custom");
    QVERIFY(custom);
    QVERIFY2(custom->isChecked(), "an uncommented PAR2_ARGS_CUSTOM must reach the dialog");
    QCOMPARE(parity.findChild<QPlainTextEdit *>("par2Arguments")->toPlainText(), mine);
    // Touch another setting: the dialog now saves, and must still not touch them.
    auto *percentage = parity.findChild<QSpinBox *>("par2DefaultPct");
    QVERIFY(percentage);
    percentage->setValue(percentage->value() == 10 ? 12 : 10);
    parity.accept();

    ngPost.saveConfig();
    QFile file(PathHelper::configFilePath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString saved = QString::fromUtf8(file.readAll());
    QVERIFY2(saved.contains("PAR2_ARGS_CUSTOM = " + mine + "\n"), qPrintable(saved));

    // Reading it back keeps it, so the save that follows cannot drift either.
    QVERIFY(ngPost.parseDefaultConfig().isEmpty());
    ngPost.saveConfig();
    file.close();
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(QString::fromUtf8(file.readAll()).contains("PAR2_ARGS_CUSTOM = " + mine + "\n"));
}


void TestMainWindow::a_setting_appended_below_the_servers_is_merged()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    // No PAR2_TOOL: automatic discovery never falls back to another engine, so
    // the arguments read back below are the file's whatever is installed here.
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nPAR2_PCT = 10\n\n"
                              "[server]\nhost = news.example.com\nport = 563\nssl = true\n"
                              "user = me\npass = secret\nconnection = 5\nenabled = true\n",
                              &error);
    QVERIFY2(window, qPrintable(error));
    ngPost.saveConfig(); // the file as ngPost writes it: servers below the settings
    auto log = [window]() {
        return window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    };
    const QString conf = PathHelper::configFilePath();
    auto configText = [&conf]() {
        QFile file(conf);
        return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                                : QString();
    };

    // Appended at the very end, below the [server] block: ngPost reads both
    // lines as settings when it starts, so the merge must as well.
    const QString mine = "c -l -m1024 -r8";
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(
            file.write(QString("PAR2_ARGS_CUSTOM = %1\nMY_OWN_NOTE = keep-me\n").arg(mine).toUtf8())
            > 0);
    }
    ngPost.saveConfig();

    QCOMPARE(ngPost.par2ArgsInUse(), mine);
    QVERIFY2(log().contains("taken from your file: par2_args_custom"), qPrintable(log()));
    QVERIFY2(!log().contains("were edited by hand"), "a setting is not a change to a block");
    QString saved = configText();
    QCOMPARE(saved.count("PAR2_ARGS_CUSTOM = " + mine + "\n"), 1);
    QVERIFY2(saved.contains("MY_OWN_NOTE = keep-me\n"), qPrintable(saved));
    QVERIFY(saved.indexOf("PAR2_ARGS_CUSTOM = " + mine) < saved.indexOf("\n[server]\n"));
    QVERIFY2(saved.contains("host = news.example.com\n"), qPrintable(saved));

    // A key the block owns is still a change to that block, and still said.
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(
                    saved.replace("host = news.example.com", "host = news.other.example").toUtf8())
                > 0);
    }
    ngPost.saveConfig();
    QVERIFY2(log().contains("were edited by hand"), qPrintable(log()));
    QVERIFY(configText().contains("host = news.example.com\n"));
}

void TestMainWindow::lengths_edited_in_the_file_are_not_reverted_by_the_tabs()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              "GROUPS = alt.binaries.test\nLENGTH_NAME = 22\nLENGTH_PASS = 15\n",
                              &error);
    QVERIFY2(window, qPrintable(error));
    ngPost.saveConfig();
    const QString conf = PathHelper::configFilePath();
    auto configText = [&conf]() {
        QFile file(conf);
        return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                                : QString();
    };
    {
        QString text = configText();
        QVERIFY2(text.contains("LENGTH_NAME = 22\n"), qPrintable(text));
        QVERIFY2(text.contains("LENGTH_PASS = 15\n"), qPrintable(text));
        text.replace("LENGTH_NAME = 22", "LENGTH_NAME = 30");
        text.replace("LENGTH_PASS = 15", "LENGTH_PASS = 20");
        QFile file(conf);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(text.toUtf8()) > 0);
    }

    // The second save is the one that used to write the spin box back over the
    // file, after the first had announced the value as taken.
    ngPost.saveConfig();
    ngPost.saveConfig();
    const QString saved = configText();
    QVERIFY2(saved.contains("LENGTH_NAME = 30\n"), qPrintable(saved));
    QVERIFY2(saved.contains("LENGTH_PASS = 20\n"), qPrintable(saved));
    const QString log = window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    QVERIFY2(log.contains("used only after a restart: length_name"), qPrintable(log));
    QVERIFY2(!log.contains("taken from your file: length_name"), qPrintable(log));
    QVERIFY2(log.contains("taken from your file: length_pass"), qPrintable(log));
    // This run keeps the name length its tabs show and post with...
    const auto boxes = window->findChildren<QSpinBox *>("nameLengthSB");
    QVERIFY(!boxes.isEmpty());
    for (QSpinBox *box : boxes)
        QCOMPARE(box->value(), 22);
    // ...and the dialog now offers the password length of the file.
    CompressionSettingsDialog dialog(&ngPost, window);
    auto *length = dialog.findChild<QSpinBox *>("rarLengthSB");
    QVERIFY(length);
    QCOMPARE(length->value(), 20);
}

void TestMainWindow::parity_settings_taken_from_the_file_reach_the_posting_tabs()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\nPAR2_PCT = 10\n", &error);
    QVERIFY2(window, qPrintable(error));
    ngPost.saveConfig();
    const QString conf = PathHelper::configFilePath();
    const QString mine = "c -l -m1024 -r8";
    {
        QFile file(conf);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY2(text.contains("PAR2_PCT = 10\n"), qPrintable(text));
        text.replace("PAR2_PCT = 10", "PAR2_PCT = 33");
        text += QString("PAR2_ARGS_CUSTOM = %1\n").arg(mine);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QVERIFY(file.write(text.toUtf8()) > 0);
    }
    ngPost.saveConfig();
    QCOMPARE(ngPost.par2DefaultPercentage(), 33u);

    // Both the quick post and the auto post tab: their default redundancy and
    // the arguments their tooltip says a post runs with.
    const auto boxes = window->findChildren<QSpinBox *>("redundancySB");
    QVERIFY(boxes.size() >= 2);
    for (QSpinBox *box : boxes) {
        QVERIFY2(box->specialValueText().contains("33"), qPrintable(box->specialValueText()));
        QVERIFY2(box->toolTip().contains(mine), qPrintable(box->toolTip()));
    }
}

void TestMainWindow::saving_the_parity_dialog_keeps_the_engine_fallback_right()
{
#ifndef Q_OS_UNIX
    QSKIP("The test isolates automatic discovery using PATH");
#else
    HomeSandbox sandbox;
    const QString binDir = sandbox.rootPath() + "/bin";
    QVERIFY(QDir().mkpath(binDir));
    const QString installed = binDir + "/par2";
    {
        QFile fake(installed);
        QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(fake.write("#!/bin/sh\nexit 0\n") > 0);
        fake.close();
        QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));
    }
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", binDir.toLocal8Bit());
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
    QCOMPARE(ngPost.par2ToolInUse(), par2::Tool::Par2cmdline);
    QCOMPARE(ngPost.par2PathForTest(), installed);
    auto log = [window]() {
        return window->findChild<QTextBrowser *>("logBrowser")->toPlainText();
    };

    {
        // Only the redundancy changes. ParPar is still missing, and this run
        // keeps posting with the engine it fell back to -- not with an empty
        // path, which is what the window's own path field holds for ParPar.
        Par2SettingsDialog parity(&ngPost, {}, false, false, window);
        auto *percentage = parity.findChild<QSpinBox *>("par2DefaultPct");
        QVERIFY(percentage);
        percentage->setValue(12);
        parity.accept();
    }
    QCOMPARE(ngPost.par2ToolInUse(), par2::Tool::Par2cmdline);
    QCOMPARE(ngPost.par2PathForTest(), installed);
    // The missing engine did not change: said at startup, not again.
    QCOMPARE(log().count("PAR2_TOOL = parpar is not installed here"), 1);

    {
        // Choosing the installed engine ends the fallback, and the arguments
        // the window builds for it are the ones this run passes.
        Par2SettingsDialog parity(&ngPost, {}, false, false, window);
        auto *tool = parity.findChild<QComboBox *>("par2Tool");
        QVERIFY(tool);
        tool->setCurrentIndex(tool->findData(int(par2::Tool::Par2cmdline)));
        auto *percentage = parity.findChild<QSpinBox *>("par2DefaultPct");
        QVERIFY(percentage);
        percentage->setValue(14);
        parity.accept();
    }
    QCOMPARE(ngPost.par2ToolInUse(), par2::Tool::Par2cmdline);
    QCOMPARE(ngPost.par2PathForTest(), installed);
    QVERIFY(!ngPost.par2ArgsInUse().isEmpty());
    QCOMPARE(ngPost.par2ArgsInUse(), ngPost.par2ArgsConfigured());
#endif
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

void TestMainWindow::shutdown_waits_for_every_post_data()
{
    QTest::addColumn<bool>("submitAll");
    QTest::addColumn<bool>("autoClose");
    QTest::addColumn<bool>("startDefault");
    QTest::addColumn<QString>("resolveBlockers");
    // These two rows protect the existing backend queue barrier.
    QTest::newRow("five queued posts") << true << false << true << QString("post");
    QTest::newRow("five queued posts, auto close") << true << true << true << QString("post");
    // Every row below requires the prepared-tab barrier and its deferred recheck.
    for (const bool autoClose : { false, true }) {
        const QByteArray suffix = autoClose ? ", auto close" : "";
        QTest::newRow("submit unstarted posts" + suffix)
            << false << autoClose << true << QString("post");
        QTest::newRow("submit default tab" + suffix)
            << false << autoClose << false << QString("post");
        QTest::newRow("clear unstarted posts" + suffix)
            << false << autoClose << true << QString("clear");
        QTest::newRow("clear default tab" + suffix)
            << false << autoClose << false << QString("clear");
        QTest::newRow("close unstarted posts" + suffix)
            << false << autoClose << true << QString("close");
    }
}

void TestMainWindow::shutdown_waits_for_every_post()
{
    QFETCH(bool, submitAll);
    QFETCH(bool, autoClose);
    QFETCH(bool, startDefault);
    QFETCH(QString, resolveBlockers);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    const auto markerPath = root + "/shutdown-marker";
    QString error;
    auto *window = bootWindow(
        ngPost,
        QString("GROUPS = alt.binaries.test\nthread = 1\nnzbPath = %1\n"
                "AUTO_CLOSE_TABS = %2\nSHUTDOWN_CMD = \"%3\" --ngpost-test-shutdown \"%4\"\n"
                "[server]\nhost = 127.0.0.1\nport = %5\nssl = false\nconnection = 1\n")
            .arg(root,
                 autoClose ? "true" : "false",
                 QCoreApplication::applicationFilePath(),
                 markerPath)
            .arg(mock.port()),
        &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    QList<QPointer<PostingWidget>> posts{ first };
    for (int i = 1; i < 5; ++i)
        posts << window->addNewQuickTab(tabs->count() - 1);
    for (int i = 0; i < posts.size(); ++i) {
        QFile file(root + QString("/source-%1.bin").arg(i));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(64000, 'a' + i));
        file.close();
        posts[i]->addPath(file.fileName(), 0);
    }
    QPointer<PostingWidget> started = posts[startDefault ? 0 : 4];
    // Empty tabs must not prevent shutdown once the five real posts finish.
    window->addNewQuickTab(tabs->count() - 1);
    bool confirmed = false;
    QTimer confirm;
    connect(&confirm, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *question = qobject_cast<QMessageBox *>(widget)) {
                confirmed = true;
                question->done(QMessageBox::Yes);
            }
    });
    confirm.start(5);
    window->findChild<QCheckBox *>("shutdownCB")->setChecked(true);
    confirm.stop();
    QVERIFY(confirmed);

    if (submitAll)
        window->findChild<QPushButton *>("postAllTabsButton")->click();
    else
        started->postFiles(true);

    QTRY_VERIFY_WITH_TIMEOUT(!started
                                 || (autoClose ? started->previewFiles().isEmpty()
                                               : started->isPostingFinished()),
                             15000);
    QVERIFY2(!QFile::exists(markerPath), "Shutdown ran while posts remained queued or unstarted");
    if (!submitAll) {
        const QString blockedMessage =
            "Shutdown postponed: some posting tabs have not been submitted.";
        auto *log = window->findChild<QTextBrowser *>("logBrowser");
        QTRY_COMPARE(log->toPlainText().count(blockedMessage), 1);
        QList<QPointer<PostingWidget>> blockers;
        for (const auto &post : posts)
            if (post && post != started) {
                QVERIFY(post->canSubmit());
                blockers << post;
                emit post->submissionEligibilityChanged();
            }
        QTest::qWait(150);
        QVERIFY2(!QFile::exists(markerPath), "Shutdown ignored prepared tabs");
        QCOMPARE(log->toPlainText().count(blockedMessage), 1);
        if (resolveBlockers == "post") {
            window->findChild<QPushButton *>("postAllTabsButton")->click();
        } else {
            for (int i = 0; i < blockers.size(); ++i) {
                auto &post = blockers[i];
                const bool last = i == blockers.size() - 1;
                if (resolveBlockers == "close") {
                    emit tabs->tabBar()->tabCloseRequested(tabs->indexOf(post));
                    QVERIFY(post.isNull());
                } else {
                    auto *clear = post->findChild<QPushButton *>("clearFilesButton");
                    QVERIFY(clear);
                    if (last) {
                        // A transiently empty list must not trigger shutdown inside
                        // its mutation: re-add before the deferred check runs.
                        const auto source = post->previewFiles().first().filePath();
                        clear->click();
                        post->addPath(source, 0);
                        QTest::qWait(150);
                        QVERIFY(!QFile::exists(markerPath));
                        QCOMPARE(log->toPlainText().count(blockedMessage), 1);
                    }
                    clear->click();
                    QVERIFY(post->previewFiles().isEmpty());
                }
                if (!last) {
                    QTest::qWait(50);
                    QVERIFY(!QFile::exists(markerPath));
                    QCOMPARE(log->toPlainText().count(blockedMessage), 1);
                }
            }
        }
    }
    const auto readMarker = [&] {
        QFile marker(markerPath);
        return marker.open(QIODevice::ReadOnly) ? marker.readAll() : QByteArray();
    };
    // Wait for the content, not for the child to have merely opened the file.
    // No explicit maybeFinishApplication() here: UI changes must wake it up.
    QTRY_COMPARE_WITH_TIMEOUT(readMarker(), QByteArray("shutdown\n"), 20000);
    QCOMPARE(mock.receivedArticles().size(), resolveBlockers == "post" ? 5 : 1);
    for (const auto &post : posts)
        QVERIFY(!post || post->isPostingFinished() || post->previewFiles().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 1);
    // Count launches directly after the first process has exited. A second
    // launch is observable even before the child starts or writes anything.
    for (int i = 0; i < 3; ++i) {
        ngPost.maybeFinishApplication();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(ngPost.shutdownStartCountForTest(), 1);
    }
    QCOMPARE(readMarker(), QByteArray("shutdown\n"));
}

namespace
{
bool armTestShutdown(MainWindow *window)
{
    bool confirmed = false;
    QTimer confirm;
    QObject::connect(&confirm, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *question = qobject_cast<QMessageBox *>(widget)) {
                confirmed = true;
                question->done(QMessageBox::Yes);
            }
    });
    confirm.start(5);
    window->findChild<QCheckBox *>("shutdownCB")->setChecked(true);
    return confirmed;
}

QString shutdownTestConfig(const QString &root, quint16 port)
{
    return QString("GROUPS = alt.binaries.test\nthread = 1\nnzbPath = %1\n"
                   "SHUTDOWN_CMD = \"%2\" --ngpost-test-shutdown \"%1/shutdown-marker\"\n"
                   "[server]\nhost = 127.0.0.1\nport = %3\nssl = false\nconnection = 1\n")
        .arg(root, QCoreApplication::applicationFilePath())
        .arg(port);
}

bool addShutdownTestFile(PostingWidget *post, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray(64000, 'a'));
    file.close();
    post->addPath(path, 0);
    return true;
}
}

void TestMainWindow::shutdown_requires_new_completed_post_data()
{
    QTest::addColumn<bool>("withHistory");
    QTest::addColumn<bool>("startDefault");
    QTest::addColumn<bool>("rearm");
    QTest::newRow("nothing ever posted") << false << true << false;
    QTest::newRow("clear finished default tab") << true << true << false;
    QTest::newRow("close old finished tab") << true << false << false;
    QTest::newRow("rearm after deferred completion") << true << true << true;
}

void TestMainWindow::shutdown_requires_new_completed_post()
{
    QFETCH(bool, withHistory);
    QFETCH(bool, startDefault);
    QFETCH(bool, rearm);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *defaultPost = qobject_cast<PostingWidget *>(tabs->widget(2));
    QPointer<PostingWidget> other = window->addNewQuickTab(tabs->count() - 1);
    auto *completed = startDefault ? defaultPost : other.data();
    auto *blocker = startDefault ? other.data() : defaultPost;
    if (withHistory) {
        QVERIFY(addShutdownTestFile(completed, root + "/old.bin"));
        if (rearm) {
            QVERIFY(addShutdownTestFile(blocker, root + "/blocker.bin"));
            QVERIFY(armTestShutdown(window));
        }
        completed->postFiles(true);
        QTRY_VERIFY_WITH_TIMEOUT(completed->isPostingFinished(), 15000);
        QCOMPARE(mock.receivedArticles().size(), 1);
        if (rearm) {
            QTRY_VERIFY(window->findChild<QTextBrowser *>("logBrowser")
                            ->toPlainText()
                            .contains("Shutdown postponed:"));
            window->findChild<QCheckBox *>("shutdownCB")->setChecked(false);
        }
    }
    QVERIFY(armTestShutdown(window));
    // Finish-check signals from old tabs must never grant shutdown eligibility.
    defaultPost->findChild<QPushButton *>("clearFilesButton")->click();
    emit tabs->tabBar()->tabCloseRequested(tabs->indexOf(other));
    QVERIFY(other.isNull());
    QVERIFY(addShutdownTestFile(defaultPost, root + "/temporary.bin"));
    defaultPost->findChild<QPushButton *>("clearFilesButton")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    ngPost.maybeFinishApplication();
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    QVERIFY(!QFile::exists(root + "/shutdown-marker"));

    // The same armed request becomes eligible after a genuinely new transfer.
    QVERIFY(addShutdownTestFile(defaultPost, root + "/new.bin"));
    defaultPost->postFiles(true);
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), withHistory ? 2 : 1);
}

void TestMainWindow::shutdown_waits_during_vpn_confirmation_data()
{
    QTest::addColumn<bool>("proceed");
    QTest::newRow("continue") << true;
    QTest::newRow("cancel") << false;
}

void TestMainWindow::shutdown_waits_during_vpn_confirmation()
{
    QFETCH(bool, proceed);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *last = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(first, root + "/first.bin"));
    QVERIFY(addShutdownTestFile(last, root + "/last.bin"));
    QVERIFY(armTestShutdown(window));
    first->postFiles(true);
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished(), 15000);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    // Fake only detection of installed prerequisites; no helper is executed.
    auto *vpn = ngPost.vpnManager();
    vpn->setHelperInstalledForTest(true);
    vpn->setAutoConnect(true);
    QVERIFY(vpn->shouldConfirmMasterSwitchWithoutProfile());
    bool sawDialog = false;
    bool blocked = false;
    QTimer respond;
    connect(&respond, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *question = qobject_cast<QMessageBox *>(widget);
            // macOS ignores QMessageBox titles: recognise the box by its text.
            if (!question || !question->text().startsWith("VPN is enabled globally"))
                continue;
            respond.stop();
            sawDialog = true;
            // Run the real queued checks while the confirmation is open.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            ngPost.maybeFinishApplication();
            blocked = ngPost.shutdownStartCountForTest() == 0;
            for (auto *button : question->buttons())
                if (question->buttonRole(button)
                    == (proceed ? QMessageBox::AcceptRole : QMessageBox::RejectRole)) {
                    button->click();
                    return;
                }
        }
    });
    respond.start(5);
    last->postFiles(true);
    respond.stop();
    QVERIFY(sawDialog);
    QVERIFY2(blocked, "Shutdown started inside the VPN confirmation");
    if (!proceed) {
        QVERIFY(last->canSubmit());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        last->findChild<QPushButton *>("clearFilesButton")->click();
    }
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), proceed ? 2 : 1);
}

void TestMainWindow::shutdown_waits_for_posts_added_after_completion()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *second = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(first, root + "/first.bin"));
    QVERIFY(addShutdownTestFile(second, root + "/second.bin"));
    QVERIFY(armTestShutdown(window));
    first->postFiles(true);
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished(), 15000);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    // Completion is latched, but the queue is never snapshotted at arming.
    auto *late = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(late, root + "/late.bin"));
    second->postFiles(true);
    QTRY_VERIFY_WITH_TIMEOUT(second->isPostingFinished(), 15000);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    QVERIFY(late->canSubmit());
    // A further job is submitted while this last transfer is still active.
    late->postFiles(true);
    auto *queued = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(queued, root + "/queued.bin"));
    queued->postFiles(true);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QVERIFY(late->isPostingFinished());
    QVERIFY(queued->isPostingFinished());
    QCOMPARE(mock.receivedArticles().size(), 4);
}

void TestMainWindow::shutdown_requires_an_actual_transfer()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(
        ngPost,
        QString("TMP_DIR = %1\nRAR_SOURCE = custom\nRAR_PATH = %1/missing-rar\n").arg(root)
            + shutdownTestConfig(root, mock.port()),
        &error);
    QVERIFY2(window, qPrintable(error));
    auto *post = qobject_cast<PostingWidget *>(
        window->findChild<QTabWidget *>("postTabWidget")->widget(2));
    QVERIFY(addShutdownTestFile(post, root + "/failed.bin"));
    QVERIFY(armTestShutdown(window));
    post->findChild<QCheckBox *>("compressCB")->setChecked(true);
    post->postFiles(true);
    QTRY_VERIFY_WITH_TIMEOUT(post->isPostingFinished(), 15000);
    QCOMPARE(mock.receivedArticles().size(), 0);
    post->findChild<QPushButton *>("clearFilesButton")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    ngPost.maybeFinishApplication();
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    const QString waiting = "Shutdown postponed: waiting for a completed post";
    auto *log = window->findChild<QTextBrowser *>("logBrowser");
    QTRY_COMPARE(log->toPlainText().count(waiting), 1);
    ngPost.maybeFinishApplication();
    QCOMPARE(log->toPlainText().count(waiting), 1);
    // A failure does not disarm the request: a later real post can satisfy it.
    post->findChild<QCheckBox *>("compressCB")->setChecked(false);
    QVERIFY(addShutdownTestFile(post, root + "/posted.bin"));
    post->postFiles(true);
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), 1);
}

void TestMainWindow::shutdown_handles_terminal_connection_loss_data()
{
    QTest::addColumn<bool>("autoResume");
    QTest::addColumn<bool>("preparedPost");
    QTest::newRow("lost connections, queue empty") << false << false;
    QTest::newRow("lost connections, another post prepared") << false << true;
    QTest::newRow("automatic reconnection still pending") << true << false;
}

void TestMainWindow::shutdown_handles_terminal_connection_loss()
{
    QFETCH(bool, autoResume);
    QFETCH(bool, preparedPost);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "30" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost,
                              QString("NO_RESUME_AUTO = %1\n").arg(autoResume ? "false" : "true")
                                  + shutdownTestConfig(root, mock.port()),
                              &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *prepared = qobject_cast<PostingWidget *>(tabs->widget(2));
    if (preparedPost)
        QVERIFY(addShutdownTestFile(prepared, root + "/later.bin"));
    QFile file(root + "/large.bin");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(8 * 1024 * 1024));
    file.close();
    PostingJobOptions options;
    options.files = { QFileInfo(file) };
    options.inputPaths = { file.fileName() };
    options.nzbFilePath = root + "/large.nzb";
    options.grpList = { "alt.binaries.test" };
    options.from = "poster@example.invalid";
    options.articleSizeBytes = 4096;
    QPointer<PostingJob> job = new PostingJob(&ngPost, options);
    QSignalSpy lostConnections(job, &PostingJob::noMoreConnection);
    bool incomplete = false;
    bool transferred = false;
    connect(job, &PostingJob::noMoreConnection, window, [&] {
        incomplete = !job->hasPostFinished();
        transferred = job->nbArticlesUploaded() > job->nbArticlesFailed();
    }, Qt::DirectConnection);
    QVERIFY(armTestShutdown(window));
    QVERIFY(ngPost.startPostingJob(job));
    QTRY_VERIFY_WITH_TIMEOUT(job && job->nbArticlesUploaded() > job->nbArticlesFailed(), 15000);
    QVERIFY(!job->hasPostFinished());
    mock.stop(); // cut the actual transport after at least one confirmed article
    if (autoResume) {
        QTRY_VERIFY_WITH_TIMEOUT(job && job->isPaused(), 15000);
        QCOMPARE(job->pauseReason(), PostingJob::PauseReason::ConnectionBackoff);
        QCOMPARE(lostConnections.count(), 0);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        // Stopping the paused job preserves it for resume; it does not authorize shutdown.
        job->onStopPosting();
        QTRY_VERIFY_WITH_TIMEOUT(job.isNull(), 15000);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        return;
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(lostConnections.count(), 1, 15000);
        QVERIFY(incomplete);
        QVERIFY(transferred);
    }
    if (preparedPost) {
        QTRY_VERIFY_WITH_TIMEOUT(job.isNull(), 15000);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        QVERIFY(prepared->canSubmit());
        prepared->findChild<QPushButton *>("clearFilesButton")->click();
    }
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QVERIFY(!mock.receivedArticles().isEmpty());
}

void TestMainWindow::shutdown_ignores_completion_queued_before_arming_data()
{
    QTest::addColumn<bool>("previouslyArmed");
    QTest::newRow("arm after actual finish, before queued notification") << false;
    QTest::newRow("rearm after actual finish, before queued notification") << true;
}

void TestMainWindow::shutdown_ignores_completion_queued_before_arming()
{
    QFETCH(bool, previouslyArmed);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    QFile file(root + "/source.bin");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("payload");
    file.close();
    PostingJobOptions options;
    options.files = { QFileInfo(file) };
    options.inputPaths = { file.fileName() };
    options.nzbFilePath = root + "/old.nzb";
    options.grpList = { "alt.binaries.test" };
    options.from = "poster@example.invalid";
    QPointer<PostingJob> old = new PostingJob(&ngPost, options);
    bool armedAfterFinish = false;
    connect(old, &PostingJob::postingFinished, window, [&] {
        // Direct delivery runs after _finishPosting but before NgPost's queued
        // completion handler. No timestamps or artificial delays are involved.
        ngPost.setShutdownWhenDone(false);
        ngPost.setShutdownWhenDone(true);
        armedAfterFinish = true;
    }, Qt::DirectConnection);
    if (previouslyArmed)
        QVERIFY(armTestShutdown(window));
    QVERIFY(ngPost.startPostingJob(old));
    QTRY_VERIFY_WITH_TIMEOUT(old.isNull(), 15000);
    QVERIFY(armedAfterFinish);
    ngPost.maybeFinishApplication();
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    options.nzbFilePath = root + "/new.nzb";
    QVERIFY(ngPost.startPostingJob(new PostingJob(&ngPost, options)));
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), 2);
}

void TestMainWindow::shutdown_ignores_requested_cancellation_data()
{
    QTest::addColumn<QString>("stopAction");
    QTest::addColumn<bool>("previousCompletion");
    for (const auto &action :
         { "button", "monitoring", "vpn", "all", "completion", "close", "close_completion" }) {
        QTest::newRow(action) << QString(action) << false;
        QTest::newRow(qPrintable(QString("previous completion, %1").arg(action)))
            << QString(action) << true;
    }
}

void TestMainWindow::shutdown_ignores_requested_cancellation()
{
    QFETCH(QString, stopAction);
    QFETCH(bool, previousCompletion);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "20" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *post = qobject_cast<PostingWidget *>(tabs->widget(2));
    QFile source(root + "/source.bin");
    QVERIFY(source.open(QIODevice::WriteOnly));
    QVERIFY(source.resize(stopAction.endsWith("completion") ? 4096 : 8 * 1024 * 1024));
    source.close();
    PostingJobOptions options;
    options.files = { QFileInfo(source) };
    options.inputPaths = { source.fileName() };
    options.nzbFilePath = root + "/stopped.nzb";
    options.grpList = { "alt.binaries.test" };
    options.from = "poster@example.invalid";
    options.articleSizeBytes = 4096;
    const bool monitoring = stopAction == "monitoring";
    QVERIFY(armTestShutdown(window));
    if (previousCompletion) {
        // A finishes while B is still prepared, granting shutdown eligibility.
        post->addPath(source.fileName(), 0);
        auto *first = window->addNewQuickTab(tabs->count() - 1);
        QVERIFY(addShutdownTestFile(first, root + "/first.bin"));
        first->postFiles(true);
        QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished(), 15000);
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        post->findChild<QPushButton *>("clearFilesButton")->click();
    }
    QPointer<PostingJob> job = new PostingJob(&ngPost, options, monitoring ? nullptr : post);
    if (!monitoring)
        post->attachResumeJob(job, options.files, true);
    bool canceledOnCompletion = false;
    if (stopAction == "completion") {
        connect(job, &PostingJob::filePosted, window, [&] {
            // onNntpFilePosted will finish naturally as soon as this returns,
            // before the queued stop handler can run. Record intent at click time.
            post->findChild<QPushButton *>("postButton")->click();
            canceledOnCompletion = true;
        }, Qt::DirectConnection);
    }
    QVERIFY(ngPost.startPostingJob(job));
    if (stopAction != "completion") {
        if (stopAction != "close_completion") {
            QTRY_VERIFY_WITH_TIMEOUT(job && job->nbArticlesUploaded() > job->nbArticlesFailed(),
                                     15000);
            QVERIFY(!job->hasPostFinished());
        }
        if (stopAction == "button") {
            post->findChild<QPushButton *>("postButton")->click();
        } else if (monitoring) {
            // Called by the confirmed Stop Monitoring action.
            ngPost.closeAllMonitoringJobs();
        } else if (stopAction.startsWith("close")) {
            bool confirmed = false;
            bool blocked = false;
            QElapsedTimer deadline;
            deadline.start();
            QTimer confirm;
            connect(&confirm, &QTimer::timeout, window, [&] {
                for (auto *widget : QApplication::topLevelWidgets()) {
                    auto *box = qobject_cast<QMessageBox *>(widget);
                    if (box && box->text().startsWith("ngPost is currently posting.")) {
                        if (stopAction == "close_completion" && job && deadline.elapsed() < 15000)
                            continue;
                        ngPost.maybeFinishApplication();
                        blocked = ngPost.shutdownStartCountForTest() == 0;
                        confirmed = true;
                        box->done(QMessageBox::Yes);
                    }
                }
            });
            confirm.start(5);
            window->close();
            QVERIFY(confirmed);
            QVERIFY(blocked);
            if (stopAction == "close_completion")
                QVERIFY(job.isNull());
        } else if (stopAction == "all") {
            ngPost.closeAllPostingJobs();
        } else {
            job->pause(PostingJob::PauseReason::VpnRecovery);
            bool chosePreserve = false;
            QTimer choose;
            connect(&choose, &QTimer::timeout, window, [&] {
                for (auto *widget : QApplication::topLevelWidgets()) {
                    auto *box = qobject_cast<QMessageBox *>(widget);
                    if (!box || !box->text().startsWith("The posting job is paused"))
                        continue;
                    for (auto *button : box->buttons())
                        if (box->buttonRole(button) == QMessageBox::RejectRole) {
                            chosePreserve = true;
                            button->click();
                            return;
                        }
                }
            });
            choose.start(5);
            emit ngPost.vpnManager()->recoveryExhausted(VpnManager::FailureKind::TunnelLost);
            choose.stop();
            QVERIFY(chosePreserve);
        }
    }
    QTRY_VERIFY_WITH_TIMEOUT(job.isNull(), 15000);
    QCOMPARE(canceledOnCompletion, stopAction == "completion");
    ngPost.maybeFinishApplication();
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    QVERIFY(!mock.receivedArticles().isEmpty());
    // Cleaning the canceled tab must not accidentally turn its earlier data
    // transfer into an authorization to shut down.
    post->findChild<QPushButton *>("clearFilesButton")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    ngPost.maybeFinishApplication();
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
    QVERIFY(!QFile::exists(root + "/shutdown-marker"));
    // A later, normally completed post can still fulfill the armed request.
    QVERIFY(addShutdownTestFile(post, root + "/next.bin"));
    post->postFiles(true);
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
}

void TestMainWindow::shutdown_test_command_must_match()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    const QString expected = QString("\"%1\" --ngpost-test-shutdown \"%2/shutdown-marker\"")
                                 .arg(QCoreApplication::applicationFilePath(), sandbox.rootPath());
    QVERIFY(!ngPost.allowShutdownCommandForTest(expected));
    QVERIFY(!ngPost.allowShutdownCommandForTest(QString()));
}

void TestMainWindow::shutdown_waits_during_input_dialogs_data()
{
    QTest::addColumn<QString>("dialogKind");
    QTest::addColumn<bool>("selectInput");
    QTest::newRow("history resume, cancel") << QString("history") << false;
    QTest::newRow("resume center, cancel") << QString("resume") << false;
    QTest::newRow("select files, cancel") << QString("files") << false;
    QTest::newRow("select files, accept") << QString("files") << true;
    QTest::newRow("select folder, cancel") << QString("folder") << false;
    QTest::newRow("select folder, accept") << QString("folder") << true;
}

void TestMainWindow::shutdown_waits_during_input_dialogs()
{
    QFETCH(QString, dialogKind);
    QFETCH(bool, selectInput);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *empty = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(first, root + "/first.bin"));
    QVERIFY(QDir().mkpath(root + "/input"));
    QFile input(root + "/input/next.bin");
    QVERIFY(input.open(QIODevice::WriteOnly));
    QCOMPARE(input.write(QByteArray(64000, 'b')), qint64(64000));
    input.close();
    QVERIFY(armTestShutdown(window));
    bool sawDialog = false;
    bool blocked = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || !first->isPostingFinished())
            return;
        answer.stop();
        sawDialog = true;
        // A real post finished in the nested event loop. Neither a queued
        // recheck nor a direct check may start shutdown before the answer.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        ngPost.maybeFinishApplication();
        blocked = ngPost.shutdownStartCountForTest() == 0;
        if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
            if (selectInput) {
                // selectFile() does not update the line edit of an already
                // visible dialog on every Qt platform. Enter the path as a user would.
                auto *name = files->findChild<QLineEdit *>("fileNameEdit");
                if (name) {
                    name->setText(dialogKind == "folder" ? root + "/input" : input.fileName());
                    QMetaObject::invokeMethod(files, "accept", Qt::DirectConnection);
                } else
                    files->reject();
            } else
                files->reject();
        } else
            dialog->done(QMessageBox::No);
    });
    // Bound failures: a missed completion must fail instead of hanging CI.
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, window, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    answer.start(5);
    timeout.start(15000);
    first->postFiles(true);
    if (dialogKind == "history")
        QVERIFY(!window->resumePostForTest(1));
    else if (dialogKind == "resume") {
        auto *table = window->resumeTableForTest();
        const QSignalBlocker selectionSignals(table);
        table->setRowCount(1);
        auto *item = new QTableWidgetItem("test resume");
        item->setData(Qt::UserRole, 1);
        table->setItem(0, 0, item);
        table->selectRow(0);
        QVERIFY(QMetaObject::invokeMethod(window, "_onResumePost", Qt::DirectConnection));
    } else
        QVERIFY(QMetaObject::invokeMethod(empty,
                                          dialogKind == "files" ? "onSelectFilesClicked"
                                                                : "onSelectFolderClicked",
                                          Qt::DirectConnection));
    answer.stop();
    timeout.stop();
    QVERIFY(sawDialog);
    QVERIFY2(blocked, "Shutdown started while waiting for user input");
    if (selectInput) {
        QVERIFY(empty->canSubmit());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        if (dialogKind == "folder")
            // A folder requires compression. Clearing it resolves the blocker
            // without relying on an installed archiver in this dialog test.
            empty->findChild<QPushButton *>("clearFilesButton")->click();
        else
            empty->postFiles(true);
    }
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), selectInput && dialogKind == "files" ? 2 : 1);
}

void TestMainWindow::shutdown_waits_during_post_all_overwrite_data()
{
    QTest::addColumn<bool>("overwrite");
    QTest::newRow("overwrite") << true;
    QTest::newRow("keep existing nzb") << false;
}

void TestMainWindow::shutdown_waits_during_post_all_overwrite()
{
    QFETCH(bool, overwrite);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({ "--slow-mode-ms", "40" }));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *running = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *prepared = window->addNewQuickTab(tabs->count() - 1);
    QVERIFY(addShutdownTestFile(running, root + "/running.bin"));
    QVERIFY(addShutdownTestFile(prepared, root + "/prepared.bin"));
    const auto nzbPath = prepared->findChild<QLineEdit *>("nzbFileEdit")->text();
    QFile existing(nzbPath.endsWith(".nzb") ? nzbPath : nzbPath + ".nzb");
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("original nzb");
    existing.close();
    QVERIFY(armTestShutdown(window));
    running->postFiles(true);

    bool askedWhileRunning = false;
    bool blocked = false;
    QElapsedTimer waited;
    QTimer answer;
    connect(&answer, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *question = qobject_cast<QMessageBox *>(widget);
            if (!question || !question->text().contains("already exists"))
                continue;
            if (!waited.isValid()) {
                waited.start();
                askedWhileRunning = !running->isPostingFinished();
            }
            // Let the running post end inside the question's own event loop.
            if (!running->isPostingFinished() && waited.elapsed() < 15000)
                return;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            ngPost.maybeFinishApplication();
            blocked = running->isPostingFinished() && ngPost.shutdownStartCountForTest() == 0;
            answer.stop();
            question->done(overwrite ? QMessageBox::Yes : QMessageBox::No);
            return;
        }
    });
    answer.start(5);
    window->findChild<QPushButton *>("postAllTabsButton")->click();
    answer.stop();
    QVERIFY(askedWhileRunning);
    QVERIFY2(blocked, "Shutdown started while Post All was asking about an existing nzb");
    if (!overwrite) {
        QVERIFY(prepared->canSubmit());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        ngPost.maybeFinishApplication();
        QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
        prepared->findChild<QPushButton *>("clearFilesButton")->click();
    }
    QTRY_COMPARE_WITH_TIMEOUT(ngPost.shutdownStartCountForTest(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.shutdownInProgressForTest(), 15000);
    QCOMPARE(mock.receivedArticles().size(), overwrite ? 2 : 1);
}

void TestMainWindow::shutdown_rechecks_are_coalesced_and_only_when_armed()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    const auto root = sandbox.rootPath();
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(root, mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *post = qobject_cast<PostingWidget *>(
        window->findChild<QTabWidget *>("postTabWidget")->widget(2));
    const auto addBurst = [&](const QString &prefix) {
        for (int i = 0; i < 50; ++i)
            if (!addShutdownTestFile(post, root + QString("/%1-%2.bin").arg(prefix).arg(i)))
                return false;
        return true;
    };

    QVERIFY(addBurst("idle"));
    post->findChild<QPushButton *>("clearFilesButton")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(ngPost.shutdownRecheckCountForTest(), 0);

    QVERIFY(armTestShutdown(window));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    const int armed = ngPost.shutdownRecheckCountForTest();
    QVERIFY(addBurst("armed"));
    post->findChild<QPushButton *>("clearFilesButton")->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(ngPost.shutdownRecheckCountForTest(), armed + 1);
    QCOMPARE(ngPost.shutdownStartCountForTest(), 0);
}

void TestMainWindow::global_post_controls_pause_resume_and_cancel_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

void TestMainWindow::global_post_controls_pause_resume_and_cancel()
{
    QFETCH(bool, dark);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "100"}));
    const QPalette original = qApp->palette();
    const auto restore = qScopeGuard([&] { qApp->setPalette(original); });
    QPalette palette = original;
    palette.setColor(QPalette::Window, dark ? QColor(30, 30, 30) : QColor(Qt::white));
    palette.setColor(QPalette::WindowText, dark ? QColor(Qt::white) : QColor(Qt::black));
    qApp->setPalette(palette);
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    auto *window = bootWindow(ngPost,
        QString("GROUPS = alt.binaries.test\nthread = 1\nTMP_DIR = %1\nnzbPath = %1\n[server]\nhost = 127.0.0.1\nport = %2\nssl = false\nconnection = 1\n")
            .arg(sandbox.rootPath()).arg(mock.port()), &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *all = window->findChild<QPushButton *>("postAllTabsButton");
    auto *pause = window->findChild<QPushButton *>("pauseButton");
    auto *stop = window->findChild<QPushButton *>("stopAllTabsButton");
    QVERIFY(all && pause && stop);
    QVERIFY(!pause->isEnabled());
    QVERIFY(!stop->isEnabled());
    QVERIFY(!all->icon().isNull());
    QVERIFY(!stop->icon().isNull());
    QCOMPARE(pause->iconSize(), all->iconSize());
    QCOMPARE(stop->iconSize(), all->iconSize());
    QCOMPARE(pause->size(), stop->size());
    QCOMPARE(pause->height(), all->sizeHint().height());
    QCOMPARE(pause->width(), pause->height());
    QCOMPARE(stop->icon().pixmap(stop->iconSize()).toImage(),
             QIcon(":/icons/stop.png").pixmap(stop->iconSize()).toImage());
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    auto *empty = window->addNewQuickTab(0);
    const QList<PostingWidget *> posts{first, second, third};
    for (int i = 0; i < posts.size(); ++i) {
        QCOMPARE(posts[i]->findChild<QPushButton *>("postButton")->text(),
                 QString("Start Quick Post #%1").arg(i + 1));
        const QString path = sandbox.rootPath() + QString("/global%1.bin").arg(i);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(QByteArray(4000000, 'a' + i)), qint64(4000000));
        f.close();
        posts[i]->addPath(path, 0);
    }
    all->click();
    QVERIFY(pause->isEnabled());
    QVERIFY(stop->isEnabled());
    QCOMPARE(tabs->tabBar()->tabTextColor(2),
             dark ? QColor(0x4c, 0xff, 0x4c) : QColor(Qt::darkGreen));
    const auto textIsRendered = [tabs](QColor color) {
        const QImage image = tabs->tabBar()->grab().toImage();
        const QRect area = tabs->tabBar()->tabRect(2).intersected(image.rect());
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x)
                if (image.pixelColor(x, y) == color) return true;
        return false;
    };
    QVERIFY(textIsRendered(dark ? QColor(0x4c, 0xff, 0x4c) : QColor(Qt::darkGreen)));

    pause->click();
    QVERIFY(ngPost.isPaused());
    QCOMPARE(pause->toolTip(), QString("Resume all tabs"));
    QCOMPARE(pause->icon().pixmap(pause->iconSize()).toImage(),
             QIcon(":/icons/play.png").pixmap(pause->iconSize()).toImage());
    QVERIFY(textIsRendered(dark ? QColor(Qt::yellow) : QColor(160, 110, 0)));
    for (auto *post : posts)
        QCOMPARE(tabs->tabBar()->tabTextColor(tabs->indexOf(post)),
                 dark ? QColor(Qt::yellow) : QColor(160, 110, 0));
    QCOMPARE(tabs->tabBar()->tabTextColor(tabs->indexOf(empty)), palette.color(QPalette::WindowText));
    // Finishing the active tab while globally paused must leave the queue held.
    first->findChild<QPushButton *>("postButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(first->isPostingFinished() && !ngPost.isPosting(), 10000);
    QVERIFY(ngPost.hasPostingJobs());
    QVERIFY(pause->isEnabled());
    QVERIFY(stop->isEnabled());
    QVERIFY(second->isPosting());
    QCOMPARE(mock.receivedArticles().size(), 0);
    pause->click();
    QVERIFY(!ngPost.isPaused());
    QVERIFY(ngPost.isPosting());
    QCOMPARE(pause->toolTip(), QString("Pause all tabs"));
    QCOMPARE(pause->icon().pixmap(pause->iconSize()).toImage(),
             QIcon(":/icons/pause.png").pixmap(pause->iconSize()).toImage());
    QTRY_VERIFY_WITH_TIMEOUT(!mock.receivedArticles().isEmpty(), 10000);
    pause->click();
    QVERIFY(ngPost.isPaused());
    stop->click();
    QVERIFY(!stop->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs(), 10000);
    QTRY_VERIFY(second->isPostingFinished() && third->isPostingFinished());
    QVERIFY(!ngPost.isPaused());
    QVERIFY(!pause->isEnabled());
    QVERIFY(!stop->isEnabled());
    QVERIFY(!empty->isPostingFinished());
    for (auto *post : posts) {
        QCOMPARE(tabs->tabBar()->tabTextColor(tabs->indexOf(post)), palette.color(QPalette::WindowText));
        QCOMPARE(post->findChild<QPushButton *>("postButton")->text(),
                 QString("Start Quick Post #%1").arg(post->displayNumber()));
    }
    // A new workflow can be submitted after global cancellation.
    empty->addPath(sandbox.rootPath() + "/global0.bin", 0);
    empty->findChild<QLineEdit *>("nzbFileEdit")->setText(sandbox.rootPath() + "/again.nzb");
    empty->onPostFiles();
    QVERIFY(pause->isEnabled());
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs() && empty->isPostingFinished(), 10000);
}

void TestMainWindow::global_post_controls_translations()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString err;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &err);
    QVERIFY2(window, qPrintable(err));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    for (auto *post : {first, second}) {
        QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + QString("/%1.bin").arg(post->jobNumber())));
        post->onPostFiles();
    }
    ngPost.pause();
    for (const QString lang : {"en", "fr", "de", "es", "nl", "pt", "zh"}) {
        QTranslator translator;
        QVERIFY(translator.load(QString(":/lang/ngPost_%1.qm").arg(lang)));
        qApp->installTranslator(&translator);
        QCoreApplication::processEvents();
        QCOMPARE(third->findChild<QPushButton *>("postButton")->text(),
                 translator.translate("PostingWidget", "Start Quick Post #%1").arg(3));
        QVERIFY(tabs->tabText(tabs->indexOf(third)).endsWith("#3"));
        QCOMPARE(window->findChild<QPushButton *>("pauseButton")->toolTip(),
                 translator.translate("MainWindow", "Resume all tabs"));
        QCOMPARE(window->findChild<QPushButton *>("stopAllTabsButton")->accessibleName(),
                 translator.translate("MainWindow", "Stop all tabs"));
        QCOMPARE(first->findChild<QPushButton *>("postButton")->text(),
                 QCoreApplication::translate("PostingWidget", "Stop Posting"));
        QCOMPARE(second->findChild<QPushButton *>("postButton")->text(),
                 QCoreApplication::translate("PostingWidget", "Cancel Posting"));
        QVERIFY(ngPost.isPaused());
        const auto *all = window->findChild<QPushButton *>("postAllTabsButton");
        for (const auto *control : { window->findChild<QPushButton *>("pauseButton"),
                                     window->findChild<QPushButton *>("stopAllTabsButton") }) {
            QCOMPARE(control->size(), QSize(all->sizeHint().height(), all->sizeHint().height()));
            QCOMPARE(control->iconSize(), all->iconSize());
        }
        QVERIFY(!translator.translate("MainWindow", "Cancel all active and queued posts").isEmpty());
        qApp->removeTranslator(&translator);
    }
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY(!ngPost.hasPostingJobs());
}

void TestMainWindow::global_cancel_during_confirmation()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "100"}));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    for (auto *post : {first, second, third})
        QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + QString("/%1.bin").arg(post->jobNumber())));
    QFile existing(second->findChild<QLineEdit *>("nzbFileEdit")->text());
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("original nzb");
    existing.close();
    bool canceled = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, window, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->text().contains("already exists")) continue;
            if (!canceled) {
                window->findChild<QPushButton *>("stopAllTabsButton")->click();
                canceled = true;
            }
            // The global stop has already finished before the user answers Yes.
            if (ngPost.hasPostingJobs()) return;
            answer.stop();
            box->done(QMessageBox::Yes);
        }
    });
    answer.start(5);
    window->findChild<QPushButton *>("postAllTabsButton")->click();
    answer.stop();
    QVERIFY(canceled);
    QTRY_VERIFY(!ngPost.hasPostingJobs());
    QVERIFY(second->canSubmit());
    QVERIFY(third->canSubmit());
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("original nzb"));
}

void TestMainWindow::global_cancel_external_tool_data()
{
    QTest::addColumn<bool>("successfulExit");
    QTest::addColumn<bool>("parity");
    QTest::addColumn<bool>("prepack");
    QTest::newRow("compressor ignores terminate") << false << false << false;
    QTest::newRow("compressor exits zero after stop") << true << false << false;
    QTest::newRow("parity ignores terminate") << false << true << false;
    QTest::newRow("parity exits zero after stop") << true << true << false;
    QTest::newRow("cancel compression ahead of active post") << false << false << true;
    QTest::newRow("cancel parity ahead of active post") << false << true << true;
}

void TestMainWindow::global_cancel_external_tool()
{
    QFETCH(bool, successfulExit);
    QFETCH(bool, parity);
    QFETCH(bool, prepack);
    HomeSandbox sandbox;
    const QString helper = sandbox.rootPath() + "/ngpost-controlled-tool"
#ifdef Q_OS_WIN
        + ".exe"
#endif
        ;
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "50"}));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port())
                             + (prepack ? "PREPARE_PACKING = true\n" : ""), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *post = qobject_cast<PostingWidget *>(tabs->widget(2));
    if (prepack) {
        QFile input(sandbox.rootPath() + "/active.bin");
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(QByteArray(16000000, 'a')), qint64(16000000));
        input.close();
        post->addPath(input.fileName(), 0);
        post->onPostFiles();
        QTRY_VERIFY_WITH_TIMEOUT(!mock.receivedArticles().isEmpty(), 10000);
        post = window->addNewQuickTab(0);
    }
    QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + "/source.bin"));
    PostingJobOptions options;
    options.files = post->previewFiles();
    options.nzbFilePath = sandbox.rootPath() + "/packed.nzb";
    options.tmpPath = sandbox.rootPath();
    options.rarName = "packed";
    options.rarPath = helper;
    options.rarTool = "rar";
    options.doCompress = !parity;
    options.doPar2 = parity;
    options.par2Tool = par2::Tool::Par2cmdline;
    options.par2Path = helper;
    options.par2Arguments = "c -r10";
    QPointer<PostingJob> job = new PostingJob(&ngPost, options, post);
    QSignalSpy started(job, &PostingJob::postingStarted);
    post->attachResumeJob(job, options.files, !prepack);
    QCOMPARE(ngPost.startPostingJob(job), !prepack);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(helper + ".started"), 10000);
    if (successfulExit) {
        QFile release(helper + ".release");
        QVERIFY(release.open(QIODevice::WriteOnly));
    }
    QElapsedTimer elapsed;
    elapsed.start();
    window->findChild<QPushButton *>("stopAllTabsButton")->click();
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, window, [&] { ++heartbeats; });
    heartbeat.start(10);
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs() && post->isPostingFinished(), 5000);
    QVERIFY2(elapsed.elapsed() < 5000, "Cancellation must kill an uncooperative external tool promptly");
    if (!successfulExit) QVERIFY2(heartbeats > 5, "Cancellation froze the GUI thread");
    QCOMPARE(started.count(), 0);
    if (!prepack) QCOMPARE(mock.receivedArticles().size(), 0);
    QVERIFY(QFile::exists(sandbox.rootPath() + "/source.bin"));
}

void TestMainWindow::canceled_job_never_starts()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *post = qobject_cast<PostingWidget *>(tabs->widget(2));
    QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + "/source.bin"));
    PostingJobOptions options;
    options.files = post->previewFiles();
    options.nzbFilePath = sandbox.rootPath() + "/never-created.nzb";
    QPointer<PostingJob> job = new PostingJob(&ngPost, options, post);
    QSignalSpy started(job, &PostingJob::postingStarted);
    post->attachResumeJob(job, options.files, true);
    QVERIFY(ngPost.startPostingJob(job));
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY(!ngPost.hasPostingJobs() && post->isPostingFinished());
    QCOMPARE(started.count(), 0);
    QVERIFY(!QFile::exists(options.nzbFilePath));
    QCOMPARE(mock.receivedArticles().size(), 0);
}

void TestMainWindow::global_pause_holds_pending_and_new_posts_data()
{
    QTest::addColumn<bool>("preparePacking");
    QTest::addColumn<bool>("cancelPending");
    QTest::newRow("resume queued posts") << false << false;
    QTest::newRow("cancel queued posts") << false << true;
    QTest::newRow("resume with prepare packing") << true << false;
    QTest::newRow("cancel with prepare packing") << true << true;
}

void TestMainWindow::global_pause_holds_pending_and_new_posts()
{
    QFETCH(bool, preparePacking);
    QFETCH(bool, cancelPending);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "50"}));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port())
                             + (preparePacking ? "PREPARE_PACKING = true\n" : ""), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    for (auto *post : {first, second, third})
        QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + QString("/%1.bin").arg(post->jobNumber())));
    first->onPostFiles();
    second->onPostFiles();
    auto *pause = window->findChild<QPushButton *>("pauseButton");
    auto *stop = window->findChild<QPushButton *>("stopAllTabsButton");
    pause->click();
    // Pause retains its meaning after the current post has been canceled.
    first->onPostFiles();
    QTRY_VERIFY(first->isPostingFinished() && !ngPost.isPosting());
    third->onPostFiles();
    QVERIFY(third->isPosting());
    QVERIFY(ngPost.isPaused());
    QVERIFY(!ngPost.isPosting());
    QVERIFY(pause->isEnabled() && stop->isEnabled());
    if (cancelPending) stop->click();
    else pause->click();
    QTRY_VERIFY_WITH_TIMEOUT(second->isPostingFinished() && third->isPostingFinished(), 10000);
    QTRY_VERIFY(!ngPost.hasPostingJobs());
    QCOMPARE(mock.receivedArticles().size(), cancelPending ? 0 : 2);
    QVERIFY(!pause->isEnabled() && !stop->isEnabled());
    QVERIFY(!ngPost.isPaused());
}

void TestMainWindow::quick_post_numbers_icons_and_palette()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    window->closeTab(second);
    auto *fourth = window->addNewQuickTab(0);
    tabs->tabBar()->moveTab(tabs->indexOf(fourth), tabs->indexOf(third));
    for (auto *post : {third, fourth}) {
        QVERIFY(tabs->tabText(tabs->indexOf(post)).endsWith(QString("#%1").arg(post->displayNumber())));
        QCOMPARE(post->findChild<QPushButton *>("postButton")->text(),
                 QString("Start Quick Post #%1").arg(post->displayNumber()));
    }
    QCOMPARE(fourth->displayNumber(), 4u);
    const QImage icon = tabs->tabIcon(2).pixmap(24, 24).toImage();
    QVERIFY(!icon.isNull());
    bool yellow = false;
    for (int y = 0; y < icon.height(); ++y)
        for (int x = 0; x < icon.width(); ++x) {
            const QColor color = icon.pixelColor(x, y);
            yellow |= color.alpha() > 200 && color.red() > 220 && color.green() > 170 && color.blue() < 100;
        }
    QVERIFY2(yellow, "The Quick Post lightning must render yellow with the installed Qt plugins");
    const auto original = qApp->palette();
    const auto restore = qScopeGuard([&] { qApp->setPalette(original); });
    for (bool dark : {true, false, true}) {
        auto palette = original;
        palette.setColor(QPalette::Window, dark ? QColor(30, 30, 30) : QColor(Qt::white));
        palette.setColor(QPalette::WindowText, dark ? QColor(Qt::white) : QColor(Qt::black));
        qApp->setPalette(palette);
        QCoreApplication::processEvents();
        QCOMPARE(tabs->tabBar()->tabTextColor(2), dark ? QColor(Qt::white) : QColor(Qt::black));
    }
}

void TestMainWindow::quick_post_numbering_lifecycle_and_reset()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    QVERIFY(first);
    QCOMPARE(first->jobNumber(), 1u);
    QCOMPARE(first->displayNumber(), 1u);
    QVERIFY(tabs->tabText(2).endsWith("#1"));

    // 1. Sequential additions when idle
    auto *second = window->addNewQuickTab(0);
    auto *third = window->addNewQuickTab(0);
    auto *fourth = window->addNewQuickTab(0);
    QCOMPARE(second->displayNumber(), 2u);
    QCOMPARE(third->displayNumber(), 3u);
    QCOMPARE(fourth->displayNumber(), 4u);
    QCOMPARE(second->jobNumber(), 2u);
    QCOMPARE(third->jobNumber(), 3u);
    QCOMPARE(fourth->jobNumber(), 4u);

    // 2. Close an intermediate tab (#2) while others remain open: must not reuse #2
    window->closeTab(second);
    auto *fifth = window->addNewQuickTab(0);
    QCOMPARE(fifth->displayNumber(), 5u);
    QCOMPARE(fifth->jobNumber(), 5u);

    // 3. Close the highest tab (#5) while others (#3, #4) remain open: must not reuse #5
    window->closeTab(fifth);
    auto *sixth = window->addNewQuickTab(0);
    QCOMPARE(sixth->displayNumber(), 6u);
    QCOMPARE(sixth->jobNumber(), 6u);

    // 4. Simulate active queue / running post on first tab
    PostingJobOptions options;
    options.grpList = { "alt.binaries.test" };
    options.from = "test@example.invalid";
    options.nzbFilePath = sandbox.rootPath() + "/test.nzb";
    QPointer<PostingJob> job = new PostingJob(&ngPost, options, first);
    first->attachResumeJob(job, {}, true);
    QVERIFY(first->isPosting());

    // Close all remaining extra tabs while the queue is running
    window->closeTab(third);
    window->closeTab(fourth);
    window->closeTab(sixth);
    QCOMPARE(window->findChild<QTabWidget *>("postTabWidget")->count(), 4);

    // Adding a tab while the queue is running must NOT go backwards or reset to 2
    auto *seventh = window->addNewQuickTab(0);
    QCOMPARE(seventh->displayNumber(), 7u);
    QCOMPARE(seventh->jobNumber(), 7u);

    // 5. Job finishes, but extra tab #7 is still open: must not reset
    first->onPostingJobDone();
    delete job;
    QVERIFY(!first->isPosting());
    auto *eighth = window->addNewQuickTab(0);
    QCOMPARE(eighth->displayNumber(), 8u);

    // 6. Close all extra tabs now that everything is idle: reset occurs
    window->closeTab(seventh);
    window->closeTab(eighth);
    QCOMPARE(window->findChild<QTabWidget *>("postTabWidget")->count(), 4);

    // Now tout est terminé ET fermé: next tab resets cleanly to #2
    auto *resetTab = window->addNewQuickTab(0);
    QCOMPARE(resetTab->displayNumber(), 2u);
    QCOMPARE(resetTab->jobNumber(), 2u);
    QVERIFY(tabs->tabText(tabs->indexOf(resetTab)).endsWith("#2"));
    QCOMPARE(resetTab->findChild<QPushButton *>("postButton")->text(),
             QString("Start Quick Post #2"));

    // Next tab continues monotonically to #3
    auto *afterReset = window->addNewQuickTab(0);
    QCOMPARE(afterReset->displayNumber(), 3u);
    QCOMPARE(afterReset->jobNumber(), 3u);
}

void TestMainWindow::quick_post_numbering_with_backend_queue_data()
{
    QTest::addColumn<bool>("pendingOnly");
    QTest::newRow("active monitor job") << false;
    QTest::newRow("paused pending monitor job") << true;
}

void TestMainWindow::quick_post_numbering_with_backend_queue()
{
    QFETCH(bool, pendingOnly);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *extra = window->addNewQuickTab(0);
    QCOMPARE(extra->jobNumber(), 2u);
    auto *first = qobject_cast<PostingWidget *>(
        window->findChild<QTabWidget *>("postTabWidget")->widget(2));
    QVERIFY(addShutdownTestFile(first, sandbox.rootPath() + "/source.bin"));
    PostingJobOptions options;
    options.files = first->previewFiles();
    options.grpList = { "alt.binaries.test" };
    options.from = "test@example.invalid";
    options.nzbFilePath = sandbox.rootPath() + "/monitor.nzb";
    QPointer<PostingJob> active = new PostingJob(&ngPost, options);
    QVERIFY(ngPost.startPostingJob(active));
    ngPost.pause();
    if (pendingOnly) {
        options.nzbFilePath = sandbox.rootPath() + "/pending.nzb";
        auto *pending = new PostingJob(&ngPost, options);
        QVERIFY(!ngPost.startPostingJob(pending));
        emit active->stopPosting();
        QTRY_VERIFY(!ngPost.isPosting());
    }
    QVERIFY(ngPost.hasPostingJobs());
    QVERIFY(!first->isPosting());
    window->closeTab(extra);
    auto *next = window->addNewQuickTab(0);
    QCOMPARE(next->jobNumber(), 3u);
    window->closeTab(next);
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY(!ngPost.hasPostingJobs());
    auto *reset = window->addNewQuickTab(0);
    QCOMPARE(reset->jobNumber(), 2u);
    QCOMPARE(mock.receivedArticles().size(), 0);
}

void TestMainWindow::quick_post_numbering_from_new_and_auto_tabs()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    QVERIFY(
        QMetaObject::invokeMethod(tabs->tabBar(), "tabBarClicked", Q_ARG(int, tabs->count() - 1)));
    auto *second = qobject_cast<PostingWidget *>(tabs->widget(3));
    QVERIFY(second);
    QCOMPARE(second->jobNumber(), 2u);
    auto *automatic = window->findChild<AutoPostWidget *>();
    QVERIFY(automatic);
    automatic->findChild<QCheckBox *>("compressCB")->setChecked(false);
    automatic->findChild<QCheckBox *>("startJobsCB")->setChecked(false);
    auto *files = automatic->findChild<QListWidget *>("filesList");
    QVERIFY(files);
    for (int i = 0; i < 2; ++i) {
        QFile file(sandbox.rootPath() + QString("/auto-%1.bin").arg(i));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("test");
        file.close();
        files->addItem(file.fileName());
    }
    QVERIFY(QMetaObject::invokeMethod(automatic, "onGenQuickPosts"));
    QCOMPARE(tabs->count(), 7);
    for (int index = 4; index <= 5; ++index) {
        auto *post = qobject_cast<PostingWidget *>(tabs->widget(index));
        QVERIFY(post);
        QCOMPARE(post->jobNumber(), uint(index - 1));
        QCOMPARE(post->previewFiles().size(), 1);
        QVERIFY(tabs->tabText(index).endsWith(QString("#%1").arg(post->jobNumber())));
    }
    // The UI close path also preserves the high-water mark.
    QVERIFY(QMetaObject::invokeMethod(tabs->tabBar(), "tabCloseRequested", Q_ARG(int, 5)));
    QCOMPARE(window->addNewQuickTab(0)->jobNumber(), 5u);
    QVERIFY(!ngPost.hasPostingJobs());
}

void TestMainWindow::progress_label_tracks_the_running_post_data()
{
    QTest::addColumn<int>("number");
    QTest::newRow("default quick post") << 1;
    QTest::newRow("moved quick post after closing a tab") << 3;
    QTest::newRow("monitor job without a posting tab") << -1;
}

void TestMainWindow::progress_label_tracks_the_running_post()
{
    QFETCH(int, number);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *label = window->findChild<QLabel *>("jobLabel");
    QVERIFY(label);
    QCOMPARE(label->text(), QString("<b><u>Post #1</u></b>"));
    auto *post = qobject_cast<PostingWidget *>(tabs->widget(2));
    if (number == 3) {
        auto *second = window->addNewQuickTab(0);
        post = window->addNewQuickTab(0);
        window->closeTab(second);
        auto *fourth = window->addNewQuickTab(0);
        tabs->tabBar()->moveTab(tabs->indexOf(post), tabs->indexOf(fourth));
    }
    QVERIFY(addShutdownTestFile(post, sandbox.rootPath() + "/source.bin"));
    PostingJobOptions options;
    options.files = post->previewFiles();
    options.grpList = { "alt.binaries.test" };
    options.from = "test@example.invalid";
    options.nzbFilePath = sandbox.rootPath() + "/progress.nzb";
    auto *job = new PostingJob(&ngPost, options, number > 0 ? post : nullptr);
    QSignalSpy started(job, &PostingJob::postingStarted);
    if (number > 0)
        post->attachResumeJob(job, options.files, true);
    QVERIFY(ngPost.startPostingJob(job));
    // Hold the real transfer while exercising selection and language changes.
    ngPost.pause();
    QTRY_COMPARE(started.count(), 1);
    const QString expected = QString("<b><u>Post #%1</u></b>")
                                 .arg(number > 0 ? QString::number(number) : "Auto");
    QTRY_COMPARE(label->text(), expected);
    if (number > 0) {
        QFile incoming(sandbox.rootPath() + "/incoming.bin");
        QVERIFY(incoming.open(QIODevice::WriteOnly));
        incoming.write("queued monitor input");
        incoming.close();
        QVERIFY(QMetaObject::invokeMethod(&ngPost,
                                          "onNewFileToProcess",
                                          Qt::DirectConnection,
                                          Q_ARG(QFileInfo, QFileInfo(incoming.fileName()))));
        QCOMPARE(label->text(), expected);
    }
    // Select History, whose index is unrelated to the running post's number.
    tabs->setCurrentIndex(0);
    for (const QString &language : ngPost.languages()) {
        ngPost.changeLanguage(language);
        QCoreApplication::processEvents();
        QCOMPARE(label->text(), expected);
    }
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY(!ngPost.hasPostingJobs());
    // Completed progress keeps its identity even after another retranslation.
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(window, &languageChange);
    QCOMPARE(label->text(), expected);
}

void TestMainWindow::global_cancel_preserves_history_and_resume_data()
{
    QTest::addColumn<bool>("compressed");
    QTest::newRow("source files") << false;
    QTest::newRow("generated archive") << true;
}

void TestMainWindow::global_cancel_preserves_history_and_resume()
{
    QFETCH(bool, compressed);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start({"--slow-mode-ms", "60"}));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = {arg0.data(), nullptr};
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, shutdownTestConfig(sandbox.rootPath(), mock.port()), &error);
    QVERIFY2(window, qPrintable(error));
    auto *history = ngPost.historyService();
    QVERIFY(history);
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(2));
    auto *queued = window->addNewQuickTab(0);
    const QString source = sandbox.rootPath() + "/source.bin";
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(8192, 'h')), qint64(8192));
    file.close();
    PostingJobOptions options;
    options.files = {QFileInfo(source)};
    options.inputPaths = {source};
    options.grpList = {"alt.binaries.test"};
    options.from = "test@example.invalid";
    options.nzbFilePath = sandbox.rootPath() + "/first.nzb";
    options.articleSizeBytes = 512;
    options.tmpPath = sandbox.rootPath();
    options.rarName = "archive";
    options.rarTool = "rar";
    options.doCompress = compressed;
    if (compressed) {
        options.rarPath = sandbox.rootPath() + "/ngpost-recording-history"
#ifdef Q_OS_WIN
            + ".exe"
#endif
            ;
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), options.rarPath));
    }
    QPointer<PostingJob> active = new PostingJob(&ngPost, options, first);
    const qint64 activeId = active->historyPostId();
    QSignalSpy activeFinished(active, &PostingJob::postingFinished);
    first->attachResumeJob(active, options.files, true);
    QVERIFY(ngPost.startPostingJob(active));
    options.nzbFilePath = sandbox.rootPath() + "/queued.nzb";
    options.doCompress = false;
    QPointer<PostingJob> pending = new PostingJob(&ngPost, options, queued);
    const qint64 pendingId = pending->historyPostId();
    QSignalSpy pendingFinished(pending, &PostingJob::postingFinished);
    queued->attachResumeJob(pending, options.files, false);
    QVERIFY(!ngPost.startPostingJob(pending));
    QTRY_VERIFY_WITH_TIMEOUT(active && active->nbArticlesUploaded() > 0, 10000);
    ngPost.pause();
    QVERIFY(ngPost.isPaused());
    QVERIFY(history->flush(&error));
    PostHistoryStore::PostDetails paused;
    QVERIFY(history->loadPostDetails(activeId, &paused, &error));
    QCOMPARE(paused.post.status, QString("posting"));
    int expectedArticles = 0;
    qint64 expectedSize = 0;
    for (const auto &storedFile : paused.files) {
        expectedArticles += storedFile.totalArticles;
        expectedSize += storedFile.sizeBytes;
    }
    QVERIFY(expectedArticles > 1);
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs() && first->isPostingFinished()
                            && queued->isPostingFinished(), 10000);
    QVERIFY(!first->canSubmit() && !queued->canSubmit());
    QCOMPARE(activeFinished.count(), 1);
    QCOMPARE(pendingFinished.count(), 1);
    QVERIFY(history->flush(&error));
    auto posts = history->listPosts({}, &error);
    QCOMPARE(posts.size(), 2);
    PostHistoryStore::PostDetails stopped, unstarted;
    QVERIFY(history->loadPostDetails(activeId, &stopped, &error));
    QVERIFY(history->loadPostDetails(pendingId, &unstarted, &error));
    QCOMPARE(stopped.post.status, QString("failed"));
    QCOMPARE(stopped.post.nbArticles, expectedArticles);
    QCOMPARE(stopped.post.sizeBytes, expectedSize);
    QVERIFY(!stopped.post.finishedAt.isEmpty());
    QCOMPARE(unstarted.post.status, QString("failed"));
    QVERIFY(unstarted.files.isEmpty());
    QCOMPARE(unstarted.post.nbArticles, 0);
    QMap<QString, QString> confirmedIds;
    for (const auto &articles : stopped.articlesByFile)
        for (const auto &article : articles) {
            QVERIFY(article.status != "posting");
            if (article.status == "posted")
                confirmedIds.insert(QString::number(article.fileId) + ":" + QString::number(article.part), article.msgId);
        }
    QVERIFY(!confirmedIds.isEmpty());
    for (const auto &storedFile : stopped.files)
        QVERIFY2(QFile::exists(storedFile.originalPath), qPrintable(storedFile.originalPath));
    PostHistoryService::ResumeRow decision;
    QVERIFY(history->checkResume(activeId, &decision, &error));
    QVERIFY2(decision.state != "not_resumable" && !decision.state.isEmpty(), qPrintable(decision.reason));
    QVERIFY(!history->checkResume(pendingId, &decision, &error));
    QCOMPARE(decision.state, QString("not_resumable"));
    // Cancel a retry before its queued start: no new row, no rewritten outcome.
    auto *retry = window->addNewQuickTab(0);
    QVERIFY2(ngPost.resumePostGui(activeId, retry, &error), qPrintable(error));
    ngPost.cancelAllPostingJobs();
    QTRY_VERIFY(!ngPost.hasPostingJobs() && retry->isPostingFinished());
    PostHistoryStore::PostDetails unchanged;
    QVERIFY(history->loadPostDetails(activeId, &unchanged, &error));
    QCOMPARE(unchanged.post.status, stopped.post.status);
    QCOMPARE(unchanged.post.finishedAt, stopped.post.finishedAt);
    QCOMPARE(unchanged.post.avgSpeed, stopped.post.avgSpeed);
    QCOMPARE(unchanged.post.nbFailedArticles, stopped.post.nbFailedArticles);
    QCOMPARE(history->listPosts({}, &error).size(), 2);
    // Exercise the history UI caller too: it must create the next numbered tab.
    QTimer::singleShot(0, window, [] {
        if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            question->done(QMessageBox::Yes);
    });
    QVERIFY(window->resumePostForTest(activeId));
    auto *resumed = qobject_cast<PostingWidget *>(tabs->currentWidget());
    QVERIFY(resumed);
    QCOMPARE(resumed->jobNumber(), retry->jobNumber() + 1);
    QVERIFY(
        tabs->tabText(tabs->indexOf(resumed)).endsWith(QString("#%1").arg(resumed->jobNumber())));
    // A real retry completes the same historical post and consolidates its NZB.
    QTRY_VERIFY_WITH_TIMEOUT(resumed->isPostingFinished() && !ngPost.hasPostingJobs(), 15000);
    PostHistoryStore::PostDetails done;
    QVERIFY(history->loadPostDetails(activeId, &done, &error));
    QCOMPARE(done.post.status, QString("success"));
    QCOMPARE(done.post.nbArticles, expectedArticles);
    QCOMPARE(done.post.sizeBytes, expectedSize);
    QCOMPARE(done.post.nbFailedArticles, 0);
    QCOMPARE(history->listPosts({}, &error).size(), 2);
    int total = 0;
    for (const auto &articles : done.articlesByFile)
        for (const auto &article : articles) {
            ++total;
            QCOMPARE(article.status, QString("posted"));
            const auto key = QString::number(article.fileId) + ":" + QString::number(article.part);
            if (confirmedIds.contains(key)) QCOMPARE(article.msgId, confirmedIds.value(key));
        }
    QCOMPARE(total, expectedArticles);
    QFile nzb(done.nzbPath);
    QVERIFY(nzb.open(QIODevice::ReadOnly));
    QCOMPARE(nzb.readAll().count("<segment "), expectedArticles);
    QVERIFY(QMetaObject::invokeMethod(window, "_onHistoryRefresh", Qt::DirectConnection));
    QTRY_COMPARE(window->resumeTableForTest()->rowCount(), 0);
}

void TestMainWindow::auto_posts_can_retry_preparation_failures_data()
{
    QTest::addColumn<bool>("compress");
    QTest::addColumn<bool>("missing");
    QTest::newRow("missing par2") << false << true;
    QTest::newRow("failed par2") << false << false;
    QTest::newRow("missing compressor") << true << true;
    QTest::newRow("failed compressor") << true << false;
}

void TestMainWindow::auto_posts_can_retry_preparation_failures()
{
    QFETCH(bool, compress);
    QFETCH(bool, missing);
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    const QString suffix =
#ifdef Q_OS_WIN
        ".exe";
#else
        "";
#endif
    const QString broken = sandbox.rootPath() + "/ngpost-recording-fail" + suffix;
    const QString working = sandbox.rootPath() + "/ngpost-recording-retry" + suffix;
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), broken));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), working));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(
        ngPost,
        shutdownTestConfig(sandbox.rootPath(), mock.port())
            + QString("TMP_DIR = %1\nRAR_SOURCE = custom\nRAR_PATH = %2\n"
                      "PAR2_TOOL = par2cmdline\nPAR2_SOURCE = custom\nPAR2_PATH = %2\n")
                  .arg(sandbox.rootPath(), broken),
        &error);
    QVERIFY2(window, qPrintable(error));
    if (missing)
        QVERIFY(QFile::remove(broken));
    auto *automatic = window->findChild<AutoPostWidget *>();
    automatic->findChild<QCheckBox *>("compressCB")->setChecked(compress);
    automatic->findChild<QCheckBox *>("par2CB")->setChecked(!compress);
    automatic->findChild<QCheckBox *>("startJobsCB")->setChecked(true);
    auto *files = automatic->findChild<QListWidget *>("filesList");
    for (int i = 0; i < 2; ++i) {
        QFile source(sandbox.rootPath() + QString("/source-%1.bin").arg(i));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("retry source");
        source.close();
        files->addItem(source.fileName());
    }
    QVERIFY(QMetaObject::invokeMethod(automatic, "onGenQuickPosts"));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    QCOMPARE(tabs->count(), 6);
    auto *first = qobject_cast<PostingWidget *>(tabs->widget(3));
    auto *second = qobject_cast<PostingWidget *>(tabs->widget(4));
    QVERIFY(first && second);
    QTRY_VERIFY(!ngPost.hasPostingJobs() && first->isPostingFinished()
                && second->isPostingFinished());
    QCOMPARE(mock.receivedArticles().size(), 0);
    auto *all = window->findChild<QPushButton *>("postAllTabsButton");
    auto *stop = window->findChild<QPushButton *>("stopAllTabsButton");
    QVERIFY(first->canSubmit() && second->canSubmit());
    QVERIFY(all->isEnabled());
    QVERIFY(!stop->isEnabled());
    const auto sources = first->previewFiles();
    const auto nzb = first->findChild<QLineEdit *>("nzbFileEdit")->text();
    const auto password = first->findChild<QLineEdit *>("nzbPassEdit")->text();
    // An unchanged configuration may fail repeatedly without locking the tabs.
    all->click();
    QTRY_VERIFY(!ngPost.hasPostingJobs() && first->canSubmit() && second->canSubmit());
    QCOMPARE(first->previewFiles(), sources);
    QCOMPARE(first->findChild<QLineEdit *>("nzbFileEdit")->text(), nzb);
    QCOMPARE(first->findChild<QLineEdit *>("nzbPassEdit")->text(), password);
    if (compress) {
        CompressionSettingsDialog dialog(&ngPost, window);
        dialog.findChild<QLineEdit *>("rarEdit")->setText(working);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    } else {
        Par2SettingsDialog dialog(&ngPost, {}, false, false, window);
        dialog.findChild<QLineEdit *>("par2Path")->setText(working);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    all->click();
    QVERIFY(stop->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs() && first->isPostingFinished()
                                 && second->isPostingFinished(),
                             10000);
    QVERIFY(!mock.receivedArticles().isEmpty());
    QVERIFY(!first->canSubmit() && !second->canSubmit());
    QVERIFY(!all->isEnabled() && !stop->isEnabled());
    QCOMPARE(tabs->count(), 6);
    QVERIFY(ngPost.historyService()->flush(&error));
    const auto history = ngPost.historyService()->listPosts({}, &error);
    QCOMPARE(history.size(), 6);
    int successes = 0;
    for (const auto &post : history)
        successes += post.status == "success";
    QCOMPARE(successes, 2);
}

void TestMainWindow::preparation_retry_restores_sources_after_packing()
{
    HomeSandbox sandbox;
    ngpost::tests::MockNntpServer mock;
    QVERIFY(mock.start());
    const QString helper = sandbox.rootPath() + "/ngpost-recording-retry-source"
#ifdef Q_OS_WIN
        + ".exe"
#endif
        ;
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost,
                              shutdownTestConfig(sandbox.rootPath(), mock.port())
                                  + QString("TMP_DIR = %1\nRAR_SOURCE = custom\nRAR_PATH = %2\n")
                                        .arg(sandbox.rootPath(), helper),
                              &error);
    QVERIFY2(window, qPrintable(error));
    auto *post = window->addNewQuickTab(0);
    const QString source = sandbox.rootPath() + "/source.bin";
    QVERIFY(addShutdownTestFile(post, source));
    post->findChild<QCheckBox *>("compressCB")->setChecked(true);
    post->findChild<QCheckBox *>("par2CB")->setChecked(false);
    post->findChild<QLineEdit *>("compressNameEdit")->setText("retry-archive");
    auto *nzb = post->findChild<QLineEdit *>("nzbFileEdit");
    nzb->setText(sandbox.rootPath() + "/missing/output.nzb");
    post->onPostFiles();
    QTRY_VERIFY(!ngPost.hasPostingJobs() && post->isPostingFinished());
    QVERIFY(post->canSubmit());
    QCOMPARE(mock.receivedArticles().size(), 0);
    QCOMPARE(post->previewFiles(), QFileInfoList{ QFileInfo(source) });
    nzb->setText(sandbox.rootPath() + "/fixed.nzb");
    // Individual retry uses the restored sources too.
    post->findChild<QPushButton *>("postButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!ngPost.hasPostingJobs() && post->isPostingFinished(), 10000);
    QVERIFY2(!mock.receivedArticles().isEmpty(),
             qPrintable(window->findChild<QTextBrowser *>("logBrowser")->toPlainText()));
    QVERIFY(!post->canSubmit());
    QVERIFY(QFile::exists(source));
}

void TestMainWindow::posting_controls_do_not_overlap_tab_scrollers()
{
    HomeSandbox sandbox;
    int argc = 1;
    QByteArray arg0("tst_MainWindow");
    char *argv[] = { arg0.data(), nullptr };
    NgPost ngPost(argc, argv);
    QString error;
    auto *window = bootWindow(ngPost, "GROUPS = alt.binaries.test\n", &error);
    QVERIFY2(window, qPrintable(error));
    auto *tabs = window->findChild<QTabWidget *>("postTabWidget");
    auto *all = window->findChild<QPushButton *>("postAllTabsButton");
    for (int i = 0; i < 20; ++i)
        window->addNewQuickTab(0);
    window->show();
    for (const QString &language : { QString("fr"), QString("en"), QString("de") }) {
        ngPost.changeLanguage(language);
        for (int width : { 900, 1200 }) {
            window->resize(width, 800);
            QCoreApplication::processEvents();
            const QRect controls(all->mapTo(tabs, QPoint()), all->size());
            int visibleScrollers = 0;
            for (auto *button : tabs->tabBar()->findChildren<QToolButton *>()) {
                if (!button->isVisible())
                    continue;
                ++visibleScrollers;
                const QRect arrow(button->mapTo(tabs, QPoint()), button->size());
                QVERIFY2(!arrow.intersects(controls),
                         qPrintable(QString("%1 at %2px: arrow [%3,%4], controls [%5,%6]")
                                        .arg(language)
                                        .arg(width)
                                        .arg(arrow.left())
                                        .arg(arrow.right())
                                        .arg(controls.left())
                                        .arg(controls.right())));
                QVERIFY(tabs->rect().contains(arrow));
            }
            QCOMPARE(visibleScrollers, 2);
        }
    }
}
