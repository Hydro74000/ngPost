// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "FakeQueueExecutor.h"

namespace queue
{

void FakeQueueExecutor::prepareStep(qint64 postId,
                                    const QString &runId,
                                    const QString &setId,
                                    PrepStep step)
{
    _commands.append({ Command::Kind::Prepare, postId, runId, setId, step, Disposition::Keep });
}

void FakeQueueExecutor::activate(qint64 postId, const QString &runId, const QString &setId)
{
    _commands.append(
        { Command::Kind::Activate, postId, runId, setId, PrepStep::Compress, Disposition::Keep });
}

void FakeQueueExecutor::pauseTransfer(qint64 postId)
{
    _commands.append(
        { Command::Kind::Pause, postId, {}, {}, PrepStep::Compress, Disposition::Keep });
}

void FakeQueueExecutor::resumeTransfer(qint64 postId)
{
    _commands.append(
        { Command::Kind::Resume, postId, {}, {}, PrepStep::Compress, Disposition::Keep });
}

void FakeQueueExecutor::interrupt(qint64 postId, const QString &runId, Disposition disposition)
{
    _commands.append(
        { Command::Kind::Interrupt, postId, runId, {}, PrepStep::Compress, disposition });
}

void FakeQueueExecutor::retryFinalization(qint64 postId, const QString &runId)
{
    _commands.append({ Command::Kind::RetryFinalization,
                       postId,
                       runId,
                       {},
                       PrepStep::Compress,
                       Disposition::Keep });
}

void FakeQueueExecutor::abandonPlanning(qint64 postId, const QString &runId)
{
    _commands.append(
        { Command::Kind::Abandon, postId, runId, {}, PrepStep::Compress, Disposition::Abandon });
}

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
    const QString runId = c->runId; // a slot may append commands
    emit transferFinished(postId, runId, success);
    return true;
}

bool FakeQueueExecutor::ackFinalized(qint64 postId, bool ok, const QString &error)
{
    const Command *c = last(Command::Kind::Activate, postId);
    if (!c)
        return false;
    const QString runId = c->runId; // a slot may append commands
    emit finalized(postId, runId, ok, error);
    return true;
}

bool FakeQueueExecutor::ackQuiesced(qint64 postId, bool ok, bool manifestEngaged)
{
    const Command *c = last(Command::Kind::Interrupt, postId);
    if (!c)
        return false;
    const QString runId = c->runId; // a slot may append commands
    emit quiesced(postId, runId, ok, manifestEngaged);
    return true;
}

} // namespace queue
