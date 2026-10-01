#pragma once
#include "qcae/ipc_api.hpp"
#include <QJsonArray>
#include <cstdint>
#include <span>
namespace qcae {
struct EntitySummary;
}
namespace qcae::ipc {
struct EntityRowsJson {
    QJsonArray rows;
    bool complete{};
    std::uint64_t encoded_byte_upper_bound{};
};
QJsonObject record_entity_json(const EntitySummary&, const DocumentView&);
Result<EntityRowsJson> entity_rows_json(const RecordSnapshot&,
                                        std::span<const std::string> identities,
                                        std::size_t row_limit = 1000,
                                        std::size_t encoded_byte_limit = 65536);
QJsonObject profile_json(const ProfileDefinition&);
std::optional<QJsonObject> dispatch_model(MemoryApplication&,
                                          const QJsonObject&,
                                          const Caller&,
                                          const IModelCodec*,
                                          const ProfileDefinition*);
} // namespace qcae::ipc
