#include "qcae/core.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
qcae::Model imported_model() {
    using namespace qcae;
    auto id = [](const char* value) { return EntityId(value); };
    Model model;
    model.materials.push_back({id("mat"), "Steel", 210000, .3});
    model.nodes.push_back({id("n1"), {0, 0, 0}});
    model.nodes.push_back({id("n2"), {1000, 0, 0}});
    model.sections.push_back({id("sec"), "Beam section", id("mat"), 100, 200, 300, 400});
    model.beams.push_back({id("beam"), id("sec"), {id("n1"), id("n2")}, {0, 1, 0}});
    model.parts.push_back({id("part"), "Wing", {id("n1"), id("n2"), id("beam")}});
    model.includes.push_back({id("include"), "root.bdf", std::nullopt,
                              {id("mat"), id("n1"), id("n2"), id("sec"), id("beam"), id("part")}});
    model.sources.push_back({id("n1"), "source", id("include"),
                             {"nastran", "1", "digest"}, "GRID", 1});
    return model;
}
}

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
    for (int operation = 0; operation < 3; ++operation) {
        bool reached_success = false;
        const char* operation_name = operation == 0 ? "commit" : operation == 1 ? "undo" : "redo";
        for (long allocation = 0; allocation < 2048; ++allocation) {
            MemoryApplication app;
            const auto document = *app.create_document(caller, "Imported", "create").value;
            WriteContext context{document.document, 0};
            const auto preview = *app.preview_import(caller, context, imported_model()).value;
            if (operation > 0) {
                app.commit(caller, context, preview.id, "initial");
                context.expected_revision = 1;
            }
            if (operation == 2) {
                app.undo(caller, context, "initial-undo");
                context.expected_revision = 2;
            }
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
                std::printf("imported %s: %ld allocation-failure positions preserved state\n",
                            operation_name, allocation);
                break;
            }
            if (after.info.revision != before.info.revision ||
                after.info.content_state != before.info.content_state ||
                static_cast<const Model&>(after) != static_cast<const Model&>(before) ||
                history_after.cursor != history_before.cursor ||
                history_after.items.size() != history_before.items.size() ||
                app.operation(caller, document.document, operation_name, "tested").ok()) {
                std::fprintf(stderr, "imported %s partially published at allocation %ld\n",
                             operation_name, allocation);
                return 1;
            }
        }
        if (!reached_success) return 1;
    }
    return 0;
}
