#include "qcae/query.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include "qcae/sqlite_store.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

struct ProbeRelation {
  qcae::EntityId id, from, to;
};
namespace qcae {
template <> struct RecordTraits<ProbeRelation> {
  inline static constexpr RecordTypeId type_id{900001};
  static const void *token() {
    static const char value{};
    return &value;
  }
};
} // namespace qcae
using namespace qcae;
void check(bool value, const char *label) {
  if (!value)
    throw std::runtime_error(label);
  std::cout << "PASS: " << label << '\n';
}
template <class T> T good(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.error ? result.error->message
                                          : "Missing result");
  return std::move(*result.value);
}
RecordDescriptor relation_descriptor() {
  RecordDescriptor descriptor;
  descriptor.type = RecordTraits<ProbeRelation>::type_id;
  descriptor.name = "ProbeRelation";
  descriptor.query_kind = "probe_relation";
  descriptor.cpp_type_token = RecordTraits<ProbeRelation>::token();
  const std::vector<RecordTypeId> targets{RecordTraits<records::Node>::type_id};
  descriptor.fields = {
      {RecordFieldId{2}, "from", RecordFieldKind::reference, false, "",
       targets},
      {RecordFieldId{3}, "to", RecordFieldKind::reference, false, "", targets}};
  descriptor.encode = [](const void *object) {
    const auto &value = *static_cast<const ProbeRelation *>(object);
    return RecordInput{{RecordTraits<ProbeRelation>::type_id, value.id.value},
                       1,
                       {{RecordFieldId{2}, RecordFieldKind::reference, "",
                         record_wire::text(value.from.value)},
                        {RecordFieldId{3}, RecordFieldKind::reference, "",
                         record_wire::text(value.to.value)}}};
  };
  descriptor.decode =
      [](const RecordInput &input) -> std::shared_ptr<const void> {
    return std::make_shared<const ProbeRelation>(ProbeRelation{
        EntityId(input.key.identity),
        EntityId(record_wire::read_text(
            record_wire::require(input, RecordFieldId{2}).payload)),
        EntityId(record_wire::read_text(
            record_wire::require(input, RecordFieldId{3}).payload))});
  };
  descriptor.owned_bytes = [](const void *object) {
    const auto &value = *static_cast<const ProbeRelation *>(object);
    return sizeof(value) + value.id.value.size() + value.from.value.size() +
           value.to.value.size();
  };
  descriptor.references = [](const void *object,
                             const RecordReferenceVisitor &visitor) {
    const auto &value = *static_cast<const ProbeRelation *>(object);
    const std::array<RecordTypeId, 1> targets{
        RecordTraits<records::Node>::type_id};
    visitor(RecordFieldId{2}, value.from.value, targets);
    visitor(RecordFieldId{3}, value.to.value, targets);
  };
  descriptor.validate = [](const void *object, const DocumentView &) {
    const auto &value = *static_cast<const ProbeRelation *>(object);
    if (value.from == value.to)
      throw RecordError(ErrorCode::invalid_input, "Self relation rejected");
  };
  return descriptor;
}
std::shared_ptr<const RecordRegistry> extended_registry() {
  auto registry = std::make_shared<RecordRegistry>();
  for (auto descriptor : generated_record_descriptors())
    registry->add(std::move(descriptor));
  registry->add(relation_descriptor());
  registry->add_rule(records::validate_relations);
  registry->add_rule([](const DocumentView &view) {
    view.visit(RecordTraits<records::Node>::type_id, [](const Record &record) {
      if (record->get<records::Node>().position[0] > 1000)
        throw RecordError(ErrorCode::invalid_input,
                          "Probe boundary condition: x <= 1000");
    });
  });
  registry->freeze();
  return registry;
}
RecordApplicationOptions
options(const std::filesystem::path &workspace,
        const std::shared_ptr<const RecordRegistry> &registry) {
  RecordApplicationOptions result;
  result.registry = registry;
  auto store = std::make_shared<SqliteWorkspaceStore>(workspace.string());
  result.records = store;
  result.projects = store;
  return result;
}
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("Supply an empty evidence runtime directory");
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    auto registry = extended_registry();
    Caller caller{"extension-audit"};
    EntityId left, right, relation;
    DocumentRef original;
    unsigned checks{};
    const auto workspace = directory / "work.sqlite";
    const auto project = directory / "saved.qcae";
    {
      RecordApplication app(options(workspace, registry));
      auto info = good(app.create_document(caller, "Probe", "new"));
      original = info.document;
      auto execute = [&](const char *name, const RecordPrepare &prepare) {
        info = good(app.current_document());
        auto result = app.execute(caller, {info.document, info.revision}, name,
                                  name, prepare, name);
        info = good(app.current_document());
        return result;
      };
      good(execute("seed", [&](const DocumentView &view,
                               const RecordIdentityAllocator &allocate) {
        left = allocate();
        right = allocate();
        relation = allocate();
        EditSession edit(view);
        edit.put(records::Node{left, {0, 0, 0}, std::nullopt});
        edit.put(records::Node{right, {1000, 0, 0}, std::nullopt});
        edit.put(ProbeRelation{relation, left, right});
        return Result<RecordPreparedOperation>{
            Status::success,
            RecordPreparedOperation{edit.prepare(), "Seed", relation, "seed", 0,
                                    true},
            {}};
      }));
      auto view = good(app.snapshot(info.document)).records;
      check(view.count(RecordTraits<ProbeRelation>::type_id) == 1,
            "new record commits without existing product edits");
      ++checks;
      check(good(query_entities(view, 0, 10, "probe_relation")).total == 1 &&
                good(query_fields(view, relation)).fields.size() == 2,
            "new kind entity and field queries use existing descriptors");
      ++checks;
      check(good(query_references(view, right, true, 0, 10)).total == 1,
            "new relation incoming references are queryable");
      ++checks;
      auto before = info.revision;
      auto invalid =
          execute("outside-boundary", [&](const DocumentView &current,
                                          const RecordIdentityAllocator &) {
            EditSession edit(current);
            edit.update<records::Node>(
                right, [](auto &node) { node.position[0] = 2000; });
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{edit.prepare(), "Invalid", right,
                                        "outside-boundary", 0, false},
                {}};
          });
      check(
          !invalid.ok() && info.revision == before,
          "added boundary rule rejects invalid write without revision advance");
      ++checks;
      invalid =
          execute("delete-referenced", [&](const DocumentView &current,
                                           const RecordIdentityAllocator &) {
            EditSession edit(current);
            edit.erase({RecordTraits<records::Node>::type_id, right.value});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{edit.prepare(), "Invalid delete", right,
                                        "delete-referenced", 0, false},
                {}};
          });
      check(!invalid.ok() && info.revision == before,
            "new relation protects referenced node from deletion");
      ++checks;
      good(execute("delete-composite", [&](const DocumentView &current,
                                           const RecordIdentityAllocator &) {
        EditSession edit(current);
        edit.erase({RecordTraits<ProbeRelation>::type_id, relation.value});
        edit.erase({RecordTraits<records::Node>::type_id, right.value});
        return Result<RecordPreparedOperation>{
            Status::success,
            RecordPreparedOperation{edit.prepare(), "Composite delete", right,
                                    "delete-composite", 0, false},
            {}};
      }));
      check(!good(app.snapshot(info.document))
                 .records.find_identity(relation.value),
            "composite deletion publishes atomically");
      ++checks;
      good(app.undo(caller, {info.document, info.revision}, "undo-delete"));
      info = good(app.current_document());
      check(good(app.snapshot(info.document))
                    .records.find_identity(relation.value) &&
                good(app.snapshot(info.document))
                    .records.find_identity(right.value),
            "undo restores new relation and target");
      ++checks;
      good(app.redo(caller, {info.document, info.revision}, "redo-delete"));
      info = good(app.current_document());
      check(!good(app.snapshot(info.document))
                 .records.find_identity(relation.value),
            "redo deletes new relation again");
      ++checks;
      good(app.undo(caller, {info.document, info.revision}, "undo-for-save"));
      info = good(app.current_document());
      good(app.save_document(caller, {info.document, info.revision},
                             project.string(), true, "save"));
    }
    {
      RecordApplication app(options(workspace, registry));
      auto info = good(app.recover_document(caller, "recover"));
      const auto view = good(app.snapshot(info.document)).records;
      const auto value =
          view.find_identity(relation.value)->get<ProbeRelation>();
      check(value.from == left && value.to == right &&
                info.document.id == original.id &&
                info.document.epoch != original.epoch,
            "SQLite restart recovery preserves new record identities and "
            "payload");
      ++checks;
      good(app.close_document(caller, {info.document, info.revision},
                              ClosePolicy::discard, "close"));
      info = good(app.open_document(caller, project.string(), "normal-open"));
      check(good(app.snapshot(info.document))
                        .records.find_identity(relation.value)
                        ->get<ProbeRelation>()
                        .to == right &&
                info.document.id != original.id,
            "project save and normal open preserve new relation with new "
            "document ID");
      ++checks;
    }
    check(checks == 10, "all ten isolated extension checks passed");
    std::cout << "Existing production files modified: 0. Not exercised: engine "
                 "IPC, GUI, solver profile, task extension.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
