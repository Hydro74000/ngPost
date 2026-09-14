//========================================================================
// Windows ACL helpers for the files ngPost must keep to itself:
// ephemeral VPN state, and the configuration holding NNTP, proxy and
// archive credentials. NTFS ignores the POSIX bits Qt maps onto it, so a
// DACL is the only thing that actually restricts these.
//========================================================================

#ifndef WINDOWSSECURITY_H
#define WINDOWSSECURITY_H

#include <QString>

namespace WindowsSecurity
{
//! True when \a sid names a principal that already holds administrative power,
//! so its write access to a file ngPost is about to run elevated grants nothing
//! it could not take anyway: LocalSystem, BUILTIN\\Administrators, and
//! TrustedInstaller -- which owns everything under Program Files.
//!
//! Deliberately short. Backup and Server Operators are administrative in
//! practice but are not the default owners of an install directory, so a build
//! that finds one of them holding write access on the script it is about to
//! elevate should say so rather than wave it through.
//!
//! Defined on every platform, and taking the SID as text, so the policy itself
//! is unit-testable without Windows -- only its enforcement needs a DACL.
bool isPrivilegedTrusteeSid(QString const &sid);

#ifdef Q_OS_WIN
//! Identity of the actual caller, unaffected by USERNAME/environment spoofing.
QString currentUserSid();
//! Absolute system executable, never resolved through PATH.
QString systemPowerShell();
//! Replace inherited ACLs with an owner/SYSTEM-only protected DACL.
bool protectOwnerAndSystem(QString const &path);

//! Replace inherited ACLs with a protected DACL naming only the user this
//! process runs as -- the NTFS equivalent of chmod 0600, or 0700 with
//! \a inheritable set for a directory so files created in it get the same.
//!
//! Named by SID rather than by the "owner rights" well-known SID: a file
//! created while ngPost ran elevated is owned by Administrators, and an
//! owner-scoped ACE would then lock the ordinary user out of their own
//! configuration. Elevation does not change the token's user SID, so this
//! stays correct either way.
//!
//! SYSTEM is deliberately absent. It grants nothing an administrator cannot
//! already take, and the configuration is user data, not machine state.
bool protectCurrentUserOnly(QString const &path, bool inheritable);

//! True when nothing short of an administrator can modify \a path -- the file
//! itself, and the directory holding it, since replacing a file only needs
//! write access to its parent.
//!
//! This is what the Linux side already demands of `--bin-dir` before the helper
//! will run a bundled binary as root. It is asked here for the same reason: the
//! PowerShell helpers are launched through `Start-Process -Verb RunAs`, so a
//! script an ordinary account can rewrite is an administrator shell waiting for
//! the next time the user edits a WireGuard profile. The portable zip puts them
//! wherever the user unpacked it.
//!
//! Conservative by construction, and fail-closed: a NULL DACL, an unreadable
//! one, or a single ALLOW entry granting write to a principal outside
//! isPrivilegedTrusteeSid() all answer false. DENY entries that might cancel
//! such an ALLOW are not evaluated -- the full precedence rules are not worth
//! reimplementing to turn a refusal into an acceptance. \a detail, when given,
//! receives a sentence naming the path and, where it is known, the trustee.
bool onlyPrivilegedPrincipalsCanWrite(QString const &path, QString *detail = nullptr);
#endif
}

#endif // WINDOWSSECURITY_H
