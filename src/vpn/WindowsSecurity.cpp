//========================================================================
// Windows ACL helpers for the files ngPost must keep to itself:
// ephemeral VPN state, and the configuration holding NNTP, proxy and
// archive credentials. NTFS ignores the POSIX bits Qt maps onto it, so a
// DACL is the only thing that actually restricts these.
//========================================================================

#include "WindowsSecurity.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>

namespace WindowsSecurity
{
bool isPrivilegedTrusteeSid(QString const &sid)
{
    // S-1-5-18 LocalSystem, S-1-5-32-544 BUILTIN\Administrators, and the
    // TrustedInstaller service SID that owns the Program Files tree.
    static QString const kTrustedInstaller = QStringLiteral(
        "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");

    QString const normalised = sid.trimmed().toUpper();
    return normalised == QLatin1String("S-1-5-18")
        || normalised == QLatin1String("S-1-5-32-544")
        || normalised == kTrustedInstaller.toUpper();
}
}

#ifdef Q_OS_WIN

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace
{
//! Apply \a sddl as the file's whole DACL, inheritance from the parent cut.
bool applyProtectedDacl(QString const &path, wchar_t const *sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr))
        return false;

    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    // present == TRUE with a null DACL means "no access control at all", i.e.
    // full access for everyone, and SetNamedSecurityInfoW would apply exactly
    // that. Every SDDL passed here yields a real DACL; the guard is what keeps
    // a future edit from turning this call into the opposite of its purpose.
    bool const ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)
        && present
        && dacl != nullptr
        && SetNamedSecurityInfoW(
               const_cast<wchar_t *>(reinterpret_cast<wchar_t const *>(path.utf16())),
               SE_FILE_OBJECT,
               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
               nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return ok;
}

//! The string SID of the user this process runs as, empty when unavailable.
QString tokenUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return QString();

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return QString();
    }

    QByteArray buffer(static_cast<int>(size), '\0');
    QString    sid;
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        auto const *user = reinterpret_cast<TOKEN_USER const *>(buffer.constData());
        LPWSTR      text = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &text)) {
            sid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid;
}
}

namespace WindowsSecurity
{
QString currentUserSid() { return tokenUserSid(); }

QString systemPowerShell()
{
    wchar_t directory[MAX_PATH + 1] = {};
    UINT const length = GetSystemDirectoryW(directory, MAX_PATH + 1);
    if (!length || length > MAX_PATH) return {};
    return QString::fromWCharArray(directory, int(length))
        + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
}

bool protectCurrentUserOnly(QString const &path, bool inheritable)
{
    QString const sid = currentUserSid();
    if (sid.isEmpty())
        return false;

    // OICI on a directory: object and container inherit, so a configuration
    // file written later starts out as restricted as the folder holding it.
    QString const sddl = QStringLiteral("D:P(A;%1;FA;;;%2)")
                                 .arg(inheritable ? QStringLiteral("OICI") : QString(), sid);
    return applyProtectedDacl(path, reinterpret_cast<wchar_t const *>(sddl.utf16()));
}

bool protectOwnerAndSystem(QString const &path)
{
    return applyProtectedDacl(path, L"D:P(A;;FA;;;SY)(A;;FA;;;OW)");
}

namespace
{
//! Rights that let a trustee replace, truncate or re-permission the object --
//! everything needed to substitute the script we are about to run elevated.
//! FILE_DELETE_CHILD matters on the directory: deleting the script and writing
//! a new one in its place never touches the old file's own DACL.
constexpr DWORD kWriteLikeRights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD
                                 | DELETE | WRITE_DAC | WRITE_OWNER
                                 | GENERIC_WRITE | GENERIC_ALL;

//! False as soon as one ALLOW entry hands write-like rights to a trustee that
//! is not already administrative -- or as soon as the OWNER is not one, because
//! an owner keeps implicit WRITE_DAC on NTFS and can therefore grant itself
//! back anything this function just checked was absent. Reading the DACL alone
//! answers "who may write now", never "who may decide that".
bool pathIsAdminOnly(QString const &path, QString *detail)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL                 dacl       = nullptr;
    PSID                 owner      = nullptr;
    DWORD const          status     = GetNamedSecurityInfoW(
        reinterpret_cast<wchar_t const *>(path.utf16()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);

    if (status != ERROR_SUCCESS) {
        if (detail)
            *detail = QStringLiteral("cannot read the permissions of '%1' (error %2)")
                          .arg(path).arg(status);
        return false;
    }

    QString ownerSid;
    if (owner) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(owner, &text)) {
            ownerSid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    if (ownerSid.isEmpty() || !isPrivilegedTrusteeSid(ownerSid)) {
        LocalFree(descriptor);
        if (detail) {
            *detail = ownerSid.isEmpty()
                ? QStringLiteral("'%1' has no identifiable owner").arg(path)
                : QStringLiteral("'%1' is owned by %2, which can rewrite its permissions")
                      .arg(path, ownerSid);
        }
        return false;
    }

    // A present-but-null DACL is not "no permissions", it is full access for
    // everyone -- the same trap applyProtectedDacl() guards on the way in.
    if (!dacl) {
        LocalFree(descriptor);
        if (detail)
            *detail = QStringLiteral("'%1' has no access control at all").arg(path);
        return false;
    }

    bool adminOnly = true;
    for (WORD i = 0; adminOnly && i < dacl->AceCount; ++i) {
        void *entry = nullptr;
        if (!GetAce(dacl, i, &entry))
            continue;

        auto const *header = static_cast<ACE_HEADER const *>(entry);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue; // see the header: DENY precedence is deliberately not modelled
        // An inherit-only entry describes what children get, not this object.
        if (header->AceFlags & INHERIT_ONLY_ACE)
            continue;

        auto const *allowed = static_cast<ACCESS_ALLOWED_ACE const *>(entry);
        if ((allowed->Mask & kWriteLikeRights) == 0)
            continue;

        QString trustee;
        LPWSTR  text = nullptr;
        auto   *sid  = reinterpret_cast<PSID>(const_cast<DWORD *>(&allowed->SidStart));
        if (ConvertSidToStringSidW(sid, &text)) {
            trustee = QString::fromWCharArray(text);
            LocalFree(text);
        }

        if (trustee.isEmpty() || !isPrivilegedTrusteeSid(trustee)) {
            adminOnly = false;
            if (detail) {
                *detail = trustee.isEmpty()
                    ? QStringLiteral("'%1' grants write access to an unidentifiable account")
                          .arg(path)
                    : QStringLiteral("'%1' grants write access to %2").arg(path, trustee);
            }
        }
    }

    LocalFree(descriptor);
    return adminOnly;
}
} // namespace

bool onlyPrivilegedPrincipalsCanWrite(QString const &path, QString *detail)
{
    QFileInfo const info(path);
    if (!info.exists()) {
        if (detail)
            *detail = QStringLiteral("'%1' does not exist").arg(path);
        return false;
    }

    if (!pathIsAdminOnly(info.absoluteFilePath(), detail))
        return false;

    // Every directory up to the root, not just the immediate parent. Write
    // access anywhere on the chain is enough: renaming an ancestor and putting
    // another tree in its place substitutes the script without any entry on the
    // chain below ever changing. A protected leaf inside a writable ancestor is
    // exactly the layout an attacker would build, and checking only the parent
    // would have accepted it.
    QDir directory = info.absoluteDir();
    for (;;) {
        QString const here = directory.absolutePath();
        if (!pathIsAdminOnly(here, detail))
            return false;
        if (directory.isRoot() || !directory.cdUp())
            break;
    }
    return true;
}
}

#endif // Q_OS_WIN
