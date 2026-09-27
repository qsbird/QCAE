#include "engine_test_contribution.hpp"
#include "qcae/core.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejects_duplicate_contribution() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contributions.front());
    bool rejected{};
    try {
        (void)qcae::ipc::assemble_engine(contributions);
    } catch (const qcae::RecordError&) {
        rejected = true;
    }
    check(rejected, "duplicate contribution must be rejected");
}
void rejects_duplicate_operation() {
    auto contributions = qcae::ipc::default_engine_contributions();
    const auto extra = contribution_test::contribution();
    contributions.push_back(extra);
    contributions.push_back({"test.duplicate-operation", {}, extra.operations});
    auto assembly = qcae::ipc::assemble_engine(contributions);
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    bool rejected{};
    try {
        qcae::ipc::TypedHost host(facade.record_application(), {}, assembly.operations);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "duplicate operation must prevent host publication");
}
void rejects_reserved_operations() {
    using namespace qcae;
    using namespace qcae::operations;
    MemoryApplication facade;
    for (const auto* id : {"entity.fields",
                           "task.status",
                           "task.cancel",
                           "task.reconcile",
                           "project.create",
                           "entity.query",
                           "runtime.handshake"}) {
        bool rejected{};
        try {
            ipc::TypedHost host(
                facade.record_application(),
                {},
                [id](OperationRegistry& registry,
                     RecordApplication&,
                     std::function<TaskService&()>) {
                    return registry.declare_unavailable(
                        {id, 1, "test.reserved.v1", OperationEffect::read_only, {}, {}},
                        "Reserved-name probe");
                });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "contributed declarations must not shadow existing host operations");
    }
}
void freezes_registry_and_shares_application() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contribution_test::contribution());
    auto assembly = qcae::ipc::assemble_engine(contributions);
    check(assembly.records->frozen(), "assembled records must be frozen");
    check(assembly.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "test record must reach assembled registry");
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    const auto created = facade.create_document({"test"}, "Assembled", "create");
    check(created.ok(), "common project operation must remain usable");
    const auto view = facade.record_application().snapshot(created.value->document);
    check(view.ok() && view.value->records.registry() == assembly.records,
          "compatibility facade must use exactly the assembled registry");
    qcae::ipc::TypedHost host(facade.record_application(), {}, std::move(assembly.operations));
    check(host.supports("test.relation.create"), "contributed operation must reach typed host");
    const auto standard = qcae::ipc::assemble_engine(qcae::ipc::default_engine_contributions());
    check(!standard.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "production records must exclude test contribution");
    qcae::ipc::TypedHost without(facade.record_application(), {}, standard.operations);
    check(!without.supports("test.relation.create"), "disabled operation must be absent");
}
} // namespace
int main() {
    try {
        rejects_duplicate_contribution();
        rejects_duplicate_operation();
        rejects_reserved_operations();
        freezes_registry_and_shares_application();
        std::cout << "PASS: static engine assembly, duplicate rejection and shared authority\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
