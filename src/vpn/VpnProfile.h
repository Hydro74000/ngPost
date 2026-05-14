//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef VPNPROFILE_H
#define VPNPROFILE_H

#include "VpnManager.h"

#include <QString>

//! One user-configured VPN entry. ngPost can hold a list of these; one is
//! "active" at any time (used when a job triggers an auto-connect).
//!
//! The .ovpn or .conf file is COPIED into <configDir>/vpn/ at import. The
//! resulting basename is `configFileName`. The original source path is
//! discarded after copy.
//!
//! For OpenVPN profiles with `hasAuth == true`, credentials live in the
//! system keychain (QtKeychain service "ngPost-vpn", key = profile name) —
//! NOT in this struct, NOT in ngPost.conf.
struct VpnProfile
{
    QString             name;            //!< unique, also used as keychain key
    VpnManager::Backend backend;
    QString             configFileName;  //!< basename, lives in <configDir>/vpn/
    bool                hasAuth;         //!< OpenVPN: user/pass required at connect

    VpnProfile()
        : name(), backend(VpnManager::Backend::OpenVPN), configFileName(), hasAuth(false)
    {}

    //! Resolve the absolute path of the .ovpn/.conf file under configDir/vpn/.
    QString absoluteConfigPath() const;

    bool isValid() const { return !name.isEmpty() && !configFileName.isEmpty(); }
};

#endif // VPNPROFILE_H
