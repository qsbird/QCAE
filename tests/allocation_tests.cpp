#include "qcae/core.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace { long fail_after = -1; }
void* operator new(std::size_t size) {
    if (fail_after == 0) { fail_after = -1; throw std::bad_alloc(); }
    if (fail_after > 0) --fail_after;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

int main() {
    using namespace qcae;
    const Caller caller{"allocation-test"};
    for (int operation = 0; operation < 3; ++operation) {
        bool reached_success = false;
        const char* operation_name = operation == 0 ? "commit" : operation == 1 ? "undo" : "redo";
        for (long allocation = 0; allocation < 512; ++allocation) {
            MemoryApplication app;
            const auto document = *app.create_document(caller, "Beam", "create").value;
            WriteContext context{document.document, 0};
            const auto preview = *app.preview(caller, context, CreateMaterial{"Steel", {210, "GPa"}}).value;
            if (operation > 0) { app.commit(caller, context, preview.id, "initial"); context.expected_revision = 1; }
            if (operation == 2) { app.undo(caller, context, "initial-undo"); context.expected_revision = 2; }
            const auto before = *app.snapshot(document.document).value;
            const auto history_before = *app.history(document.document).value;
            bool threw = false;
            Result<ChangeReceipt> outcome;
            fail_after = allocation;
            try {
                if (operation == 0) outcome = app.commit(caller, context, preview.id, "tested");
                if (operation == 1) outcome = app.undo(caller, context, "tested");
                if (operation == 2) outcome = app.redo(caller, context, "tested");
            } catch (const std::bad_alloc&) { threw = true; }
            fail_after = -1;
            const auto after = *app.snapshot(document.document).value;
            const auto history_after = *app.history(document.document).value;
            if (!threw) {
                if (!outcome.ok() || after.info.revision != before.info.revision + 1) return 1;
                reached_success = true;
                std::printf("%s: %ld allocation-failure positions preserved state\n", operation_name, allocation);
                break;
            }
            if (after.info.revision != before.info.revision || after.info.content_state != before.info.content_state ||
                after.materials != before.materials || history_after.cursor != history_before.cursor ||
                history_after.items.size() != history_before.items.size() ||
                app.operation(caller, document.document, operation_name, "tested").ok()) {
                std::fprintf(stderr, "%s partially published at allocation %ld\n", operation_name, allocation);
                return 1;
            }
        }
        if (!reached_success) return 1;
    }
    return 0;
}
