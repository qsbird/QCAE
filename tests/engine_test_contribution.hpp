#pragma once
#include "qcae/engine_contributions.hpp"
#include "qcae/records.hpp"

namespace contribution_test {
struct Relation {
    qcae::EntityId id, from, to;
    std::string name;
};
struct CreateRelation {
    qcae::EntityId from, to;
    std::string name;
};
} // namespace contribution_test

namespace qcae {
template <> struct RecordTraits<contribution_test::Relation> {
    inline static constexpr RecordTypeId type_id{900001};
    static const void* token() {
        static const char value{};
        return &value;
    }
};
} // namespace qcae
namespace qcae::operations {
template <> struct InputTraits<contribution_test::CreateRelation> {
    static constexpr std::string_view schema_id = "test.relation.create.v1";
    static OperationDefinition definition() {
        return {"test.relation.create",
                1,
                std::string(schema_id),
                OperationEffect::document_write,
                {true, true, true, true, false},
                {{1, "from_id", "entity_id", {}, true},
                 {2, "to_id", "entity_id", {}, true},
                 {3, "name", "string", {}, true}}};
    }
    static Result<contribution_test::CreateRelation> from_value(const Value& value) {
        constexpr std::array<std::string_view, 3> fields{"from_id", "to_id", "name"};
        const auto object = wire::object_fields(value, fields, "parameters");
        if (!object.ok())
            return {object.status, {}, object.error};
        const auto from = wire::entity_id((*object.value)->at("from_id"), "from_id");
        if (!from.ok())
            return {from.status, {}, from.error};
        const auto to = wire::entity_id((*object.value)->at("to_id"), "to_id");
        if (!to.ok())
            return {to.status, {}, to.error};
        const auto name = wire::string_value((*object.value)->at("name"), "name");
        if (!name.ok())
            return {name.status, {}, name.error};
        return {Status::success,
                contribution_test::CreateRelation{*from.value, *to.value, *name.value},
                {}};
    }
};
} // namespace qcae::operations

namespace contribution_test {
inline qcae::RecordDescriptor relation_descriptor() {
    using namespace qcae;
    RecordDescriptor descriptor;
    descriptor.type = RecordTraits<Relation>::type_id;
    descriptor.name = "TestRelation";
    descriptor.query_kind = "test_relation";
    descriptor.cpp_type_token = RecordTraits<Relation>::token();
    descriptor.display_name = [](const void* object) -> std::string_view {
        return static_cast<const Relation*>(object)->name;
    };
    const std::vector<RecordTypeId> nodes{RecordTraits<records::Node>::type_id};
    descriptor.fields = {{RecordFieldId{2}, "from", RecordFieldKind::reference, false, "", nodes},
                         {RecordFieldId{3}, "to", RecordFieldKind::reference, false, "", nodes},
                         {RecordFieldId{4}, "name", RecordFieldKind::text, false, "", {}}};
    descriptor.encode = [](const void* object) {
        const auto& value = *static_cast<const Relation*>(object);
        return RecordInput{
            {RecordTraits<Relation>::type_id, value.id.value},
            1,
            {{RecordFieldId{2},
              RecordFieldKind::reference,
              "",
              record_wire::text(value.from.value)},
             {RecordFieldId{3}, RecordFieldKind::reference, "", record_wire::text(value.to.value)},
             {RecordFieldId{4}, RecordFieldKind::text, "", record_wire::text(value.name)}}};
    };
    descriptor.decode = [](const RecordInput& input) -> std::shared_ptr<const void> {
        return std::make_shared<const Relation>(Relation{
            EntityId(input.key.identity),
            EntityId(record_wire::read_text(record_wire::require(input, RecordFieldId{2}).payload)),
            EntityId(record_wire::read_text(record_wire::require(input, RecordFieldId{3}).payload)),
            record_wire::read_text(record_wire::require(input, RecordFieldId{4}).payload)});
    };
    descriptor.owned_bytes = [](const void* object) {
        const auto& value = *static_cast<const Relation*>(object);
        return sizeof(value) + value.id.value.size() + value.from.value.size() +
               value.to.value.size() + value.name.size();
    };
    descriptor.references = [](const void* object, const RecordReferenceVisitor& visitor) {
        const auto& value = *static_cast<const Relation*>(object);
        const std::array targets{RecordTraits<records::Node>::type_id};
        visitor(RecordFieldId{2}, value.from.value, targets);
        visitor(RecordFieldId{3}, value.to.value, targets);
    };
    descriptor.validate = [](const void* object, const DocumentView&) {
        const auto& value = *static_cast<const Relation*>(object);
        if (value.from == value.to)
            throw RecordError(ErrorCode::invalid_input, "Self relation is invalid", "from_id");
    };
    return descriptor;
}
inline qcae::ipc::EngineContribution contribution() {
    using namespace qcae;
    using namespace qcae::operations;
    return {
        "test.relation",
        [](RecordRegistry& registry) {
            registry.add(relation_descriptor());
            registry.add_rule([](const DocumentView& view) {
                view.visit(RecordTraits<Relation>::type_id, [&](const Record& record) {
                    const auto& relation = record->get<Relation>();
                    const auto from = view.find<records::Node>(relation.from);
                    const auto to = view.find<records::Node>(relation.to);
                    if (!from || !to ||
                        from->get<records::Node>().position[0] >=
                            to->get<records::Node>().position[0])
                        throw RecordError(ErrorCode::invalid_input,
                                          "Test relation requires increasing X",
                                          "from_id");
                });
            });
        },
        [](OperationRegistry& registry, RecordApplication& app, std::function<TaskService&()>) {
            return registry.register_typed<CreateRelation>(
                InputTraits<CreateRelation>::definition(),
                [&app](const OperationContext& context,
                       const CreateRelation& input) -> Result<Value> {
                    const std::array signature_parts{input.from.value, input.to.value, input.name};
                    const auto signature = record_wire::strings(signature_parts);
                    const auto receipt = app.execute(
                        context.caller,
                        {*context.document, *context.expected_revision},
                        "test.relation.create",
                        signature,
                        [input, signature](const DocumentView& view,
                                           const RecordIdentityAllocator& allocate)
                            -> Result<RecordPreparedOperation> {
                            const auto id = allocate();
                            EditSession edit(view);
                            edit.put(Relation{id, input.from, input.to, input.name});
                            return {
                                Status::success,
                                RecordPreparedOperation{
                                    edit.prepare(), "Create test relation", id, signature, 0, true},
                                {}};
                        },
                        context.idempotency_key);
                    if (!receipt.ok())
                        return {receipt.status, {}, receipt.error};
                    return {Status::success, change_receipt_value(*receipt.value), {}};
                });
        }};
}
} // namespace contribution_test
