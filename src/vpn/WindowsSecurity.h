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
#endif
}

#endif // WINDOWSSECURITY_H
