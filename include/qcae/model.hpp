#pragma once
#include "qcae/types.hpp"
#include <array>
#include <variant>
#include <vector>

namespace qcae {
// Canonical engineering units: mm, N, MPa. BDF input must explicitly declare mm-N-MPa.
struct Vec3 { double x{}, y{}, z{}; bool operator==(const Vec3&) const = default; };
struct Node { EntityId id; Vec3 position; bool operator==(const Node&) const = default; };
struct BeamSection {
    EntityId id; std::string name; EntityId material;
    double area_mm2{}, i1_mm4{}, i2_mm4{}, torsion_mm4{};
    bool operator==(const BeamSection&) const = default;
};
struct Beam {
    EntityId id; EntityId section; std::array<EntityId, 2> nodes; Vec3 orientation;
    bool operator==(const Beam&) const = default;
};
struct Part { EntityId id; std::string name; std::vector<EntityId> members; bool operator==(const Part&) const = default; };
struct Assembly { EntityId id; std::string name; std::vector<EntityId> children; bool operator==(const Assembly&) const = default; };
struct EntitySet { EntityId id; std::string name; std::vector<EntityId> members; bool operator==(const EntitySet&) const = default; };
struct IncludeDocument {
    EntityId id; std::string path; std::optional<EntityId> parent; std::vector<EntityId> members;
    bool operator==(const IncludeDocument&) const = default;
};
struct NodalForce { EntityId id; EntityId node; Vec3 force_n; bool operator==(const NodalForce&) const = default; };
struct Constraint { EntityId id; std::vector<EntityId> nodes; std::string dofs; bool operator==(const Constraint&) const = default; };
struct AnalysisDefinition {
    EntityId id; std::string name; TargetBinding target;
    std::vector<EntityId> forces; std::vector<EntityId> constraints;
    bool operator==(const AnalysisDefinition&) const = default;
};
struct SourceIdentifier {
    EntityId entity; std::string source_model_id; EntityId include;
    ProfileRef profile; std::string name_space; std::uint64_t number{};
    bool operator==(const SourceIdentifier&) const = default;
};
struct Model {
    std::vector<Material> materials;
    std::vector<Node> nodes;
    std::vector<BeamSection> sections;
    std::vector<Beam> beams;
    std::vector<Part> parts;
    std::vector<Assembly> assemblies;
    std::vector<EntitySet> sets;
    std::vector<IncludeDocument> includes;
    std::vector<NodalForce> forces;
    std::vector<Constraint> constraints;
    std::vector<AnalysisDefinition> analyses;
    std::vector<SourceIdentifier> sources;
    bool operator==(const Model&) const = default;
};
struct Reference { EntityId from; EntityId to; std::string role; bool operator==(const Reference&) const = default; };
struct EntitySummary { EntityId id; std::string kind; std::string name; };
// Enumerates authority references. Reverse queries derive from the same immutable snapshot.
std::vector<Reference> model_references(const Model&);
std::vector<EntitySummary> model_entities(const Model&);
std::vector<Diagnostic> validate_model(const Model&);
std::vector<EntityId> affected_analyses(const Model&, const EntityId&);
struct UpsertPart { Part value; };
struct UpsertAssembly { Assembly value; };
struct UpsertSet { EntitySet value; };
struct MoveNode { EntityId id; Vec3 position; };
struct DeleteEntity { EntityId id; };
using ModelEdit = std::variant<UpsertPart, UpsertAssembly, UpsertSet, MoveNode, DeleteEntity>;
} // namespace qcae
