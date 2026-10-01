#include "cantilever_checks.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace qcae::features::analysis::detail {
namespace {
// These rules describe the baseline scenario, rather than a particular solver
// deck, mesh density, coordinate direction or numerical benchmark. The geometric
// tolerance is dimensionless; it is not a solver-result acceptance tolerance.
constexpr double angular_tolerance = 1e-10;
enum Rule : std::size_t {
    missing_load,
    missing_constraint,
    missing_beam,
    single_case,
    connectivity,
    straight,
    missing_section,
    missing_material,
    uniform_section,
    orientation,
    single_load,
    single_constraint,
    fixed_end,
    missing_fixed_dofs,
    tip_load,
    transverse_load
};
constexpr std::array definitions{
    RuleDefinition{"missing-load",
                   1,
                   "Linear static analysis requires a referenced nodal force",
                   Status::needs_input},
    RuleDefinition{"missing-constraint",
                   1,
                   "Linear static analysis requires a referenced constraint",
                   Status::needs_input},
    RuleDefinition{"missing-beam",
                   1,
                   "Cantilever analysis requires a beam with physical endpoints",
                   Status::needs_input},
    RuleDefinition{"cantilever-single-case", 1, "Cantilever analysis requires one load case"},
    RuleDefinition{"cantilever-connectivity",
                   1,
                   "Cantilever beams and nodes must form one unbranched connected chain"},
    RuleDefinition{"cantilever-straight",
                   1,
                   "Cantilever beam segments must follow one straight line without backtracking"},
    RuleDefinition{"missing-section",
                   1,
                   "Each cantilever beam requires a complete beam section",
                   Status::needs_input},
    RuleDefinition{"missing-material",
                   1,
                   "Cantilever material requires Young's modulus and Poisson's ratio",
                   Status::needs_input},
    RuleDefinition{"cantilever-uniform-section",
                   1,
                   "Cantilever beams require uniform section and linear isotropic material values"},
    RuleDefinition{"cantilever-orientation",
                   1,
                   "Cantilever beams require a consistent nonparallel section orientation"},
    RuleDefinition{"cantilever-single-load",
                   1,
                   "Cantilever analysis requires exactly one referenced nodal force"},
    RuleDefinition{"cantilever-single-constraint",
                   1,
                   "Cantilever analysis requires exactly one referenced constraint"},
    RuleDefinition{
        "cantilever-fixed-end", 1, "Cantilever constraint must fix exactly one chain endpoint"},
    RuleDefinition{"missing-fixed-dofs",
                   1,
                   "Cantilever fixed end requires all three translations and three rotations",
                   Status::needs_input},
    RuleDefinition{"cantilever-tip-load",
                   1,
                   "Cantilever nodal force must act at the endpoint opposite the fixed end"},
    RuleDefinition{"cantilever-transverse-load",
                   1,
                   "Cantilever force must be finite, nonzero and transverse to the beam axis"}};

using Vector = std::array<double, 3>;
double norm(const Vector& value) {
    return std::hypot(value[0], value[1], value[2]);
}
double dot(const Vector& left, const Vector& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}
Vector difference(const Vector& left, const Vector& right) {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}
Vector unit(const Vector& value, double length) {
    return {value[0] / length, value[1] / length, value[2] / length};
}
Vector cross(const Vector& left, const Vector& right) {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}
bool same_properties(const records::BeamSection& left,
                     const records::Material& left_material,
                     const records::BeamSection& right,
                     const records::Material& right_material) {
    return left.area_mm2 == right.area_mm2 && left.i1_mm4 == right.i1_mm4 &&
           left.i2_mm4 == right.i2_mm4 && left.torsion_mm4 == right.torsion_mm4 &&
           left_material.young_modulus_mpa == right_material.young_modulus_mpa &&
           left_material.poisson_ratio == right_material.poisson_ratio;
}

class CantileverCheck {
  public:
    CantileverCheck(const DocumentView& view, const Record& analysis, CheckReport& report)
        : view_(view), analysis_(analysis), report_(report) {}

    void run() {
        const auto& analysis = analysis_->get<records::AnalysisDefinition>();
        const std::vector<EntityId>* forces = &analysis.forces;
        const std::vector<EntityId>* constraints = &analysis.constraints;
        if (analysis.load_cases) {
            if (analysis.load_cases->size() > 1) {
                add(single_case,
                    analysis_->key(),
                    "load_cases",
                    std::to_string(analysis.load_cases->size()),
                    "1");
                return;
            }
            if (!analysis.load_cases->empty()) {
                const auto load_case = view_.find<records::LoadCase>(analysis.load_cases->front());
                if (!load_case) {
                    add(single_case,
                        analysis_->key(),
                        "load_cases",
                        "missing reference",
                        "1 resolved case");
                    return;
                }
                forces = &load_case->get<records::LoadCase>().forces;
                constraints = &load_case->get<records::LoadCase>().constraints;
            }
        }
        const auto load_field = analysis.load_cases ? "load_cases.forces" : "forces";
        const auto constraint_field =
            analysis.load_cases ? "load_cases.constraints" : "constraints";
        if (forces->empty())
            add(missing_load, analysis_->key(), load_field, "0", ">=1");
        else if (forces->size() != 1)
            add(single_load, analysis_->key(), load_field, std::to_string(forces->size()), "1");
        if (constraints->empty())
            add(missing_constraint, analysis_->key(), constraint_field, "0", ">=1");
        else if (constraints->size() != 1)
            add(single_constraint,
                analysis_->key(),
                constraint_field,
                std::to_string(constraints->size()),
                "1");

        view_.visit(RecordTraits<records::Beam>::type_id,
                    [&](const Record& record) { beams_.push_back(&record->get<records::Beam>()); });
        if (beams_.empty()) {
            add(missing_beam, analysis_->key(), "beams", "0", ">=1");
            return;
        }
        const bool connected = build_chain();
        const bool straight_chain = connected && check_straight();
        check_properties(straight_chain);
        check_boundary(*forces, *constraints, connected, straight_chain);
    }

  private:
    struct NodeLinks {
        const records::Node* node{};
        std::vector<std::string_view> neighbors;
    };
    void
    add(Rule index, RecordKey entity, std::string field, std::string actual, std::string expected) {
        if (!reported_.insert(index).second)
            return;
        const auto& rule = definitions[index];
        if (rule.outcome == Status::failed || report_.outcome == Status::success)
            report_.outcome = rule.outcome;
        report_.issues.push_back({std::string(rule.id),
                                  rule.version,
                                  "error",
                                  std::move(entity),
                                  report_.input,
                                  std::move(field),
                                  std::string(rule.description),
                                  std::move(actual),
                                  std::move(expected)});
    }
    template <class T> static RecordKey key(const T& record) {
        return {RecordTraits<T>::type_id, record.id.value};
    }
    bool build_chain() {
        view_.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
            const auto& node = record->get<records::Node>();
            links_.emplace(node.id.value, NodeLinks{&node, {}});
        });
        std::set<std::pair<std::string_view, std::string_view>> edges;
        for (const auto* beam : beams_) {
            const auto a = links_.find(beam->nodes[0].value), b = links_.find(beam->nodes[1].value);
            const std::string_view left = beam->nodes[0].value, right = beam->nodes[1].value;
            const auto edge = std::minmax(left, right);
            if (a == links_.end() || b == links_.end() || a == b || !edges.emplace(edge).second) {
                add(connectivity,
                    key(*beam),
                    "nodes",
                    "missing or duplicate edge",
                    "one chain edge");
                return false;
            }
            a->second.neighbors.push_back(b->first);
            b->second.neighbors.push_back(a->first);
        }
        std::vector<std::string_view> ends;
        for (const auto& [id, links] : links_) {
            if (links.neighbors.size() == 1)
                ends.push_back(id);
            else if (links.neighbors.size() != 2) {
                add(connectivity,
                    key(*links.node),
                    "beams.nodes",
                    std::to_string(links.neighbors.size()),
                    "endpoint degree 1 or interior degree 2");
                return false;
            }
        }
        if (ends.size() != 2 || edges.size() + 1 != links_.size()) {
            add(connectivity,
                analysis_->key(),
                "beams.nodes",
                std::to_string(ends.size()),
                "2 endpoints");
            return false;
        }
        std::set<std::string_view> visited;
        std::string_view current = ends.front(), previous;
        while (visited.insert(current).second) {
            const auto& links = links_.at(current);
            chain_.push_back(links.node);
            const auto next = std::find_if(links.neighbors.begin(),
                                           links.neighbors.end(),
                                           [&](auto id) { return id != previous; });
            if (next == links.neighbors.end())
                break;
            previous = current;
            current = *next;
        }
        if (visited.size() != links_.size()) {
            add(connectivity,
                analysis_->key(),
                "beams.nodes",
                "disconnected component",
                "one connected chain");
            return false;
        }
        return true;
    }
    bool check_straight() {
        const auto axis = difference(chain_.back()->position, chain_.front()->position);
        const double length = norm(axis);
        if (!std::isfinite(length) || length <= 0) {
            add(straight,
                analysis_->key(),
                "position",
                "coincident or nonfinite endpoints",
                "distinct finite endpoints");
            return false;
        }
        axis_ = unit(axis, length);
        for (std::size_t index = 1; index < chain_.size(); ++index) {
            const auto step = difference(chain_[index]->position, chain_[index - 1]->position);
            const auto step_length = norm(step);
            if (!std::isfinite(step_length) || step_length <= 0 ||
                dot(unit(step, step_length), axis_) <= 0 ||
                norm(cross(unit(step, step_length), axis_)) > angular_tolerance) {
                add(straight,
                    key(*chain_[index]),
                    "position",
                    "nonstraight or backtracking segment",
                    "positive axial progression; angular residual <= 1e-10");
                return false;
            }
        }
        return true;
    }
    void check_properties(bool straight_chain) {
        const records::BeamSection* baseline = nullptr;
        const records::Material* baseline_material = nullptr;
        std::optional<Vector> baseline_normal;
        for (const auto* beam : beams_) {
            const auto section =
                beam->section ? view_.find<records::BeamSection>(*beam->section) : Record{};
            if (!section) {
                add(missing_section, key(*beam), "section", "absent", "resolved complete section");
            } else {
                const auto& value = section->get<records::BeamSection>();
                const auto material = view_.find<records::Material>(value.material);
                if (!material || !material->get<records::Material>().poisson_ratio) {
                    add(missing_material,
                        key(value),
                        "material.poisson_ratio",
                        "absent",
                        "finite -1 < nu < 0.5");
                } else if (!baseline) {
                    baseline = &value;
                    baseline_material = &material->get<records::Material>();
                } else if (!same_properties(*baseline,
                                            *baseline_material,
                                            value,
                                            material->get<records::Material>())) {
                    add(uniform_section,
                        key(*beam),
                        "section",
                        value.id.value,
                        "same physical section and material values");
                }
            }
            if (!straight_chain)
                continue;
            const auto orientation_length = norm(beam->orientation);
            const auto normal = cross(axis_, unit(beam->orientation, orientation_length));
            const auto normal_length = norm(normal);
            if (!std::isfinite(orientation_length) || orientation_length <= 0 ||
                !std::isfinite(normal_length) || normal_length <= angular_tolerance) {
                add(orientation,
                    key(*beam),
                    "orientation",
                    "parallel or invalid",
                    "finite nonparallel section plane");
            } else {
                const auto normalized = unit(normal, normal_length);
                if (baseline_normal &&
                    norm(cross(*baseline_normal, normalized)) > angular_tolerance)
                    add(orientation,
                        key(*beam),
                        "orientation",
                        "varying section plane",
                        "uniform section plane (sign independent)");
                else if (!baseline_normal)
                    baseline_normal = normalized;
            }
        }
    }
    void check_boundary(const std::vector<EntityId>& forces,
                        const std::vector<EntityId>& constraints,
                        bool connected,
                        bool straight_chain) {
        const records::Constraint* support = nullptr;
        if (constraints.size() == 1) {
            const auto record = view_.find<records::Constraint>(constraints.front());
            if (!record) {
                add(fixed_end,
                    analysis_->key(),
                    "constraints",
                    "missing reference",
                    "resolved endpoint constraint");
            } else {
                support = &record->get<records::Constraint>();
                if (support->dofs.size() != 6 ||
                    support->dofs.find_first_not_of("123456") != std::string::npos ||
                    std::set<char>(support->dofs.begin(), support->dofs.end()).size() != 6)
                    add(missing_fixed_dofs,
                        key(*support),
                        "dofs",
                        support->dofs,
                        "123456 (any order)");
                if (connected &&
                    (support->nodes.size() != 1 || (support->nodes.front() != chain_.front()->id &&
                                                    support->nodes.front() != chain_.back()->id))) {
                    add(fixed_end,
                        key(*support),
                        "nodes",
                        std::to_string(support->nodes.size()),
                        "exactly one chain endpoint");
                    support = nullptr;
                }
            }
        }
        if (forces.size() != 1)
            return;
        const auto record = view_.find<records::NodalForce>(forces.front());
        if (!record) {
            add(tip_load, analysis_->key(), "forces", "missing reference", "resolved nodal force");
            return;
        }
        const auto& force = record->get<records::NodalForce>();
        if (connected && support && support->nodes.size() == 1) {
            const auto& tip = support->nodes.front() == chain_.front()->id ? chain_.back()->id
                                                                           : chain_.front()->id;
            if (force.node != tip)
                add(tip_load, key(force), "node", force.node.value, tip.value);
        }
        const double force_length = norm(force.force_n);
        if (!std::isfinite(force_length) || force_length <= 0 ||
            (straight_chain &&
             std::abs(dot(unit(force.force_n, force_length), axis_)) > angular_tolerance))
            add(transverse_load,
                key(force),
                "force_n",
                "zero, nonfinite or axial force component",
                "nonzero force; absolute axial cosine <= 1e-10");
    }

    const DocumentView& view_;
    const Record& analysis_;
    CheckReport& report_;
    std::set<Rule> reported_;
    std::vector<const records::Beam*> beams_;
    std::map<std::string_view, NodeLinks, std::less<>> links_;
    std::vector<const records::Node*> chain_;
    Vector axis_{};
};
} // namespace
std::span<const RuleDefinition> cantilever_rules() noexcept {
    return definitions;
}
void check_cantilever(const DocumentView& view, const Record& analysis, CheckReport& report) {
    CantileverCheck(view, analysis, report).run();
}
} // namespace qcae::features::analysis::detail
