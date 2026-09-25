#pragma once

#include "qcae/core.hpp"
#include <QJsonObject>

namespace qcae::ipc {
QJsonObject failure(const QString& request_id, const QString& code, const QString& message,
                    const QString& status = QStringLiteral("failed"));
QJsonObject dispatch(MemoryApplication&, const QJsonObject&, const Caller& trusted_caller);
} // namespace qcae::ipc
