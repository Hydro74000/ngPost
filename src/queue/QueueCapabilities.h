// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// What can be done to an entry, computed from facts rather than from its
// displayed state alone (plan_queue.md §3.5). The view enables a button only
// when the capability is allowed; PostQueue checks it again on execution.
//
//========================================================================

#ifndef QUEUE_QUEUECAPABILITIES_H
#define QUEUE_QUEUECAPABILITIES_H

#include "queue/QueueEntry.h"

#include <QString>

namespace queue
{

enum class Action {
    Move,
    PostNext,
    PostNow,
    Hold,
    Release,
    Edit,
    Duplicate,
    Remove,
    Revalidate,
    RetryFinalization,
    AbandonPlanning
};

//! Facts about the queue around an entry that its own fields do not tell.
struct QueueFacts
{
    bool ownedLocally = true;     //!< false: another live instance owns it
    bool intentSwitching = false; //!< "post now" is switching and targets this entry
};

struct Capability
{
    bool allowed = false;
    bool needsConfirmation = false;
    QString reason; //!< stable code explaining a refusal, or what a confirmation is about

    static Capability yes(bool confirm = false, const QString &why = {})
    {
        return { true, confirm, why };
    }
    static Capability no(const QString &why) { return { false, false, why }; }
};

Capability capability(Action action, const QueueEntry &entry, const QueueFacts &facts = {});

} // namespace queue

#endif // QUEUE_QUEUECAPABILITIES_H
