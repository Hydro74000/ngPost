// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// An external preparation tool (rar, 7z, par2) run under a supervisor that
// proves its stop (plan_queue.md §6.3, lot C5.3): Started -> Quiescing ->
// StoppedVerified. The tool runs only once the controller allowed it, i.e.
// once it recorded how the tool is supervised; its files may be renamed back
// or deleted only once it is StoppedVerified. Linux for now (tools/ToolSupervisor).
//
//========================================================================

#ifndef TOOLS_EXTERNALTOOLPROCESS_H
#define TOOLS_EXTERNALTOOLPROCESS_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
class QSocketNotifier;

class ExternalToolProcess : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Launching,       //!< the supervisor starts
        AwaitingGo,      //!< supervised, the tool not started yet
        Running,         //!< the tool (or one of its descendants) may write
        Quiescing,       //!< asked to stop
        StoppedVerified, //!< nothing it started can write any more
        Unproven         //!< ended without proof: its files are left alone
    };
    Q_ENUM(State)

    //! How this platform proves a stop: "linux_subreaper", or "unavailable".
    static QString capability();

    explicit ExternalToolProcess(QObject *parent = nullptr);
    //! A tool still running when its controller goes is stopped by its
    //! supervisor (end of the lifeline), and the proof written.
    ~ExternalToolProcess() override;

    //! The program run as supervisor: ngPost itself by default.
    void setSupervisorProgram(const QString &program) { _supervisorProgram = program; }
    //! Starts the supervisor; the tool waits for allowExecution().
    bool start(const QString &program,
               const QStringList &arguments,
               const QString &workingDirectory,
               const QString &proofPath);
    //! The supervision is recorded: the tool may run.
    void allowExecution();
    //! Stops the tool and every descendant; stopped() tells when it is proven.
    void stop();

    State state() const { return _state; }
    //! What identifies this supervision: to record before allowExecution().
    QString identityJson() const { return _identityJson; }
    //! The tool's own status: -1 never started, 127 not executable, its exit
    //! code, or 128 + the signal that ended it.
    int toolStatus() const { return _toolStatus; }

    //! What a recorded supervision says after its controller is gone: its
    //! proof (StoppedVerified), its supervisor still alive (Running), or
    //! neither (Unproven).
    static State proofState(const QString &identityJson);

#ifdef NGPOST_TESTING
    //! The protocol channel lost: the supervisor must still prove its stop.
    void dropProtocolForTest();
#endif

signals:
    void ready(const QString &capability, const QString &identityJson);
    void started(qint64 toolPid);
    void execFailed(int error);
    void toolExited(int status);
    void standardOutput(const QByteArray &data);
    void standardError(const QByteArray &data);
    //! The supervisor ended: @p verified when its proof is there and ours.
    void stopped(bool verified, int toolStatus);

private:
    //! Platform channels before the supervisor starts; their use once it runs.
    bool openChannels();
    void listenToSupervisor();
    void onProtocol();
    void onConnection();
    void onSupervisorFinished();
    void handleLine(const QByteArray &line);
    void closeChannels();
    void sendCommand(const QByteArray &line);

    QString _supervisorProgram;
    //! Not a child of this object: it outlives a controller destroyed early
    //! (QProcess would kill it), until the supervisor has proven its stop.
    QProcess *_supervisor = nullptr;
    QSocketNotifier *_notifier = nullptr;
    int _fromSupervisor = -1;        //!< Linux: read end of its fd 3
    int _toSupervisor = -1;          //!< Linux: write end of its fd 4, the lifeline
    QLocalServer *_server = nullptr; //!< Windows: the named pipe it connects to
    QLocalSocket *_socket = nullptr; //!< Windows: both ways, and the lifeline
    int _childWrite = -1;            //!< Linux: its fd 3, until it holds its own copy
    int _childRead = -1;             //!< Linux: its fd 4, likewise
    QByteArray _queuedCommands;      //!< Windows: asked before it connected
    QByteArray _pending;
    QString _proofPath;
    QString _nonce;
    QString _identityJson;
    State _state = State::Idle;
    int _toolStatus = -1;
    bool _readyReceived = false;
};

#endif // TOOLS_EXTERNALTOOLPROCESS_H
