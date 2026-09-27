#pragma once
#include "qcae/record_application.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>

namespace qcae::ipc {
struct EventStreamLimits {
    std::size_t max_events{256}, max_subscribers{64};
};
struct EventStreamStats {
    std::uint64_t emitted_events{}, metadata_bytes_encoded{}, metadata_bytes_copied{};
    std::size_t retained_events{}, subscribers{};
};
// Event-loop-owned, disposable notifications. Queries and committed records stay authoritative.
class EventStream {
  public:
    EventStream(RecordApplication&, QString engine_instance_id, EventStreamLimits = {});
    ~EventStream();
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;
    // Invoke after requests and periodically for background task facts. Failure creates a gap;
    // it never throws after a business commit or changes an accepted operation's outcome.
    void refresh() noexcept;
    bool supports(const QString& operation) const noexcept;
    QJsonObject dispatch(const QJsonObject& request, const QString& connection_id);
    QJsonArray drain(const QString& connection_id, std::size_t limit = 64);
    void unsubscribe(const QString& connection_id);
    EventStreamStats stats() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace qcae::ipc
