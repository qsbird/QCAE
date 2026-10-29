// Consumers that translate records into another representation must refuse record types they do
// not list. These tests classify every production entity per consumer and prove that an entity
// type no consumer knows is refused without changing any consumer.
#include "qcae/analysis_features.hpp"
#include "qcae/analysis_input.hpp"
#include "qcae/edit_session.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

struct Probe {
    qcae::EntityId id;
    std::string name;
};
namespace qcae {
template <> struct RecordTraits<Probe> {
    inline static constexpr RecordTypeId type_id{900101};
    static const void* token() {
        static const char token{};
        return &token;
    }
};
} // namespace qcae

namespace {
using namespace qcae;

void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> void refused(const Result<T>& result, const std::string& consumer) {
    check(!result.ok() && result.error && result.error->code == ErrorCode::unsupported_capability &&
              result.error->message.find("Probe") != std::string::npos,
          consumer + " did not refuse an unlisted record type");
}
template <class T> void not_refused(const Result<T>& result, const std::string& consumer) {
    check(!result.error || result.error->code != ErrorCode::unsupported_capability,
          consumer + " refused a document without unlisted record types");
}

RecordDescriptor probe_descriptor() {
    RecordDescriptor descriptor;
    descriptor.type = RecordTraits<Probe>::type_id;
    descriptor.name = "Probe";
    descriptor.query_kind = "probe";
    descriptor.cpp_type_token = RecordTraits<Probe>::token();
    descriptor.display_name = [](const void* value) -> std::string_view {
        return static_cast<const Probe*>(value)->name;
    };
    descriptor.fields = {{RecordFieldId{2}, "name", RecordFieldKind::text, false, "", {}}};
    descriptor.encode = [](const void* object) {
        const auto& value = *static_cast<const Probe*>(object);
        return RecordInput{
            {RecordTraits<Probe>::type_id, value.id.value},
            1,
            {{RecordFieldId{2}, RecordFieldKind::text, "", record_wire::text(value.name)}}};
    };
    descriptor.decode = [](const RecordInput& input) -> std::shared_ptr<const void> {
        const auto name =
            record_wire::read_text(record_wire::require(input, RecordFieldId{2}).payload);
        return std::make_shared<const Probe>(Probe{EntityId(input.key.identity), name});
    };
    descriptor.owned_bytes = [](const void* object) {
        const auto& value = *static_cast<const Probe*>(object);
        return sizeof(value) + value.id.value.size() + value.name.size();
    };
    descriptor.references = [](const void*, const RecordReferenceVisitor&) {};
    descriptor.validate = [](const void*, const DocumentView&) {};
    return descriptor;
}

struct Consumer {
    std::string name;
    std::span<const RecordTypeId> understood;
    // Production types the consumer deliberately refuses.
    std::set<RecordTypeId> refused;
};
std::vector<Consumer> consumers() {
    const std::set<RecordTypeId> shells{RecordTraits<records::Tri3>::type_id,
                                        RecordTraits<records::ShellSection>::type_id};
    return {{"Cantilever analysis input and checks",
             features::analysis::analysis_record_types(),
             shells},
            {"Model projection for Nastran export and export preview",
             model_projection_record_types(),
             shells}};
}

// A new entity type fails here until each consumer either lists it or it is added to that
// consumer's refused set above; refusal itself needs no consumer change.
void production_types_are_classified() {
    const auto registry = make_record_registry();
    for (const auto& consumer : consumers()) {
        for (const auto type : consumer.understood)
            check(registry->find(type) != nullptr,
                  consumer.name + " lists an unregistered record type");
        for (const auto type : registry->types()) {
            const bool listed =
                std::ranges::find(consumer.understood, type) != consumer.understood.end();
            const bool refused = consumer.refused.contains(type);
            check(listed != refused,
                  registry->find(type)->name + " is " +
                      (listed ? "both listed and refused" : "neither listed nor refused") + " by " +
                      consumer.name +
                      "; list it in the consumer if it is handled, otherwise add it to the "
                      "consumer's refused set in tests/record_consumer_tests.cpp");
        }
    }
}

void unlisted_type_is_refused_by_every_consumer() {
    auto registry = std::make_shared<RecordRegistry>();
    for (auto descriptor : generated_record_descriptors())
        registry->add(std::move(descriptor));
    registry->add(probe_descriptor());
    registry->add_rule(records::validate_relations);
    registry->freeze();
    const DocumentView empty(registry, {{DocumentId("consumers"), DocumentEpoch("epoch")}, 1});
    EditSession edit(empty);
    edit.put(Probe{EntityId("probe"), "unlisted"});
    const auto probed = edit.prepare().candidate;

    const EntityId analysis("analysis");
    const auto checks = [&](const DocumentView& view) {
        return features::analysis::RuleCatalog{}.check(view, analysis);
    };
    const auto input = [&](const DocumentView& view) {
        return features::analysis::freeze_analysis_input(view, analysis, ProfileRef{}, {});
    };
    const auto nastran = [&](const DocumentView& view) {
        return validate_nastran_export(view, analysis, NastranCodec{});
    };
    refused(checks(probed), "Analysis checks");
    refused(input(probed), "Analysis input");
    refused(nastran(probed), "Nastran export");
    not_refused(checks(empty), "Analysis checks");
    not_refused(input(empty), "Analysis input");
    not_refused(nastran(empty), "Nastran export");

    const auto error = unsupported_record_type(probed, model_projection_record_types(), "Export");
    check(error && error->code() == ErrorCode::unsupported_capability &&
              error->field() == "probe" &&
              std::string(error->what()) == "Export does not support Probe records",
          "The shared check did not name the unlisted record type");
    check(!unsupported_record_type(empty, {}, "Empty"),
          "A document without records reported an unsupported type");
}
} // namespace

int main() {
    try {
        production_types_are_classified();
        unlisted_type_is_refused_by_every_consumer();
        std::cout << "PASS: record consumers refuse unlisted record types\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
