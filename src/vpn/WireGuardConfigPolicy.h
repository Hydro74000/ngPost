//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef WIREGUARDCONFIGPOLICY_H
#define WIREGUARDCONFIGPOLICY_H

#include <QByteArray>
#include <QString>
#include <QStringList>

//! What a WireGuard profile is allowed to ask of the privileged process that
//! reads it.
//!
//! The Linux helper has sanitised these profiles since revision 4
//! (`sanitize_wireguard_profile`). Windows had no equivalent: a `.conf` sitting
//! in the user's own configuration folder went straight to
//! `wireguard.exe /installtunnelservice`, which ngPost runs **elevated**. The
//! file is writable by any process running as that user, and the tunnel service
//! it produces runs as SYSTEM.
//!
//! Two reasons to refuse rather than pass through, both taken from the helper:
//!
//!  1. `PreUp` / `PostUp` / `PreDown` / `PostDown` name commands. WireGuard for
//!     Windows only runs them when the machine-wide `DangerousScriptExecution`
//!     registry value is enabled -- so this is a conditional exposure, not an
//!     unconditional one -- but ngPost needs none of the four, and a tunnel
//!     that merely carries NNTP has no business running commands as SYSTEM.
//!     Unlike the Linux helper, which accepts them because `prepare_wg_config`
//!     strips them before `wg setconf` ever sees them, Windows hands the file
//!     over unchanged. What cannot be stripped must be refused.
//!
//!  2. wireguard-tools reports an unrecognised key by echoing the offending
//!     line, and that line reaches a log the caller can read back. Pointing the
//!     profile at a file that is not a profile would turn the error path into a
//!     read primitive. Only keys that are already ours are ever named back.
//!
//! The whitelist is the same one the helper carries, and
//! tst_WireGuardConfigPolicy reads the script to prove the two agree. Where
//! they deliberately differ -- the four script keys -- the test pins that
//! difference too, so it cannot drift into an accident.
namespace WireGuardConfigPolicy
{
//! Why a profile was refused, or Accepted.
enum class Outcome
{
    Accepted,
    DangerousKey,  //!< a key whose value is a command line
    UnknownKey,    //!< not in the whitelist; refused because it was never reviewed
    UnknownSection,//!< only [Interface] and [Peer] exist
    Malformed,     //!< unreadable, binary, oversized, or a key outside any section
};

struct Verdict
{
    Outcome outcome = Outcome::Accepted;

    //! The offending key or section, reduced to [A-Za-z0-9._-] and truncated.
    //! The profile may be any file the caller named, so a line of it must never
    //! be quoted back verbatim into a message or a log: a token that is not one
    //! of ours comes back as "withheld".
    QString key;

    int lineNumber = 0; //!< 1-based, 0 when the fault is the file as a whole.

    //! Human-readable, safe to show and to log.
    QString reason;

    bool isAccepted() const { return outcome == Outcome::Accepted; }
};

//! Largest profile accepted. A real one is well under a kilobyte; the cap
//! bounds the work done on a file a privileged process is about to read.
constexpr qint64 kMaxConfigBytes = qint64(1024) * 1024;

//! Inspect the bytes of a WireGuard profile.
Verdict inspect(QByteArray const &config);

//! Read \a path and inspect it. Malformed when it cannot be read or is larger
//! than kMaxConfigBytes.
Verdict inspectFile(QString const &path);

//! Keys accepted in [Interface], lower-case, sorted. Includes the four script
//! keys: they are recognised, and then refused by dangerousKeys().
QStringList interfaceKeys();

//! Keys accepted in [Peer], lower-case, sorted.
QStringList peerKeys();

//! The keys whose value is a command line, lower-case, sorted. Recognised so
//! they can be named in the refusal -- the user has to know which line to
//! remove -- but never accepted.
QStringList dangerousKeys();
}

#endif // WIREGUARDCONFIGPOLICY_H
