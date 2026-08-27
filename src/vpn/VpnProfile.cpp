//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "VpnProfile.h"

#include "utils/PathHelper.h"

#include <QDir>

QString VpnProfile::absoluteConfigPath() const
{
    if (configFileName.isEmpty())
        return QString();
    if (configBaseDir.isEmpty())
        return PathHelper::vpnDir() + "/" + configFileName;
    return QDir(configBaseDir).filePath(QStringLiteral("vpn/") + configFileName);
}
