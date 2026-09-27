#include "qcae/core.hpp"
#include "qcae/legacy_migration.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include "qcae/sqlite_store.hpp"
#include "../features/legacy_api/src/legacy_state.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace {
using namespace qcae;
namespace fs = std::filesystem;
void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value) {
    check(value.ok(), value.error ? value.error->message : "Missing result");
    return std::move(*value.value);
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
QJsonObject json(const fs::path& path) {
    const auto bytes = read_file(path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &error);
    check(error.error == QJsonParseError::NoError && document.isObject(), "Invalid golden JSON");
    return document.object();
}
std::string text(const QJsonObject& value, const char* key) {
    check(value.value(key).isString(), std::string("Missing golden string: ") + key);
    return value.value(key).toString().toStdString();
}
EntityId id(const QJsonObject& value, const char* key) {
    return EntityId(text(value, key));
}
std::vector<EntityId> ids(const QJsonObject& value, const char* key) {
    check(value.value(key).isArray(), std::string("Missing golden IDs: ") + key);
    std::vector<EntityId> result;
    for (const auto& item : value.value(key).toArray()) {
        check(item.isString(), "Malformed golden ID");
        result.emplace_back(item.toString().toStdString());
    }
    return result;
}
double real(const QJsonObject& value, const char* key) {
    check(value.value(key).isDouble(), std::string("Missing golden real: ") + key);
    return value.value(key).toDouble();
}
Vec3 vector(const QJsonObject& value, const char* key) {
    const auto array = value.value(key).toArray();
    check(array.size() == 3, "Golden vector has three components");
    return {array[0].toDouble(), array[1].toDouble(), array[2].toDouble()};
}
ProfileRef profile(const QJsonObject& value) {
    return {text(value, "profile_id"),
            text(value, "profile_version"),
            text(value, "definition_digest")};
}
Model golden_model(const QJsonObject& snapshot) {
    Model result;
    const auto source_profile = profile(snapshot.value("profile_ref").toObject());
    for (const auto& item : snapshot.value("entities").toArray()) {
        const auto entity = item.toObject();
        const auto entity_id = id(entity, "entity_id");
        const auto kind = text(entity, "kind");
        const auto name = text(entity, "name");
        if (kind == "material") {
            std::optional<double> poisson;
            if (entity.contains("poisson_ratio"))
                poisson = real(entity, "poisson_ratio");
            result.materials.push_back(
                {entity_id, name, real(entity, "young_modulus_mpa"), poisson});
        } else if (kind == "node")
            result.nodes.push_back({entity_id, vector(entity, "position_mm")});
        else if (kind == "section")
            result.sections.push_back({entity_id,
                                       name,
                                       id(entity, "material_id"),
                                       real(entity, "area_mm2"),
                                       real(entity, "i1_mm4"),
                                       real(entity, "i2_mm4"),
                                       real(entity, "torsion_mm4")});
        else if (kind == "beam") {
            const auto nodes = ids(entity, "nodes");
            check(nodes.size() == 2, "Golden beam endpoints");
            result.beams.push_back({entity_id,
                                    id(entity, "section_id"),
                                    {nodes[0], nodes[1]},
                                    vector(entity, "orientation")});
        } else if (kind == "part")
            result.parts.push_back({entity_id, name, ids(entity, "members")});
        else if (kind == "assembly")
            result.assemblies.push_back({entity_id, name, ids(entity, "children")});
        else if (kind == "set")
            result.sets.push_back({entity_id, name, ids(entity, "members")});
        else if (kind == "include") {
            std::optional<EntityId> parent;
            if (entity.contains("parent_id"))
                parent = id(entity, "parent_id");
            result.includes.push_back(
                {entity_id, text(entity, "path"), parent, ids(entity, "members")});
        } else if (kind == "force")
            result.forces.push_back({entity_id, id(entity, "node_id"), vector(entity, "force_n")});
        else if (kind == "constraint")
            result.constraints.push_back({entity_id, ids(entity, "nodes"), text(entity, "dofs")});
        else if (kind == "analysis")
            result.analyses.push_back(
                {entity_id,
                 name,
                 {profile(entity.value("profile_ref").toObject()), "linear_static"},
                 ids(entity, "forces"),
                 ids(entity, "constraints")});
        else
            throw std::runtime_error("Unknown golden entity kind: " + kind);
        for (const auto& source : entity.value("sources").toArray()) {
            const auto value = source.toObject();
            result.sources.push_back({entity_id,
                                      text(value, "source_model_id"),
                                      id(value, "include_id"),
                                      source_profile,
                                      text(value, "namespace"),
                                      std::stoull(text(value, "number"))});
        }
    }
    check(result.sources.size() ==
              static_cast<std::size_t>(snapshot.value("source_identifier_count").toInt()),
          "Golden source mapping count");
    return result;
}
void order_model(Model& model) {
    const auto order = [](auto& collection) {
        std::sort(collection.begin(), collection.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
    };
    order(model.materials);
    order(model.nodes);
    order(model.sections);
    order(model.beams);
    order(model.parts);
    order(model.assemblies);
    order(model.sets);
    order(model.includes);
    order(model.forces);
    order(model.constraints);
    order(model.analyses);
    std::sort(model.sources.begin(), model.sources.end(), [](const auto& left, const auto& right) {
        return std::tie(left.source_model_id, left.entity, left.name_space, left.number) <
               std::tie(right.source_model_id, right.entity, right.name_space, right.number);
    });
}
void compare_model(Model actual, const QJsonObject& snapshot) {
    auto expected = golden_model(snapshot);
    order_model(actual);
    order_model(expected);
    check(actual == expected,
          "All twelve collections, entity values, references and source mappings match golden");
    auto references = model_references(actual);
    std::vector<Reference> golden;
    for (const auto& item : snapshot.value("references").toArray()) {
        const auto value = item.toObject();
        golden.push_back({id(value, "from"), id(value, "to"), text(value, "role")});
    }
    const auto less = [](const auto& left, const auto& right) {
        return std::tie(left.from, left.to, left.role) < std::tie(right.from, right.to, right.role);
    };
    std::sort(references.begin(), references.end(), less);
    std::sort(golden.begin(), golden.end(), less);
    check(references == golden, "Every authority reference matches golden");
}
void compare_history(const HistorySnapshot& actual, const QJsonObject& snapshot) {
    const auto expected = snapshot.value("history").toObject();
    const auto items = expected.value("items").toArray();
    check(actual.cursor == static_cast<std::size_t>(expected.value("cursor").toInt()) &&
              actual.items.size() == static_cast<std::size_t>(items.size()),
          "History cursor and redo suffix preserved");
    for (std::size_t index = 0; index < actual.items.size(); ++index) {
        const auto item = items[static_cast<qsizetype>(index)].toObject();
        check(actual.items[index].transaction.value == text(item, "transaction_id") &&
                  actual.items[index].label == text(item, "label") &&
                  actual.items[index].applied == item.value("applied").toBool(),
              "History stable identity and label preserved");
    }
}
void commit_migration(SqliteWorkspaceStore& destination, const LoadedRows& image) {
    check(destination.load_rows().generation == 0, "Migration only writes a fresh destination");
    StoreBatch batch{0, "legacy-migration", {}};
    for (const auto& row : image.rows)
        batch.mutations.push_back({row.key, row.value});
    check(destination.commit_rows(batch).generation == 1,
          "Detached migration committed atomically");
}
std::vector<std::string> split_key(std::string_view key) {
    std::vector<std::string> result;
    while (!key.empty()) {
        const auto colon = key.find(':');
        check(colon != std::string_view::npos, "Length-prefixed fact key");
        const auto size = std::stoull(std::string(key.substr(0, colon)));
        key.remove_prefix(colon + 1);
        check(size <= key.size(), "Fact component in bounds");
        result.emplace_back(key.substr(0, size));
        key.remove_prefix(size);
    }
    check(result.size() == 3, "Caller, operation and idempotency components");
    return result;
}
void compare_facts(MemoryApplication& application,
                   const RecordStateImage& original,
                   const DocumentInfo& recovered,
                   SqliteWorkspaceStore& store) {
    const auto generation = store.load_rows().generation;
    for (const auto& [key, fact] : original.operations) {
        const auto parts = split_key(key);
        const auto receipt =
            good(application.operation({parts[0]}, recovered.document, parts[1], parts[2]));
        check(receipt.replayed && receipt.transaction == fact.receipt.transaction &&
                  receipt.committed_revision == fact.receipt.committed_revision &&
                  receipt.current_revision == recovered.revision &&
                  receipt.current_content_state == recovered.content_state,
              "Idempotent operation facts retained");
    }
    for (const auto& [key, fact] : original.host_operations) {
        const auto parts = split_key(key);
        const auto info = good(application.host_operation({parts[0]}, parts[1], parts[2]));
        check(info.document.id == fact.result.document.id &&
                  info.document.epoch == fact.result.document.epoch &&
                  info.content_state == fact.result.content_state &&
                  info.revision == fact.result.revision,
              "Host operation facts retain their original outcome");
    }
    check(store.load_rows().generation == generation, "Fact lookup never recommits");
}
void test_fixture(const fs::path& directory, const QJsonObject& entry, const std::string& working) {
    const auto path = directory / text(entry, "path");
    const auto bytes = read_file(path);
    const auto expected_bytes = read_file(directory / text(entry, "expectation_path"));
    const auto digest = [](const std::string& value) {
        return QCryptographicHash::hash(QByteArray::fromStdString(value),
                                        QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    };
    check(digest(bytes) == text(entry, "sha256") &&
              digest(expected_bytes) == text(entry, "expectation_sha256"),
          "Frozen source and expected JSON hashes match manifest");
    const auto snapshot =
        json(directory / text(entry, "expectation_path")).value("snapshot").toObject();
    const auto expected_info = snapshot.value("document").toObject();
    const auto registry = make_record_registry();
    auto destination = std::make_shared<SqliteWorkspaceStore>(working);
    if (text(entry, "kind") == "project") {
        const auto project_path = working + ".project.qcae";
        fs::copy_file(path, project_path);
        MemoryApplication application({}, destination);
        const auto info = good(application.open_document({"migration-test"}, project_path, "open"));
        compare_model(good(application.snapshot(info.document)), snapshot);
        check(info.document.id.value != text(expected_info, "document_id") &&
                  info.document.epoch.value != text(expected_info, "document_epoch") &&
                  info.revision == 0,
              "Normal open allocates fresh document and epoch with fresh history");
        check(info.project_id == text(expected_info, "project_id") &&
                  info.content_state == text(expected_info, "content_state") && !info.dirty &&
                  info.saved_path == fs::canonical(project_path).string(),
              "Normal open retains project/content identity");
        check(good(application.history(info.document)).items.empty(),
              "Saved project opens without workspace history");
        check(read_file(project_path) == bytes,
              "Normal open leaves consumed project bytes unchanged");
    } else {
        const auto source = read_legacy_workspace_readonly(path.string());
        const auto image = migrate_legacy_workspace(source, registry);
        const auto original = decode_record_state_image(image.rows, registry);
        check(original.document &&
                  original.document->document.id.value == text(expected_info, "document_id") &&
                  original.document->document.epoch.value == text(expected_info, "document_epoch"),
              "Detached migration preserves document identity");
        commit_migration(*destination, image);
        MemoryApplication application({}, destination);
        check(application.recovery_available() && !application.current_document().ok(),
              "Recovery remains explicit");
        const auto info = good(application.recover_document({"migration-test"}, "recover"));
        compare_model(good(application.snapshot(info.document)), snapshot);
        compare_history(good(application.history(info.document)), snapshot);
        check(info.document.id == original.document->document.id &&
                  info.document.epoch != original.document->document.epoch &&
                  info.revision == original.document->revision &&
                  info.content_state == original.document->content_state,
              "Recovery retains DocId/revision/content and expires the old epoch");
        const auto stale = application.preview({"migration-test"},
                                               {original.document->document, info.revision},
                                               CreateMaterial{"stale", {1, "MPa"}});
        check(!stale.ok() && stale.error->code == ErrorCode::document_epoch_expired,
              "Old epoch cannot edit migrated document");
        compare_facts(application, original, info, *destination);
        if (text(entry, "fixture_id") == "redo-workspace") {
            good(application.redo({"migration-test"}, {info.document, info.revision}, "redo"));
            const auto redone = good(application.current_document());
            auto expected = golden_model(snapshot);
            check(expected.materials.size() == 1, "Redo golden material");
            expected.materials[0].young_modulus_mpa = 200000;
            auto actual = static_cast<Model>(good(application.snapshot(redone.document)));
            order_model(expected);
            order_model(actual);
            check(actual == expected &&
                      redone.content_state == original.history.back()->content_state,
                  "Redo changes exactly the intended material and content state");
            good(application.undo(
                {"migration-test"}, {redone.document, redone.revision}, "undo-redo"));
            compare_model(good(application.snapshot(info.document)), snapshot);
        }
    }
    check(read_file(path) == bytes, "Frozen source byte changes: zero");
}
void write_legacy_info(state_codec::Writer& writer, const DocumentInfo& info) {
    writer.text(info.document.id.value);
    writer.text(info.document.epoch.value);
    writer.number(info.revision);
    writer.text(info.content_state);
    writer.text(info.name);
    writer.text(info.project_id);
    writer.text(info.saved_path);
    writer.text(info.saved_content_state);
}
// This test-only writer creates pending-save cases from a frozen decoded state.
// It never writes the protected source container.
std::string encode_legacy_workspace(const legacy_detail::Data& data) {
    state_codec::Writer writer;
    writer.text("QCAE-WORKSPACE");
    writer.number(2);
    writer.text(data.application_nonce);
    writer.number(data.next_id);
    writer.boolean(data.document.has_value());
    if (data.document)
        write_legacy_info(writer, *data.document);
    state_codec::write_model(writer, data.model);
    writer.text(data.initial_content_state);
    writer.number(data.history.size());
    for (const auto& entry : data.history) {
        writer.text(entry.transaction.value);
        writer.text(entry.label);
        writer.text(entry.content_state);
        write_model_delta(writer, entry.delta);
    }
    writer.number(data.cursor);
    writer.number(data.operations.size());
    for (const auto& [key, fact] : data.operations) {
        writer.text(key);
        writer.text(fact.signature);
        writer.text(fact.receipt.transaction.value);
        writer.number(fact.receipt.committed_revision);
        writer.number(fact.receipt.current_revision);
        writer.text(fact.receipt.current_content_state);
    }
    writer.number(data.host_operations.size());
    for (const auto& [key, fact] : data.host_operations) {
        writer.text(key);
        writer.text(fact.signature);
        write_legacy_info(writer, fact.result);
    }
    writer.boolean(data.save_intent.has_value());
    if (data.save_intent) {
        const auto& intent = *data.save_intent;
        writer.text(intent.host_key);
        writer.text(intent.signature);
        writer.text(intent.path);
        writer.text(intent.token);
        writer.text(intent.project_id);
        writer.text(intent.snapshot);
        writer.boolean(intent.save_as);
    }
    writer.boolean(data.recoverable);
    return writer.take();
}
void test_save_intent(const fs::path& directory,
                      const std::string& temporary,
                      bool matching_token) {
    const auto source_path = directory / "unsaved-workspace.sqlite";
    const auto original_bytes = read_file(source_path);
    const auto source = read_legacy_workspace_readonly(source_path.string());
    auto old = legacy_detail::decode_data(source.payload, {});
    const auto name = matching_token ? "verified-save" : "changed-save";
    const auto target = (fs::path(temporary) / (std::string(name) + ".qcae")).string();
    state_codec::Writer project;
    project.text("QCAE-PROJECT");
    project.number(1);
    project.text("pending-project");
    project.text(old.document->name);
    project.text(old.document->content_state);
    state_codec::write_model(project, old.model);
    old.save_intent = legacy_detail::SaveIntent{"14:migration-test13:save_document4:save",
                                                "original-save-signature",
                                                target,
                                                "original-save-token",
                                                "pending-project",
                                                project.take(),
                                                true};
    auto destination = std::make_shared<SqliteWorkspaceStore>(
        (fs::path(temporary) / (std::string(name) + ".sqlite")).string());
    const auto registry = make_record_registry();
    const auto migrated =
        migrate_legacy_workspace({source.generation, encode_legacy_workspace(old)}, registry);
    const auto state = decode_record_state_image(migrated.rows, registry);
    check(state.save_intent && state.save_intent->snapshot == old.save_intent->snapshot &&
              state.save_intent->token == old.save_intent->token &&
              state.save_intent->signature == old.save_intent->signature &&
              state.save_intent->host_key == old.save_intent->host_key &&
              state.save_intent->path == target &&
              state.save_intent->project_id == "pending-project" && state.save_intent->save_as,
          "Pending save verification retains exact old snapshot, token and facts");
    commit_migration(*destination, migrated);
    const auto canonical_target = destination->acquire_project(target);
    destination->publish_project(
        canonical_target,
        {matching_token ? "original-save-token" : "different-token", old.save_intent->snapshot});
    MemoryApplication application({}, destination);
    const auto info = good(application.recover_document({"migration-test"}, "recover"));
    auto expected = old.model;
    auto actual = static_cast<Model>(good(application.snapshot(info.document)));
    order_model(expected);
    order_model(actual);
    check(actual == expected, "Save reconciliation never replaces working engineering data");
    check(!decode_record_state_image(destination->load_rows().rows, registry).save_intent,
          "Resolved or changed save intent is cleared after recovery");
    const auto outcome = application.host_operation({"migration-test"}, "save_document", "save");
    if (matching_token)
        check(!info.dirty && info.project_id == "pending-project" &&
                  info.saved_path == canonical_target && outcome.ok(),
              "Matching token and bytes verify the published save");
    else
        check(info.dirty && info.saved_path.empty() && !outcome.ok(),
              "Changed token cannot become a successful save fact");
    check(read_file(source_path) == original_bytes,
          "Save-intent tests leave frozen source unchanged");
}
void test_owned_handler_injection(const std::string& path) {
    OwnedRowHandler handler{StoreSpace::task_record,
                            "test-owner",
                            [](const OwnedRowImage& row) {
                                check(row.schema_version == 1 && row.payload &&
                                          *row.payload == "payload",
                                      "Injected handler validates payload");
                            },
                            {},
                            {}};
    DocumentInfo old;
    {
        auto store = std::make_shared<SqliteWorkspaceStore>(path);
        MemoryApplication application({}, store, {}, {handler});
        old = good(application.create_document({"migration-test"}, "Owned handler", "create"));
        const StoreKey key{StoreSpace::task_record, "owned-task"};
        auto row = std::make_shared<const OwnedRowImage>(OwnedRowImage{
            key, "test-owner", 1, std::make_shared<const std::string>("payload"), {}});
        const OwnedRowUpdate update{key, {}, std::move(row)};
        good(application.record_application().update_owned_rows(
            {"migration-test"}, old.document, {&update, 1}));
    }
    auto store = std::make_shared<SqliteWorkspaceStore>(path);
    MemoryApplication application({}, store, {}, {handler});
    const auto info = good(application.recover_document({"migration-test"}, "recover"));
    const auto rows = good(application.record_application().owned_rows(
        info.document, StoreSpace::task_record, "test-owner"));
    check(rows.size() == 1 && *rows[0]->payload == "payload" && info.document.id == old.document.id,
          "Facade forwards owned handlers to the same durable RecordApplication");
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Fixture directory argument required");
        const fs::path directory(argv[1]);
        QTemporaryDir sandbox;
        check(sandbox.isValid(), "Temporary test destination available");
        const auto temporary = sandbox.path().toStdString();
        const auto manifest = json(directory / "manifest.json");
        std::size_t count{};
        for (const auto& item : manifest.value("fixtures").toArray()) {
            const auto entry = item.toObject();
            std::cout << "Checking " << text(entry, "fixture_id") << std::endl;
            test_fixture(directory,
                         entry,
                         (fs::path(temporary) / (text(entry, "fixture_id") + ".sqlite")).string());
            ++count;
        }
        check(count == 6, "All six frozen fixtures migrated");
        test_save_intent(directory, temporary, true);
        test_save_intent(directory, temporary, false);
        test_owned_handler_injection((fs::path(temporary) / "owned.sqlite").string());
        std::cout << "PASS: six frozen legacy fixtures migrated; source byte changes 0; "
                     "redo/facts/save-intent/epoch retained\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
