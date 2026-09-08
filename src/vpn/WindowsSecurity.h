//========================================================================
// Windows ACL helpers for ephemeral VPN state.
//========================================================================

#ifndef WINDOWSSECURITY_H
#define WINDOWSSECURITY_H

#include <QString>

namespace WindowsSecurity
{
#ifdef Q_OS_WIN
//! Replace inherited ACLs with an owner/SYSTEM-only protected DACL.
bool protectOwnerAndSystem(QString const &path);
#endif
}

#endif // WINDOWSSECURITY_H
