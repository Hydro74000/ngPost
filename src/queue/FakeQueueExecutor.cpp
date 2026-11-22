// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "FakeQueueExecutor.h"

#include <QTimer>

#include <algorithm>

namespace queue
{

namespace
{

constexpr int kTickMs = 100;
constexpr qint64 kSimulatedRate = 68LL * 1024
    * 1024; //!< bytes per second shown by the J0 milestone

} // namespace

void FakeQueueExecutor::setSimulation(bool enabled, int stepMs, int transferMs)
{
    _simulate = enabled;
    _stepMs = std::max(kTickMs, stepMs);
    _transferMs = std::max(kTickMs, transferMs);
}

void FakeQueueExecutor::prepareStep(qint64 postId,
                                    const QString &runId,
                                    const QString &setId,
                                    PrepStep step)
{
    _commands.append({ Command::Kind::Prepare, postId, runId, setId, step, Disposition::Keep });
    if (_simulate)
        simulateStep(_commands.constLast());
}

void FakeQueueExecutor::activate(qint64 postId, const QString &runId, const QString &setId)
{
    _commands.append(
        { Command::Kind::Activate, postId, runId, setId, PrepStep::Compress, Disposition::Keep });
    if (_simulate)
        simulateTransfer(postId);
}

void FakeQueueExecutor::pauseTransfer(qint64 postId)
{
    _commands.append(
        { Command::Kind::Pause, postId, {}, {}, PrepStep::Compress, Disposition::Keep });
    if (_simulated.contains(postId))
        _simulated[postId].paused = true;
}

void FakeQueueExecutor::resumeTransfer(qint64 postId)
{
    _commands.append(
        { Command::Kind::Resume, postId, {}, {}, PrepStep::Compress, Disposition::Keep });
    if (_simulated.contains(postId))
        _simulated[postId].paused = false;
}

void FakeQueueExecutor::interrupt(qint64 postId, const QString &runId, Disposition disposition)
{
    _commands.append(
        { Command::Kind::Interrupt, postId, runId, {}, PrepStep::Compress, disposition });
    if (_simulate)
        simulateInterrupt(_commands.constLast());
}

void FakeQueueExecutor::retryFinalization(qint64 postId, const QString &runId)
{
    _commands.append({ Command::Kind::RetryFinalization,
                       postId,
                       runId,
                       {},
                       PrepStep::Compress,
                       Disposition::Keep });
    if (_simulate)
        QTimer::singleShot(400, this, [this, postId, runId] {
            emit finalized(postId, runId, true, {});
        });
}

void FakeQueueExecutor::abandonPlanning(qint64 postId, const QString &runId)
{
    _commands.append(
        { Command::Kind::Abandon, postId, runId, {}, PrepStep::Compress, Disposition::Abandon });
    if (_simulate)
        QTimer::singleShot(400, this, [this, postId, runId] {
            emit finalized(postId, runId, true, {});
        });
}

// --- simulation (J0) ---------------------------------------------------------------

QTimer *FakeQueueExecutor::ticker(qint64 postId)
{
    Simulated &sim = _simulated[postId];
    if (!sim.timer) {
        sim.timer = new QTimer(this);
        sim.timer->setInterval(kTickMs);
    }
    sim.timer->disconnect();
    return sim.timer;
}

void FakeQueueExecutor::stopSimulation(qint64 postId)
{
    auto it = _simulated.find(postId);
    if (it == _simulated.end())
        return;
    if (it->timer) {
        it->timer->stop();
        it->timer->deleteLater();
    }
    _simulated.erase(it);
}

void FakeQueueExecutor::simulateStep(const Command &command)
{
    _simulated[command.postId].percent = 0;
    _simulated[command.postId].transfer = false;
    QTimer *timer = ticker(command.postId);
    const int increment = std::max(1, 100 * kTickMs / _stepMs);
    connect(timer, &QTimer::timeout, this, [this, command, increment] {
        Simulated &sim = _simulated[command.postId];
        sim.percent = std::min(100, sim.percent + increment);
        emit stepProgress(command.postId, command.step, sim.percent);
        if (sim.percent < 100)
            return;
        stopSimulation(command.postId);
        emit stepFinished(command.postId,
                          command.runId,
                          command.setId,
                          command.step,
                          true,
                          {},
                          50'000'000);
    });
    timer->start();
}

void FakeQueueExecutor::simulateTransfer(qint64 postId)
{
    const QString runId = last(Command::Kind::Activate, postId)->runId;
    Simulated &sim = _simulated[postId];
    sim.percent = 0;
    sim.transfer = true;
    sim.paused = false;
    QTimer *timer = ticker(postId);
    const int increment = std::max(1, 100 * kTickMs / _transferMs);
    connect(timer, &QTimer::timeout, this, [this, postId, runId, increment] {
        Simulated &s = _simulated[postId];
        if (s.paused)
            return;
        s.percent = std::min(100, s.percent + increment);
        emit progress(postId, s.percent, kSimulatedRate);
        if (s.percent < 100)
            return;
        stopSimulation(postId);
        emit transferFinished(postId, runId, true);
        QTimer::singleShot(400, this, [this, postId, runId] {
            emit finalized(postId, runId, true, {});
        });
    });
    QTimer::singleShot(0, this, [this, postId, runId] { emit transferStarted(postId, runId); });
    timer->start();
}

void FakeQueueExecutor::simulateInterrupt(const Command &command)
{
    const bool transferStartedBefore = _simulated.value(command.postId).transfer;
    stopSimulation(command.postId);
    const qint64 postId = command.postId;
    const QString runId = command.runId;
    if (command.disposition == Disposition::Stop) {
        QTimer::singleShot(300, this, [this, postId, runId] {
            emit transferFinished(postId, runId, false);
            emit finalized(postId, runId, true, {});
        });
        return;
    }
    QTimer::singleShot(400, this, [this, postId, runId, transferStartedBefore] {
        emit quiesced(postId, runId, true, transferStartedBefore);
    });
}

// --- manual acknowledgements (tests) ---------------------------------------------------

const FakeQueueExecutor::Command *FakeQueueExecutor::last(Command::Kind kind, qint64 postId) const
{
    for (auto it = _commands.crbegin(); it != _commands.crend(); ++it)
        if (it->kind == kind && it->postId == postId)
            return &*it;
    return nullptr;
}

int FakeQueueExecutor::count(Command::Kind kind, qint64 postId) const
{
    int n = 0;
    for (const Command &c : _commands)
        if (c.kind == kind && (postId == 0 || c.postId == postId))
            ++n;
    return n;
}

bool FakeQueueExecutor::ackStep(qint64 postId, bool ok, qint64 bytes, const QString &error)
{
    const Command *c = last(Command::Kind::Prepare, postId);
    if (!c)
        return false;
    const Command copy = *c;
    emit stepFinished(postId, copy.runId, copy.setId, copy.step, ok, error, bytes);
    return true;
}

bool FakeQueueExecutor::ackTransferStarted(qint64 postId)
{
    const Command *c = last(Command::Kind::Activate, postId);
    if (!c)
        return false;
    const QString runId = c->runId; // a slot may append commands
    emit transferStarted(postId, runId);
    return true;
}

bool FakeQueueExecutor::ackTransferFinished(qint64 postId, bool success)
{
    const Command *c = last(Command::Kind::Activate, postId);
    if (!c)
        return false;
    const QString runId = c->runId;
    emit transferFinished(postId, runId, success);
    return true;
}

bool FakeQueueExecutor::ackFinalized(qint64 postId, bool ok, const QString &error)
{
    const Command *c = last(Command::Kind::Activate, postId);
    if (!c)
        return false;
    const QString runId = c->runId;
    emit finalized(postId, runId, ok, error);
    return true;
}

bool FakeQueueExecutor::ackQuiesced(qint64 postId, bool ok, bool manifestEngaged)
{
    const Command *c = last(Command::Kind::Interrupt, postId);
    if (!c)
        return false;
    const QString runId = c->runId;
    emit quiesced(postId, runId, ok, manifestEngaged);
    return true;
}

} // namespace queue
