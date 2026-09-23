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
// Creating a sibling does not replace an existing, protected child. In
// particular FILE_APPEND_DATA means FILE_ADD_SUBDIRECTORY on a directory,
// and Windows grants that right to Authenticated Users on C:\ by default.
constexpr DWORD kDirectoryReplacementRights = FILE_DELETE_CHILD | DELETE
                                             | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;
constexpr DWORD kFileWriteRights = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE
                                  | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

//! The string form of \a sid, empty when there is none or it cannot be read.
QString sidString(PSID sid)
{
    QString result;
    LPWSTR text = nullptr;
    if (sid && ConvertSidToStringSidW(sid, &text)) {
        result = QString::fromWCharArray(text);
        LocalFree(text);
    }
    return result;
}

//! WOF compression, deduplication and cloud placeholders also carry the
//! reparse attribute. Only name-surrogate tags redirect into an unchecked tree.
bool reparsePointIsSafe(QString const &path, QString const &nativePath, QString *detail)
{
    WIN32_FIND_DATAW data{ };
    HANDLE const search = FindFirstFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), &data);
    if (search == INVALID_HANDLE_VALUE) {
        if (detail)
            *detail = QStringLiteral("cannot read the reparse tag of '%1'").arg(path);
        return false;
    }
    FindClose(search);
    if (IsReparseTagNameSurrogate(data.dwReserved0)) {
        if (detail)
            *detail = QStringLiteral("'%1' is a name-surrogate reparse point").arg(path);
        return false;
    }
    return true;
}

bool ownerIsPrivileged(QString const &path, PSID owner, QString *detail)
{
    QString const ownerSid = sidString(owner);
    if (!ownerSid.isEmpty() && isPrivilegedTrusteeSid(ownerSid))
        return true;
    if (detail) {
        *detail = ownerSid.isEmpty()
            ? QStringLiteral("'%1' has no identifiable owner").arg(path)
            : QStringLiteral("'%1' is owned by %2, which can rewrite its permissions")
                  .arg(path, ownerSid);
    }
    return false;
}

//! False when the access rule \a entry lets a non-administrative trustee write.
bool aceIsAdminOnly(QString const &path, void *entry, DWORD writeRights, QString *detail)
{
    auto const *header = static_cast<ACE_HEADER const *>(entry);
    // An inherit-only entry describes what children get, not this object.
    if (header->AceFlags & INHERIT_ONLY_ACE)
        return true;
    if (header->AceType == ACCESS_DENIED_ACE_TYPE)
        return true; // DENY precedence is deliberately not modelled
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        if (detail)
            *detail = QStringLiteral("'%1' has an unsupported access rule").arg(path);
        return false;
    }

    auto const *allowed = static_cast<ACCESS_ALLOWED_ACE const *>(entry);
    if ((allowed->Mask & writeRights) == 0)
        return true;

    QString const trustee = sidString(
        reinterpret_cast<PSID>(const_cast<DWORD *>(&allowed->SidStart)));
    if (!trustee.isEmpty() && isPrivilegedTrusteeSid(trustee))
        return true;
    if (detail) {
        *detail = trustee.isEmpty()
            ? QStringLiteral("'%1' grants write access to an unidentifiable account").arg(path)
            : QStringLiteral("'%1' grants write access to %2").arg(path, trustee);
    }
    return false;
}

bool daclIsAdminOnly(QString const &path, PACL dacl, DWORD writeRights, QString *detail)
{
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        void *entry = nullptr;
        if (!GetAce(dacl, i, &entry)) {
            if (detail)
                *detail = QStringLiteral("cannot read an access rule of '%1'").arg(path);
            return false;
        }
        if (!aceIsAdminOnly(path, entry, writeRights, detail))
            return false;
    }
    return true;
}

//! False as soon as one ALLOW entry hands write-like rights to a trustee that
//! is not already administrative -- or as soon as the OWNER is not one, because
//! an owner keeps implicit WRITE_DAC on NTFS and can therefore grant itself
//! back anything this function just checked was absent. Reading the DACL alone
//! answers "who may write now", never "who may decide that".
bool pathIsAdminOnly(QString const &path, QString *detail)
{
    QString const nativePath = QDir::toNativeSeparators(path);
    DWORD const attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (detail)
            *detail = QStringLiteral("cannot read the attributes of '%1'").arg(path);
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && !reparsePointIsSafe(path, nativePath, detail))
        return false;
    DWORD const writeRights = (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? kDirectoryReplacementRights : kFileWriteRights;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL                 dacl       = nullptr;
    PSID                 owner      = nullptr;
    DWORD const          status     = GetNamedSecurityInfoW(
        reinterpret_cast<wchar_t const *>(nativePath.utf16()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);

    if (status != ERROR_SUCCESS) {
        if (detail)
            *detail = QStringLiteral("cannot read the permissions of '%1' (error %2)")
                          .arg(path).arg(status);
        return false;
    }

    // owner and dacl point into descriptor: it is released only once both
    // have been read.
    bool adminOnly = ownerIsPrivileged(path, owner, detail);
    // A present-but-null DACL is not "no permissions", it is full access for
    // everyone -- the same trap applyProtectedDacl() guards on the way in.
    if (adminOnly && !dacl) {
        if (detail)
            *detail = QStringLiteral("'%1' has no access control at all").arg(path);
        adminOnly = false;
    }
    adminOnly = adminOnly && daclIsAdminOnly(path, dacl, writeRights, detail);
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

    // Check replacement rights on every directory up to the root.
    // Renaming an ancestor and putting another tree in its place substitutes
    // the script without any entry on the chain below ever changing.
    // A protected leaf inside a writable ancestor is
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
