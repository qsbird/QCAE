#include "qcae/artifacts_local.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include "qcae/sqlite_store.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace qcae;
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value) {
    require(value.ok(), value.error ? value.error->message : "Missing value");
    return std::move(*value.value);
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Model fixture(const NastranCodec& codec) {
    const auto root = std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran";
    ImportRequest request{
        "cantilever.bdf", {}, codec.definition().reference, "package-fixture", "mm-N-MPa"};
    for (const auto* path :
         {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"})
        request.resources.push_back({path, read_file(root / path)});
    auto decoded = codec.decode(request);
    require(decoded.report.complete && decoded.candidate.has_value(), "Cantilever import failed");
    return *decoded.candidate;
}
RecordApplicationOptions options(const std::shared_ptr<SqliteWorkspaceStore>& store,
                                 const ProfileRef& installed,
                                 bool enforce) {
    RecordApplicationOptions options;
    options.registry = make_record_registry();
    options.records = store;
    options.projects = store;
    options.profiles_supported = [installed, enforce](const DocumentView& view) {
        bool valid = true;
        view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& image) {
            valid =
                valid &&
                (!enforce || image->get<records::AnalysisDefinition>().target.profile == installed);
        });
        view.visit(RecordTraits<records::SourceIdentifier>::type_id, [&](const Record& image) {
            valid =
                valid && (!enforce || image->get<records::SourceIdentifier>().profile == installed);
        });
        return valid;
    };
    return options;
}
} // namespace
int main() {
    using namespace qcae;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("qcae-nastran-package-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(directory);
        NastranCodec codec;
        const auto installed = codec.definition().reference;
        const auto imported = fixture(codec);
        const auto view = records_from_model(imported, make_record_registry());
        const auto plan = good(validate_nastran_export(view, imported.analyses.front().id, codec));
        verify_nastran_readback(plan, plan.resources, codec);
        auto corrupted = plan.resources;
        for (auto& resource : corrupted)
            if (resource.path == "mesh/nodes.bdf") {
                const auto at = resource.text.find("100.0");
                require(at != std::string::npos, "Fixture coordinate unavailable");
                resource.text.replace(at, 5, "101.0");
            }
        bool rejected = false;
        try {
            verify_nastran_readback(plan, corrupted, codec);
        } catch (const RecordError&) {
            rejected = true;
        }
        require(rejected, "Semantic readback accepted a changed node coordinate");
        const auto ui = nastran_ui_contribution(codec);
        require(ui.operation == "model.export" && ui.profile == installed && ui.units == "mm-N-MPa",
                "UI contribution is not bound to real export semantics");
        const ProfileRef old{
            "qcae.nastran.linear-static",
            "0.1.0",
            "sha256:bb856be32336514d1aebf67b524ed0403664291cb4ba9e473e922ae7bb71b662"};
        auto model = imported;
        for (auto& item : model.analyses)
            item.target.profile = old;
        for (auto& item : model.sources)
            item.profile = old;
        auto invented = old;
        invented.definition_digest.back() = '3';
        auto unknown_model = model;
        for (auto& item : unknown_model.analyses)
            item.target.profile = invented;
        for (auto& item : unknown_model.sources)
            item.profile = invented;
        const auto unsupported = migrate_nastran_profile(
            records_from_model(unknown_model, make_record_registry()), invented, installed);
        require(!unsupported.ok() && unsupported.error->code == ErrorCode::schema_unsupported,
                "Matching source/input claims admitted an unknown old profile definition");
        auto wrong_destination = installed;
        wrong_destination.definition_digest += "-wrong";
        require(!migrate_nastran_profile(
                     records_from_model(model, make_record_registry()), old, wrong_destination)
                     .ok(),
                "Migration accepted an unavailable destination profile definition");
        const Caller caller{"package-migration-test"};
        const auto source = directory / "old.qcae";
        {
            auto store =
                std::make_shared<SqliteWorkspaceStore>((directory / "old.sqlite").string());
            RecordApplication app(options(store, installed, false));
            auto info = good(app.create_document(caller, "Old cantilever", "create"));
            good(app.execute(
                caller,
                {info.document, info.revision},
                "test.seed",
                "seed-old",
                [model](const DocumentView& before, const RecordIdentityAllocator&) {
                    auto candidate = records_from_model(model, before.registry(), before.version());
                    PreparedRecordChange change{
                        before.version(),
                        std::move(candidate),
                        record_changes_from_models({}, model, before.registry()),
                        {}};
                    return Result<RecordPreparedOperation>{
                        Status::success,
                        RecordPreparedOperation{std::move(change),
                                                "Seed old project",
                                                model.analyses.front().id,
                                                "seed-old",
                                                0,
                                                false},
                        {}};
                },
                "seed"));
            info = good(app.current_document());
            info = good(app.save_document(
                caller, {info.document, info.revision}, source.string(), true, "save-old"));
            good(app.close_document(
                caller, {info.document, info.revision}, ClosePolicy::discard, "close-old"));
        }
        const auto source_digest = artifact_sha256(read_file(source));
        {
            auto store =
                std::make_shared<SqliteWorkspaceStore>((directory / "new.sqlite").string());
            RecordApplication app(options(store, installed, true));
            require(!app.open_document(caller, source.string(), "ordinary").ok(),
                    "Ordinary open silently rebound an old profile");
            auto migration = [&](const ProfileRef& expected) {
                return ProjectRecordMigration{
                    record_wire::strings(std::array<std::string, 2>{
                        record_wire::profile(expected), record_wire::profile(installed)}),
                    [expected, installed](const DocumentView& candidate) {
                        auto prepared = migrate_nastran_profile(candidate, expected, installed);
                        if (!prepared.ok())
                            throw RecordError(prepared.error->code, prepared.error->message);
                        return std::move(*prepared.value);
                    }};
            };
            auto wrong = old;
            wrong.definition_digest += "-wrong";
            require(!app.open_migrated_document(caller, source.string(), "wrong", migration(wrong))
                         .ok(),
                    "Migration accepted the wrong old semantic digest");
            auto info = good(
                app.open_migrated_document(caller, source.string(), "migrate", migration(old)));
            require(info.dirty && info.saved_path.empty(),
                    "Migrated project reused the protected source save path");
            const auto migrated = good(app.snapshot(info.document));
            const auto result = model_from_records(migrated.records);
            require(result.nodes == model.nodes && result.materials == model.materials &&
                        result.beams == model.beams && result.sections == model.sections &&
                        result.sources.size() == model.sources.size(),
                    "Migration changed physical fields or numbering map");
            for (const auto& item : result.analyses)
                require(item.target.profile == installed, "Analysis retained an old target");
            for (const auto& item : result.sources)
                require(item.profile == installed, "Source numbering retained an old profile");
            const auto replay = good(
                app.open_migrated_document(caller, source.string(), "migrate", migration(old)));
            require(replay.document.id == info.document.id,
                    "Identical migration replay activated another document");
            require(
                !app.open_migrated_document(caller, source.string(), "migrate", migration(wrong))
                     .ok(),
                "Same migration key accepted different source semantics");
            good(app.close_document(caller,
                                    {info.document, info.revision},
                                    ClosePolicy::keep_recovery,
                                    "keep-migrated"));
            info = good(app.recover_document(caller, "recover-migrated"));
            require(info.dirty && info.saved_path.empty(),
                    "Recovery lost the unsaved migrated content state");
            const auto destination = (directory / "new.qcae").string();
            info = good(app.save_document(
                caller, {info.document, info.revision}, destination, true, "save-new"));
            const auto canonical_destination = std::filesystem::canonical(destination).string();
            require(!info.dirty && info.saved_path == canonical_destination,
                    "Save-as did not establish a new migrated save point");
            good(app.close_document(
                caller, {info.document, info.revision}, ClosePolicy::discard, "close-new"));
            info = good(app.open_document(caller, destination, "open-new"));
            require(!info.dirty && info.saved_path == canonical_destination,
                    "Normal reopen did not retain the migrated save point");
            const auto reopened = model_from_records(good(app.snapshot(info.document)).records);
            require(reopened.analyses == result.analyses && reopened.sources == result.sources &&
                        reopened.nodes == result.nodes && reopened.beams == result.beams,
                    "Reopened migrated project changed its bound profile or physical model");
        }
        require(artifact_sha256(read_file(source)) == source_digest,
                "Profile migration modified the protected source file");
        std::filesystem::remove_all(directory);
        std::cout << "PASS: Nastran validation/readback and explicit atomic profile migration\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
