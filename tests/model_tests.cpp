#include "qcae/core.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace qcae;
namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template <class T> const T& good(const Result<T>& result, const std::string& message) {
    check(result.ok(), message + (result.error ? ": " + result.error->message : ""));
    return *result.value;
}
template <class T> void bad(const Result<T>& result, ErrorCode code, const std::string& message) {
    check(!result.ok() && result.error && result.error->code == code, message);
}
EntityId id(const char* value) { return EntityId(value); }
WriteContext at(const DocumentInfo& info) { return {info.document, info.revision}; }

Model beam_model() {
    Model model;
    model.materials.push_back({id("mat"), "Steel", 210000, .3});
    model.nodes.push_back({id("n1"), {0, 0, 0}});
    model.nodes.push_back({id("n2"), {1000, 0, 0}});
    model.sections.push_back({id("sec"), "Rect", id("mat"), 100, 200, 300, 400});
    model.beams.push_back({id("b1"), id("sec"), {id("n1"), id("n2")}, {0, 1, 0}});
    model.parts.push_back({id("p1"), "Wing", {id("n1"), id("n2"), id("b1")}});
    model.assemblies.push_back({id("a1"), "Plane", {id("p1")}});
    model.sets.push_back({id("set"), "Tips", {id("n2")}});
    model.includes.push_back({id("root"), "root.bdf", std::nullopt,
        {id("mat"), id("n1"), id("sec"), id("b1"), id("p1"), id("a1"), id("set"),
         id("force"), id("fix"), id("analysis")}});
    model.includes.push_back({id("child"), "child.bdf", id("root"), {id("n2")}});
    model.forces.push_back({id("force"), id("n2"), {0, -100, 0}});
    model.constraints.push_back({id("fix"), {id("n1")}, "123456"});
    model.analyses.push_back({id("analysis"), "Static", {{"nastran", "1", "digest"}, "static"},
                              {id("force")}, {id("fix")}});
    model.sources.push_back({id("n1"), "source-1", id("root"), {"nastran", "1", "digest"}, "GRID", 1});
    model.sources.push_back({id("n2"), "source-1", id("child"), {"nastran", "1", "digest"}, "GRID", 2});
    model.sources.push_back({id("mat"), "source-1", id("root"), {"nastran", "1", "digest"}, "MAT1", 1});
    return model;
}

void validation_and_queries() {
    Model model = beam_model();
    check(validate_model(model).empty(), "valid imported beam model");
    check(model_entities(model).size() == 13, "all entity kinds enumerated");
    const auto refs = model_references(model);
    check(std::any_of(refs.begin(), refs.end(), [](const Reference& ref) {
        return ref.from == id("sec") && ref.to == id("mat") && ref.role == "section.material";
    }), "section material reference");
    check(affected_analyses(model, id("mat")) == std::vector<EntityId>{id("analysis")},
          "material to section to beam invalidates analysis");
    check(affected_analyses(model, id("n2")) == std::vector<EntityId>{id("analysis")},
          "node to beam and load invalidates analysis");
    check(affected_analyses(model, id("p1")).empty(), "organization is not physical input");
    check(affected_analyses(model, id("set")).empty(), "set is not physical input");

    auto expect_invalid = [&](const Model& candidate, const std::string& label) {
        check(!validate_model(candidate).empty(), label);
    };
    { auto wrong = model; wrong.materials[0].poisson_ratio = .5; expect_invalid(wrong, "nu upper bound"); }
    { auto wrong = model; wrong.sections[0].area_mm2 = 0; expect_invalid(wrong, "section area"); }
    { auto wrong = model; wrong.beams[0].orientation = {1, 0, 0}; expect_invalid(wrong, "parallel orientation"); }
    { auto wrong = model; wrong.nodes[1].position = wrong.nodes[0].position; expect_invalid(wrong, "zero length"); }
    { auto wrong = model; wrong.forces[0].force_n.x = std::numeric_limits<double>::quiet_NaN();
      expect_invalid(wrong, "nonfinite force"); }
    { auto wrong = model; wrong.sections[0].material = id("n1"); expect_invalid(wrong, "typed reference"); }
    { auto wrong = model; wrong.nodes[1].id = id("n1"); expect_invalid(wrong, "global id collision"); }
    { auto wrong = model; wrong.parts.push_back({id("p2"), "Other", {id("b1")}});
      expect_invalid(wrong, "beam has one part"); }
    { auto wrong = model; wrong.parts.push_back({id("p2"), "Other", {id("n1")}});
      check(validate_model(wrong).empty(), "nodes can be shared among parts"); }
    { auto wrong = model; wrong.assemblies[0].children.push_back(id("a1"));
      expect_invalid(wrong, "assembly cycle"); }
    { auto wrong = model; wrong.sets[0].members.push_back(id("b1"));
      expect_invalid(wrong, "heterogeneous set"); }
    { auto wrong = model; wrong.includes[1].parent = id("child");
      expect_invalid(wrong, "include cycle"); }
    { auto wrong = model; wrong.includes[1].path = "root.bdf";
      expect_invalid(wrong, "duplicate include path"); }
    { auto wrong = model; wrong.includes[1].members.push_back(id("n1"));
      expect_invalid(wrong, "duplicate include owner"); }
    { auto wrong = model; wrong.sources[1].include = id("root");
      expect_invalid(wrong, "source include ownership"); }
    { auto wrong = model; wrong.sources[1].number = 1;
      expect_invalid(wrong, "source namespace numbering"); }
    { auto other = model; other.sources[1].name_space = "OTHER"; other.sources[1].number = 1;
      check(validate_model(other).empty(), "source namespaces separate numbering"); }
    { auto other = model; other.sources[1].source_model_id = "other-source"; other.sources[1].number = 1;
      check(validate_model(other).empty(), "source models separate numbering"); }
    { auto wrong = model; wrong.analyses[0].target.profile.definition_digest.clear();
      expect_invalid(wrong, "analysis target required"); }
    { auto wrong = model; wrong.constraints[0].dofs = "7";
      expect_invalid(wrong, "constraint DOF range"); }
    { auto wrong = model; wrong.constraints[0].dofs = "11";
      expect_invalid(wrong, "constraint DOF duplicate"); }
}

void application_flow() {
    MemoryApplication app;
    Caller alice{"alice"}, bob{"bob"};
    const auto created = good(app.create_document(alice, "M1", "create"), "create");
    Model model = beam_model();
    { auto wrong = model; wrong.constraints[0].dofs = "7";
      bad(app.preview_import(alice, at(created), wrong), ErrorCode::invalid_input,
          "invalid DOF rejected by import preview"); }
    { auto wrong = model; wrong.constraints[0].dofs = "11";
      bad(app.preview_import(alice, at(created), wrong), ErrorCode::invalid_input,
          "duplicate DOF rejected by import preview"); }
    const auto import = good(app.preview_import(alice, at(created), model), "preview import");
    bad(app.commit(bob, at(created), import.id, "steal"), ErrorCode::preview_expired, "actor binding");
    const auto first = good(app.commit(alice, at(created), import.id, "import"), "commit import");
    check(first.committed_revision == 1, "import revision");
    auto snap = good(app.snapshot(created.document), "snapshot");
    check(static_cast<const Model&>(snap) == model, "whole model imported atomically");
    bad(app.preview_import(alice, at(snap.info), model), ErrorCode::invalid_input, "empty-only import");
    check(good(app.commit(alice, at(created), import.id, "import"), "import replay").replayed,
          "import retry preserved");

    auto stale = at(snap.info);
    const auto move = good(app.preview_edit(alice, stale, MoveNode{id("n2"), {1200, 0, 0}}),
                           "move preview");
    const auto moved = good(app.commit(alice, stale, move.id, "move"), "move commit");
    check(moved.committed_revision == 2 && good(app.snapshot(created.document), "moved").nodes[1].position.x == 1200,
          "move applied");
    bad(app.commit(alice, stale, move.id, "new-key"), ErrorCode::revision_conflict, "stale commit");
    bad(app.preview_edit(alice, stale, UpsertPart{{id("p1"), "Old", {}}}),
        ErrorCode::revision_conflict, "stale edit");
    snap = good(app.snapshot(created.document), "after move");
    bad(app.preview_edit(alice, at(snap.info), DeleteEntity{id("mat")}),
        ErrorCode::invalid_input, "referenced material deletion");
    bad(app.preview_edit(alice, at(snap.info), UpsertPart{{id("n1"), "Wrong", {}}}),
        ErrorCode::invalid_input, "upsert kind isolation");
    bad(app.preview_edit(alice, at(snap.info), MoveNode{id("n2"), {0, 0, 0}}),
        ErrorCode::invalid_input, "invalid beam geometry rejected at preview");
    const auto part = good(app.preview_edit(alice, at(snap.info),
                                 UpsertPart{{EntityId{}, "New part", {id("n1")}}}), "create part");
    check(part.creates_entity && !part.affected_entity.value.empty(), "allocated part ID");
    good(app.commit(alice, at(snap.info), part.id, "part"), "commit part");
    auto after_part = good(app.snapshot(created.document), "with part");
    check(after_part.parts.size() == 2, "part created");
    const auto deletion = good(app.preview_edit(alice, at(after_part.info),
                                     DeleteEntity{part.affected_entity}), "delete part preview");
    good(app.commit(alice, at(after_part.info), deletion.id, "delete"), "delete part");
    check(good(app.snapshot(created.document), "deleted part").parts.size() == 1, "part deleted");

    auto current = good(app.snapshot(created.document), "before undo");
    good(app.undo(bob, at(current.info), "undo-delete"), "undo delete");
    check(good(app.snapshot(created.document), "undo delete snapshot").parts.size() == 2,
          "undo restores entity");
    current = good(app.snapshot(created.document), "before second undo");
    good(app.undo(bob, at(current.info), "undo-part"), "undo part");
    check(good(app.snapshot(created.document), "undo part snapshot").parts.size() == 1,
          "undo restores organization");
    current = good(app.snapshot(created.document), "before redo");
    good(app.redo(bob, at(current.info), "redo-part"), "redo part");
    check(good(app.snapshot(created.document), "redo part snapshot").parts.size() == 2,
          "redo restores organization");
}

void resource_limits() {
    Caller caller{"limited"};
    Limits limits;
    limits.max_entities = 12;
    MemoryApplication entity_app(limits);
    auto doc = good(entity_app.create_document(caller, "Small", "create"), "entity doc");
    bad(entity_app.preview_import(caller, at(doc), beam_model()), ErrorCode::resource_limit,
        "entity quota rejects import");
    check(good(entity_app.snapshot(doc.document), "after rejected import").info.revision == 0,
          "rejected import atomic");

    limits = {};
    limits.max_relations = 1;
    MemoryApplication relation_app(limits);
    doc = good(relation_app.create_document(caller, "Small", "create"), "relation doc");
    bad(relation_app.preview_import(caller, at(doc), beam_model()), ErrorCode::resource_limit,
        "relation quota rejects import");

    limits = {};
    limits.max_materials = 0;
    MemoryApplication material_app(limits);
    doc = good(material_app.create_document(caller, "Small", "create"), "material doc");
    bad(material_app.preview_import(caller, at(doc), beam_model()), ErrorCode::resource_limit,
        "material quota rejects import");
}

void delete_owned_entity() {
    MemoryApplication app;
    Caller caller{"delete-test"};
    auto doc = good(app.create_document(caller, "Delete", "create"), "create delete model");
    Model model = beam_model();
    model.nodes.push_back({id("free"), {2000, 0, 0}});
    model.includes[0].members.push_back(id("free"));
    model.sources.push_back({id("free"), "source-1", id("root"),
                             {"nastran", "1", "digest"}, "GRID", 3});
    auto preview = good(app.preview_import(caller, at(doc), model), "preview delete model");
    good(app.commit(caller, at(doc), preview.id, "import"), "commit delete model");
    auto before = good(app.snapshot(doc.document), "before delete");
    preview = good(app.preview_edit(caller, at(before.info), DeleteEntity{id("free")}),
                   "delete sourced node");
    good(app.commit(caller, at(before.info), preview.id, "delete"), "commit sourced deletion");
    const auto after = good(app.snapshot(doc.document), "after sourced deletion");
    check(after.nodes.size() == model.nodes.size() - 1 && after.sources.size() == model.sources.size() - 1,
          "deleting entity removes its own provenance");
    check(after.includes[0].members.size() == model.includes[0].members.size() - 1,
          "deleting entity removes include membership");
    check(validate_model(after).empty(), "deletion preserves model validity");
}

void imported_history() {
    MemoryApplication app;
    Caller caller{"history"};
    const auto doc = good(app.create_document(caller, "History", "create"), "history document");
    const Model model = beam_model();
    const auto preview = good(app.preview_import(caller, at(doc), model), "history import preview");
    good(app.commit(caller, at(doc), preview.id, "import"), "history import commit");
    auto current = good(app.snapshot(doc.document), "history imported snapshot");
    good(app.undo(caller, at(current.info), "undo"), "undo imported model");
    current = good(app.snapshot(doc.document), "history undone snapshot");
    check(model_entities(current).empty() && current.sources.empty() && !current.info.dirty,
          "undo clears whole imported model and restores clean state");
    good(app.redo(caller, at(current.info), "redo"), "redo imported model");
    current = good(app.snapshot(doc.document), "history redone snapshot");
    check(static_cast<const Model&>(current) == model,
          "redo restores entities, organization, references, and provenance");
}
} // namespace

int main() {
    try {
        validation_and_queries();
        application_flow();
        resource_limits();
        delete_owned_entity();
        imported_history();
        std::cout << "model tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
