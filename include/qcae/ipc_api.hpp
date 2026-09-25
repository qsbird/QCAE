#pragma once

#include "qcae/core.hpp"
#include "qcae/model_codec.hpp"
#include "qcae/profile_provider.hpp"
#include <QJsonObject>

namespace qcae::ipc {
QJsonObject failure(const QString& request_id,
                    const QString& code,
                    const QString& message,
                    const QString& status = QStringLiteral("failed"));
QJsonObject dispatch(MemoryApplication&,
                     const QJsonObject&,
                     const Caller& trusted_caller,
                     const IModelCodec* codec = nullptr,
                     const ProfileDefinition* profile = nullptr);
} // namespace qcae::ipc
