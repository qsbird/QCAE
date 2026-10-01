#pragma once

#include "qcae/core.hpp"
#include "qcae/model_codec.hpp"
#include "qcae/profile_provider.hpp"
#include <QJsonObject>
#include <string_view>

namespace qcae {
class SelectionService;
}
namespace qcae::ipc {
class TypedHost;
// Planned catalog entries may be implemented by a typed static contribution.
// Implemented compatibility operations retain exclusive ownership of their names.
bool compatibility_operation_supported(std::string_view);
// The same authoritative summary is used by project queries and opted-in events.
QJsonObject info_json(const DocumentInfo&);
QJsonObject failure(const QString& request_id,
                    const QString& code,
                    const QString& message,
                    const QString& status = QStringLiteral("failed"));
QJsonObject dispatch(MemoryApplication&,
                     const QJsonObject&,
                     const Caller& trusted_caller,
                     const IModelCodec* codec = nullptr,
                     const ProfileDefinition* profile = nullptr,
                     SelectionService* selections = nullptr,
                     TypedHost* typed = nullptr,
                     bool display_services = false);
} // namespace qcae::ipc
