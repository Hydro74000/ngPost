#ifndef WINDOWS_SERVICE_CONTROL_H
#define WINDOWS_SERVICE_CONTROL_H
#include <QString>
#include <functional>

namespace WindowsServiceControl {
enum class State { Stopped, Running, StartPending, StopPending, Unknown };
enum class StopResult { Stopped, Pending, Failed };
// One non-blocking polling step. Successful submission is NEVER completion.
inline StopResult stopStep(std::function<State()> query, std::function<bool()> request)
{
    switch (query()) {
    case State::Stopped: return StopResult::Stopped;
    case State::StartPending:
    case State::StopPending: return StopResult::Pending;
    case State::Running: return request() ? StopResult::Pending : StopResult::Failed;
    case State::Unknown: return StopResult::Failed;
    }
    return StopResult::Failed;
}
#ifdef Q_OS_WIN
State query(QString const &name, QString *error);
bool requestStop(QString const &name, QString *error);
bool startDemand(QString const &name, QString *error);
#endif
}
#endif
