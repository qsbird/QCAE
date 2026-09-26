#pragma once
#include "qcae/ipc_api.hpp"
namespace qcae::ipc {
QJsonObject profile_json(const ProfileDefinition&);
std::optional<QJsonObject> dispatch_model(MemoryApplication&,
                                          const QJsonObject&,
                                          const Caller&,
                                          const IModelCodec*,
                                          const ProfileDefinition*);
} // namespace qcae::ipc
