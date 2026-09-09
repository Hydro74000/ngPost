//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#include "OpenVpnConfigPolicy.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace
{
//! Directives OpenVPN evaluates as plain configuration: transport, crypto
//! selection, timers, routing. None of them makes the root process run code,
//! read a path the profile chose, or write one.
char const *const kAllowed[] = {
    "allow-compression",       "auth",
    "auth-nocache",            "auth-retry",
    "auth-user-pass",          "block-outside-dns",
    "cipher",                  "client",
    "comp-lzo",                "compress",
    "connect-retry",           "connect-retry-max",
    "connect-timeout",         "data-ciphers",
    "data-ciphers-fallback",   "dev",
    "dev-type",                "dhcp-option",
    "disable-occ",             "explicit-exit-notify",
    "fast-io",                 "float",
    "fragment",                "hand-window",
    "http-proxy",              "http-proxy-retry",
    "http-proxy-timeout",      "inactive",
    "keepalive",               "key-direction",
    "link-mtu",                "lport",
    "mssfix",                  "mute",
    "mute-replay-warnings",    "ncp-ciphers",
    "ncp-disable",             "nobind",
    "ns-cert-type",            "opt-verify",
    "peer-fingerprint",        "persist-key",
    "persist-remote-ip",       "persist-tun",
    "ping",                    "ping-exit",
    "ping-restart",            "ping-timer-rem",
    "port",                    "proto",
    "pull",                    "pull-filter",
    "rcvbuf",                  "remote",
    "remote-cert-eku",         "remote-cert-ku",
    "remote-cert-tls",         "remote-random",
    "remote-random-hostname",  "reneg-bytes",
    "reneg-pkts",              "reneg-sec",
    "resolv-retry",            "route-nopull",
    "rport",                   "server-poll-timeout",
    "sndbuf",                  "socket-flags",
    "socks-proxy",             "socks-proxy-retry",
    "static-challenge",        "suppress-timestamps",
    "tls-cipher",              "tls-ciphersuites",
    "tls-client",              "tls-version-max",
    "tls-version-min",         "topology",
    "tran-window",             "tun-mtu",
    "tun-mtu-extra",           "verb",
    "verify-x509-name",
};

//! Directives whose argument names a file. Inline (`<ca>...</ca>`) is the form
//! ngPost wants; a bare sibling file name is accepted so a provider bundle of
//! several files still imports. Never a path: the process reading it is root,
//! and "ca /etc/shadow" would be a read primitive with the parse error as its
//! output channel.
char const *const kFileBearing[] = {
    "ca",       "cert",      "crl-verify", "dh",     "extra-certs",
    "key",      "pkcs12",    "secret",     "tls-auth", "tls-crypt",
    "tls-crypt-v2",
};

//! Recognised, accepted, and left out of the configuration ngPost generates.
//!
//! These are routing statements. `--route-nopull` governs only what the SERVER
//! pushes, so one written in the profile itself is applied regardless, and a
//! profile could hand the root process this machine's routing table. ngPost
//! does its own policy routing -- a source-address rule into a table of its own
//! -- so it has no use for them either.
//!
//! Dropped rather than refused because `redirect-gateway` is in very nearly
//! every provider profile: rejecting the file outright would turn a directive
//! that ends up doing nothing into "ngPost cannot import your VPN". The helper
//! also passes `--route-noexec`, so this list is the second of two independent
//! reasons no route from a profile is ever installed.
char const *const kDropped[] = {
    "redirect-gateway", "redirect-private", "route", "route-delay", "route-metric",
};

//! Refused with a reason of their own. Everything absent from every list here
//! is refused too -- see the header -- but these are the ones worth naming,
//! because a profile carrying one was not written by accident.
struct DeniedDirective
{
    char const *name;
    char const *reason;
};

DeniedDirective const kDenied[] = {
    { "askpass", "makes the root OpenVPN process read a file the profile chose" },
    { "auth-user-pass-verify", "runs a command from the root OpenVPN process" },
    { "capath", "makes the root OpenVPN process read a directory the profile chose" },
    { "cd", "changes the working directory of the root OpenVPN process" },
    { "chroot", "changes the filesystem root of the root OpenVPN process" },
    { "client-connect", "runs a command from the root OpenVPN process" },
    { "client-config-dir", "makes the root OpenVPN process read a directory the profile chose" },
    { "client-disconnect", "runs a command from the root OpenVPN process" },
    { "config", "includes another configuration file, which would bypass this whole check" },
    { "daemon", "detaches the root OpenVPN process from ngPost's supervision" },
    { "dev-node", "opens a device node the profile chose, as root" },
    { "down", "runs a command from the root OpenVPN process" },
    { "down-pre", "runs a command from the root OpenVPN process" },
    { "engine", "loads a cryptographic engine into the root OpenVPN process" },
    { "group", "changes the privileges of the root OpenVPN process" },
    { "ifconfig-pool-persist", "makes the root OpenVPN process write a file the profile chose" },
    { "http-proxy-user-pass", "hands a file ngPost cannot vouch for to the proxy the profile "
                              "names, so its content leaves the machine" },
    { "iproute", "replaces the command OpenVPN runs to configure the interface" },
    { "ipchange", "runs a command from the root OpenVPN process" },
    { "learn-address", "runs a command from the root OpenVPN process" },
    { "log", "makes the root OpenVPN process write a file the profile chose" },
    { "log-append", "makes the root OpenVPN process write a file the profile chose" },
    { "management", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-client", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-client-auth", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-client-pf", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-external-cert", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-external-key", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-hold", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-query-passwords", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-query-proxy", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-query-remote", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-signal", "ngPost owns the management channel it uses to drive the tunnel" },
    { "management-up-down", "ngPost owns the management channel it uses to drive the tunnel" },
    { "pkcs11-providers", "loads a shared library into the root OpenVPN process" },
    { "plugin", "loads a shared library into the root OpenVPN process, whatever script-security says" },
    { "providers", "loads a cryptographic provider into the root OpenVPN process" },
    { "route-pre-down", "runs a command from the root OpenVPN process" },
    { "route-up", "runs a command from the root OpenVPN process" },
    { "script-security", "ngPost fixes this to 0; a profile must not raise it" },
    { "setcon", "changes the security context of the root OpenVPN process" },
    { "setenv", "sets the environment of the processes OpenVPN starts" },
    { "setenv-safe", "sets the environment of the processes OpenVPN starts" },
    { "status", "makes the root OpenVPN process write a file the profile chose" },
    { "status-version", "makes the root OpenVPN process write a file the profile chose" },
    { "tls-export-cert", "makes the root OpenVPN process write into a directory the profile chose" },
    { "tls-verify", "runs a command from the root OpenVPN process" },
    { "tmp-dir", "makes the root OpenVPN process write into a directory the profile chose" },
    { "up", "runs a command from the root OpenVPN process" },
    { "up-restart", "runs a command from the root OpenVPN process" },
    { "user", "changes the privileges of the root OpenVPN process" },
    { "writepid", "makes the root OpenVPN process write a file the profile chose" },
};

//! Inline blocks whose body is opaque -- PEM, a static key, a fingerprint list.
//! `connection` is not here: its body is more directives, and it is validated
//! line by line like the rest of the file.
char const *const kInlineBlobTags[] = {
    "ca",         "cert",        "crl-verify",  "dh",         "extra-certs",
    "key",        "peer-fingerprint", "pkcs12", "secret",     "tls-auth",
    "tls-crypt",  "tls-crypt-v2",
};

QSet<QString> const &allowedSet()
{
    static QSet<QString> const set = [] {
        QSet<QString> s;
        for (char const *name : kAllowed)
            s.insert(QString::fromLatin1(name));
        return s;
    }();
    return set;
}

QSet<QString> const &fileBearingSet()
{
    static QSet<QString> const set = [] {
        QSet<QString> s;
        for (char const *name : kFileBearing)
            s.insert(QString::fromLatin1(name));
        return s;
    }();
    return set;
}

QSet<QString> const &droppedSet()
{
    static QSet<QString> const set = [] {
        QSet<QString> s;
        for (char const *name : kDropped)
            s.insert(QString::fromLatin1(name));
        return s;
    }();
    return set;
}

QSet<QString> const &inlineBlobSet()
{
    static QSet<QString> const set = [] {
        QSet<QString> s;
        for (char const *name : kInlineBlobTags)
            s.insert(QString::fromLatin1(name));
        return s;
    }();
    return set;
}

//! The reason \a directive is refused, or nullptr when it is not on the list.
char const *deniedReason(QString const &directive)
{
    for (DeniedDirective const &entry : kDenied)
        if (directive == QLatin1String(entry.name))
            return entry.reason;
    return nullptr;
}

//! \a token reduced to what a message may safely repeat. The profile can be any
//! file the caller named, so its bytes are never quoted back as they are.
QString safeLabel(QString const &token)
{
    QString clean;
    for (QChar const c : token) {
        if (clean.size() >= 32)
            break;
        if (c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
            || c == QLatin1Char('-'))
            clean += c;
    }
    return clean.isEmpty() ? QStringLiteral("(unprintable)") : clean;
}

//! Whitespace-separated tokens, with a surrounding pair of quotes removed.
//! OpenVPN's own parser is richer than this; anything it accepts and this does
//! not simply fails the checks below, which is the safe direction.
QStringList tokenize(QString const &line)
{
    QStringList tokens;
    for (QString token : line.split(QRegularExpression(QStringLiteral("\\s+")),
                                    Qt::SkipEmptyParts)) {
        if (token.size() >= 2
            && ((token.startsWith(QLatin1Char('"')) && token.endsWith(QLatin1Char('"')))
                || (token.startsWith(QLatin1Char('\'')) && token.endsWith(QLatin1Char('\'')))))
            token = token.mid(1, token.size() - 2);
        tokens << token;
    }
    return tokens;
}

//! True when \a name is a file that may sit next to the profile: a plain name,
//! no directory part, no traversal, and nothing a shell or a parser would read
//! as an option.
bool isPlainSiblingName(QString const &name)
{
    if (name.isEmpty() || name.size() > 128)
        return false;
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    if (name.startsWith(QLatin1Char('-')))
        return false;
    for (QChar const c : name) {
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
              || c == QLatin1Char('-')))
            return false;
    }
    return true;
}

OpenVpnConfigPolicy::Verdict reject(OpenVpnConfigPolicy::Outcome outcome,
                                    QString const               &directive,
                                    int                          lineNumber,
                                    QString const               &reason)
{
    OpenVpnConfigPolicy::Verdict v;
    v.outcome    = outcome;
    v.directive  = directive;
    v.lineNumber = lineNumber;
    v.reason     = reason;
    return v;
}
}

namespace OpenVpnConfigPolicy
{
Verdict inspect(QByteArray const &config)
{
    if (config.size() > kMaxConfigBytes)
        return reject(Outcome::Malformed, QString(), 0,
                      QStringLiteral("the profile is larger than a VPN profile ever is"));
    if (config.contains('\0'))
        return reject(Outcome::Malformed, QString(), 0,
                      QStringLiteral("the profile contains binary data, not OpenVPN directives"));

    QStringList const lines = QString::fromUtf8(config).split(QLatin1Char('\n'));
    QStringList       kept;
    QString           openBlob;   //!< tag of the opaque block being skipped
    int               blobLine  = 0;
    bool              inConnection = false;

    for (int i = 0; i < lines.size(); ++i) {
        int const     lineNumber = i + 1;
        QString const line       = lines.at(i).trimmed(); // also drops a CR from CRLF

        if (!openBlob.isEmpty()) {
            kept << lines.at(i).trimmed();
            if (line.compare(QStringLiteral("</%1>").arg(openBlob), Qt::CaseInsensitive) == 0)
                openBlob.clear();
            continue;
        }

        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;

        if (line.startsWith(QLatin1Char('<'))) {
            if (!line.endsWith(QLatin1Char('>')))
                return reject(Outcome::Malformed, safeLabel(line), lineNumber,
                              QStringLiteral("the inline block on this line is not terminated"));

            QString const tag     = line.mid(1, line.size() - 2).trimmed().toLower();
            bool const    closing = tag.startsWith(QLatin1Char('/'));
            QString const name    = closing ? tag.mid(1) : tag;

            if (closing) {
                if (name == QLatin1String("connection") && inConnection) {
                    inConnection = false;
                    kept << line;
                    continue;
                }
                return reject(Outcome::Malformed, safeLabel(name), lineNumber,
                              QStringLiteral("this inline block was closed but never opened"));
            }
            if (name == QLatin1String("connection")) {
                if (inConnection)
                    return reject(Outcome::Malformed, safeLabel(name), lineNumber,
                                  QStringLiteral("connection blocks cannot be nested"));
                inConnection = true;
                kept << line;
                continue;
            }
            if (!inlineBlobSet().contains(name))
                return reject(Outcome::UnknownDirective, safeLabel(name), lineNumber,
                              QStringLiteral("ngPost does not recognise this inline block"));
            openBlob = name;
            blobLine = lineNumber;
            kept << line;
            continue;
        }

        QStringList const tokens = tokenize(line);
        if (tokens.isEmpty())
            continue;

        QString directive = tokens.first();
        while (directive.startsWith(QLatin1String("--")))
            directive = directive.mid(2);
        directive = directive.toLower();

        if (char const *reason = deniedReason(directive))
            return reject(Outcome::DangerousDirective, safeLabel(directive), lineNumber,
                          QString::fromLatin1(reason));

        // Recognised, and deliberately not carried into the generated config.
        if (droppedSet().contains(directive))
            continue;

        bool const fileBearing = fileBearingSet().contains(directive);
        if (!fileBearing && !allowedSet().contains(directive))
            return reject(Outcome::UnknownDirective, safeLabel(directive), lineNumber,
                          QStringLiteral("ngPost has not reviewed this directive, so it will not "
                                         "hand it to a process running as root"));

        // ngPost passes the credentials on OpenVPN's command line, from a file
        // it wrote itself with the permissions it chose. A path here would make
        // root read one of the profile's choosing instead.
        if (directive == QLatin1String("auth-user-pass") && tokens.size() > 1)
            return reject(Outcome::UnsafeArgument, safeLabel(directive), lineNumber,
                          QStringLiteral("ngPost supplies the credentials itself; this directive "
                                         "must carry no file name"));

        // OpenVPN's proxy directives take an authentication FILE as a
        // positional argument, and then hand its content to the proxy the
        // profile itself named. That is not merely a root read: it is the read
        // and the way off the machine in one line. Only the host/port form is
        // accepted, plus OpenVPN's two non-file auth keywords.
        if (directive == QLatin1String("http-proxy")) {
            bool const keywordOk = tokens.size() < 4
                                || tokens.at(3) == QLatin1String("auto")
                                || tokens.at(3) == QLatin1String("auto-nct");
            static QStringList const authMethods{ QStringLiteral("none"),
                                                  QStringLiteral("basic"),
                                                  QStringLiteral("ntlm"),
                                                  QStringLiteral("ntlm2") };
            bool const methodOk = tokens.size() < 5
                               || authMethods.contains(tokens.at(4).toLower());
            if (tokens.size() < 3 || tokens.size() > 5 || !keywordOk || !methodOk)
                return reject(Outcome::UnsafeArgument, safeLabel(directive), lineNumber,
                              QStringLiteral("only \"http-proxy <host> <port>\" is accepted, "
                                             "optionally followed by auto or auto-nct: any other "
                                             "third argument names a credentials file that the "
                                             "proxy would then be sent"));
        }
        if (directive == QLatin1String("socks-proxy") && tokens.size() > 3)
            return reject(Outcome::UnsafeArgument, safeLabel(directive), lineNumber,
                          QStringLiteral("only \"socks-proxy <host> <port>\" is accepted: a third "
                                         "argument names a credentials file that the proxy would "
                                         "then be sent"));

        if (fileBearing) {
            if (tokens.size() < 2)
                return reject(Outcome::UnsafeArgument, safeLabel(directive), lineNumber,
                              QStringLiteral("this directive needs a file name or an inline block"));
            if (!isPlainSiblingName(tokens.at(1)))
                return reject(Outcome::UnsafeArgument, safeLabel(directive), lineNumber,
                              QStringLiteral("only a plain file name next to the profile is "
                                             "accepted here, or an inline <block>"));
        }

        kept << line;
    }

    if (!openBlob.isEmpty())
        return reject(Outcome::Malformed, safeLabel(openBlob), blobLine,
                      QStringLiteral("this inline block is never closed"));
    if (inConnection)
        return reject(Outcome::Malformed, QStringLiteral("connection"), 0,
                      QStringLiteral("a connection block is never closed"));

    Verdict verdict;
    verdict.sanitizedConfig = kept.join(QLatin1Char('\n')).toUtf8();
    if (!verdict.sanitizedConfig.isEmpty())
        verdict.sanitizedConfig += '\n';
    return verdict;
}

Verdict inspectFile(QString const &path)
{
    QFileInfo const info(path);
    if (!info.exists() || !info.isFile())
        return reject(Outcome::Malformed, QString(), 0,
                      QStringLiteral("the profile does not exist, or is not a regular file"));
    if (info.size() > kMaxConfigBytes)
        return reject(Outcome::Malformed, QString(), 0,
                      QStringLiteral("the profile is larger than a VPN profile ever is"));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return reject(Outcome::Malformed, QString(), 0,
                      QStringLiteral("the profile cannot be read"));

    return inspect(file.read(kMaxConfigBytes + 1));
}

QStringList allowedDirectives()
{
    QStringList list;
    for (char const *name : kAllowed)
        list << QString::fromLatin1(name);
    for (char const *name : kFileBearing)
        list << QString::fromLatin1(name);
    list.removeDuplicates();
    std::sort(list.begin(), list.end());
    return list;
}

QStringList deniedDirectives()
{
    QStringList list;
    for (DeniedDirective const &entry : kDenied)
        list << QString::fromLatin1(entry.name);
    std::sort(list.begin(), list.end());
    return list;
}

QStringList fileBearingDirectives()
{
    QStringList list;
    for (char const *name : kFileBearing)
        list << QString::fromLatin1(name);
    std::sort(list.begin(), list.end());
    return list;
}

QStringList droppedDirectives()
{
    QStringList list;
    for (char const *name : kDropped)
        list << QString::fromLatin1(name);
    std::sort(list.begin(), list.end());
    return list;
}

QStringList inlineBlockTags()
{
    QStringList list;
    for (char const *name : kInlineBlobTags)
        list << QString::fromLatin1(name);
    std::sort(list.begin(), list.end());
    return list;
}
}
