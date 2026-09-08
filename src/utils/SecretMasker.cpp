//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "SecretMasker.h"

namespace
{
//! Switches whose value is glued to the switch itself, so the whole argv entry
//! is a secret. Longest prefix first: "-hp" must be recognised before "-p"
//! would ever be tried against it.
constexpr char const *kGluedSecretSwitches[] = {
    "-hp", //!< rar: password + encrypted headers -- what ngPost passes
    "-p",  //!< 7z: password; also rar's password-without-header-encryption
};
}

namespace SecretMasker
{
QString mask()
{
    return QStringLiteral("********");
}

QString maskedArg(QString const &arg)
{
    for (char const *sw : kGluedSecretSwitches) {
        QString const prefix = QString::fromLatin1(sw);
        // A bare switch carries no secret: rar and 7z then prompt for the
        // password on their own, and blanking it would hide which mode ran.
        if (arg.size() > prefix.size() && arg.startsWith(prefix))
            return prefix + mask();
    }
    return arg;
}

QString maskedArgs(QStringList const &args)
{
    QStringList masked;
    masked.reserve(args.size());
    for (QString const &arg : args)
        masked << maskedArg(arg);
    return masked.join(QLatin1Char(' '));
}
}
