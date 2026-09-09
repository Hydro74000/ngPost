//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef OPENVPNCONFIGPOLICY_H
#define OPENVPNCONFIGPOLICY_H

#include <QByteArray>
#include <QString>
#include <QStringList>

//! What a .ovpn profile is allowed to ask of an OpenVPN process ngPost starts.
//!
//! On Linux that process runs as root, reached through a Polkit rule that lets
//! the configured user call the helper without a password. The profile was the
//! way in: OpenVPN's `plugin` directive loads a shared library into its own
//! process, and it does so whatever `--script-security` says -- the two are
//! separate mechanisms, and only the second one governs external *scripts*. A
//! profile is a file in the user's own config folder, so any process running as
//! that user could write one, name a library it controls, and be root.
//!
//! The answer here is a whitelist, not a list of forbidden directives: OpenVPN
//! has hundreds of options and gains more with each release, so anything this
//! file has not been taught about is refused rather than passed through. That
//! is a deliberate trade -- an exotic but legitimate profile is rejected with a
//! message naming the directive, and the user removes it or asks for it to be
//! added -- against a single missed directive being a root shell.
//!
//! It runs in two places on purpose. Here it gives an error before the Polkit
//! prompt, and it is unit-testable without root. In the privileged helper it
//! runs again, because the helper must not trust the ngPost that invoked it;
//! that copy is the one that actually defends the boundary. Their two lists are
//! kept identical by tst_OpenVpnConfigPolicy.
namespace OpenVpnConfigPolicy
{
//! Why a profile was refused, or Accepted.
enum class Outcome
{
    Accepted,
    DangerousDirective, //!< a directive whose whole purpose is to run our code elsewhere
    UnknownDirective,   //!< not in the whitelist; refused because it was never reviewed
    UnsafeArgument,     //!< a path that would make root read a file of the caller's choosing
    Malformed,          //!< unreadable, binary, oversized, or an unterminated inline block
};

struct Verdict
{
    Outcome outcome = Outcome::Accepted;

    //! The offending directive, reduced to [A-Za-z0-9._-] and truncated. The
    //! profile may be any file the caller named -- ngPost is asked to open it
    //! as the user, the helper as root -- so a line of it must never be quoted
    //! back verbatim into a message or a log.
    QString directive;

    int lineNumber = 0; //!< 1-based, 0 when the fault is the file as a whole.

    //! Human-readable, safe to show and to log.
    QString reason;

    //! The profile rebuilt from the accepted directives alone: comments and
    //! blank lines dropped, line endings normalised. Only set when accepted.
    QByteArray sanitizedConfig;

    bool isAccepted() const { return outcome == Outcome::Accepted; }
};

//! Largest profile accepted. A real one is a few kilobytes; the cap bounds the
//! work done on a file that root is about to read.
constexpr qint64 kMaxConfigBytes = 1024 * 1024;

//! Inspect the bytes of a .ovpn profile.
Verdict inspect(QByteArray const &config);

//! Read \a path and inspect it. Malformed when it cannot be read or is larger
//! than kMaxConfigBytes.
Verdict inspectFile(QString const &path);

//! The whitelist, sorted. Exposed so the test can compare it against the
//! helper script's copy.
QStringList allowedDirectives();

//! Directives refused with an explanation of their own, sorted.
QStringList deniedDirectives();

//! Directives whose argument names a file, and which are therefore accepted
//! inline or with a plain sibling file name, never with a path. Sorted.
QStringList fileBearingDirectives();

//! Directives recognised but left out of the generated configuration: routing
//! statements ngPost neither needs nor lets a profile make. Sorted.
QStringList droppedDirectives();

//! Inline blocks whose body is an opaque blob rather than more directives.
//! `connection` is absent: its body is validated line by line. Sorted.
QStringList inlineBlockTags();
}

#endif // OPENVPNCONFIGPOLICY_H
