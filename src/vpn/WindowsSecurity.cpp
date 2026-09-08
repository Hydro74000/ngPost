//========================================================================
// Windows ACL helpers for the files ngPost must keep to itself:
// ephemeral VPN state, and the configuration holding NNTP, proxy and
// archive credentials. NTFS ignores the POSIX bits Qt maps onto it, so a
// DACL is the only thing that actually restricts these.
//========================================================================

#include "WindowsSecurity.h"

#include <QByteArray>

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
QString currentUserSid()
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
}

#endif // Q_OS_WIN
