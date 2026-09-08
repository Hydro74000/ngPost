//========================================================================
// Windows ACL helpers for ephemeral VPN state.
//========================================================================

#include "WindowsSecurity.h"

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

namespace WindowsSecurity
{
bool protectOwnerAndSystem(QString const &path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1,
            &descriptor, nullptr))
        return false;

    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    // present == TRUE with a null DACL means "no access control at all", i.e.
    // full access for everyone, and SetNamedSecurityInfoW would apply exactly
    // that. The SDDL above always yields a real DACL; the guard is here so
    // editing it can never turn this call into the opposite of its purpose.
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
}

#endif // Q_OS_WIN
