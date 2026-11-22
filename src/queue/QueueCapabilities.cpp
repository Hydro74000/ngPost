// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The action x state matrix of plan_queue.md §3.5. Held, blocked and the
// domain only ever restrict what the life cycle allows.
//
//========================================================================

#include "QueueCapabilities.h"

namespace queue
{

namespace
{

//! Rules that depend on the flags only, whatever the life cycle.
bool flagRule(Action action, const QueueEntry &entry, Capability &out)
{
    switch (action) {
    case Action::Release:
        out = entry.held ? Capability::yes() : Capability::no(QStringLiteral("not_held"));
        return true;
    case Action::Hold:
        if (entry.held) {
            out = Capability::no(QStringLiteral("already_held"));
            return true;
        }
        return false;
    case Action::Revalidate:
        out = entry.blocked.isBlocked() ? Capability::yes()
                                        : Capability::no(QStringLiteral("not_blocked"));
        return true;
    case Action::PostNow:
        if (entry.held || entry.blocked.isBlocked()) {
            out = Capability::no(entry.held ? QStringLiteral("held") : QStringLiteral("blocked"));
            return true;
        }
        return false;
    default: return false;
    }
}

Capability removeCapability(const QueueEntry &entry)
{
    if (entry.transferManifestEngaged)
        return Capability::yes(true, QStringLiteral("partial_transfer_kept"));
    if (entry.retainedBytes() > 0)
        return Capability::yes(true, QStringLiteral("archives_kept"));
    return Capability::yes();
}

Capability waitingCapability(Action action, const QueueEntry &entry)
{
    switch (action) {
    case Action::Move:
    case Action::PostNext:
    case Action::PostNow:
    case Action::Hold:
    case Action::Duplicate: return Capability::yes();
    case Action::Edit:
        if (entry.transferManifestEngaged)
            return Capability::yes(false, QStringLiteral("manifest_engaged_restricted"));
        if (entry.lifecycle == Lifecycle::Prepared)
            return Capability::yes(false, QStringLiteral("recipe_change_invalidates_artifacts"));
        return Capability::yes();
    case Action::Remove: return removeCapability(entry);
    default: return Capability::no(QStringLiteral("not_finalizing"));
    }
}

Capability preparingCapability(Action action)
{
    switch (action) {
    case Action::Move:
    case Action::PostNext:
    case Action::PostNow:
    case Action::Hold:
    case Action::Duplicate: return Capability::yes();
    case Action::Remove: return Capability::yes(true, QStringLiteral("stops_preparation"));
    default: return Capability::no(QStringLiteral("preparing"));
    }
}

Capability postingCapability(Action action)
{
    switch (action) {
    case Action::Hold: return Capability::yes(false, QStringLiteral("interruption_kept"));
    case Action::Duplicate: return Capability::yes();
    case Action::Remove: return Capability::yes(true, QStringLiteral("stops_transfer"));
    default: return Capability::no(QStringLiteral("active"));
    }
}

Capability finalizingCapability(Action action, const QueueEntry &entry)
{
    const bool blocked = entry.blocked.isBlocked();
    switch (action) {
    case Action::Duplicate: return Capability::yes();
    case Action::RetryFinalization:
        return blocked ? Capability::yes() : Capability::no(QStringLiteral("finalizing"));
    case Action::AbandonPlanning:
        return blocked ? Capability::yes(true, QStringLiteral("nzb_not_finalized"))
                       : Capability::no(QStringLiteral("finalizing"));
    default: return Capability::no(QStringLiteral("finalizing"));
    }
}

Capability deferredCapability(Action action, const QueueEntry &entry)
{
    switch (action) {
    case Action::Move: return Capability::yes(false, QStringLiteral("maintenance_only"));
    case Action::Remove: return removeCapability(entry);
    default: return Capability::no(QStringLiteral("deferred"));
    }
}

} // namespace

Capability capability(Action action, const QueueEntry &entry, const QueueFacts &facts)
{
    if (!facts.ownedLocally || entry.domain == Domain::Foreign)
        return Capability::no(QStringLiteral("foreign"));
    if (facts.intentSwitching)
        return Capability::no(QStringLiteral("intent_switching"));
    if (entry.domain == Domain::Deferred)
        return deferredCapability(action, entry);

    const bool busy = entry.lifecycle == Lifecycle::Quiescing
        || entry.lifecycle == Lifecycle::Recovering;
    if (busy) {
        if (action == Action::Revalidate && entry.blocked.isBlocked())
            return Capability::yes();
        return Capability::no(QStringLiteral("busy"));
    }
    if (entry.lifecycle == Lifecycle::Finalizing)
        return finalizingCapability(action, entry);

    Capability flagged;
    if (flagRule(action, entry, flagged))
        return flagged;

    switch (entry.lifecycle) {
    case Lifecycle::Queued:
    case Lifecycle::Prepared: return waitingCapability(action, entry);
    case Lifecycle::Preparing: return preparingCapability(action);
    case Lifecycle::Posting: return postingCapability(action);
    default: return Capability::no(QStringLiteral("busy"));
    }
}

} // namespace queue
