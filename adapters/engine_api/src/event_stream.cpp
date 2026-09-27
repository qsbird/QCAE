#include "qcae/event_stream.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/task_service.hpp"
#include <QJsonDocument>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>

namespace qcae::ipc {
namespace {
QString qs(const std::string& text) {
    return QString::fromStdString(text);
}
QString number(std::uint64_t value) {
    return QString::number(static_cast<qulonglong>(value));
}
bool sameDocument(const DocumentRef& a, const DocumentRef& b) {
    return a.id == b.id && a.epoch == b.epoch;
}
bool sameMetadata(const DocumentInfo& a, const DocumentInfo& b) {
    return a.name == b.name && a.project_id == b.project_id && a.saved_path == b.saved_path &&
           a.saved_content_state == b.saved_content_state && a.dirty == b.dirty &&
           a.durable == b.durable;
}
std::uint64_t unsignedField(const QJsonObject& object, const char* name, std::uint64_t fallback) {
    if (!object.contains(QLatin1String(name)))
        return fallback;
    const auto value = object.value(QLatin1String(name));
    if (!value.isString())
        throw std::invalid_argument(std::string(name) + " must be an unsigned integer string");
    const auto text = value.toString().toStdString();
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument(std::string(name) + " must be an unsigned integer string");
    return result;
}
} // namespace

struct EventStream::State {
    struct Subscriber {
        std::uint64_t sequence{}, generation{};
    };
    struct TaskSeen {
        std::shared_ptr<const OwnedRowImage> row;
        std::uint64_t sequence{};
    };
    State(RecordApplication& application, QString instance, EventStreamLimits bounds)
        : app(application), engine_instance(std::move(instance)), limits(bounds) {}
    RecordApplication& app;
    QString engine_instance;
    EventStreamLimits limits;
    std::optional<DocumentInfo> document;
    std::map<std::string, TaskSeen> tasks;
    std::deque<QJsonObject> events;
    std::map<QString, Subscriber> subscribers;
    std::uint64_t sequence{}, generation{};
    bool invalidated{};
    EventStreamStats ledger;

    std::uint64_t oldest() const noexcept {
        return events.empty() ? sequence : sequence - events.size() + 1;
    }
    bool cursorGap(const QString& instance, std::uint64_t cursor) const noexcept {
        return instance != engine_instance || cursor > sequence ||
               (cursor < sequence && (events.empty() || cursor < oldest() - 1));
    }
    QJsonObject gap() const {
        return {{"frame_type", "event_gap"},
                {"engine_instance_id", engine_instance},
                {"sequence", number(sequence)},
                {"resync_required", true},
                {"reason", "Event cursor expired; query the current document and tasks"}};
    }
    void invalidate() noexcept {
        if (invalidated)
            return;
        invalidated = true;
        ++generation;
        if (sequence != std::numeric_limits<std::uint64_t>::max())
            ++sequence;
        events.clear();
        document.reset();
        tasks.clear();
    }
    void append(const char* name, const DocumentInfo& info, QJsonObject data) {
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Event sequence exhausted");
        QJsonObject event{{"frame_type", "event"},
                          {"engine_instance_id", engine_instance},
                          {"sequence", number(++sequence)},
                          {"event", name},
                          {"document_id", qs(info.document.id.value)},
                          {"document_epoch", qs(info.document.epoch.value)},
                          {"revision", number(info.revision)},
                          {"data", std::move(data)}};
        ledger.metadata_bytes_encoded += QJsonDocument(event).toJson(QJsonDocument::Compact).size();
        ++ledger.emitted_events;
        events.push_back(std::move(event));
        while (events.size() > limits.max_events)
            events.pop_front();
    }
    QJsonObject read(const QString& instance, std::uint64_t cursor, std::size_t limit) {
        const bool resync = invalidated || cursorGap(instance, cursor);
        QJsonArray page;
        auto next = resync ? sequence : cursor;
        if (!resync)
            for (const auto& event : events) {
                const auto event_sequence = unsignedField(event, "sequence", 0);
                if (event_sequence <= cursor)
                    continue;
                if (page.size() == static_cast<qsizetype>(limit))
                    break;
                page.append(event);
                next = event_sequence;
            }
        ledger.metadata_bytes_copied += page.size() * sizeof(QJsonObject);
        return {{"engine_instance_id", engine_instance},
                {"current_sequence", number(sequence)},
                {"oldest_sequence", number(oldest())},
                {"next_sequence", number(next)},
                {"resync_required", resync},
                {"events", page}};
    }
};

EventStream::EventStream(RecordApplication& app, QString instance, EventStreamLimits limits)
    : state_(std::make_unique<State>(app, std::move(instance), limits)) {
    if (state_->engine_instance.isEmpty() || !limits.max_events || !limits.max_subscribers)
        throw std::invalid_argument("Event stream requires instance identity and positive limits");
}
EventStream::~EventStream() = default;

void EventStream::refresh() noexcept {
    try {
        const auto current = state_->app.current_document();
        std::optional<DocumentInfo> info;
        if (current.ok())
            info = *current.value;
        else if (current.error->code != ErrorCode::document_not_found) {
            state_->invalidate();
            return;
        }
        const bool context_changed =
            info.has_value() != state_->document.has_value() ||
            (info && !sameDocument(info->document, state_->document->document));
        const bool revision_changed = info && state_->document && !context_changed &&
                                      info->revision != state_->document->revision;
        const bool metadata_changed =
            info && state_->document && !context_changed && !sameMetadata(*info, *state_->document);
        std::vector<std::shared_ptr<const OwnedRowImage>> rows;
        if (info) {
            // Storage uncertainty can leave the old RAM revision unchanged. Do not
            // advance an event cursor while authoritative recovery is required.
            const auto health = state_->app.changes_since(info->document, info->revision);
            if (!health.ok()) {
                state_->invalidate();
                return;
            }
            auto loaded = state_->app.owned_rows(
                info->document, StoreSpace::task_record, "qcae.runtime.task");
            if (!loaded.ok()) {
                state_->invalidate();
                return;
            }
            rows = std::move(*loaded.value);
        }
        bool tasks_changed = rows.size() != state_->tasks.size();
        for (const auto& row : rows) {
            const auto seen = state_->tasks.find(row->key.identity);
            tasks_changed = tasks_changed || seen == state_->tasks.end() || seen->second.row != row;
        }
        if (!context_changed && !revision_changed && !metadata_changed && !tasks_changed) {
            state_->invalidated = false;
            return;
        }
        // Prepare a disposable event candidate. No allocation follows its publication.
        State candidate = *state_;
        candidate.invalidated = false;
        candidate.ledger.metadata_bytes_copied +=
            candidate.events.size() * sizeof(QJsonObject) +
            candidate.tasks.size() * sizeof(State::TaskSeen) +
            candidate.subscribers.size() * sizeof(State::Subscriber);
        if (context_changed) {
            const auto& event_info = info ? *info : *candidate.document;
            candidate.append("DocumentChanged",
                             event_info,
                             {{"active", info.has_value()}, {"resync_required", true}});
            candidate.append("HistoryChanged", event_info, {{"resync_required", true}});
            candidate.tasks.clear();
        } else if (revision_changed) {
            const auto changes =
                candidate.app.changes_since(info->document, candidate.document->revision);
            if (!changes.ok()) {
                state_->invalidate();
                return;
            }
            if (changes.value->resync_required) {
                candidate.append("DocumentChanged", *info, {{"resync_required", true}});
                candidate.append("HistoryChanged", *info, {{"resync_required", true}});
            } else {
                for (const auto& change : changes.value->changes) {
                    DocumentInfo version = *info;
                    version.revision = change.revision;
                    const QJsonObject data{{"base_revision", number(change.base_revision)},
                                           {"transaction_id", qs(change.transaction.value)},
                                           {"resync_required", false}};
                    candidate.append("DocumentChanged", version, data);
                    candidate.append("HistoryChanged", version, data);
                }
                info->revision = changes.value->current_revision;
            }
        }
        if (metadata_changed && !revision_changed)
            candidate.append("ProjectMetadataChanged", *info, {{"resync_required", true}});
        candidate.document = info;
        std::map<std::string, State::TaskSeen> next_tasks;
        for (const auto& row : rows) {
            const auto previous = candidate.tasks.find(row->key.identity);
            if (previous != candidate.tasks.end() && previous->second.row == row) {
                next_tasks.emplace(row->key.identity, previous->second);
                continue;
            }
            if (!row->payload || row->schema_version != 1)
                throw std::invalid_argument("Invalid authoritative task row");
            const auto task = decode_task_record(*row->payload);
            const auto task_sequence = task.events.empty() ? 0 : task.events.back().sequence;
            if (previous == candidate.tasks.end() || previous->second.sequence != task_sequence) {
                QJsonObject data{{"task_id", qs(task.id)},
                                 {"state", task_state_name(task.state)},
                                 {"progress", task.progress},
                                 {"input_revision", number(task.input.revision)},
                                 {"task_sequence", number(task_sequence)}};
                if (task.receipt)
                    data.insert("committed_revision", number(task.receipt->committed_revision));
                candidate.append("JobChanged", *info, std::move(data));
            }
            next_tasks.emplace(row->key.identity, State::TaskSeen{row, task_sequence});
        }
        candidate.tasks = std::move(next_tasks);
        state_->document.swap(candidate.document);
        state_->tasks.swap(candidate.tasks);
        state_->events.swap(candidate.events);
        state_->sequence = candidate.sequence;
        state_->ledger = candidate.ledger;
        state_->invalidated = candidate.invalidated;
    } catch (...) {
        state_->invalidate();
    }
}

bool EventStream::supports(const QString& operation) const noexcept {
    return operation == "events.subscribe" || operation == "events.read";
}
QJsonObject EventStream::dispatch(const QJsonObject& request, const QString& connection) {
    const auto id = request.value("request_id").toString();
    if (id.toUtf8().size() > 128)
        return failure({}, "INVALID_INPUT", "request_id exceeds 128 UTF-8 bytes");
    try {
        for (auto it = request.begin(); it != request.end(); ++it)
            if (it.key() != "api_version" && it.key() != "request_id" && it.key() != "operation" &&
                it.key() != "parameters" && it.key() != "requested_version")
                throw std::invalid_argument("Unexpected event request field");
        if (!request.value("request_id").isString() || id.isEmpty() ||
            !request.value("operation").isString() ||
            request.value("operation").toString().isEmpty())
            throw std::invalid_argument("request_id and operation require non-empty strings");
        if (!request.value("api_version").isString())
            throw std::invalid_argument("api_version requires a string");
        if (request.value("api_version") != "1.1")
            return failure(id, "API_VERSION_UNSUPPORTED", "Expected API version 1.1");
        if (request.contains("requested_version")) {
            const auto version = request.value("requested_version");
            const auto numeric = version.toDouble();
            if (!version.isDouble() || !std::isfinite(numeric) || numeric < 1 ||
                numeric > std::numeric_limits<std::uint32_t>::max() ||
                std::floor(numeric) != numeric)
                throw std::invalid_argument("requested_version must be a positive uint32 integer");
            if (numeric != 1)
                return failure(id, "SCHEMA_UNSUPPORTED", "Event contract version is not installed");
        }
        const auto operation = request.value("operation").toString();
        if (!supports(operation))
            return failure(id, "UNSUPPORTED_CAPABILITY", "Unknown event operation");
        if (!request.value("parameters").isObject())
            throw std::invalid_argument("parameters must be an object");
        const auto parameters = request.value("parameters").toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            if (it.key() != "engine_instance_id" && it.key() != "after_sequence" &&
                it.key() != "limit")
                throw std::invalid_argument("Unexpected event parameter");
        if (parameters.contains("engine_instance_id") &&
            !parameters.value("engine_instance_id").isString())
            throw std::invalid_argument("engine_instance_id must be a string");
        const auto instance = parameters.value("engine_instance_id").toString();
        const auto cursor = unsignedField(parameters, "after_sequence", 0);
        std::uint64_t limit = 64;
        if (parameters.value("limit").isDouble()) {
            const auto numeric = parameters.value("limit").toDouble();
            if (!std::isfinite(numeric) || numeric < 1 || numeric > 64 ||
                std::floor(numeric) != numeric)
                throw std::invalid_argument("Event read limit must be an integer in 1..64");
            limit = static_cast<std::uint64_t>(numeric);
        } else
            limit = unsignedField(parameters, "limit", 64);
        if (!limit || limit > 64)
            throw std::invalid_argument("Event read limit must be 1..64");
        auto data = state_->read(instance, cursor, limit);
        if (operation == "events.subscribe") {
            if (connection.isEmpty())
                throw std::invalid_argument("Subscription requires a trusted connection");
            if (!state_->subscribers.contains(connection) &&
                state_->subscribers.size() >= state_->limits.max_subscribers)
                return failure(id, "RESOURCE_LIMIT", "Event subscriber limit reached");
            const bool resync = data.value("resync_required").toBool();
            state_->subscribers[connection] = {resync ? state_->sequence : cursor,
                                               state_->generation};
            // Subscription baseline contains cursor metadata; queued events arrive via drain.
            data.remove("events");
            data.insert("next_sequence", number(resync ? state_->sequence : cursor));
        }
        return {{"request_id", id}, {"status", "success"}, {"data", data}};
    } catch (const std::exception& error) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(error.what()));
    }
}
QJsonArray EventStream::drain(const QString& connection, std::size_t limit) {
    QJsonArray result;
    auto found = state_->subscribers.find(connection);
    if (found == state_->subscribers.end())
        return result;
    auto& subscriber = found->second;
    if (subscriber.generation != state_->generation ||
        state_->cursorGap(state_->engine_instance, subscriber.sequence)) {
        result.append(state_->gap());
        subscriber = {state_->sequence, state_->generation};
        return result;
    }
    const auto data = state_->read(
        state_->engine_instance, subscriber.sequence, std::clamp<std::size_t>(limit, 1, 64));
    result = data.value("events").toArray();
    subscriber.sequence = unsignedField(data, "next_sequence", subscriber.sequence);
    return result;
}
void EventStream::unsubscribe(const QString& connection) {
    state_->subscribers.erase(connection);
}
EventStreamStats EventStream::stats() const {
    auto result = state_->ledger;
    result.retained_events = state_->events.size();
    result.subscribers = state_->subscribers.size();
    return result;
}
} // namespace qcae::ipc
