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
#include <QObject>
#include <QString>

class QTimer;
class VpnBackend;
struct NntpServerParams;

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
    Backend       backend() const { return _backend; }
    QHostAddress  tunIp() const { return _tunIp; }
    QString       tunInterface() const { return _tunIface; }
    //! Global "auto-connect on job start" preference.
    bool          autoConnect() const { return _autoConnect; }
    bool          isConnected() const { return _state == State::Connected; }
    QString const &configPath() const { return _configPath; }

    void setAutoConnect(bool v);
    void setBackend(Backend b);
    void setConfigPath(QString const &p);

    static QString backendToString(Backend b);
    static Backend backendFromString(QString const &s, bool *ok = nullptr);
    static QString stateToString(State s);

    //! Look up the privileged helper script (ngpost-vpn-helper.sh) on disk.
    //! Prefers the system-installed path (/var/lib/ngpost/...) when present,
    //! otherwise falls back to the in-tree copy for development builds.
    //! Returns the first path that exists, or empty if none.
    static QString helperScriptPath();

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

    //! Phase 3 orchestration.

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
    void logLine(QString const &line);
    void installStateChanged(bool installed);
    //! VPN preferences changed (auto-connect, backend, config path). NgPost
    //! listens to this to auto-persist the conf without manual Save.
    void configChanged();
    //! A job is being held back because the VPN is required but unavailable.
    //! GUI catches this and shows a popup; the job stays in queue.
    void vpnRequiredButUnavailable(JobBlockReason reason, QString const &detail);

private slots:
    void onBackendReady(QString const &iface, QHostAddress const &ip);
    void onBackendFailed(QString const &reason);
    void onBackendStopped();
    void onAutoDisconnectTimeout();

private:
    void _setState(State s);
    void _instantiateBackend();
    void _destroyBackend();
    void _cancelAutoDisconnect();

    bool         _autoConnect;
    Backend      _backend;
    QString      _configPath;
    State        _state;
    QHostAddress _tunIp;
    QString      _tunIface;
    VpnBackend  *_currentBackend;

    bool    _autoStartedByJob;
    int     _activeJobsNeedingVpn; //!< how many jobs hold the tunnel open
    QTimer *_autoDisconnectTimer;

    static VpnManager *sInstance;
};

#endif // VPNMANAGER_H
