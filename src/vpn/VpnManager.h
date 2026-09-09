//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNMANAGER_H
#define VPNMANAGER_H

#include "VpnBackend.h"
#include "VpnPlatform.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QTimer;
struct NntpServerParams;
struct VpnProfile;

class VpnManager : public QObject
{
    Q_OBJECT
public:
    enum class State { Disabled, Starting, Connected, LeaseBusy, Reconnecting, Stopping, Failed };
    Q_ENUM(State)

    enum class Backend { OpenVPN, WireGuard };
    Q_ENUM(Backend)

    using FailureKind = VpnFailureKind;

    enum class VpnHealth { Healthy, Suspect, RecoveringInternally, Restarting };
    Q_ENUM(VpnHealth)

    //! Reason why a job cannot start despite needing the VPN.
    enum class JobBlockReason {
        None,                   //!< all good
        HelperNotInstalled,
        NoConfigSelected,
        ConfigUnreadable,
        VpnFailed,              //!< VPN attempted to start but failed
        LeaseBusy,
        HelperOutdated,
        UnattributedVpnState,
        RecoveryExhausted
    };
    Q_ENUM(JobBlockReason)

    explicit VpnManager(QObject *parent = nullptr);
    ~VpnManager() override;

    bool          start();
    void          stop();
    //! User-initiated stop. Unlike shutdown teardown, this first turns any
    //! active VPN job into a durable User pause.
    void          disconnectByUser();
    State         state() const { return _state; }
    QHostAddress  tunIp() const { return _tunIp; }
    QString       tunInterface() const { return _tunIface; }
    //! DNS server pushed by the VPN, populated from the helper READY message.
    //! Used by NntpConnection to perform DNS resolution through the VPN.
    QHostAddress  dnsServer() const { return _dnsServer; }
    //! Global override: when true, the VPN auto-starts on every
    //! job AND every NNTP connection (across all servers) binds to the tun,
    //! regardless of per-server `useVpn`. When false, per-server `useVpn`
    //! decides which servers route through the tunnel.
    //!
    //! The master switch is *neutralised at runtime when the VPN cannot be used*
    //! (`vpnFeatureAvailable()` plus a selected/readable active profile). A
    //! `VPN_AUTO_CONNECT = true` hand-edited into the config on a machine with no
    //! helper or no VPN profile must NOT block posting — it falls back to direct
    //! connections. The per-server `useVpn` checkbox stays the real fail-closed
    //! guard. `autoConnect()` returns the raw stored value and is preserved for
    //! the config plumbing that persists the `VPN_AUTO_CONNECT` key, so the
    //! user's choice survives a save.
    bool          forceAllConnectionsThroughVpn() const;
    bool          autoConnect() const { return _autoConnect; }
    bool          isConnected() const { return _state == State::Connected; }
    VpnHealth     health() const { return _health; }
    //! True when the master switch is enabled but would be ignored because the
    //! VPN helper/prerequisites are present while no usable active profile is
    //! selected/configured. GUI callers use this to warn before posting direct.
    bool          shouldConfirmMasterSwitchWithoutProfile(QString *detail = nullptr) const;

    void setAutoConnect(bool v);
    void setLeaseWaitMinutes(int minutes);
    int leaseWaitMinutes() const { return _leaseWaitMinutes; }
    int effectiveLeaseWaitMinutes() const { return _cliMode ? _leaseWaitMinutes : 0; }
    void setRecoveryMaxAttempts(int attempts);
    int recoveryMaxAttempts() const { return _recoveryMaxAttempts; }
    void setCliMode(bool cli) { _cliMode = cli; }
    static QString currentProcessStartTime();

    //! Convenience getters that read from the active profile, used by older
    //! call sites awaiting the profile-aware UI rework. Return defaults if
    //! no active profile is set.
    Backend backend() const;
    QString configPath() const;

    //! Profile management.
    QList<VpnProfile> const &profiles() const { return _profiles; }
    VpnProfile const *activeProfile() const;
    QString  activeProfileName() const { return _activeProfileName; }
    void     setActiveProfileName(QString const &name); // emits configChanged
    int      findProfileIndex(QString const &name) const;

    //! Add a new profile. If `name` collides, returns false.
    bool addProfile(VpnProfile const &p);
    //! Update an existing profile by name. The profile's `name` may change;
    //! `oldName` identifies the entry to replace.
    using ConfigRollback = std::function<bool()>;
    bool updateProfile(QString const &oldName, VpnProfile const &p,
                       bool configFileChanged = true,
                       ConfigRollback restorePreviousConfig = {});
    //! Remove a profile by name. Also deletes its config file from
    //! <configDir>/vpn/ and removes credentials from the keychain.
    bool removeProfile(QString const &name);

    //! Used by the conf parser to bulk-load profiles at startup. Bypasses
    //! configChanged emission.
    void setProfilesFromConfig(QList<VpnProfile> const &profiles,
                                QString const &activeName);

    static QString backendToString(Backend b);
    static Backend backendFromString(QString const &s, bool *ok = nullptr);
    static QString stateToString(State s);

    //! Look up the privileged helper script (ngpost-vpn-helper.sh) on disk.
    //! Prefers the system-installed path (/var/lib/ngpost/...) when present,
    //! otherwise falls back to the in-tree copy for development builds.
    //! Returns the first path that exists, or empty if none.
    static QString helperScriptPath();

    //! Program used to launch the privileged helper. Defaults to `pkexec`
    //! (PolicyKit), but the `NGPOST_HELPER_LAUNCHER` environment variable
    //! lets unattended environments (CI, headless servers without a polkit
    //! agent) substitute `sudo`, `sudo -n`, or another wrapper. Example:
    //!     NGPOST_HELPER_LAUNCHER="sudo -n" ./ngPost ...
    //! When the variable holds multiple whitespace-separated tokens, the
    //! first token is the program and the rest are prepended to argv.
    static QString     helperLauncherProgram();
    static QStringList helperLauncherPrefixArgs();

#ifdef Q_OS_WIN
    //! The Program Files roots to search, most specific first. "C:/Program
    //! Files" is not a given -- Windows can live on another drive, and a
    //! 32-bit process under WOW64 sees different values -- so the environment
    //! is the authority and the C: literals are only a last resort.
    static QStringList windowsProgramFilesRoots();
#endif

    //! Canonical install location for the privileged helper. The polkit rule
    //! whitelists this exact path so the rule and the installer agree.
    static constexpr const char *kInstalledHelperPath =
        "/var/lib/ngpost/ngpost-vpn-helper.sh";

    //! Is the installed helper current (wire v2, security revision 3 on Linux)?
    bool isHelperInstalled() const;

    //! Does this build carry a native VPN integration at all? Compile-time,
    //! and the single question every VPN affordance asks: the GUI hides its
    //! controls on a false answer, and job admission never forms a VPN
    //! requirement it could not satisfy. Distinct from vpnFeatureAvailable(),
    //! which asks the runtime question "is it usable right now".
    static constexpr bool vpnPlatformSupported()
    {
#if defined(NGPOST_VPN_SUPPORTED)
        return true;
#else
        return false;
#endif
    }

    //! True when the VPN feature can actually be used on this platform, i.e. a
    //! helper we can spawn is reachable. This is the capability gate shared by
    //! admitJob (job admission), forceAllConnectionsThroughVpn (routing) and
    //! jobNeedsVpn, so all three agree. Non-Windows: a helper script resolves
    //! (installed OR in-tree/dev copy). Windows: OpenVPN/WireGuard present.
    bool vpnFeatureAvailable() const;

    //! Copy the bundled VPN scripts and polkit template into a private
    //! temporary directory. This keeps pkexec away from AppImage/FUSE paths,
    //! which some systems refuse to execute after security updates.
    bool stageBundledResources(QString const &destination,
                               QString *error = nullptr) const;

    //! 1 pkexec → copies helper + uninstaller to /var/lib/ngpost/ and writes
    //! the per-user polkit rule. Returns true on success.
    bool runInstall();

    //! 1 pkexec → removes everything the install put in place.
    bool runUninstall();

    //! Async, fire-and-forget cleanup of any stale state from a previous run
    //! that died without going through the normal teardown. No-op (and no
    //! prompt) if the helper isn't installed.
    void runStartupStaleCleanup();
    void runStartupCleanup() { runStartupStaleCleanup(); }
    bool cleanupUnattributed(bool confirmed);

#if defined(Q_OS_WIN) || defined(NGPOST_TESTING)
    //! Windows-only: register a WireGuard tunnel as a service via the
    //! bundled install-wg-tunnel.ps1 (UAC prompted, runs once at profile
    //! creation). Returns true on success.
    bool registerWindowsWireGuardTunnel(QString const &confAbsPath);

    //! Symmetric uninstall.
    bool unregisterWindowsWireGuardTunnel(QString const &serviceName);
#endif

#ifdef NGPOST_TESTING
    //! Narrow dependency injection used by the portable profile-transaction
    //! tests. On Windows it also guarantees that tests never trigger UAC.
    using WireGuardServiceHook = std::function<bool(QString const &)>;
    void setWireGuardServiceHooksForTest(WireGuardServiceHook registerHook,
                                         WireGuardServiceHook unregisterHook);
    //! Install a fake backend and the state needed to exercise terminal-signal
    //! cleanup without launching a helper or a Windows service.
    void setBackendForTest(VpnBackend *backend, State state = State::Starting);
    bool hasBackendForTest() const { return _currentBackend != nullptr; }
    void setAutoStartedByJobForTest(bool value) { _autoStartedByJob = value; }
    void setBackendStartInProgressForTest(bool value) { _backendStartInProgress = value; }
    bool finishBackendStartForTest(bool started) { return _finishBackendStart(started); }
    static bool linuxOwnerManifestForTest(QByteArray bytes, qint64 *ownerPid,
                                          QString *ownerStart);
    static bool helperDeclaresProtocol2ForTest(QByteArray const &prefix);
#endif

    //! VPN orchestration.

    //! Admission verdict for a starting job.
    enum class Admission {
        Proceed,    //!< VPN not needed, or already Connected — let the job run
        Wait,       //!< VPN is being brought up — caller should queue and wait
                    //!< for `stateChanged(Connected)` before activating
        Blocked     //!< Cannot bring up VPN — caller should keep the job in
                    //!< queue and not activate it; popup signal is emitted
    };
    Q_ENUM(Admission)

    //! Does this server set require the tunnel? True if the effective global
    //! autoConnect applies OR any of the given enabled servers has useVpn=true.
    bool jobNeedsVpn(QList<NntpServerParams *> const &activeServers) const;

    //! Decide whether a job can start now. Side-effect: if VPN is needed but
    //! Disabled, kicks off `start()` asynchronously and returns Wait. Emits
    //! `vpnRequiredButUnavailable` if Blocked. `vpnRequirementFrozen` keeps a
    //! queued/resumed job fail-closed if the live configuration changes after
    //! that job originally entered the queue.
    Admission admitJob(QList<NntpServerParams *> const &activeServers);
    Admission admitJob(QList<NntpServerParams *> const &activeServers,
                       bool vpnRequirementFrozen);

    //! Track that a job holds the tunnel open. Cancels any pending auto-
    //! disconnect timer.
    void retainForJob();

    //! Track that a job released its hold. When the count reaches zero AND
    //! the VPN was auto-started, schedule auto-disconnect after the grace
    //! window.
    void releaseForJob();
    bool retryVpn();
    void requestRecovery(FailureKind reason);
    bool hasActiveVpnJobs() const { return _activeJobsNeedingVpn > 0; }

    //! Was the tunnel started by the auto-connect mechanism (vs. user click)?
    bool isAutoStarted() const { return _autoStartedByJob; }

    //! Process-wide accessor used by NntpConnection / NntpCheckCon to bind sockets.
    //! Set by NgPost on construction; null until then.
    static VpnManager *instance() { return sInstance; }

    static constexpr int kAutoDisconnectMs = 30000; //!< 30 s grace before auto-stop

signals:
    void stateChanged(State newState);
    //! Full verbose stream — routed to the dedicated VPN log panel.
    void logLine(QString const &line);
    //! High-level status events — routed to the main Posting log so the user
    //! sees init / connected / disconnected / failed alongside upload activity,
    //! without the openvpn/wireguard verbosity drowning the main log.
    void statusLine(QString const &line);
    void installStateChanged(bool installed);
    //! VPN preferences changed (auto-connect, profiles, active profile).
    //! NgPost listens to this to auto-persist the conf without manual Save.
    void configChanged();
    //! Emitted whenever the profile list or active profile changes — the
    //! VPN dialog uses this to refresh its selector.
    void profilesChanged();
    //! A job is being held back because the VPN is required but unavailable.
    //! GUI catches this and shows a popup; the job stays in queue.
    void vpnRequiredButUnavailable(JobBlockReason reason, QString const &detail);
    void vpnInterrupted(FailureKind reason);
    void recoveryExhausted(FailureKind reason);
    void manualDisconnectRequested();
    void unattributedVpnStateDetected(QString const &diagnostic, bool legacyOwnerActive);

private slots:
    void onBackendReady(QString const &iface, QHostAddress const &ip,
                        QHostAddress const &dns);
    void onBackendTerminated(BackendTermination const &termination);
    void onBackendHealthChanged(VpnBackendHealth health, QString const &reason);
    void onBackendRestartReady(quint64 attemptId, QString const &iface,
                               QHostAddress const &ip, QHostAddress const &dns);
    void onBackendRestartFailed(quint64 attemptId, FailureKind failure,
                                QString const &detail);
    void onAutoDisconnectTimeout();
    //! Wait until the reported tunnel IP is usable for source binding before
    //! emitting Connected.
    void _pollTunIpAvailability();

private:
    void _setState(State s);
    void _instantiateBackend();
    bool _finishBackendStart(bool started);
    void _destroyBackend();
    //! Detach all terminal signals before stopping so a synchronous `stopped`
    //! cannot re-enter VpnManager and destroy the same backend twice.
    void _stopAndDestroyBackend();
    //! Zero out + delete the short-lived auth-user-pass file we wrote for
    //! the current openvpn invocation, if any.
    void _shredRuntimeAuthFile();
    void _cancelAutoDisconnect();
    //! Declare the tunnel up once `_tunIp` is confirmed usable.
    void _completeReady();
    void _setHealth(VpnHealth health);
    void _scheduleExternalRestart(FailureKind reason);
    void _performExternalRestart();
    bool _consumeRecoveryAttempt();
#ifdef Q_OS_WIN
    bool _acquireWindowsLease(QString *detail);
    void _releaseWindowsLease();
    bool _publishWindowsOwner(Backend backend, QString *detail);
    QString _windowsOwnerDiagnostic() const;
#endif
    void _finishRecoveryExhausted(FailureKind reason, QString const &detail);
    void _clearTunnelIdentity();
    JobBlockReason _blockReasonForFailure(FailureKind failure) const;
    //! True when a VPN profile is selected and its config file can be read.
    bool _activeProfileUsable(QString *detail = nullptr) const;

    bool         _autoConnect;
    State        _state;
    VpnHealth    _health;
    QHostAddress _tunIp;
    QString      _tunIface;
    QHostAddress _dnsServer;
    VpnBackend  *_currentBackend;
    bool         _backendStartInProgress;
    bool         _backendFailedDuringStart;
    quint64      _nextRunId;
    quint64      _currentAttemptId;

    // Pending-ready bookkeeping while the reported tunnel IP becomes usable.
    QTimer      *_tunPollTimer;
    int          _tunPollAttempts;
    static constexpr int kTunPollMaxAttempts = 50;         // 50 x 100ms = 5s
    static constexpr int kTunPollMaxAttemptsWindows = 200; // 200 x 100ms = 20s
    static constexpr int kTunPollIntervalMs  = 100;

    QList<VpnProfile> _profiles;
    QString           _activeProfileName;

    //! Path to the short-lived auth-user-pass file we created for the
    //! currently-running openvpn invocation, if any. Empty otherwise.
    //! Cleared (file shredded) when the backend stops.
    QString _runtimeAuthFilePath;

    bool    _autoStartedByJob;
    int     _activeJobsNeedingVpn; //!< how many jobs hold the tunnel open
    QTimer *_autoDisconnectTimer;

    bool    _cliMode;
    int     _leaseWaitMinutes;
    int     _recoveryMaxAttempts;
    int     _recoveryAttempts;
    int     _externalRecoveryAttempts;
    bool    _recoveryActive;
    FailureKind _recoveryReason;
    QTimer *_recoveryTimer;
    QTimer *_healthyResetTimer;

#ifdef Q_OS_WIN
    void *_windowsLeaseHandle;
#endif

#ifdef NGPOST_TESTING
    WireGuardServiceHook _testRegisterWireGuardService;
    WireGuardServiceHook _testUnregisterWireGuardService;
#endif

    static VpnManager *sInstance;
};

#endif // VPNMANAGER_H
