//========================================================================
//
// Copyright (C) 2026 ngPost contributors
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnProfile.h"

#include "utils/PathHelper.h"

QString VpnProfile::absoluteConfigPath() const
{
    if (configFileName.isEmpty())
        return QString();
    return PathHelper::vpnDir() + "/" + configFileName;
}
