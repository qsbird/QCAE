#include "qcae/task_application.hpp"
#include <atomic>

namespace qcae {
namespace {
constexpr const char* owner = "qcae.runtime.task";
template <class T> Result<T> good(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> bad(ErrorCode code, const std::string& message) {
    return {code == ErrorCode::revision_conflict ? Status::conflict : Status::failed,
            {},
            Diagnostic{code, message, {}}};
}
std::shared_ptr<const OwnedRowImage> row(const TaskRecord& task) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::task_record, task.id},
                      owner,
                      1,
                      std::make_shared<const std::string>(encode_task_record(task)),
                      {}});
}
TaskRecord read(const OwnedRowImage& image) {
    if (image.key.space != StoreSpace::task_record || image.owner != owner ||
        image.schema_version != 1 || !image.payload)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported task row owner/schema");
    try {
        auto task = decode_task_record(*image.payload);
        if (task.id != image.key.identity)
            throw RecordError(ErrorCode::schema_unsupported, "Task row identity mismatch");
        return task;
    } catch (const std::exception& error) {
        throw RecordError(ErrorCode::schema_unsupported, error.what());
    }
}
bool supported_profile(const ProfileRef& profile,
                       const std::function<bool(const ProfileRef&)>& supported) {
    const bool any = !profile.profile_id.empty() || !profile.profile_version.empty() ||
                     !profile.definition_digest.empty();
    if (!any)
        return true;
    return !profile.profile_id.empty() && !profile.profile_version.empty() &&
           !profile.definition_digest.empty() && supported(profile);
}
} // namespace
OwnedRowHandler task_row_handler() {
    auto sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
    OwnedRowHandler result;
    result.space = StoreSpace::task_record;
    result.owner = owner;
    result.validate = [sequence](const OwnedRowImage& image) {
        const auto task = read(image);
        const auto current = task.events.back().sequence;
        auto previous = sequence->load();
        while (previous < current && !sequence->compare_exchange_weak(previous, current)) {
        }
    };
    result.recover =
        [sequence](const OwnedRowImage& image) -> std::shared_ptr<const OwnedRowImage> {
        auto task = read(image);
        if (task_terminal(task.state) && task.state != TaskState::outcome_unknown)
            return {};
        if (sequence->load() == UINT64_MAX || task.events.size() >= 128)
            throw RecordError(ErrorCode::resource_limit, "Task recovery event quota exceeded");
        task.state = TaskState::interrupted;
        task.events.push_back({sequence->fetch_add(1) + 1, task.state, task.progress});
        task.diagnostic =
            Diagnostic{ErrorCode::storage_failure,
                       "Task interrupted by process/session restart; it was not rerun",
                       task.id};
        return row(task);
    };
    result.blocks_close = [](const OwnedRowImage& image) {
        const auto task = read(image);
        return !task_terminal(task.state) || task.state == TaskState::outcome_unknown;
    };
    return result;
}
TaskPublisher record_task_publisher(RecordApplication& app,
                                    std::function<bool(const ProfileRef&)> profile_supported) {
    if (!profile_supported)
        throw std::invalid_argument(
            "Task publisher requires an explicit profile compatibility predicate");
    TaskPublisher publisher;
    publisher.validate_session = [&app](const DocumentRef& document) {
        const auto snapshot = app.snapshot(document);
        if (!snapshot.ok())
            return Result<bool>{snapshot.status, {}, snapshot.error};
        return good(true);
    };
    publisher.validate_input = [&app, profile_supported](const TaskInputContext& context) {
        const auto snapshot = app.snapshot(context.document);
        if (!snapshot.ok())
            return Result<bool>{snapshot.status, {}, snapshot.error};
        if (snapshot.value->info.revision != context.revision)
            return bad<bool>(ErrorCode::revision_conflict, "Task input revision is stale");
        if (!supported_profile(context.profile, profile_supported))
            return bad<bool>(ErrorCode::schema_unsupported,
                             "Task profile semantics are unavailable or incompatible");
        return good(true);
    };
    publisher.load = [&app]() -> Result<std::vector<TaskRecord>> {
        const auto info = app.current_document();
        if (!info.ok())
            return good(std::vector<TaskRecord>{});
        if (app.recovery_available())
            return bad<std::vector<TaskRecord>>(ErrorCode::storage_uncertain,
                                                "Application must explicitly recover first");
        const auto rows = app.owned_rows(info.value->document, StoreSpace::task_record, owner);
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        try {
            std::vector<TaskRecord> tasks;
            for (const auto& image : *rows.value)
                tasks.push_back(read(*image));
            return good(std::move(tasks));
        } catch (const RecordError& error) {
            return bad<std::vector<TaskRecord>>(error.code(), error.what());
        }
    };
    publisher.persist = [&app](const TaskRecord& task, const std::optional<TaskRecord>& expected) {
        try {
            const auto next = row(task);
            const OwnedRowUpdate update{
                next->key, expected ? row(*expected)->payload : nullptr, next};
            return app.update_owned_rows(task.caller, task.input.document, std::span(&update, 1));
        } catch (const RecordError& error) {
            return bad<bool>(error.code(), error.what());
        } catch (const std::exception& error) {
            return bad<bool>(ErrorCode::invalid_input, error.what());
        }
    };
    publisher.publish = [&app, profile_supported](const std::shared_ptr<const TaskPayload>& payload,
                                                  const TaskRecord& expected,
                                                  TaskRecord complete) -> Result<TaskRecord> {
        const auto candidate = std::dynamic_pointer_cast<const RecordTaskPayload>(payload);
        if (!candidate || candidate->operation.change.changes.empty())
            return bad<TaskRecord>(ErrorCode::invalid_input,
                                   "Task did not produce a complete record candidate");
        if (!supported_profile(expected.input.profile, profile_supported))
            return bad<TaskRecord>(ErrorCode::schema_unsupported,
                                   "Task profile changed before publication");
        const auto previous = row(expected);
        const auto result = app.execute(
            expected.caller,
            {expected.input.document, expected.input.revision},
            expected.operation,
            expected.signature,
            [candidate](const DocumentView&, const RecordIdentityAllocator&) {
                return good(candidate->operation);
            },
            "task-publication:" + expected.id,
            [&](const ChangeReceipt& receipt) {
                complete.receipt = receipt;
                auto next = row(complete);
                return std::vector<OwnedRowUpdate>{{next->key, previous->payload, std::move(next)}};
            });
        if (!result.ok())
            return {result.status, {}, result.error};
        if (result.value->replayed) {
            const auto rows =
                app.owned_rows(expected.input.document, StoreSpace::task_record, owner);
            if (!rows.ok())
                return {rows.status, {}, rows.error};
            for (const auto& image : *rows.value)
                if (image->key.identity == expected.id)
                    return good(read(*image));
            return bad<TaskRecord>(ErrorCode::schema_unsupported, "Committed task fact is missing");
        }
        return good(std::move(complete));
    };
    return publisher;
}
} // namespace qcae
