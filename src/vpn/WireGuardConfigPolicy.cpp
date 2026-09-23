//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "WireGuardConfigPolicy.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

namespace
{
//! Mirrors WG_INTERFACE_KEYS in src/vpn/scripts/ngpost-vpn-helper.sh. The
//! wg-quick-only keys (address, dns, mtu, table, ...) are part of the list
//! because every provider profile carries them.
char const *const kInterfaceKeys[] = {
    "address", "addresses", "dns", "fwmark", "listenport", "mtu",
    "postdown", "postup", "predown", "preup",
    "privatekey", "saveconfig", "table",
};

//! Mirrors WG_PEER_KEYS in the same script.
char const *const kPeerKeys[] = {
    "allowedips", "endpoint", "persistentkeepalive", "presharedkey", "publickey",
};

//! The four whose value is a command line. Present in kInterfaceKeys so the
//! parser recognises them and can name them back; refused on sight here.
char const *const kDangerousKeys[] = { "postdown", "postup", "predown", "preup" };

QStringList sortedList(char const *const *first, size_t count)
{
    QStringList out;
    out.reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i)
        out << QString::fromLatin1(first[i]);
    out.sort();
    return out;
}

bool contains(char const *const *first, size_t count, QString const &needle)
{
    for (size_t i = 0; i < count; ++i)
        if (needle == QLatin1String(first[i]))
            return true;
    return false;
}

//! A token is safe to show only when it is already one of ours; anything else
//! is a fragment of whatever file the caller pointed us at.
QString label(QString const &token, bool known)
{
    if (!known)
        return QStringLiteral("withheld");
    QString safe;
    safe.reserve(token.size());
    for (QChar const c : token) {
        if (c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
            || c == QLatin1Char('-'))
            safe += c;
    }
    if (safe.isEmpty())
        return QStringLiteral("withheld");
    return safe.left(32);
}

WireGuardConfigPolicy::Verdict refuse(WireGuardConfigPolicy::Outcome outcome,
                                      QString const                 &token,
                                      bool                           known,
                                      int                            line,
                                      QString const                 &reason)
{
    WireGuardConfigPolicy::Verdict verdict;
    verdict.outcome    = outcome;
    verdict.key        = label(token, known);
    verdict.lineNumber = line;
    verdict.reason     = reason;
    return verdict;
}

using Outcome = WireGuardConfigPolicy::Outcome;
using Verdict = WireGuardConfigPolicy::Verdict;
enum class Section {
    None,
    Interface,
    Peer
};

//! \a raw without its line ending, its inline comment and surrounding blanks.
QString normalizedLine(QByteArray const &raw)
{
    QString line = QString::fromUtf8(raw);
    if (line.endsWith(QLatin1Char('\r')))
        line.chop(1);
    // WireGuard removes inline comments before parsing sections or keys.
    int const comment = line.indexOf(QLatin1Char('#'));
    if (comment >= 0)
        line.truncate(comment);
    return line.trimmed();
}

//! Switches \a section on an "[Interface]" or "[Peer]" header, refuses any other.
Verdict readSectionHeader(QString const &line, int lineNumber, Section &section)
{
    if (!line.endsWith(QLatin1Char(']')))
        return refuse(Outcome::UnknownSection,
                      QString(),
                      false,
                      lineNumber,
                      QCoreApplication::translate("WireGuardConfigPolicy",
                                                  "malformed section header"));
    QString const name = line.mid(1, line.size() - 2).trimmed().toLower();
    if (name == QLatin1String("interface")) {
        section = Section::Interface;
    } else if (name == QLatin1String("peer")) {
        section = Section::Peer;
    } else {
        return refuse(
            Outcome::UnknownSection,
            name,
            false,
            lineNumber,
            QCoreApplication::translate("WireGuardConfigPolicy",
                                        "only [Interface] and [Peer] sections are allowed"));
    }
    return Verdict{ };
}

Verdict inspectKeyLine(QString const &line, Section section, int lineNumber)
{
    int const separator = line.indexOf(QLatin1Char('='));
    if (separator <= 0)
        return refuse(Outcome::Malformed,
                      QString(),
                      false,
                      lineNumber,
                      QCoreApplication::translate("WireGuardConfigPolicy",
                                                  "expected a Key = Value line"));

    QString const key = line.left(separator).trimmed().toLower();
    if (key.isEmpty())
        return refuse(Outcome::Malformed,
                      QString(),
                      false,
                      lineNumber,
                      QCoreApplication::translate("WireGuardConfigPolicy",
                                                  "expected a Key = Value line"));

    bool const knownInterface = contains(kInterfaceKeys,
                                         sizeof(kInterfaceKeys) / sizeof(*kInterfaceKeys),
                                         key);
    bool const knownPeer = contains(kPeerKeys, sizeof(kPeerKeys) / sizeof(*kPeerKeys), key);
    bool const known = knownInterface || knownPeer;

    if (section == Section::None)
        return refuse(
            Outcome::Malformed,
            key,
            known,
            lineNumber,
            QCoreApplication::translate("WireGuardConfigPolicy",
                                        "a key appears before any [Interface] or [Peer] section"));

    // Checked before the section match so the message says what is actually
    // wrong: the key is refused for what it does, not for where it sits.
    if (contains(kDangerousKeys, sizeof(kDangerousKeys) / sizeof(*kDangerousKeys), key))
        return refuse(Outcome::DangerousKey,
                      key,
                      true,
                      lineNumber,
                      QCoreApplication::translate(
                          "WireGuardConfigPolicy",
                          "'%1' runs a command when the tunnel goes up or down, which ngPost "
                          "never needs. Remove that line from the profile.")
                          .arg(key));

    bool const fits = (section == Section::Interface) ? knownInterface : knownPeer;
    if (!fits)
        return refuse(Outcome::UnknownKey,
                      key,
                      known,
                      lineNumber,
                      known ? QCoreApplication::translate("WireGuardConfigPolicy",
                                                          "'%1' does not belong to this section")
                                  .arg(key)
                            : QCoreApplication::translate(
                                  "WireGuardConfigPolicy",
                                  "this profile carries a key ngPost has not reviewed"));
    return Verdict{ };
}
} // namespace

namespace WireGuardConfigPolicy
{
QStringList interfaceKeys()
{
    return sortedList(kInterfaceKeys, sizeof(kInterfaceKeys) / sizeof(*kInterfaceKeys));
}

QStringList peerKeys()
{
    return sortedList(kPeerKeys, sizeof(kPeerKeys) / sizeof(*kPeerKeys));
}

QStringList dangerousKeys()
{
    return sortedList(kDangerousKeys, sizeof(kDangerousKeys) / sizeof(*kDangerousKeys));
}

Verdict inspect(QByteArray const &config)
{
    if (config.size() > kMaxConfigBytes)
        return refuse(Outcome::Malformed, QString(), false, 0,
                      QCoreApplication::translate(
                          "WireGuardConfigPolicy",
                          "the WireGuard profile is larger than a profile ever is"));

    // A NUL means this is not a text profile. Refusing beats reading half of it:
    // whatever follows would be parsed under an assumption already known false.
    if (config.contains('\0'))
        return refuse(Outcome::Malformed, QString(), false, 0,
                      QCoreApplication::translate(
                          "WireGuardConfigPolicy",
                          "the WireGuard profile contains binary data"));

    Section section = Section::None;

    QList<QByteArray> const lines = config.split('\n');
    for (int index = 0; index < lines.size(); ++index) {
        int const lineNumber = index + 1;
        QString const line = normalizedLine(lines.at(index));

        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char(';')))
            continue;

        Verdict const verdict = line.startsWith(QLatin1Char('['))
            ? readSectionHeader(line, lineNumber, section)
            : inspectKeyLine(line, section, lineNumber);
        if (!verdict.isAccepted())
            return verdict;
    }

    return Verdict{};
}

Verdict inspectFile(QString const &path)
{
    QFileInfo const info(path);
    if (!info.exists() || !info.isFile())
        return refuse(Outcome::Malformed, QString(), false, 0,
                      QCoreApplication::translate("WireGuardConfigPolicy",
                                                  "the WireGuard profile cannot be read"));
    if (info.size() > kMaxConfigBytes)
        return refuse(Outcome::Malformed, QString(), false, 0,
                      QCoreApplication::translate(
                          "WireGuardConfigPolicy",
                          "the WireGuard profile is larger than a profile ever is"));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return refuse(Outcome::Malformed, QString(), false, 0,
                      QCoreApplication::translate("WireGuardConfigPolicy",
                                                  "the WireGuard profile cannot be read"));

    return inspect(file.readAll());
}
}
