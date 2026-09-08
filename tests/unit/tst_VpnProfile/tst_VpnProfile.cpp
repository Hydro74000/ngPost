// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_VpnProfile.cpp — VpnProfile struct + VpnManager::Backend enum helpers.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QPointer>

#include "nntp/NntpServerParams.h"
#include "PostingJob.h"
#include "TestEnv.h"
#include "utils/PathHelper.h"
#include "vpn/OpenVpnBackend.h"
#include "vpn/VpnBackend.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"
#include "vpn/VpnProtocol.h"

using ngpost::tests::HomeSandbox;

namespace {
struct TestVpnHelperFile
{
    QString path;
    bool created;

    TestVpnHelperFile()
        : path(), created(false)
    {
        QDir appDir(QCoreApplication::applicationDirPath());
        appDir.mkpath(QStringLiteral("vpn/scripts"));
        path = appDir.filePath(QStringLiteral("vpn/scripts/ngpost-vpn-helper.sh"));
        if (QFileInfo::exists(path))
            return;

        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("#!/bin/sh\nexit 0\n");
            f.close();
            QFile::setPermissions(path,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                QFileDevice::ReadOther | QFileDevice::ExeOther);
            created = true;
        }
    }

    ~TestVpnHelperFile()
    {
        if (created)
            QFile::remove(path);
    }
};

class FakeVpnBackend : public VpnBackend
{
public:
    explicit FakeVpnBackend(QObject *parent = nullptr)
        : VpnBackend(parent)
    {}

    bool start(QString const &) override { return true; }
    void stop() override
    {
        ++stopCalls;
        running = false;
        _requestStop();
        _emitTerminationOnce(VpnTerminationKind::RequestedStop,
                             VpnFailureKind::None, QString());
    }
    void stopAndWait(int) override
    {
        ++stopAndWaitCalls;
        running = false;
        // Deliberately synchronous: VpnManager must have disconnected this
        // signal before calling us or onBackendStopped() will re-enter cleanup.
        _requestStop();
        _emitTerminationOnce(VpnTerminationKind::RequestedStop,
                             VpnFailureKind::None, QString());
    }
    bool isRunning() const override { return running; }
    void setActive(bool active) override
    {
        ++setActiveCalls;
        lastActive = active;
    }

    void fail(QString const &reason,
              VpnFailureKind failure = VpnFailureKind::Internal) {
        _emitTerminationOnce(VpnTerminationKind::StartFailure,
                             failure, reason);
    }
    void reportSameIncidentTwice() {
        _markReady();
        _emitTerminationOnce(VpnTerminationKind::UnexpectedExit,
                             VpnFailureKind::HelperExited, QStringLiteral("crashed"));
        _emitTerminationOnce(VpnTerminationKind::UnexpectedExit,
                             VpnFailureKind::ProcessExited, QStringLiteral("finished"));
    }
    void reportHealth(VpnBackendHealth health, QString const &reason = QString()) {
        emit healthChanged(health, reason);
    }
    void crashAfterReady() {
        _markReady();
        running = false;
        _emitTerminationOnce(VpnTerminationKind::UnexpectedExit,
                             VpnFailureKind::HelperExited,
                             QStringLiteral("helper killed"));
    }
    void becomeReady() {
        _markReady();
        emit ready(QStringLiteral("lo"), QHostAddress(QHostAddress::LocalHost),
                   QHostAddress());
    }

    bool running = true;
    int stopCalls = 0;
    int stopAndWaitCalls = 0;
    int setActiveCalls = 0;
    bool lastActive = false;
};
}

class TestVpnProfile : public QObject
{
    Q_OBJECT

private slots:
    //! Default-constructed profile is invalid (name + configFileName empty).
    void default_profile_is_invalid();

    //! A profile is "valid" iff both `name` and `configFileName` are non-empty.
    void isValid_requires_name_and_config_file();

    //! absoluteConfigPath() is "<vpnDir>/<configFileName>". Empty configFile →
    //! empty result (guarded path).
    void absoluteConfigPath_under_vpnDir();
    //! A profile loaded through -c remains relative to that config folder.
    void absoluteConfigPath_uses_declaring_config_folder();
    void absoluteConfigPath_empty_when_configFile_empty();

    //! backendToString() ↔ backendFromString() round-trip for both backends.
    void backend_roundtrip();

    //! backendFromString() accepts both the canonical name AND a short alias,
    //! is case-insensitive, and tolerates surrounding whitespace.
    void backendFromString_accepts_aliases();
    void backendFromString_is_case_insensitive();
    void backendFromString_trims_whitespace();

    //! Unknown backend strings set ok=false and fall back to OpenVPN.
    void backendFromString_unknown_sets_ok_false();

    //! Lease waiting and recovery limits have safe defaults and remain inside
    //! the ranges accepted by the configuration parser.
    void recovery_settings_have_safe_defaults_and_bounds();

    //! The global VPN_AUTO_CONNECT switch is ignored when no active VPN profile
    //! is selected/configured.
    void master_switch_ignored_without_active_vpn_profile();
    void queued_vpn_requirement_remains_fail_closed();

    //! Per-server Use VPN remains fail-closed and logs clear guidance when VPN
    //! setup is incomplete.
    void per_server_useVpn_blocks_and_logs_guidance_when_vpn_incomplete();

    //! Once the service pipe has gone away, the management socket/retry loop
    //! still means OpenVPN is alive and must be stopped during teardown.
    void openvpn_windows_activity_includes_management_phase();
    void openvpn_management_password_prompt_is_framed_without_losing_bytes();

    //! A terminal failure detaches the backend before stopping it, and an
    //! auto-started Wait job is notified even though retainForJob() has not run.
    void backend_failure_stops_once_and_notifies_waiting_job();
    void synchronous_lease_busy_keeps_typed_state();
    void backend_reports_one_terminal_event_per_run();
    void helper_specific_error_then_finished_reports_one_terminal_event();
    void auto_disconnect_callback_is_guarded_after_suspect();
    void auto_disconnect_ignores_retained_recovery_job();
    void typed_pause_reason_preserves_user_priority();
    void helper_exit_with_active_job_enters_recovery_once();
    void recreated_backend_reasserts_retained_job_activity();
    void repeated_internal_recovery_obeys_finite_budget();
    void confirmed_down_promotes_internal_recovery_immediately();
    void releasing_last_job_stops_active_recovery();
    void startup_failure_cannot_enter_runtime_recovery();
    void linux_owner_manifest_rejects_partial_or_duplicated_records();
    void helper_v2_marker_is_checked_before_elevation();
    void protocol_v2_recognizes_every_message();
    void protocol_v2_rejects_duplicate_or_malformed_fields();

    //! Windows WireGuard profile edits preserve the old service until the new
    //! one is ready and undo partial transitions on failure. Hooks make the
    //! service transaction portable and guarantee no UAC in tests.
    void wireguard_update_service_transaction_rolls_back();
};

void TestVpnProfile::default_profile_is_invalid()
{
    VpnProfile p;
    QCOMPARE(p.name, QString());
    QCOMPARE(p.configFileName, QString());
    QCOMPARE(p.hasAuth, false);
    QCOMPARE(p.backend, VpnManager::Backend::OpenVPN);
    QVERIFY(!p.isValid());
}

void TestVpnProfile::isValid_requires_name_and_config_file()
{
    VpnProfile p;
    p.name = "MyProf";
    QVERIFY2(!p.isValid(), "profile with name but no configFileName must be invalid");

    p.name = "";
    p.configFileName = "myprof.ovpn";
    QVERIFY2(!p.isValid(), "profile with configFileName but no name must be invalid");

    p.name = "MyProf";
    QVERIFY(p.isValid());
}

void TestVpnProfile::absoluteConfigPath_under_vpnDir()
{
    HomeSandbox sandbox;

    VpnProfile p;
    p.name = "MyProf";
    p.configFileName = "myprof.ovpn";

    const QString abs = p.absoluteConfigPath();
    QCOMPARE(abs, PathHelper::vpnDir() + QStringLiteral("/myprof.ovpn"));
    QVERIFY(abs.startsWith(sandbox.rootPath()));
}

void TestVpnProfile::absoluteConfigPath_uses_declaring_config_folder()
{
    HomeSandbox sandbox;
    VpnProfile p;
    p.name = QStringLiteral("Explicit");
    p.configFileName = QStringLiteral("explicit.ovpn");
    p.configBaseDir = sandbox.rootPath() + QStringLiteral("/old-config");

    QCOMPARE(p.absoluteConfigPath(),
             QDir(p.configBaseDir).filePath(QStringLiteral("vpn/explicit.ovpn")));
    QVERIFY(!p.absoluteConfigPath().startsWith(PathHelper::vpnDir()));
}

void TestVpnProfile::absoluteConfigPath_empty_when_configFile_empty()
{
    HomeSandbox sandbox;

    VpnProfile p;
    p.name = "Anon";

    QCOMPARE(p.absoluteConfigPath(), QString());
}

void TestVpnProfile::backend_roundtrip()
{
    using Backend = VpnManager::Backend;

    for (Backend b : { Backend::OpenVPN, Backend::WireGuard }) {
        bool ok = false;
        const QString s = VpnManager::backendToString(b);
        const Backend back = VpnManager::backendFromString(s, &ok);
        QVERIFY2(ok, qPrintable(QStringLiteral("backendFromString failed for canonical name '%1'").arg(s)));
        QCOMPARE(back, b);
    }
}

void TestVpnProfile::backendFromString_accepts_aliases()
{
    bool ok = false;

    QCOMPARE(VpnManager::backendFromString("openvpn", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("ovpn", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("wireguard", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);

    QCOMPARE(VpnManager::backendFromString("wg", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_is_case_insensitive()
{
    bool ok = false;
    QCOMPARE(VpnManager::backendFromString("OpenVPN", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("WIREGUARD", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("WG", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_trims_whitespace()
{
    bool ok = false;
    QCOMPARE(VpnManager::backendFromString("  openvpn  ", &ok), VpnManager::Backend::OpenVPN);
    QVERIFY(ok);
    QCOMPARE(VpnManager::backendFromString("\twireguard\n", &ok), VpnManager::Backend::WireGuard);
    QVERIFY(ok);
}

void TestVpnProfile::backendFromString_unknown_sets_ok_false()
{
    bool ok = true;
    const auto fallback = VpnManager::backendFromString("noway", &ok);
    QVERIFY2(!ok, "ok should be false for unknown backend string");
    QCOMPARE(fallback, VpnManager::Backend::OpenVPN); // documented default

    ok = true;
    VpnManager::backendFromString(QString(), &ok);
    QVERIFY(!ok);
}

void TestVpnProfile::recovery_settings_have_safe_defaults_and_bounds()
{
    VpnManager manager;
    QCOMPARE(manager.leaseWaitMinutes(), 5);
    QCOMPARE(manager.recoveryMaxAttempts(), 0);

    manager.setCliMode(false);
    QCOMPARE(manager.effectiveLeaseWaitMinutes(), 0);
    manager.setCliMode(true);
    QCOMPARE(manager.effectiveLeaseWaitMinutes(), 5);

    manager.setLeaseWaitMinutes(-1);
    QCOMPARE(manager.leaseWaitMinutes(), 0);
    manager.setLeaseWaitMinutes(2000);
    QCOMPARE(manager.leaseWaitMinutes(), 1440);

    manager.setRecoveryMaxAttempts(-1);
    QCOMPARE(manager.recoveryMaxAttempts(), 0);
    manager.setRecoveryMaxAttempts(2000);
    QCOMPARE(manager.recoveryMaxAttempts(), 1000);
}

void TestVpnProfile::master_switch_ignored_without_active_vpn_profile()
{
    HomeSandbox sandbox;
    TestVpnHelperFile helper;

    VpnManager manager;
    manager.setAutoConnect(true);
    if (!manager.vpnFeatureAvailable())
        QSKIP("No VPN helper/prerequisite available on this platform");

    QVERIFY2(!manager.forceAllConnectionsThroughVpn(),
             "VPN_AUTO_CONNECT must be neutral when no active profile is selected");

    NntpServerParams server(QStringLiteral("news.example.org"));
    QList<NntpServerParams *> servers;
    servers << &server;

    QVERIFY(!manager.jobNeedsVpn(servers));
    QCOMPARE(manager.admitJob(servers), VpnManager::Admission::Proceed);
}

void TestVpnProfile::queued_vpn_requirement_remains_fail_closed()
{
    HomeSandbox sandbox;
    TestVpnHelperFile helper;
    VpnManager manager;
    QList<NntpServerParams *> noLiveVpnRequest;

    QCOMPARE(manager.admitJob(noLiveVpnRequest), VpnManager::Admission::Proceed);
    // This models a job that entered the queue while VPN use was enabled and
    // whose requirement was frozen before the live settings changed.
    QCOMPARE(manager.admitJob(noLiveVpnRequest, true), VpnManager::Admission::Blocked);

    NntpServerParams nowRequiresVpn(QStringLiteral("news.example.org"));
    nowRequiresVpn.useVpn = true;
    QList<NntpServerParams *> changedLiveConfig{&nowRequiresVpn};
    // The inverse is just as important: a queued job frozen as direct must
    // not silently acquire a VPN dependency from later configuration edits.
    QCOMPARE(manager.admitJob(changedLiveConfig, false),
             VpnManager::Admission::Proceed);
}

void TestVpnProfile::per_server_useVpn_blocks_and_logs_guidance_when_vpn_incomplete()
{
    HomeSandbox sandbox;
    VpnManager manager;

    NntpServerParams server(QStringLiteral("news.example.org"));
    server.useVpn = true;
    QList<NntpServerParams *> servers;
    servers << &server;

    int unavailableCount = 0;
    QString unavailableDetail;
    QObject::connect(&manager, &VpnManager::vpnRequiredButUnavailable,
                     &manager,
                     [&](VpnManager::JobBlockReason, QString const &detail) {
        ++unavailableCount;
        unavailableDetail = detail;
    });

    int statusCount = 0;
    QString statusLine;
    QObject::connect(&manager, &VpnManager::statusLine,
                     &manager,
                     [&](QString const &line) {
        ++statusCount;
        statusLine = line;
    });

    QCOMPARE(manager.admitJob(servers), VpnManager::Admission::Blocked);
    QCOMPARE(unavailableCount, 1);
    QVERIFY2(statusCount > 0, "per-server Use VPN refusal must be written to the post log");

    QVERIFY(unavailableDetail.contains(QStringLiteral("Use VPN is enabled")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("news.example.org")));

    if (!VpnManager::vpnPlatformSupported()) {
        // Still fail-closed -- posting in the clear to a server the user marked
        // VPN-only is the one outcome worth refusing -- but the guidance has to
        // be actionable. There is no VPN dialog and no helper to install here,
        // so the only instruction that works is "clear the flag".
        QVERIFY(unavailableDetail.contains(
            QStringLiteral("no VPN support on this operating system")));
        QVERIFY(unavailableDetail.contains(QStringLiteral("clear its Use VPN")));
        QVERIFY2(!unavailableDetail.contains(QStringLiteral("click Install")),
                 "must not point at a helper that cannot exist on this platform");
        QVERIFY2(!unavailableDetail.contains(QStringLiteral("VPN button")),
                 "must not point at a button that is hidden on this platform");
        return;
    }

    QVERIFY(unavailableDetail.contains(QStringLiteral("VPN is not correctly configured")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("VPN button")));
    QVERIFY(unavailableDetail.contains(QStringLiteral("Use VPN checkbox")));
    QCOMPARE(statusLine, unavailableDetail);
}

void TestVpnProfile::openvpn_windows_activity_includes_management_phase()
{
    QVERIFY(!OpenVpnBackend::windowsActivityForTest(false, false, false));
    QVERIFY(OpenVpnBackend::windowsActivityForTest(true, false, false));
    QVERIFY(OpenVpnBackend::windowsActivityForTest(false, true, false));
    QVERIFY(OpenVpnBackend::windowsActivityForTest(false, false, true));
}

//! openvpn's management password prompt carries no newline, and the management
//! reader cuts on newlines only. Until the prompt is framed as a line it never
//! reaches the parser, no password is ever sent, and the connection stops for
//! good on "authenticating" -- observed against openvpn 2.7.4 on Windows.
void TestVpnProfile::openvpn_management_password_prompt_is_framed_without_losing_bytes()
{
    // The case that stalled: nothing but the unterminated prompt.
    QByteArray buffer("ENTER PASSWORD:");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ENTER PASSWORD:\n"));

    // Bytes ahead of the prompt belong to the parser too. A password refused
    // and prompted again is exactly the case where dropping them would hide
    // the only line saying why.
    buffer = QByteArray("ERROR: bad password\r\nENTER PASSWORD:");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ERROR: bad password\r\nENTER PASSWORD:\n"));

    // Bytes after it as well, and no second newline is manufactured.
    buffer = QByteArray("ENTER PASSWORD:>INFO:already talking\r\n");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ENTER PASSWORD:\n>INFO:already talking\r\n"));

    // Already a line: idempotent, in either line ending.
    buffer = QByteArray("ENTER PASSWORD:\n");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ENTER PASSWORD:\n"));

    buffer = QByteArray("ENTER PASSWORD:\r\n");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ENTER PASSWORD:\r\n"));

    // No prompt, no edit -- including a half-received one, which the next read
    // will complete.
    buffer = QByteArray(">INFO:OpenVPN Management Interface\r\n");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray(">INFO:OpenVPN Management Interface\r\n"));

    buffer = QByteArray("ENTER PASSW");
    OpenVpnBackend::terminateMgmtPasswordPromptForTest(buffer);
    QCOMPARE(buffer, QByteArray("ENTER PASSW"));
}

void TestVpnProfile::backend_failure_stops_once_and_notifies_waiting_job()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    QPointer<FakeVpnBackend> guard(backend);
    manager.setBackendForTest(backend, VpnManager::State::Starting);
    manager.setAutoStartedByJobForTest(true);

    QSignalSpy unavailableSpy(&manager, &VpnManager::vpnRequiredButUnavailable);
    backend->fail(QStringLiteral("fixture failure"));

    QCOMPARE(manager.state(), VpnManager::State::Failed);
    QVERIFY(!manager.hasBackendForTest());
    QCOMPARE(backend->stopCalls, 0);
    QCOMPARE(backend->stopAndWaitCalls, 1);
    QCOMPARE(unavailableSpy.size(), 1);
    QCOMPARE(unavailableSpy.first().at(0).value<VpnManager::JobBlockReason>(),
             VpnManager::JobBlockReason::VpnFailed);
    QCOMPARE(unavailableSpy.first().at(1).toString(), QStringLiteral("fixture failure"));
    QVERIFY(!manager.isAutoStarted());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(guard.isNull());
}

void TestVpnProfile::synchronous_lease_busy_keeps_typed_state()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Starting);
    manager.setBackendStartInProgressForTest(true);

    backend->fail(QStringLiteral("owned by another instance"),
                  VpnFailureKind::LeaseBusy);
    QVERIFY(!manager.finishBackendStartForTest(false));

    QCOMPARE(manager.state(), VpnManager::State::LeaseBusy);
    QVERIFY(!manager.hasBackendForTest());
}

void TestVpnProfile::backend_reports_one_terminal_event_per_run()
{
    FakeVpnBackend backend;
    backend.beginRun(42);
    QSignalSpy terminalSpy(&backend, &VpnBackend::terminated);
    backend.reportSameIncidentTwice();

    QCOMPARE(terminalSpy.size(), 1);
    BackendTermination const event =
        terminalSpy.first().first().value<BackendTermination>();
    QCOMPARE(event.runId, quint64(42));
    QCOMPARE(event.kind, VpnTerminationKind::UnexpectedExit);
    QCOMPARE(event.failure, VpnFailureKind::HelperExited);
    QVERIFY(event.wasReady);
}

void TestVpnProfile::helper_specific_error_then_finished_reports_one_terminal_event()
{
    OpenVpnBackend backend;
    backend.beginRun(73);
    QSignalSpy terminalSpy(&backend, &VpnBackend::terminated);

    backend.handleProtocolLineForTest(QStringLiteral("PROTOCOL 2"));
    backend.handleProtocolLineForTest(QStringLiteral(
        "LEASE_UNAVAILABLE path=/run/lock/ngpost-vpn.lock detail=permission%20denied"));
    backend.finishProcessForTest(1, QProcess::NormalExit);

    QCOMPARE(terminalSpy.size(), 1);
    BackendTermination const event =
        terminalSpy.first().first().value<BackendTermination>();
    QCOMPARE(event.runId, quint64(73));
    QCOMPARE(event.kind, VpnTerminationKind::StartFailure);
    QCOMPARE(event.failure, VpnFailureKind::LeaseUnavailable);
    QCOMPARE(event.detail, QStringLiteral("permission denied"));
}

void TestVpnProfile::auto_disconnect_callback_is_guarded_after_suspect()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Connected);
    manager.setAutoStartedByJobForTest(true);

    // Releasing the last healthy job arms the normal 30 s grace timer.  A
    // subsequent health transition must both cancel that timer and make its
    // callback harmless if an already-queued timeout is delivered anyway.
    manager.retainForJob();
    manager.releaseForJob();
    backend->reportHealth(VpnBackendHealth::Suspect, QStringLiteral("stale handshake"));
    QCOMPARE(manager.health(), VpnManager::VpnHealth::Suspect);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onAutoDisconnectTimeout",
                                      Qt::DirectConnection));
    QCOMPARE(backend->stopCalls, 0);
    QCOMPARE(manager.state(), VpnManager::State::Connected);
}

void TestVpnProfile::auto_disconnect_ignores_retained_recovery_job()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Connected);
    manager.setAutoStartedByJobForTest(true);
    manager.retainForJob();

    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));
    QVERIFY(manager.hasActiveVpnJobs());
    QVERIFY(QMetaObject::invokeMethod(&manager, "onAutoDisconnectTimeout",
                                      Qt::DirectConnection));
    QCOMPARE(backend->stopCalls, 0);
    QCOMPARE(manager.state(), VpnManager::State::Reconnecting);
}

void TestVpnProfile::typed_pause_reason_preserves_user_priority()
{
    using Reason = PostingJob::PauseReason;
    QCOMPARE(PostingJob::mergePauseReasonForTest(Reason::ConnectionBackoff,
                                                 Reason::VpnRecovery),
             Reason::VpnRecovery);
    QCOMPARE(PostingJob::mergePauseReasonForTest(Reason::VpnRecovery,
                                                 Reason::ConnectionBackoff),
             Reason::VpnRecovery);
    QCOMPARE(PostingJob::mergePauseReasonForTest(Reason::VpnRecovery,
                                                 Reason::User),
             Reason::User);
    QCOMPARE(PostingJob::mergePauseReasonForTest(Reason::User,
                                                 Reason::VpnRecovery),
             Reason::User);
}

void TestVpnProfile::helper_exit_with_active_job_enters_recovery_once()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Connected);
    manager.setAutoStartedByJobForTest(true);
    manager.retainForJob();

    QSignalSpy interruptedSpy(&manager, &VpnManager::vpnInterrupted);
    backend->crashAfterReady();

    QCOMPARE(interruptedSpy.size(), 1);
    QCOMPARE(interruptedSpy.first().first().value<VpnManager::FailureKind>(),
             VpnManager::FailureKind::HelperExited);
    QCOMPARE(manager.state(), VpnManager::State::Reconnecting);
    QCOMPARE(manager.health(), VpnManager::VpnHealth::Restarting);
    QVERIFY(manager.hasActiveVpnJobs());
    QVERIFY(!manager.hasBackendForTest());

    // A second delivery of the same backend terminal event is suppressed at
    // its source and therefore cannot consume or announce another episode.
    backend->crashAfterReady();
    QCOMPARE(interruptedSpy.size(), 1);
}

void TestVpnProfile::recreated_backend_reasserts_retained_job_activity()
{
    HomeSandbox sandbox;
    VpnManager manager;

    // Recovery preserves this reference count while the dead backend is gone.
    manager.retainForJob();
    auto *replacement = new FakeVpnBackend;
    manager.setBackendForTest(replacement, VpnManager::State::Starting);
    QCOMPARE(replacement->setActiveCalls, 0);

    replacement->becomeReady();

    QCOMPARE(manager.state(), VpnManager::State::Connected);
    QCOMPARE(replacement->setActiveCalls, 1);
    QVERIFY(replacement->lastActive);
}

void TestVpnProfile::repeated_internal_recovery_obeys_finite_budget()
{
    HomeSandbox sandbox;
    VpnManager manager;
    manager.setRecoveryMaxAttempts(1);
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Starting);
    backend->becomeReady();
    QCOMPARE(manager.state(), VpnManager::State::Connected);
    manager.retainForJob();

    QSignalSpy interruptedSpy(&manager, &VpnManager::vpnInterrupted);
    QSignalSpy exhaustedSpy(&manager, &VpnManager::recoveryExhausted);
    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));
    QCOMPARE(interruptedSpy.size(), 1);
    QCOMPARE(manager.health(), VpnManager::VpnHealth::RecoveringInternally);

    // Repeated management notifications are one episode, not attempts.
    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));
    QCOMPARE(interruptedSpy.size(), 1);
    QCOMPARE(exhaustedSpy.size(), 0);

    backend->reportHealth(VpnBackendHealth::Healthy);
    QCOMPARE(manager.state(), VpnManager::State::Connected);
    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));

    QCOMPARE(exhaustedSpy.size(), 1);
    QCOMPARE(exhaustedSpy.first().first().value<VpnManager::FailureKind>(),
             VpnManager::FailureKind::TunnelLost);
    QCOMPARE(manager.state(), VpnManager::State::Failed);
    QCOMPARE(backend->stopAndWaitCalls, 1);
    QVERIFY(!manager.hasBackendForTest());
    QVERIFY(manager.hasActiveVpnJobs());

    // The GUI Retry action resets the finite budget while retaining the job;
    // it may only resume after a later READY + Healthy transition.
    QVERIFY(manager.retryVpn());
    QCOMPARE(manager.state(), VpnManager::State::Reconnecting);
}

void TestVpnProfile::confirmed_down_promotes_internal_recovery_immediately()
{
    HomeSandbox sandbox;
    VpnManager manager;
    manager.setRecoveryMaxAttempts(1);
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Starting);
    backend->becomeReady();
    manager.retainForJob();

    QSignalSpy exhaustedSpy(&manager, &VpnManager::recoveryExhausted);
    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));
    QCOMPARE(manager.health(), VpnManager::VpnHealth::RecoveringInternally);

    backend->reportHealth(VpnBackendHealth::Down,
                          QStringLiteral("PROCESS_EXITED"));
    QCOMPARE(manager.health(), VpnManager::VpnHealth::Restarting);
    // The internal episode consumed the only allowed attempt. A confirmed
    // DOWN therefore reaches exhaustion on the immediate external-rebuild
    // callback, rather than waiting for the old 180 s timer.
    QTRY_COMPARE_WITH_TIMEOUT(exhaustedSpy.size(), 1, 1000);
    QCOMPARE(manager.state(), VpnManager::State::Failed);
}

void TestVpnProfile::releasing_last_job_stops_active_recovery()
{
    HomeSandbox sandbox;
    VpnManager manager;
    auto *backend = new FakeVpnBackend;
    manager.setBackendForTest(backend, VpnManager::State::Starting);
    backend->becomeReady();
    manager.setAutoStartedByJobForTest(true);
    manager.retainForJob();
    backend->reportHealth(VpnBackendHealth::RecoveringInternally,
                          QStringLiteral("RECONNECTING"));

    manager.releaseForJob();
    QCOMPARE(manager.state(), VpnManager::State::Disabled);
    QVERIFY(!manager.hasActiveVpnJobs());
    QVERIFY(!manager.isAutoStarted());
}

void TestVpnProfile::startup_failure_cannot_enter_runtime_recovery()
{
    HomeSandbox sandbox;
    VpnManager manager;
    QSignalSpy interruptedSpy(&manager, &VpnManager::vpnInterrupted);

    manager.requestRecovery(VpnManager::FailureKind::LeaseBusy);
    QCOMPARE(manager.state(), VpnManager::State::Disabled);
    QCOMPARE(manager.health(), VpnManager::VpnHealth::Healthy);
    QCOMPARE(interruptedSpy.size(), 0);
}

void TestVpnProfile::linux_owner_manifest_rejects_partial_or_duplicated_records()
{
    QByteArray const valid = QByteArrayLiteral(
        "v=2 owner_pid=123 owner_start=456 helper_pid=789 backend=wireguard "
        "session=123-456-789 phase=running config=/run/ngpost-vpn/session/config "
        "config_sha=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
        "vpn_pid=0 vpn_start=0 exe= iface=ngpost-wg0 tun_ip=10.0.0.2 "
        "route=1 rule=1 interface=1 end=1\n");
    qint64 ownerPid = 0;
    QString ownerStart;
    QVERIFY(VpnManager::linuxOwnerManifestForTest(valid, &ownerPid, &ownerStart));
    QCOMPARE(ownerPid, qint64(123));
    QCOMPARE(ownerStart, QStringLiteral("456"));

    QVERIFY(!VpnManager::linuxOwnerManifestForTest(valid.left(valid.size() / 2),
                                                    nullptr, nullptr));
    QVERIFY(!VpnManager::linuxOwnerManifestForTest(valid + valid,
                                                    nullptr, nullptr));
    QByteArray duplicatedField = valid;
    duplicatedField.replace("owner_pid=123", "owner_pid=123 owner_pid=999");
    QVERIFY(!VpnManager::linuxOwnerManifestForTest(duplicatedField,
                                                    nullptr, nullptr));
}

void TestVpnProfile::helper_v2_marker_is_checked_before_elevation()
{
    QVERIFY(!VpnManager::helperDeclaresProtocol2ForTest(
        QByteArrayLiteral("#!/bin/bash\n# legacy helper\n")));
    QVERIFY(!VpnManager::helperDeclaresProtocol2ForTest(
        QByteArrayLiteral("#!/bin/bash\n# NGPOST_VPN_HELPER_PROTOCOL=2\n")));
    QVERIFY(VpnManager::helperDeclaresProtocol2ForTest(
        QByteArrayLiteral("#!/bin/bash\nreadonly NGPOST_VPN_HELPER_PROTOCOL=2\n")));

    QFile current(QString::fromLatin1(NGPOST_SOURCE_ROOT)
                  + QStringLiteral("/src/vpn/scripts/ngpost-vpn-helper.sh"));
    QVERIFY(current.open(QIODevice::ReadOnly));
    QVERIFY(VpnManager::helperDeclaresProtocol2ForTest(current.read(4096)));
}

void TestVpnProfile::protocol_v2_recognizes_every_message()
{
    struct Fixture { const char *line; VpnProtocol::Type type; };
    const Fixture fixtures[] = {
        {"PROTOCOL 2", VpnProtocol::Type::Protocol},
        {"READY attempt_id=0 iface=wg0 ip=10.0.0.2 dns=-", VpnProtocol::Type::Ready},
        {"WAITING owner_pid=1 helper_pid=2 owner_uid=3 backend=wireguard since=4 deadline=5", VpnProtocol::Type::Waiting},
        {"BUSY owner_pid=1 helper_pid=2 owner_uid=3 backend=openvpn since=4 age=5", VpnProtocol::Type::Busy},
        {"SUSPECT backend=wireguard reason=stale", VpnProtocol::Type::Suspect},
        {"HEALTHY backend=wireguard", VpnProtocol::Type::Healthy},
        {"DOWN backend=wireguard reason=gone", VpnProtocol::Type::Down},
        {"ERROR failure=internal detail=bad", VpnProtocol::Type::Error},
        {"RESTART_FAILED attempt_id=2 failure=tunnel_lost detail=bad", VpnProtocol::Type::RestartFailed},
        {"LEASE_TIMEOUT owner_pid=1 waited_seconds=300", VpnProtocol::Type::LeaseTimeout},
        {"LEASE_UNAVAILABLE path=/run detail=denied", VpnProtocol::Type::LeaseUnavailable},
        {"RUNTIME_NOT_VOLATILE path=/run fstype=ext4", VpnProtocol::Type::RuntimeNotVolatile},
        {"UNATTRIBUTED_VPN_STATE resources=wg0", VpnProtocol::Type::UnattributedVpnState},
        {"LEGACY_OWNER_ACTIVE owner_pid=1 resources=openvpn", VpnProtocol::Type::LegacyOwnerActive},
        {"LOG level=info detail=hello%20world", VpnProtocol::Type::Log}
    };
    QCOMPARE(VpnProtocol::keywords().size(), 15);
    for (Fixture const &fixture : fixtures) {
        VpnProtocol::Message const parsed = VpnProtocol::parse(QString::fromLatin1(fixture.line));
        QCOMPARE(parsed.type, fixture.type);
    }
    QCOMPARE(VpnProtocol::parse(QStringLiteral("LOG level=info detail=hello%20world")).detail,
             QStringLiteral("hello world"));
}

void TestVpnProfile::protocol_v2_rejects_duplicate_or_malformed_fields()
{
    QCOMPARE(VpnProtocol::parse(QStringLiteral("BUSY owner_pid=1 owner_pid=2")).type,
             VpnProtocol::Type::Invalid);
    QCOMPARE(VpnProtocol::parse(QStringLiteral("READY attempt_id")).type,
             VpnProtocol::Type::Invalid);
    QVERIFY(VpnProtocol::parse(QStringLiteral("ERROR old helper text")).legacy);
}

void TestVpnProfile::wireguard_update_service_transaction_rolls_back()
{
    HomeSandbox sandbox;
    VpnManager manager;

    VpnProfile oldProfile;
    oldProfile.name = QStringLiteral("old");
    oldProfile.backend = VpnManager::Backend::WireGuard;
    oldProfile.configFileName = QStringLiteral("old.conf");
    oldProfile.configBaseDir = sandbox.rootPath();
    manager.setProfilesFromConfig({ oldProfile }, oldProfile.name);

    QStringList actions;
    manager.setWireGuardServiceHooksForTest(
        [&](QString const &path) {
            actions << QStringLiteral("register:") + path;
            return true;
        },
        [&](QString const &service) {
            actions << QStringLiteral("unregister:") + service;
            return true;
        });

    // A display-name-only edit leaves the unchanged WG service alone.
    VpnProfile renamed = oldProfile;
    renamed.name = QStringLiteral("renamed");
    QVERIFY(manager.updateProfile(oldProfile.name, renamed, false));
    QVERIFY(actions.isEmpty());

    // Replacing the contents behind the same basename requires U -> R.
    QVERIFY(manager.updateProfile(renamed.name, renamed, true));
    QCOMPARE(actions,
             QStringList({ QStringLiteral("unregister:WireGuardTunnel$old"),
                           QStringLiteral("register:") + renamed.absoluteConfigPath() }));

    // If installing that replacement fails, the old file is restored before
    // the old service is registered again, and the profile remains untouched.
    actions.clear();
    int registerCalls = 0;
    bool oldContentsRestored = false;
    bool rollbackRegisterSawOldContents = false;
    manager.setWireGuardServiceHooksForTest(
        [&](QString const &path) {
            actions << QStringLiteral("register:") + path;
            ++registerCalls;
            if (registerCalls == 1)
                return false; // replacement fails
            rollbackRegisterSawOldContents = oldContentsRestored;
            return oldContentsRestored;
        },
        [&](QString const &service) {
            actions << QStringLiteral("unregister:") + service;
            return true;
        });
    VpnProfile failedReplacement = renamed;
    failedReplacement.name = QStringLiteral("must-not-commit");
    QVERIFY(!manager.updateProfile(
        renamed.name, failedReplacement, true,
        [&]() {
            actions << QStringLiteral("restore-config");
            oldContentsRestored = true;
            return true;
        }));
    QCOMPARE(manager.profiles().first().name, renamed.name);
    QVERIFY(rollbackRegisterSawOldContents);
    QCOMPARE(actions,
             QStringList({ QStringLiteral("unregister:WireGuardTunnel$old"),
                           QStringLiteral("register:") + renamed.absoluteConfigPath(),
                           QStringLiteral("restore-config"),
                           QStringLiteral("register:") + renamed.absoluteConfigPath() }));

    // With a different basename the safe order is R(new), U(old). If U(old)
    // fails, U(new) rolls the newly-created service back.
    actions.clear();
    VpnProfile moved = renamed;
    moved.configFileName = QStringLiteral("new.conf");
    manager.setWireGuardServiceHooksForTest(
        [&](QString const &path) {
            actions << QStringLiteral("register:") + path;
            return true;
        },
        [&](QString const &service) {
            actions << QStringLiteral("unregister:") + service;
            return service != QStringLiteral("WireGuardTunnel$old");
        });
    QVERIFY(!manager.updateProfile(renamed.name, moved, true));
    QCOMPARE(manager.profiles().first().configFileName, renamed.configFileName);
    QCOMPARE(actions,
             QStringList({ QStringLiteral("register:") + moved.absoluteConfigPath(),
                           QStringLiteral("unregister:WireGuardTunnel$old"),
                           QStringLiteral("unregister:WireGuardTunnel$new") }));
}

QTEST_GUILESS_MAIN(TestVpnProfile)
#include "tst_VpnProfile.moc"
