//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNBACKEND_H
#define VPNBACKEND_H

#include <QHostAddress>
#include <QObject>
#include <QString>

enum class VpnFailureKind {
    None,
    Configuration,
    Authentication,
    HelperUnavailable,
    HelperOutdated,
    LeaseBusy,
    LeaseTimeout,
    LeaseUnavailable,
    RuntimeNotVolatile,
    UnattributedVpnState,
    TunnelLost,
    HelperExited,
    ProcessExited,
    Internal
};

enum class VpnTerminationKind {
    RequestedStop,
    StartFailure,
    UnexpectedExit
};

struct BackendTermination {
    quint64 runId = 0;
    VpnTerminationKind kind = VpnTerminationKind::UnexpectedExit;
    VpnFailureKind failure = VpnFailureKind::Internal;
    bool wasReady = false;
    QString detail;
};

enum class VpnBackendHealth {
    Healthy,
    Suspect,
    RecoveringInternally,
    Down
};

inline VpnFailureKind vpnFailureKindFromProtocol(
    QString const &value,
    VpnFailureKind fallback = VpnFailureKind::Internal)
{
    QString const key = value.trimmed().toLower();
    if (key == QLatin1String("none")) return VpnFailureKind::None;
    if (key == QLatin1String("configuration")) return VpnFailureKind::Configuration;
    if (key == QLatin1String("authentication")) return VpnFailureKind::Authentication;
    if (key == QLatin1String("helper_unavailable")) return VpnFailureKind::HelperUnavailable;
    if (key == QLatin1String("helper_outdated")) return VpnFailureKind::HelperOutdated;
    if (key == QLatin1String("lease_busy")) return VpnFailureKind::LeaseBusy;
    if (key == QLatin1String("lease_timeout")) return VpnFailureKind::LeaseTimeout;
    if (key == QLatin1String("lease_unavailable")) return VpnFailureKind::LeaseUnavailable;
    if (key == QLatin1String("runtime_not_volatile")) return VpnFailureKind::RuntimeNotVolatile;
    if (key == QLatin1String("unattributed_vpn_state")) return VpnFailureKind::UnattributedVpnState;
    if (key == QLatin1String("tunnel_lost")) return VpnFailureKind::TunnelLost;
    if (key == QLatin1String("helper_exited")) return VpnFailureKind::HelperExited;
    if (key == QLatin1String("process_exited")) return VpnFailureKind::ProcessExited;
    if (key == QLatin1String("internal")) return VpnFailureKind::Internal;
    return fallback;
}

class VpnBackend : public QObject
{
    Q_OBJECT
public:
    explicit VpnBackend(QObject *parent = nullptr)
        : QObject(parent), _runId(0), _stopRequested(false),
          _terminalReported(false), _wasReady(false) {}
    ~VpnBackend() override = default;

    virtual bool start(QString const &configPath) = 0;
    virtual void stop()                           = 0;
    virtual bool isRunning() const                = 0;
    virtual bool restart(quint64 attemptId) { Q_UNUSED(attemptId); return false; }
    virtual void setActive(bool active) { Q_UNUSED(active); }

    //! Assign the run identity. Only VpnManager does this, and only with a
    //! fresh non-zero id, so a late event from a previous run can always be
    //! told apart from the current one.
    void beginRun(quint64 runId) {
        _runId = runId;
        _resetRunState();
    }
    quint64 runId() const { return _runId; }

    //! Synchronous shutdown: signal stop then wait for the underlying
    //! process to actually exit, so the helper script has time to run its
    //! cleanup trap (tear down policy routing, kill openvpn). Used on app
    //! close so we don't leave the tunnel up.
    virtual void stopAndWait(int timeoutMs) {
        stop();
        Q_UNUSED(timeoutMs);
    }

    //! Retain this backend and its lease until terminated confirms shutdown.
    //! Runtime manager cleanup must use stop(), never block the GUI waiting.
    virtual bool requiresConfirmedStop() const { return false; }

signals:
    //! Tunnel is up. `dnsServer` may be null if the backend couldn't capture
    //! the DNS pushed by the VPN; in that case ngPost falls back to system DNS
    //! and the user is warned of the leak risk.
    void ready(QString const &tunInterface, QHostAddress const &tunIp,
               QHostAddress const &dnsServer = QHostAddress());
    void restartReady(quint64 attemptId, QString const &tunInterface,
                      QHostAddress const &tunIp,
                      QHostAddress const &dnsServer = QHostAddress());
    void restartFailed(quint64 attemptId, VpnFailureKind failure,
                       QString const &detail);
    void healthChanged(VpnBackendHealth health, QString const &reason);
    void terminated(BackendTermination const &termination);
    //! Cleanup is in progress, NOT a terminal event. The service may be active.
    void stopPending(QString const &detail);
    //! Full verbose stream — every line from the underlying VPN process.
    //! Routed to the dedicated VPN log panel only.
    void logLine(QString const &line);
    //! High-level status events (initiating, connected, disconnected, failed).
    //! Routed to the main Posting log so the user sees tunnel state alongside
    //! upload activity, without drowning in openvpn verbosity.
    void statusLine(QString const &line);

protected:
    //! Clear the per-run flags without touching the identity. start() uses
    //! this: inventing a runId there would let two runs share one id and
    //! defeat the manager's stale-event filter.
    void _resetRunState() {
        _stopRequested = false;
        _terminalReported = false;
        _wasReady = false;
    }
    void _markReady() { _wasReady = true; }
    void _requestStop() { _stopRequested = true; }
    bool _emitTerminationOnce(VpnTerminationKind kind,
                              VpnFailureKind failure,
                              QString const &detail) {
        if (_terminalReported)
            return false;
        _terminalReported = true;
        BackendTermination event;
        event.runId = _runId;
        event.kind = kind;
        event.failure = failure;
        event.wasReady = _wasReady;
        event.detail = detail;
        emit terminated(event);
        return true;
    }

    quint64 _runId;
    bool _stopRequested;
    bool _terminalReported;
    bool _wasReady;
};

Q_DECLARE_METATYPE(VpnFailureKind)
Q_DECLARE_METATYPE(VpnTerminationKind)
Q_DECLARE_METATYPE(BackendTermination)
Q_DECLARE_METATYPE(VpnBackendHealth)

#endif // VPNBACKEND_H
