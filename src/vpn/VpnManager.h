//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNMANAGER_H
#define VPNMANAGER_H

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QTimer;
class VpnBackend;
struct NntpServerParams;
struct VpnProfile;

class VpnManager : public QObject
{
    Q_OBJECT
public:
    enum class State { Disabled, Starting, Connected, Stopping, Failed };
    Q_ENUM(State)

    enum class Backend { OpenVPN, WireGuard };
    Q_ENUM(Backend)

    //! Reason why a job cannot start despite needing the VPN.
    enum class JobBlockReason {
        None,                   //!< all good
        HelperNotInstalled,
        NoConfigSelected,
        ConfigUnreadable,
        VpnFailed               //!< VPN attempted to start but failed
    };
    Q_ENUM(JobBlockReason)

    explicit VpnManager(QObject *parent = nullptr);
    ~VpnManager() override;

    bool          start();
    void          stop();
    State         state() const { return _state; }
    QHostAddress  tunIp() const { return _tunIp; }
    QString       tunInterface() const { return _tunIface; }
    //! DNS server pushed by the VPN, populated from the helper READY message.
    //! Used by NntpConnection to perform DNS resolution through the VPN.
    QHostAddress  dnsServer() const { return _dnsServer; }
    //! Global override: when true, the VPN auto-starts on every
    //! job AND every NNTP connection (across all servers) binds to the tun,
    //! regardless of per-server `useVpn`. When false, per-server `useVpn`
    //! decides which servers route through the tunnel. The legacy
    //! `autoConnect()` accessor is preserved as an alias for the config
    //! plumbing that still uses the `VPN_AUTO_CONNECT` key.
    bool          forceAllConnectionsThroughVpn() const { return _autoConnect; }
    bool          autoConnect() const { return _autoConnect; }
    bool          isConnected() const { return _state == State::Connected; }

    void setAutoConnect(bool v);

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
    bool updateProfile(QString const &oldName, VpnProfile const &p);
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

    //! Canonical install location for the privileged helper. The polkit rule
    //! whitelists this exact path so the rule and the installer agree.
    static constexpr const char *kInstalledHelperPath =
        "/var/lib/ngpost/ngpost-vpn-helper.sh";

    //! Did the user run "Install" at some point on this machine?
    bool isHelperInstalled() const;

    //! Source dir bundled with ngPost containing the 3 scripts + polkit
    //! template that the installer needs as input.
    QString bundledResourcesDir() const;

    //! 1 pkexec → copies helper + uninstaller to /var/lib/ngpost/ and writes
    //! the per-user polkit rule. Returns true on success.
    bool runInstall();

    //! 1 pkexec → removes everything the install put in place.
    bool runUninstall();

    //! Async, fire-and-forget cleanup of any stale state from a previous run
    //! that died without going through the normal teardown. No-op (and no
    //! prompt) if the helper isn't installed.
    void runStartupCleanup();

#ifdef Q_OS_WIN
    //! Windows-only: register a WireGuard tunnel as a service via the
    //! bundled install-wg-tunnel.ps1 (UAC prompted, runs once at profile
    //! creation). Returns true on success.
    bool registerWindowsWireGuardTunnel(QString const &confAbsPath);

    //! Symmetric uninstall.
    bool unregisterWindowsWireGuardTunnel(QString const &serviceName);
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

    //! Does this server set require the tunnel? True if global autoConnect
    //! OR any of the given enabled servers has useVpn=true.
    bool jobNeedsVpn(QList<NntpServerParams *> const &activeServers) const;

    //! Decide whether a job can start now. Side-effect: if VPN is needed but
    //! Disabled, kicks off `start()` asynchronously and returns Wait. Emits
    //! `vpnRequiredButUnavailable` if Blocked.
    Admission admitJob(QList<NntpServerParams *> const &activeServers);

    //! Track that a job holds the tunnel open. Cancels any pending auto-
    //! disconnect timer.
    void retainForJob();

    //! Track that a job released its hold. When the count reaches zero AND
    //! the VPN was auto-started, schedule auto-disconnect after the grace
    //! window.
    void releaseForJob();

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

private slots:
    void onBackendReady(QString const &iface, QHostAddress const &ip,
                        QHostAddress const &dns);
    void onBackendFailed(QString const &reason);
    void onBackendStopped();
    void onAutoDisconnectTimeout();
    //! Wait until the reported tunnel IP is usable for source binding before
    //! emitting Connected.
    void _pollTunIpAvailability();

private:
    void _setState(State s);
    void _instantiateBackend();
    void _destroyBackend();
    //! Zero out + delete the short-lived auth-user-pass file we wrote for
    //! the current openvpn invocation, if any.
    void _shredRuntimeAuthFile();
    void _cancelAutoDisconnect();
    //! Declare the tunnel up once `_tunIp` is confirmed usable.
    void _completeReady();

    bool         _autoConnect;
    State        _state;
    QHostAddress _tunIp;
    QString      _tunIface;
    QHostAddress _dnsServer;
    VpnBackend  *_currentBackend;

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

    static VpnManager *sInstance;
};

#endif // VPNMANAGER_H
