// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Stable names of the queue enums, as stored in the database and printed by
// --queue_list --json. Never translated: the view translates for display.
//
//========================================================================

#include "QueueTypes.h"

#include <array>
#include <utility>

namespace queue
{

namespace
{

constexpr std::array<std::pair<Origin, const char *>, 5> kOrigins{ {
    { Origin::Quick, "quick" },
    { Origin::Auto, "auto" },
    { Origin::Cli, "cli" },
    { Origin::Resume, "resume" },
    { Origin::Drop, "drop" },
} };

constexpr std::array<std::pair<Lifecycle, const char *>, 7> kLifecycles{ {
    { Lifecycle::Queued, "queued" },
    { Lifecycle::Preparing, "preparing" },
    { Lifecycle::Prepared, "prepared" },
    { Lifecycle::Posting, "posting" },
    { Lifecycle::Quiescing, "quiescing" },
    { Lifecycle::Finalizing, "finalizing" },
    { Lifecycle::Recovering, "recovering" },
} };

template <typename Enum, std::size_t N>
QString nameOf(const std::array<std::pair<Enum, const char *>, N> &table, Enum value)
{
    for (const auto &[e, name] : table)
        if (e == value)
            return QLatin1String(name);
    return {};
}

template <typename Enum, std::size_t N>
bool valueOf(const std::array<std::pair<Enum, const char *>, N> &table,
             const QString &text,
             Enum &out)
{
    for (const auto &[e, name] : table) {
        if (text == QLatin1String(name)) {
            out = e;
            return true;
        }
    }
    return false;
}

} // namespace

QString originName(Origin origin)
{
    return nameOf(kOrigins, origin);
}

bool parseOrigin(const QString &text, Origin &origin)
{
    return valueOf(kOrigins, text, origin);
}

QString lifecycleName(Lifecycle lifecycle)
{
    return nameOf(kLifecycles, lifecycle);
}

bool parseLifecycle(const QString &text, Lifecycle &lifecycle)
{
    return valueOf(kLifecycles, text, lifecycle);
}

} // namespace queue
