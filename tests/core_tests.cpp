#include "qcae/core.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace qcae;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <class T>
const T& good(const Result<T>& result, const std::string& message) {
    check(result.ok(), message + (result.error ? ": " + result.error->message : ""));
    return *result.value;
}

template <class T>
void bad(const Result<T>& result, Status status, ErrorCode code, const std::string& message) {
    check(!result.ok() && result.status == status && result.error && result.error->code == code,
          message);
}

WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}

DocumentInfo info(MemoryApplication& app, const DocumentRef& ref) {
    return good(app.snapshot(ref), "snapshot").info;
}

void primary_flow() {
    MemoryApplication app;
    Caller alice{"alice"}, bob{"bob"};
    const auto created = good(app.create_document(alice, "Beam", "create-1"), "create");
    check(!created.durable && !created.dirty && created.revision == 0, "initial document facts");
    check(good(app.create_document(alice, "Beam", "create-1"), "create retry").document.id ==
              created.document.id, "host create replay");
    bad(app.create_document(alice, "Other", "create-1"), Status::conflict,
        ErrorCode::idempotency_key_conflict, "create key mismatch");
    bad(app.create_document(bob, "Other", "create-2"), Status::conflict,
        ErrorCode::document_already_open, "one active document");
    bad(app.snapshot({DocumentId("alien"), created.document.epoch}), Status::failed,
        ErrorCode::document_not_found, "unknown document");
    bad(app.snapshot({created.document.id, DocumentEpoch("old")}), Status::conflict,
        ErrorCode::document_epoch_expired, "old epoch read");
    auto old_context = at(created);
    old_context.document.epoch = DocumentEpoch("old");
    bad(app.preview(alice, old_context, CreateMaterial{"Steel", {210, "GPa"}}),
        Status::conflict, ErrorCode::document_epoch_expired, "old epoch preview");

    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {210, ""}}),
        Status::needs_input, ErrorCode::missing_input, "missing unit");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {210, "psi"}}),
        Status::failed, ErrorCode::invalid_unit, "unknown unit");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {-1, "MPa"}}),
        Status::failed, ErrorCode::invalid_input, "negative value");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {0, "MPa"}}),
        Status::failed, ErrorCode::invalid_input, "zero value");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {
        std::numeric_limits<double>::quiet_NaN(), "MPa"}}),
        Status::failed, ErrorCode::invalid_input, "NaN value");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {
        std::numeric_limits<double>::infinity(), "MPa"}}),
        Status::failed, ErrorCode::invalid_input, "infinite value");
    bad(app.preview(alice, at(created), CreateMaterial{"Steel", {
        std::numeric_limits<double>::max(), "GPa"}}),
        Status::failed, ErrorCode::invalid_input, "normalization overflow");
    check(info(app, created.document).revision == 0, "invalid previews do not write");

    const auto steel = good(app.preview(alice, at(created),
                                        CreateMaterial{"Steel", {210, "GPa"}}), "steel preview");
    check(steel.creates_entity && steel.normalized_modulus_mpa == 210000.0,
          "GPa normalization");
    bad(app.commit(bob, at(created), steel.id, "stolen"), Status::conflict,
        ErrorCode::preview_expired, "preview actor binding");
    const auto extra = good(app.preview(bob, at(created),
                                        CreateMaterial{"Extra", {1, "Pa"}}), "bob preview");
    check(std::abs(extra.normalized_modulus_mpa - 1e-6) < 1e-18, "Pa normalization");
    const auto first = good(app.commit(alice, at(created), steel.id, "commit-1"), "first commit");
    check(first.committed_revision == 1 && first.current_revision == 1 && !first.replayed,
          "first receipt");
    const auto after_first = info(app, created.document);
    check(after_first.material_count == 1 && after_first.dirty && !after_first.durable,
          "committed state");
    bad(app.commit(bob, at(created), extra.id, "stale"), Status::conflict,
        ErrorCode::revision_conflict, "stale commit revision rejection");
    bad(app.commit(bob, at(after_first), PreviewId("unknown"), "unknown-preview"),
        Status::conflict, ErrorCode::preview_expired, "unknown preview at current revision");
    bad(app.preview(alice, at(created), CreateMaterial{"Stale", {1, "MPa"}}),
        Status::conflict, ErrorCode::revision_conflict, "stale preview context");
    bad(app.commit(alice, old_context, steel.id, "commit-1"), Status::conflict,
        ErrorCode::document_epoch_expired, "old epoch cannot replay write");
    auto wrong_document = at(created);
    wrong_document.document.id = DocumentId("unknown");
    bad(app.commit(alice, wrong_document, steel.id, "commit-1"), Status::failed,
        ErrorCode::document_not_found, "wrong document cannot replay write");
    const auto replay = good(app.commit(alice, at(created), steel.id, "commit-1"),
                             "commit retry despite stale revision");
    check(replay.replayed && replay.transaction == first.transaction &&
              replay.committed_revision == 1 && replay.current_revision == 1,
          "commit replay original fact");
    bad(app.commit(alice, at(created), PreviewId("other"), "commit-1"), Status::conflict,
        ErrorCode::idempotency_key_conflict, "commit same key different preview");
    bad(app.operation(bob, created.document, "commit", "commit-1"), Status::failed,
        ErrorCode::entity_not_found, "caller isolation");

    auto mutable_snapshot = good(app.snapshot(created.document), "snapshot value copy");
    mutable_snapshot.materials.front().young_modulus_mpa = 7;
    mutable_snapshot.info.name = "changed";
    check(good(app.snapshot(created.document), "snapshot again").materials.front().young_modulus_mpa ==
              210000.0, "snapshot cannot mutate model");
    check(info(app, created.document).name == "Beam", "snapshot cannot mutate document");

    const auto c1 = at(after_first);
    const auto update = good(app.preview(bob, c1,
                                         SetYoungModulus{steel.affected_entity, {220000000, "kPa"}}),
                             "kPa preview");
    check(update.normalized_modulus_mpa == 220000.0, "kPa normalization");
    const auto second = good(app.commit(bob, c1, update.id, "commit-2"), "second commit");
    check(second.committed_revision == 2, "second revision");
    const auto undo_context = at(info(app, created.document));
    const auto undone = good(app.undo(alice, undo_context, "undo-1"), "undo");
    check(undone.committed_revision == 3 && info(app, created.document).revision == 3,
          "undo increments revision");
    check(good(app.snapshot(created.document), "after undo").materials.front().young_modulus_mpa ==
              210000.0, "undo restores content");
    const auto original_retry = good(app.commit(bob, c1, update.id, "commit-2"),
                                     "undo then retry original commit");
    check(original_retry.replayed && original_retry.transaction == second.transaction &&
              original_retry.committed_revision == 2 && original_retry.current_revision == 3,
          "undone commit is not reapplied");
    const auto old_operation = good(app.operation(bob, created.document, "commit", "commit-2"),
                                    "operation query");
    check(old_operation.transaction == second.transaction && old_operation.current_revision == 3,
          "operation original fact and current head");
    const auto undo_retry = good(app.undo(alice, undo_context, "undo-1"), "undo retry");
    check(undo_retry.replayed && undo_retry.transaction == undone.transaction &&
              undo_retry.current_revision == 3, "undo replay");
    bad(app.undo(alice, at(info(app, created.document)), "undo-1"), Status::conflict,
        ErrorCode::idempotency_key_conflict, "undo key context conflict");
    const auto history_after_undo = good(app.history(created.document), "history after undo");
    check(history_after_undo.items.size() == 2 && history_after_undo.cursor == 1 &&
              history_after_undo.items[0].applied && !history_after_undo.items[1].applied,
          "history cursor marks applied entries");

    const auto redone = good(app.redo(bob, at(info(app, created.document)), "redo-1"), "redo");
    check(redone.committed_revision == 4 &&
              good(app.snapshot(created.document), "after redo").materials.front().young_modulus_mpa ==
                  220000.0, "redo restores content");
    const auto undone2 = good(app.undo(alice, at(info(app, created.document)), "undo-2"), "undo again");
    check(undone2.committed_revision == 5, "second undo revision");
    const auto branch_context = at(info(app, created.document));
    const auto branch = good(app.preview(alice, branch_context,
                                         CreateMaterial{"Aluminum", {70000, "MPa"}}),
                             "branch preview");
    good(app.commit(alice, branch_context, branch.id, "branch-1"), "branch commit");
    const auto branched_history = good(app.history(created.document), "branch history");
    check(branched_history.items.size() == 2 && branched_history.cursor == 2 &&
              branched_history.items[1].transaction != second.transaction,
          "branch trims redo entries");
    bad(app.redo(bob, at(info(app, created.document)), "redo-2"), Status::conflict,
        ErrorCode::nothing_to_redo, "redo branch gone");
    check(good(app.operation(bob, created.document, "commit", "commit-2"),
               "trimmed operation record").transaction == second.transaction,
          "branch keeps idempotent fact");

    const auto before_to_zero = info(app, created.document);
    good(app.undo(alice, at(before_to_zero), "undo-3"), "undo branch");
    good(app.undo(alice, at(info(app, created.document)), "undo-4"), "undo initial edit");
    const auto initial = info(app, created.document);
    check(initial.revision == 8 && !initial.dirty && initial.material_count == 0 &&
              initial.content_state == created.content_state, "initial content restored, clean");
    bad(app.undo(alice, at(initial), "undo-5"), Status::conflict,
        ErrorCode::nothing_to_undo, "nothing left to undo");
    check(good(app.operation(alice, created.document, "undo", "undo-4"),
               "undo operation query").committed_revision == 8, "undo operation fact");
    check(good(app.operation(bob, created.document, "redo", "redo-1"),
               "redo operation query").committed_revision == 4, "redo operation fact");
}

void quotas() {
    Caller actor{"actor"};
    Limits name_limits;
    name_limits.max_name_bytes = 5;
    MemoryApplication named(name_limits);
    bad(named.create_document(actor, std::string(2000, 'N'), "create"),
        Status::failed, ErrorCode::resource_limit, "oversized document name");
    const auto named_created = good(named.create_document(actor, "Short", "create"),
                                    "oversized create did not establish document or key");
    bad(named.preview(actor, at(named_created),
                      CreateMaterial{std::string(2000, 'M'), {1, "MPa"}}),
        Status::failed, ErrorCode::resource_limit, "oversized material name");
    check(info(named, named_created.document).revision == 0 &&
              info(named, named_created.document).material_count == 0,
          "oversized material preview leaves document unchanged");
    const auto named_preview = good(named.preview(actor, at(named_created),
                                                  CreateMaterial{"Steel", {1, "MPa"}}),
                                    "oversized preview did not consume quota");
    good(named.commit(actor, at(named_created), named_preview.id, "commit"),
         "commit after oversized preview");

    Limits limits;
    limits.max_materials = 1;
    limits.max_history_entries = 1;
    limits.max_previews = 1;
    limits.max_idempotency_records = 3;
    MemoryApplication app(limits);
    const auto created = good(app.create_document(actor, "Small", "create"), "small create");
    const auto first = good(app.preview(actor, at(created),
                                        CreateMaterial{"One", {1, "MPa"}}), "first preview");
    bad(app.preview(actor, at(created), CreateMaterial{"Two", {2, "MPa"}}),
        Status::failed, ErrorCode::resource_limit, "preview quota");
    check(info(app, created.document).revision == 0, "preview quota atomic");
    good(app.commit(actor, at(created), first.id, "first"), "small first commit");
    bad(app.preview(actor, at(info(app, created.document)),
                    CreateMaterial{"Two", {2, "MPa"}}),
        Status::failed, ErrorCode::resource_limit, "material quota");
    const auto context = at(info(app, created.document));
    const auto update = good(app.preview(actor, context,
                                         SetYoungModulus{first.affected_entity, {2, "MPa"}}),
                             "update preview before history quota");
    bad(app.commit(actor, context, update.id, "second"), Status::failed,
        ErrorCode::resource_limit, "history quota");
    check(info(app, created.document).revision == 1 &&
              good(app.snapshot(created.document), "atomic quota snapshot")
                      .materials.front().young_modulus_mpa == 1,
          "history quota leaves state untouched");
    good(app.undo(actor, context, "undo"), "undo under history quota");
    check(!info(app, created.document).dirty, "undo clean under quotas");
    const auto branch_context = at(info(app, created.document));
    const auto branch = good(app.preview(actor, branch_context,
                                         CreateMaterial{"Branch", {3, "MPa"}}),
                             "branch preview under history quota");
    bad(app.commit(actor, branch_context, branch.id, "branch"), Status::failed,
        ErrorCode::resource_limit, "idempotency record quota");
    check(info(app, created.document).revision == 2 &&
              info(app, created.document).material_count == 0,
          "idempotency quota leaves state untouched");

    Limits branch_limits;
    branch_limits.max_history_entries = 1;
    MemoryApplication branch_app(branch_limits);
    const auto branch_created = good(branch_app.create_document(actor, "Branch", "create"),
                                     "branch capacity create");
    const auto first_context = at(branch_created);
    const auto first_preview = good(branch_app.preview(actor, first_context,
                                                       CreateMaterial{"First", {1, "MPa"}}),
                                    "branch capacity preview");
    good(branch_app.commit(actor, first_context, first_preview.id, "first"),
         "branch capacity commit");
    good(branch_app.undo(actor, at(info(branch_app, branch_created.document)), "undo"),
         "branch capacity undo");
    const auto second_context = at(info(branch_app, branch_created.document));
    const auto second_preview = good(branch_app.preview(actor, second_context,
                                                        CreateMaterial{"Second", {2, "MPa"}}),
                                     "branch capacity second preview");
    good(branch_app.commit(actor, second_context, second_preview.id, "second"),
         "reclaimed redo branch capacity");
    check(good(branch_app.history(branch_created.document), "branch capacity history")
                  .items.size() == 1,
          "branch uses freed history slot");
}

void unique_engines() {
    Caller actor{"actor"};
    MemoryApplication one, two;
    const auto a = good(one.create_document(actor, "A", "same"), "engine one");
    const auto b = good(two.create_document(actor, "B", "same"), "engine two");
    check(a.document.id != b.document.id && a.document.epoch != b.document.epoch,
          "engine nonce prevents document/epoch aliasing");
}

} // namespace

int main() {
    try {
        primary_flow();
        quotas();
        unique_engines();
        std::cout << "core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "core tests failed: " << error.what() << '\n';
        return 1;
    }
}
