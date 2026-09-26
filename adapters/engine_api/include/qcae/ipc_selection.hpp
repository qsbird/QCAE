#pragma once
#include "qcae/ipc_api.hpp"
#include "qcae/query.hpp"
namespace qcae::ipc {
std::optional<QJsonObject>
dispatch_selection(MemoryApplication&, SelectionService&, const QJsonObject&, const Caller&);
}
