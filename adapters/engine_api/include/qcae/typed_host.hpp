#pragma once
#include "qcae/task_application.hpp"
#include "qcae/operation_registry.hpp"
#include <QJsonArray>
#include <QJsonObject>

namespace qcae::ipc {
// Transport composition only: all handlers share the engine's authoritative application.
class TypedHost {
  public:
    using OperationContributor = std::function<Result<bool>(
        operations::OperationRegistry&, RecordApplication&, std::function<TaskService&()>)>;
    TypedHost(RecordApplication&, std::function<bool(const ProfileRef&)>);
    // An explicit contributor replaces the default feature registrations.
    TypedHost(RecordApplication&, std::function<bool(const ProfileRef&)>, OperationContributor);
    static OperationContributor default_operations();
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
