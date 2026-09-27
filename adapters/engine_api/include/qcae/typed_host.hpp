#pragma once
#include "qcae/task_application.hpp"
#include <QJsonArray>
#include <QJsonObject>

namespace qcae::ipc {
// Transport composition only: all handlers share the engine's authoritative application.
class TypedHost {
  public:
    TypedHost(RecordApplication&, std::function<bool(const ProfileRef&)>);
    ~TypedHost();
    bool supports(std::string_view) const;
    QJsonArray capabilities() const;
    QJsonObject dispatch(const QJsonObject&, const Caller&);
    Result<bool> reconcile();

  private:
    TaskService& tasks();
    RecordApplication& app_;
    std::function<bool(const ProfileRef&)> profile_supported_;
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<TaskService> tasks_;
};
} // namespace qcae::ipc
