#include "WindowsServiceControl.h"
#ifdef Q_OS_WIN
#include <QByteArray>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
struct Handle {
    SC_HANDLE value;
    explicit Handle(SC_HANDLE h) : value(h) {}
    ~Handle() { if (value) CloseServiceHandle(value); }
};
bool failure(QString *error, DWORD code = GetLastError())
{
    if (error) *error = QStringLiteral("SCM error %1").arg(code);
    return false;
}
}
namespace WindowsServiceControl {
State query(QString const &name, QString *error)
{
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) { failure(error); return State::Unknown; }
    Handle service(OpenServiceW(manager.value, reinterpret_cast<LPCWSTR>(name.utf16()), SERVICE_QUERY_STATUS));
    if (!service.value) {
        DWORD const code = GetLastError();
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) return State::Stopped;
        failure(error, code); return State::Unknown;
    }
    SERVICE_STATUS_PROCESS status = {};
    DWORD size = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &size)) {
        failure(error); return State::Unknown;
    }
    switch (status.dwCurrentState) {
    case SERVICE_STOPPED: return State::Stopped;
    case SERVICE_START_PENDING: return State::StartPending;
    case SERVICE_STOP_PENDING: return State::StopPending;
    default: return State::Running;
    }
}
bool requestStop(QString const &name, QString *error)
{
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) return failure(error);
    Handle service(OpenServiceW(manager.value, reinterpret_cast<LPCWSTR>(name.utf16()), SERVICE_STOP));
    if (!service.value) return failure(error);
    SERVICE_STATUS status = {};
    if (ControlService(service.value, SERVICE_CONTROL_STOP, &status)) return true;
    DWORD const code = GetLastError();
    // Already inactive is only a hint: the next query must confirm STOPPED.
    if (code == ERROR_SERVICE_NOT_ACTIVE) return true;
    return failure(error, code);
}
bool startDemand(QString const &name, QString *error)
{
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) return failure(error);
    Handle service(OpenServiceW(manager.value, reinterpret_cast<LPCWSTR>(name.utf16()),
                               SERVICE_START | SERVICE_QUERY_CONFIG));
    if (!service.value) return failure(error);
    DWORD size = 0;
    QueryServiceConfigW(service.value, nullptr, 0, &size);
    if (!size || size > 65536) return failure(error);
    QByteArray buffer(int(size), '\0');
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
    if (!QueryServiceConfigW(service.value, config, size, &size)) return failure(error);
    if (config->dwStartType != SERVICE_DEMAND_START) {
        if (error) *error = QStringLiteral("Tunnel service is not start=demand; re-import the profile.");
        return false;
    }
    if (StartServiceW(service.value, 0, nullptr)) return true;
    DWORD const code = GetLastError();
    return code == ERROR_SERVICE_ALREADY_RUNNING || failure(error, code);
}
}
#endif
