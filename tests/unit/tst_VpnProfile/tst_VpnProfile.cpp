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
#include "TestEnv.h"
#include "utils/PathHelper.h"
#include "vpn/OpenVpnBackend.h"
#include "vpn/VpnBackend.h"
#include "vpn/VpnManager.h"
#include "vpn/VpnProfile.h"

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
        emit stopped();
    }
    void stopAndWait(int) override
    {
        ++stopAndWaitCalls;
        running = false;
        // Deliberately synchronous: VpnManager must have disconnected this
        // signal before calling us or onBackendStopped() will re-enter cleanup.
        emit stopped();
    }
    bool isRunning() const override { return running; }

    void fail(QString const &reason) { emit failed(reason); }

    bool running = true;
    int stopCalls = 0;
    int stopAndWaitCalls = 0;
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

    //! The global VPN_AUTO_CONNECT switch is ignored when no active VPN profile
    //! is selected/configured.
    void master_switch_ignored_without_active_vpn_profile();

    //! Per-server Use VPN remains fail-closed and logs clear guidance when VPN
    //! setup is incomplete.
    void per_server_useVpn_blocks_and_logs_guidance_when_vpn_incomplete();

    //! Once the service pipe has gone away, the management socket/retry loop
    //! still means OpenVPN is alive and must be stopped during teardown.
    void openvpn_windows_activity_includes_management_phase();

    //! A terminal failure detaches the backend before stopping it, and an
    //! auto-started Wait job is notified even though retainForJob() has not run.
    void backend_failure_stops_once_and_notifies_waiting_job();

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
