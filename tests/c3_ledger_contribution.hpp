#pragma once
#include "qcae/engine_contributions.hpp"
#include "qcae/operation_ledger.hpp"
#include "qcae/records.hpp"
#include "c3_qt_source_bridge.hpp"
#include "c3_process_observation.hpp"
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
#include "c3_sqlite_copy_bridge.h"
#endif
#include <cstdio>

namespace c3_ledger_test {
struct Seed {
    std::uint32_t node_count{};
};
struct Begin {
    std::string run_id;
    std::string operation;
};
struct End {};
struct FrameProbe {
    bool oversize{};
};
inline std::string entity_id(const char* kind, std::size_t index) {
    char digits[13]{};
    std::snprintf(digits, sizeof(digits), "%012zu", index);
    return std::string(kind) + "-" + digits;
}
} // namespace c3_ledger_test

namespace qcae::operations {
template <> struct InputTraits<c3_ledger_test::Seed> {
    static constexpr std::string_view schema_id = "test.locality.seed.v1";
    static OperationDefinition definition() {
        return {"test.locality.seed",
                1,
                std::string(schema_id),
                OperationEffect::document_write,
                {true, true, true, true, false},
                {{1, "node_count", "uint32", {}, true}}};
    }
    static Result<c3_ledger_test::Seed> from_value(const Value& value) {
        constexpr std::array<std::string_view, 1> fields{"node_count"};
        const auto object = wire::object_fields(value, fields, "parameters");
        if (!object.ok())
            return {object.status, {}, object.error};
        const auto count = wire::positive_uint32((*object.value)->at("node_count"), "node_count");
        if (!count.ok())
            return {count.status, {}, count.error};
        if (*count.value != 1000 && *count.value != 10000 && *count.value != 100000)
            return {Status::failed,
                    {},
                    Diagnostic{ErrorCode::invalid_input,
                               "Only frozen C3 fixture sizes are accepted",
                               "node_count"}};
        return {Status::success, c3_ledger_test::Seed{*count.value}, {}};
    }
};
template <> struct InputTraits<c3_ledger_test::Begin> {
    static constexpr std::string_view schema_id = "test.ledger.begin.v1";
    static OperationDefinition definition() {
        return {"test.ledger.begin",
                1,
                std::string(schema_id),
                OperationEffect::read_only,
                {true, true, true, false, false},
                {{1, "run_id", "string", {}, true}, {2, "operation", "string", {}, true}}};
    }
    static Result<c3_ledger_test::Begin> from_value(const Value& value) {
        constexpr std::array<std::string_view, 2> fields{"run_id", "operation"};
        const auto object = wire::object_fields(value, fields, "parameters");
        if (!object.ok())
            return {object.status, {}, object.error};
        const auto id = wire::string_value((*object.value)->at("run_id"), "run_id");
        if (!id.ok())
            return {id.status, {}, id.error};
        const auto operation = wire::string_value((*object.value)->at("operation"), "operation");
        if (!operation.ok())
            return {operation.status, {}, operation.error};
        if (*operation.value != "node.move" && *operation.value != "material.set_young_modulus" &&
            *operation.value != "test.ledger.frame_probe")
            return {Status::failed,
                    {},
                    Diagnostic{ErrorCode::invalid_input,
                               "Only frozen C3 local edit operations are accepted",
                               "operation"}};
        return {Status::success, c3_ledger_test::Begin{*id.value, *operation.value}, {}};
    }
};
template <> struct InputTraits<c3_ledger_test::FrameProbe> {
    static constexpr std::string_view schema_id = "test.ledger.frame_probe.v1";
    static OperationDefinition definition() {
        return {"test.ledger.frame_probe",
                1,
                std::string(schema_id),
                OperationEffect::read_only,
                {true, true, true, false, false},
                {{1, "kind", "string", {}, true}}};
    }
    static Result<c3_ledger_test::FrameProbe> from_value(const Value& value) {
        constexpr std::array<std::string_view, 1> fields{"kind"};
        const auto object = wire::object_fields(value, fields, "parameters");
        if (!object.ok())
            return {object.status, {}, object.error};
        const auto kind = wire::string_value((*object.value)->at("kind"), "kind");
        if (!kind.ok())
            return {kind.status, {}, kind.error};
        if (*kind.value != "oversize" && *kind.value != "large")
            return {Status::failed,
                    {},
                    Diagnostic{ErrorCode::invalid_input,
                               "Only fixed transport probe payloads are accepted",
                               "kind"}};
        return {Status::success, c3_ledger_test::FrameProbe{*kind.value == "oversize"}, {}};
    }
};
template <> struct InputTraits<c3_ledger_test::End> {
    static constexpr std::string_view schema_id = "test.ledger.end.v1";
    static OperationDefinition definition() {
        return {"test.ledger.end",
                1,
                std::string(schema_id),
                OperationEffect::read_only,
                {true, true, true, false, false},
                {}};
    }
    static Result<c3_ledger_test::End> from_value(const Value& value) {
        constexpr std::array<std::string_view, 0> fields{};
        const auto object = wire::object_fields(value, fields, "parameters");
        if (!object.ok())
            return {object.status, {}, object.error};
        return {Status::success, c3_ledger_test::End{}, {}};
    }
};
} // namespace qcae::operations

namespace c3_ledger_test {
inline qcae::operations::Value snapshot_value(const qcae::ledger::Snapshot& snapshot) {
    using namespace qcae::operations;
    Value::Array frames;
    for (const auto& frame : snapshot.frames)
        frames.emplace_back(Value::Object{
            {"stage",
             Value(std::string(qcae::ledger::stage_names[static_cast<std::size_t>(frame.stage)]))},
            {"bytes", Value(std::to_string(frame.bytes))},
            {"kind", Value(frame.kind)},
            {"operation", Value(frame.operation)},
            {"request_id", Value(frame.request_id)},
            {"event", Value(frame.event)}});
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
    Value::Array copy_profile;
    qcae_c3_sqlite_visit_copy_profile(
        &copy_profile,
        [](void* output, const char* function, size_t bytes, size_t calls, int aggregate) {
            static_cast<Value::Array*>(output)->emplace_back(
                Value::Object{{"function", Value(function)},
                              {"copy_kind", Value(aggregate ? "aggregate" : "explicit")},
                              {"bytes", Value(std::to_string(bytes))},
                              {"calls", Value(std::to_string(calls))}});
        });
#endif
    Value::Object stages;
    for (std::size_t stage = 0; stage < snapshot.covered.size(); ++stage) {
        Value::Object metrics;
        for (std::size_t metric = 0; metric < snapshot.values[stage].size(); ++metric) {
            const auto& value = snapshot.values[stage][metric];
            metrics.emplace(std::string(qcae::ledger::metric_names[metric]),
                            value ? Value(std::to_string(*value)) : Value("unmeasured"));
        }
        stages.emplace(std::string(qcae::ledger::stage_names[stage]),
                       Value(Value::Object{{"covered", Value(snapshot.covered[stage])},
                                           {"metrics", Value(std::move(metrics))}}));
    }
    return Value(
        Value::Object{{"run_id", Value(snapshot.identity.run_id)},
                      {"frames", Value(std::move(frames))},
                      {"frame_trace_complete", Value(snapshot.frame_trace_complete)},
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
                      {"sqlite_internal_includes_bind", Value(true)},
                      {"sqlite_copy_profile", Value(std::move(copy_profile))},
#else
                      {"sqlite_internal_includes_bind", Value(false)},
#endif
                      {"document_id", Value(snapshot.identity.document_id)},
                      {"document_epoch", Value(snapshot.identity.document_epoch)},
                      {"base_revision", Value(std::to_string(snapshot.identity.base_revision))},
                      {"stages", Value(std::move(stages))}});
}
inline qcae::ipc::EngineContribution contribution() {
    using namespace qcae;
    using namespace operations;
    return {
        "test.c3.ledger",
        {},
        [](OperationRegistry& registry, RecordApplication& app, std::function<TaskService&()>) {
            auto result = registry.register_typed<Seed>(
                InputTraits<Seed>::definition(),
                [&app](const OperationContext& context, const Seed& input) -> Result<Value> {
                    const auto signature = "fixture:" + std::to_string(input.node_count);
                    const auto receipt = app.execute(
                        context.caller,
                        {*context.document, *context.expected_revision},
                        "test.locality.seed",
                        signature,
                        [input, signature](const DocumentView& view, const RecordIdentityAllocator&)
                            -> Result<RecordPreparedOperation> {
                            if (view.size())
                                return {Status::failed,
                                        {},
                                        Diagnostic{ErrorCode::invalid_input,
                                                   "Fixture requires an empty document",
                                                   "document"}};
                            EditSession edit(view);
                            const auto material = EntityId("material-locality-0001");
                            const auto section = EntityId("section-locality-0001");
                            edit.put(records::Material{material, "Locality steel", 210000.0, 0.3});
                            edit.put(records::BeamSection{section,
                                                          "Locality section",
                                                          material,
                                                          100.0,
                                                          833.333,
                                                          833.333,
                                                          1400.0});
                            for (std::size_t index = 0; index < input.node_count; ++index) {
                                std::array<double, 3> point{static_cast<double>(index), 0, 0};
                                if (index == 0)
                                    point = {0, -100, -100};
                                else if (index + 1 == input.node_count)
                                    point = {static_cast<double>(index), 100, 100};
                                edit.put(
                                    records::Node{EntityId(entity_id("node", index)), point, {}});
                            }
                            for (std::size_t index = 0; index + 1 < input.node_count; ++index)
                                edit.put(records::Beam{EntityId(entity_id("beam", index)),
                                                       section,
                                                       {EntityId(entity_id("node", index)),
                                                        EntityId(entity_id("node", index + 1))},
                                                       {0, 1, 0},
                                                       {}});
                            return {Status::success,
                                    RecordPreparedOperation{edit.prepare(),
                                                            "Seed locality chain",
                                                            material,
                                                            signature,
                                                            0,
                                                            false},
                                    {}};
                        },
                        context.idempotency_key);
                    if (!receipt.ok())
                        return {receipt.status, {}, receipt.error};
                    return {Status::success, change_receipt_value(*receipt.value), {}};
                });
            if (!result.ok())
                return result;
            result = registry.register_typed<Begin>(
                InputTraits<Begin>::definition(),
                [&app](const OperationContext& context, const Begin& input) -> Result<Value> {
                    const auto document = app.current_document();
                    if (!document.ok())
                        return {document.status, {}, document.error};
                    if (ledger::current() || ledger::pending())
                        return {Status::conflict,
                                {},
                                Diagnostic{ErrorCode::invalid_input,
                                           "A measurement is already active",
                                           "run_id"}};
                    auto measurement = std::make_shared<ledger::OperationLedger>(
                        ledger::Identity{input.run_id,
                                         context.document->id.value,
                                         context.document->epoch.value,
                                         *context.expected_revision},
                        input.operation);
#ifdef QCAE_C3_QT_SDK_MANIFEST
                    c3_qt_source::bridge().begin(input.run_id);
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
                    c3_qt_source::harfbuzz_bridge().begin(input.run_id);
#endif
#endif
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
                    {
                        ledger::Scope scope(measurement);
                        if (!qcae_c3_sqlite_observer_begin())
                            return {Status::failed,
                                    {},
                                    Diagnostic{ErrorCode::invalid_input,
                                               "SQLite byte observer requires audited 3.51.0",
                                               "sqlite"}};
                    }
#endif
                    measurement->add(
                        ledger::Stage::records, ledger::Metric::full_model_serializations, 0);
                    measurement->add(
                        ledger::Stage::records, ledger::Metric::full_model_materializations, 0);
                    ledger::arm(std::move(measurement));
                    return {Status::success, Value(Value::Object{{"armed", Value(true)}}), {}};
                });
            if (!result.ok())
                return result;
            result = registry.register_typed<FrameProbe>(
                InputTraits<FrameProbe>::definition(),
                [](const OperationContext&, const FrameProbe& input) -> Result<Value> {
                    // This executable-only transport fixture never mutates a
                    // document and never accepts arbitrary payload sizes/text.
                    const auto bytes = input.oversize ? 2 * 1024 * 1024 : 512 * 1024;
                    return {Status::success,
                            Value(Value::Object{{"payload", Value(std::string(bytes, 'x'))}}),
                            {}};
                });
            if (!result.ok())
                return result;
            return registry.register_typed<End>(
                InputTraits<End>::definition(),
                [](const OperationContext&, const End&) -> Result<Value> {
                    const auto active = ledger::current();
                    ledger::activate({});
                    ledger::arm({});
                    if (!active)
                        return {Status::failed,
                                {},
                                Diagnostic{ErrorCode::invalid_input,
                                           "No measurement is active",
                                           "run_id"}};
                    auto result = snapshot_value(active->snapshot());
#ifdef QCAE_C3_QT_SDK_MANIFEST
                    const auto source_json = QJsonDocument(c3_qt_source::bridge().snapshot())
                                                 .toJson(QJsonDocument::Compact)
                                                 .toStdString();
                    std::get<Value::Object>(result.data)
                        .emplace("qt_source_observation_json", Value(source_json));
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
                    const auto hb_json = QJsonDocument(c3_qt_source::harfbuzz_bridge().snapshot())
                                             .toJson(QJsonDocument::Compact)
                                             .toStdString();
                    std::get<Value::Object>(result.data)
                        .emplace("harfbuzz_source_observation_json", Value(hb_json));
#endif
#endif
                    const auto memory = c3_process_observation::peak_rss();
                    std::get<Value::Object>(result.data)
                        .emplace(
                            "process_memory",
                            Value(Value::Object{
                                {"process_id", Value(std::to_string(memory.process_id))},
                                {"peak_rss_bytes",
                                 memory.bytes ? Value(std::to_string(*memory.bytes)) : Value()},
                                {"source", Value(std::string("getrusage(RUSAGE_SELF)"))},
                                {"scope", Value(std::string("process_lifetime_high_water"))}}));
                    return {Status::success, std::move(result), {}};
                });
        }};
}
} // namespace c3_ledger_test
