#include "qcae/nastran_codec.hpp"
#include "qcae/nastran_digest.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace qcae {
namespace {
constexpr std::size_t max_resources = 128, max_depth = 10, max_bytes = 1024 * 1024;
constexpr std::size_t max_line = 4096, max_entities = 100000;
constexpr std::uint64_t max_number = 99999999;
using Fields = std::vector<std::string>;

std::string trim(std::string_view v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
        v.remove_prefix(1);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
        v.remove_suffix(1);
    return std::string(v);
}
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}
Fields split(std::string_view line) {
    Fields out;
    while (true) {
        const auto comma = line.find(',');
        out.push_back(trim(line.substr(0, comma)));
        if (comma == std::string_view::npos)
            return out;
        line.remove_prefix(comma + 1);
    }
}
std::string field(const Fields& f, std::size_t i) {
    return i < f.size() ? f[i] : "";
}
bool blank_after(const Fields& f, std::size_t last) {
    for (std::size_t i = last + 1; i < f.size(); ++i)
        if (!f[i].empty())
            return false;
    return true;
}
bool uint_field(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s.size() > 8)
        return false;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), out);
    return error == std::errc{} && end == s.data() + s.size() && out > 0 && out <= max_number;
}
bool real_field(const std::string& s, double& out) {
    if (s.empty() || s.size() > 8 || s.find_first_of("dD") != std::string::npos)
        return false;
    auto [end, error] =
        std::from_chars(s.data(), s.data() + s.size(), out, std::chars_format::general);
    return error == std::errc{} && end == s.data() + s.size() && std::isfinite(out);
}
std::string path_join(const std::string& base, const std::string& child) {
    if (child.empty() || child.find_first_of("\\:'$\r\n") != std::string::npos ||
        child.front() == '/')
        return {};
    if (std::any_of(
            child.begin(), child.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return {};
    std::filesystem::path input(child);
    for (const auto& part : input)
        if (part == ".." || part == ".")
            return {};
    if (input.lexically_normal().generic_string() != child)
        return {};
    const auto path = (std::filesystem::path(base).parent_path() / input).generic_string();
    return path.empty() || path.front() == '/' ? std::string{} : path;
}
bool safe_path(const std::string& p) {
    return !path_join("root.bdf", p).empty();
}
std::string id_for(const std::string& source, const std::string& kind, std::uint64_t n) {
    return "nastran:" + std::to_string(source.size()) + ":" + source + ":" + kind + ":" +
           std::to_string(n);
}
std::string include_id(const std::string& source, const std::string& path) {
    return "nastran:" + std::to_string(source.size()) + ":" + source + ":include:" + path;
}
void issue(std::vector<FormatIssue>& out,
           std::string code,
           std::string message,
           const std::string& path = {},
           std::size_t line = 0,
           bool blocking = true) {
    out.push_back({std::move(code), std::move(message), path, line, blocking});
}
bool blocked(const std::vector<FormatIssue>& issues) {
    return std::any_of(issues.begin(), issues.end(), [](const auto& i) { return i.blocking; });
}
std::string real_text(double v, bool require_floating = false) {
    if (!std::isfinite(v))
        return {};
    char buffer[128];
    auto exact = [&](const std::string& candidate) {
        double recovered{};
        if (candidate.empty() || candidate.size() > 8 || !real_field(candidate, recovered))
            return false;
        return recovered == v && std::signbit(recovered) == std::signbit(v);
    };
    auto compact_exponent = [](std::string candidate) {
        const auto exponent = candidate.find_first_of("eE");
        if (exponent == std::string::npos)
            return candidate;
        std::string digits = candidate.substr(exponent + 1);
        const bool negative = !digits.empty() && digits.front() == '-';
        if (!digits.empty() && (digits.front() == '-' || digits.front() == '+'))
            digits.erase(0, 1);
        const auto first_nonzero = digits.find_first_not_of('0');
        digits = first_nonzero == std::string::npos ? "0" : digits.substr(first_nonzero);
        return candidate.substr(0, exponent) + "e" + (negative ? "-" : "") + digits;
    };
    auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), v, std::chars_format::general);
    if (error == std::errc{}) {
        std::string candidate(buffer, end);
        if (candidate.find_first_of(".eE") == std::string::npos) {
            if (exact(candidate + ".0"))
                return candidate + ".0";
            if (exact(candidate + "."))
                return candidate + ".";
            if (!require_floating && exact(candidate))
                return candidate;
        } else {
            if (exact(candidate))
                return candidate;
            candidate = compact_exponent(candidate);
            if (exact(candidate))
                return candidate;
        }
    }
    auto [fixed_end, fixed_error] =
        std::to_chars(buffer, buffer + sizeof(buffer), v, std::chars_format::fixed);
    if (fixed_error == std::errc{}) {
        std::string candidate(buffer, fixed_end);
        if (candidate.find('.') == std::string::npos) {
            if (exact(candidate + ".0"))
                return candidate + ".0";
            if (exact(candidate + "."))
                return candidate + ".";
            if (!require_floating && exact(candidate))
                return candidate;
        } else if (exact(candidate))
            return candidate;
    }
    for (int precision = 0; precision <= 7; ++precision) {
        auto [last, status] = std::to_chars(
            buffer, buffer + sizeof(buffer), v, std::chars_format::scientific, precision);
        if (status == std::errc{}) {
            const std::string candidate(buffer, last);
            if (exact(candidate))
                return candidate;
            const auto compact = compact_exponent(candidate);
            if (exact(compact))
                return compact;
        }
    }
    return {};
}

struct Reader {
    const ImportRequest& request;
    ImportReport& report;
    Reader(const ImportRequest& input, ImportReport& output) : request(input), report(output) {}
    Model model;
    std::map<std::string, std::string> resources;
    std::set<std::string> visited;
    std::set<std::string> active;
    std::map<std::string, std::set<std::uint64_t>> numbers;
    std::map<std::string, std::pair<std::string, std::size_t>> locations;
    std::uint64_t spc_sid{}, load_sid{};
    bool sol{}, cend{}, subcase{}, spc_seen{}, load_seen{}, displacement{}, spcforces{}, bulk{},
        enddata{};

    void error(std::string code, std::string message, const std::string& path, std::size_t line) {
        issue(report.issues, std::move(code), std::move(message), path, line);
    }
    EntityId entity(const std::string& kind, std::uint64_t n) const {
        return EntityId{id_for(request.source_model_id, kind, n)};
    }
    void source(const EntityId& entity_id,
                const EntityId& owner,
                const std::string& kind,
                std::uint64_t n) {
        model.sources.push_back(
            {entity_id, request.source_model_id, owner, report.profile, kind, n});
        for (auto& inc : model.includes)
            if (inc.id == owner) {
                inc.members.push_back(entity_id);
                break;
            }
        ++report.supported_records;
    }
    bool
    unique(const std::string& kind, std::uint64_t n, const std::string& path, std::size_t line) {
        if (!numbers[kind].insert(n).second) {
            error("duplicate_number",
                  "duplicate " + kind + " number " + std::to_string(n),
                  path,
                  line);
            return false;
        }
        return true;
    }
    bool number(const Fields& f,
                std::size_t i,
                std::uint64_t& n,
                const std::string& path,
                std::size_t line) {
        if (uint_field(field(f, i), n))
            return true;
        if (field(f, i).size() > 8) {
            error("unsupported_precision", "numeric token exceeds eight characters", path, line);
            return false;
        }
        error("invalid_number",
              "expected positive decimal number in field " + std::to_string(i),
              path,
              line);
        return false;
    }
    bool
    real(const Fields& f, std::size_t i, double& v, const std::string& path, std::size_t line) {
        if (real_field(field(f, i), v))
            return true;
        if (field(f, i).size() > 8) {
            error("unsupported_precision", "numeric token exceeds eight characters", path, line);
            return false;
        }
        error("invalid_real",
              "expected finite standard decimal real in field " + std::to_string(i),
              path,
              line);
        return false;
    }
    bool zero_or_blank(const Fields& f, std::size_t i, const std::string& path, std::size_t line) {
        if (field(f, i).empty() || field(f, i) == "0")
            return true;
        error("unsupported_field",
              "field " + std::to_string(i) + " must be blank or zero",
              path,
              line);
        return false;
    }
    bool tail(const Fields& f, std::size_t last, const std::string& path, std::size_t line) {
        if (blank_after(f, last))
            return true;
        error("unsupported_field",
              "unsupported field or continuation after field " + std::to_string(last),
              path,
              line);
        return false;
    }
    void card(const Fields& f, const EntityId& owner, const std::string& path, std::size_t line) {
        const auto name = upper(f.front());
        std::uint64_t a{}, b{}, c{}, d{};
        double x{}, y{}, z{}, w{};
        if (name == "GRID") {
            if (!number(f, 1, a, path, line) || !zero_or_blank(f, 2, path, line) ||
                !real(f, 3, x, path, line) || !real(f, 4, y, path, line) ||
                !real(f, 5, z, path, line) || !zero_or_blank(f, 6, path, line) ||
                !zero_or_blank(f, 7, path, line) || !zero_or_blank(f, 8, path, line) ||
                !tail(f, 8, path, line) || !unique(name, a, path, line))
                return;
            model.nodes.push_back({entity(name, a), {x, y, z}});
        } else if (name == "MAT1") {
            if (!number(f, 1, a, path, line) || !real(f, 2, x, path, line) ||
                !real(f, 4, y, path, line))
                return;
            if (x <= 0 || y <= -1 || y >= 0.5) {
                error("invalid_physics", "MAT1 requires E > 0 and -1 < NU < 0.5", path, line);
                return;
            }
            const double derived_g = x / (2 * (1 + y));
            if (!std::isfinite(derived_g) || derived_g <= 0) {
                error("invalid_physics", "MAT1 derived G must be finite and positive", path, line);
                return;
            }
            if (!field(f, 3).empty()) {
                if (!real(f, 3, z, path, line) || z <= 0 || z != derived_g) {
                    error("inconsistent_mat1",
                          "MAT1 G must be blank or exactly equal to E/[2(1+NU)]",
                          path,
                          line);
                    return;
                }
            }
            for (std::size_t i = 5; i <= 11; ++i)
                if (!zero_or_blank(f, i, path, line))
                    return;
            if (!tail(f, 11, path, line) || !unique(name, a, path, line))
                return;
            model.materials.push_back({entity(name, a), "MAT1 " + std::to_string(a), x, y});
        } else if (name == "PBAR") {
            if (!number(f, 1, a, path, line) || !number(f, 2, b, path, line) ||
                !real(f, 3, x, path, line) || !real(f, 4, y, path, line) ||
                !real(f, 5, z, path, line) || !real(f, 6, w, path, line) ||
                !zero_or_blank(f, 7, path, line) || !tail(f, 7, path, line) ||
                !unique(name, a, path, line))
                return;
            if (x <= 0 || y <= 0 || z <= 0 || w <= 0) {
                error("invalid_physics", "PBAR A, I1, I2, and J must be positive", path, line);
                return;
            }
            model.sections.push_back(
                {entity(name, a), "PBAR " + std::to_string(a), entity("MAT1", b), x, y, z, w});
        } else if (name == "CBAR") {
            if (field(f, 5).find_first_of(".eE") == std::string::npos) {
                error("unsupported_orientation",
                      "CBAR X1 must use floating spelling to distinguish vector from G0",
                      path,
                      line);
                return;
            }
            if (!number(f, 1, a, path, line) || !number(f, 2, b, path, line) ||
                !number(f, 3, c, path, line) || !number(f, 4, d, path, line) ||
                !real(f, 5, x, path, line) || !real(f, 6, y, path, line) ||
                !real(f, 7, z, path, line) || !tail(f, 7, path, line) ||
                !unique(name, a, path, line))
                return;
            if (c == d || x * x + y * y + z * z == 0) {
                error("invalid_physics",
                      "CBAR needs distinct nodes and a nonzero orientation vector",
                      path,
                      line);
                return;
            }
            model.beams.push_back({entity(name, a),
                                   entity("PBAR", b),
                                   {entity("GRID", c), entity("GRID", d)},
                                   {x, y, z}});
        } else if (name == "FORCE") {
            if (!number(f, 1, a, path, line) || !number(f, 2, b, path, line) ||
                !zero_or_blank(f, 3, path, line) || !real(f, 4, x, path, line) ||
                !real(f, 5, y, path, line) || !real(f, 6, z, path, line) ||
                !real(f, 7, w, path, line) || !tail(f, 7, path, line) ||
                !unique(name, a, path, line))
                return;
            if (x == 0 || (y == 0 && z == 0 && w == 0)) {
                error(
                    "invalid_physics", "FORCE magnitude and direction must be nonzero", path, line);
                return;
            }
            const Vec3 product{x * y, x * z, x * w};
            if (!std::isfinite(product.x) || !std::isfinite(product.y) ||
                !std::isfinite(product.z) || (y != 0 && product.x == 0) ||
                (z != 0 && product.y == 0) || (w != 0 && product.z == 0) ||
                (product.x == 0 && product.y == 0 && product.z == 0)) {
                error("invalid_physics",
                      "FORCE components overflow or underflow during conversion",
                      path,
                      line);
                return;
            }
            model.forces.push_back({entity(name, a), entity("GRID", b), product});
        } else if (name == "SPC1") {
            if (!number(f, 1, a, path, line) || !unique(name, a, path, line))
                return;
            const auto dofs = field(f, 2);
            if (dofs.empty() || dofs.find_first_not_of("123456") != std::string::npos ||
                std::set<char>(dofs.begin(), dofs.end()).size() != dofs.size()) {
                error("invalid_dofs",
                      "SPC1 degrees of freedom must be unique digits 1 through 6",
                      path,
                      line);
                return;
            }
            Constraint constraint{entity(name, a), {}, dofs};
            for (std::size_t i = 3; i < f.size(); ++i) {
                if (!number(f, i, b, path, line))
                    return;
                constraint.nodes.push_back(entity("GRID", b));
            }
            if (constraint.nodes.empty()) {
                error("missing_field", "SPC1 requires at least one grid", path, line);
                return;
            }
            model.constraints.push_back(std::move(constraint));
        } else {
            error("unsupported_card", "unsupported bulk card " + name, path, line);
            return;
        }
        const auto n = a;
        source(entity(name, n), owner, name, n);
        locations[entity(name, n).value] = {path, line};
        if (model.sources.size() > max_entities)
            error("resource_limit", "entity limit exceeded", path, line);
    }
    void control(const std::string& s, const std::string& path, std::size_t line) {
        const auto u = upper(s);
        auto assigned = [&](std::string_view key, std::uint64_t& n) {
            const auto equal = u.find('=');
            if (equal == std::string::npos || trim(u.substr(0, equal)) != key ||
                !uint_field(trim(u.substr(equal + 1)), n))
                return false;
            return true;
        };
        if (!sol && u == "SOL 101") {
            sol = true;
            return;
        }
        if (sol && !cend && u == "CEND") {
            cend = true;
            return;
        }
        if (cend && !subcase && u == "SUBCASE 1") {
            subcase = true;
            return;
        }
        if (subcase && !bulk && !spc_seen && assigned("SPC", spc_sid)) {
            spc_seen = true;
            return;
        }
        if (subcase && !bulk && !load_seen && assigned("LOAD", load_sid)) {
            load_seen = true;
            return;
        }
        if (subcase && !bulk && (u == "DISPLACEMENT=ALL" || u == "DISPLACEMENT = ALL") &&
            !displacement) {
            displacement = true;
            return;
        }
        if (subcase && !bulk && (u == "SPCFORCES=ALL" || u == "SPCFORCES = ALL") && !spcforces) {
            spcforces = true;
            return;
        }
        if (subcase && !bulk && u == "BEGIN BULK" && spc_seen && load_seen && displacement &&
            spcforces) {
            bulk = true;
            return;
        }
        error("unsupported_control",
              "unexpected or repeated executive/case control statement",
              path,
              line);
    }
    void deck(const std::string& path, const std::optional<EntityId>& parent, std::size_t depth) {
        if (depth > max_depth) {
            error("resource_limit", "INCLUDE depth exceeded", path, 0);
            return;
        }
        if (active.count(path)) {
            error("include_cycle", "INCLUDE cycle", path, 0);
            return;
        }
        if (!visited.insert(path).second) {
            error("include_repeat", "INCLUDE resource repeated", path, 0);
            return;
        }
        const auto found = resources.find(path);
        if (found == resources.end()) {
            error("missing_include", "resource not provided", path, 0);
            return;
        }
        active.insert(path);
        const EntityId owner{include_id(request.source_model_id, path)};
        model.includes.push_back({owner, path, parent, {}});
        std::istringstream input(found->second);
        std::string line_text;
        std::size_t line = 0;
        while (std::getline(input, line_text)) {
            ++line;
            if (!line_text.empty() && line_text.back() == '\r')
                line_text.pop_back();
            if (line_text.size() > max_line) {
                error("resource_limit", "line too long", path, line);
                continue;
            }
            const auto s = trim(line_text);
            if (s.empty() || s.front() == '$')
                continue;
            if (s.find('$') != std::string::npos) {
                error("unsupported_comment", "inline comments are unsupported", path, line);
                continue;
            }
            if (enddata) {
                error("trailing_data", "data after ENDDATA", path, line);
                continue;
            }
            const auto u = upper(s);
            if (u.rfind("INCLUDE", 0) == 0) {
                if (s.size() > 72) {
                    error("unsupported_format",
                          "INCLUDE statement exceeds 72 characters",
                          path,
                          line);
                    continue;
                }
                if (path == request.root_resource && !bulk) {
                    error("include_position", "INCLUDE must be in bulk section", path, line);
                    continue;
                }
                const auto rest = trim(s.substr(7));
                if (rest.size() < 3 || rest.front() != '\'' || rest.back() != '\'' ||
                    rest.substr(1, rest.size() - 2).find('\'') != std::string::npos) {
                    error("include_syntax", "INCLUDE requires one single-quoted path", path, line);
                    continue;
                }
                const auto target = path_join(path, rest.substr(1, rest.size() - 2));
                if (target.empty()) {
                    error("include_path", "unsafe INCLUDE path", path, line);
                    continue;
                }
                deck(target, owner, depth + 1);
                continue;
            }
            if (path == request.root_resource && !bulk) {
                control(s, path, line);
                continue;
            }
            if (u == "ENDDATA") {
                if (path != request.root_resource || enddata)
                    error("unsupported_control", "ENDDATA only once in root", path, line);
                else
                    enddata = true;
                continue;
            }
            if (s.find(',') == std::string::npos || s.front() == '+' || s.front() == '*') {
                error("unsupported_format",
                      "only single-line free-field cards are supported",
                      path,
                      line);
                continue;
            }
            const auto f = split(s);
            if (f.empty() || f.front().empty()) {
                error("unsupported_format", "missing card name", path, line);
                continue;
            }
            if (f.size() > 9) {
                error("unsupported_format",
                      "free-field physical line exceeds nine fields",
                      path,
                      line);
                continue;
            }
            card(f, owner, path, line);
        }
        active.erase(path);
    }
};
} // namespace

NastranCodec::NastranCodec() {
    definition_ = {{"qcae.nastran.linear-static", "0.2.0", std::string(nastran_definition_digest)},
                   "Nastran",
                   "linear_static",
                   false,
                   false,
                   std::string(nastran_implementation_fingerprint)};
}

ImportOutcome NastranCodec::decode(const ImportRequest& request) const {
    ImportOutcome out;
    out.report.profile = definition_.reference;
    Reader reader{request, out.report};
    if (request.source_profile != definition_.reference)
        issue(out.report.issues, "profile_mismatch", "source profile does not match Nastran codec");
    if (request.unit_system != "mm-N-MPa")
        issue(out.report.issues, "unit_system", "explicit mm-N-MPa units required");
    if (trim(request.source_model_id).empty())
        issue(out.report.issues, "source_model_id", "nonblank source model ID required");
    if (!safe_path(request.root_resource))
        issue(out.report.issues, "root_path", "unsafe root resource path", request.root_resource);
    if (request.resources.size() > max_resources)
        issue(out.report.issues, "resource_limit", "resource count exceeded");
    std::size_t bytes = 0;
    for (const auto& resource : request.resources) {
        bytes += resource.text.size();
        if (!safe_path(resource.path) ||
            !reader.resources.emplace(resource.path, resource.text).second)
            issue(out.report.issues,
                  "resource_path",
                  "unsafe or duplicate resource path",
                  resource.path);
    }
    if (bytes > max_bytes)
        issue(out.report.issues, "resource_limit", "total resource bytes exceeded");
    if (blocked(out.report.issues))
        return out;
    reader.deck(request.root_resource, std::nullopt, 1);
    for (const auto& [path, contents] : reader.resources) {
        (void)contents;
        if (!reader.visited.count(path))
            issue(out.report.issues,
                  "unused_resource",
                  "bundle resource is not reachable from root",
                  path);
    }
    if (!reader.sol || !reader.cend || !reader.subcase || !reader.bulk || !reader.enddata)
        issue(out.report.issues,
              "incomplete_deck",
              "SOL 101, CEND, SUBCASE 1, SPC/LOAD, requested outputs, BEGIN BULK and ENDDATA are "
              "required",
              request.root_resource);
    if (!reader.numbers["SPC1"].count(reader.spc_sid) ||
        !reader.numbers["FORCE"].count(reader.load_sid))
        issue(out.report.issues,
              "case_reference",
              "SPC and LOAD must reference defined singleton SPC1 and FORCE SIDs",
              request.root_resource);
    if (reader.model.constraints.size() != 1 || reader.model.forces.size() != 1)
        issue(out.report.issues,
              "unsupported_physics",
              "exactly one SPC1 and one FORCE are supported",
              request.root_resource);
    if (!blocked(out.report.issues)) {
        const EntityId analysis{id_for(request.source_model_id, "ANALYSIS", 1)};
        reader.model.analyses.push_back({analysis,
                                         "Nastran linear static",
                                         {definition_.reference, "linear_static"},
                                         {reader.entity("FORCE", reader.load_sid)},
                                         {reader.entity("SPC1", reader.spc_sid)}});
        for (const auto& b : reader.model.beams) {
            const auto ga = std::find_if(reader.model.nodes.begin(),
                                         reader.model.nodes.end(),
                                         [&](const Node& n) { return n.id == b.nodes[0]; });
            const auto gb = std::find_if(reader.model.nodes.begin(),
                                         reader.model.nodes.end(),
                                         [&](const Node& n) { return n.id == b.nodes[1]; });
            if (ga == reader.model.nodes.end() || gb == reader.model.nodes.end())
                continue;
            const Vec3 dx{gb->position.x - ga->position.x,
                          gb->position.y - ga->position.y,
                          gb->position.z - ga->position.z};
            const auto cross2 = std::pow(dx.y * b.orientation.z - dx.z * b.orientation.y, 2) +
                                std::pow(dx.z * b.orientation.x - dx.x * b.orientation.z, 2) +
                                std::pow(dx.x * b.orientation.y - dx.y * b.orientation.x, 2);
            if (cross2 <=
                1e-24 * (dx.x * dx.x + dx.y * dx.y + dx.z * dx.z) *
                    (b.orientation.x * b.orientation.x + b.orientation.y * b.orientation.y +
                     b.orientation.z * b.orientation.z))
                issue(out.report.issues,
                      "invalid_orientation",
                      "CBAR orientation is parallel to its axis",
                      request.root_resource);
        }
        for (const auto& diagnostic : validate_model(reader.model)) {
            auto location = std::make_pair(request.root_resource, std::size_t{});
            for (const auto& [id, where] : reader.locations)
                if (diagnostic.field.rfind(id, 0) == 0) {
                    location = where;
                    break;
                }
            issue(out.report.issues,
                  "invalid_model",
                  diagnostic.message,
                  location.first,
                  location.second);
        }
    }
    if (!blocked(out.report.issues)) {
        out.candidate = std::move(reader.model);
        out.report.complete = true;
    }
    return out;
}

ExportOutcome NastranCodec::encode(const Model& model,
                                   const EntityId& analysis_id,
                                   const ProfileRef& expected) const {
    ExportOutcome out;
    out.report.profile = definition_.reference;
    if (expected != definition_.reference)
        issue(out.report.issues, "profile_mismatch", "expected profile does not match codec");
    const auto analysis =
        std::find_if(model.analyses.begin(),
                     model.analyses.end(),
                     [&](const AnalysisDefinition& a) { return a.id == analysis_id; });
    if (analysis == model.analyses.end())
        issue(out.report.issues, "analysis_missing", "selected analysis not found");
    else if (analysis->target.profile != definition_.reference ||
             analysis->target.analysis_kind != "linear_static")
        issue(out.report.issues, "target_mismatch", "analysis target or version is unsupported");
    if (model.analyses.size() != 1)
        issue(out.report.issues, "unsupported_analysis", "exactly one analysis is supported");
    for (const auto& diagnostic : validate_model(model))
        issue(out.report.issues, "invalid_model", diagnostic.message);
    if (analysis != model.analyses.end()) {
        if (analysis->forces.size() != 1 || analysis->constraints.size() != 1 ||
            model.forces.size() != 1 || model.constraints.size() != 1 ||
            analysis->forces.front() != model.forces.front().id ||
            analysis->constraints.front() != model.constraints.front().id)
            issue(out.report.issues,
                  "unsupported_physics",
                  "single FORCE and SPC1 must be selected; unused physics cannot be omitted");
    }
    for (const auto& material : model.materials) {
        if (!material.poisson_ratio || material.young_modulus_mpa <= 0 ||
            *material.poisson_ratio <= -1 || *material.poisson_ratio >= 0.5) {
            issue(out.report.issues, "material_incomplete", "MAT1 requires E and NU");
        } else {
            const double derived_g =
                material.young_modulus_mpa / (2 * (1 + *material.poisson_ratio));
            if (!std::isfinite(derived_g) || derived_g <= 0)
                issue(out.report.issues,
                      "invalid_physics",
                      "MAT1 derived G must be finite and positive");
        }
    }
    for (const auto& constraint : model.constraints) {
        if (constraint.dofs.empty() ||
            constraint.dofs.find_first_not_of("123456") != std::string::npos ||
            std::set<char>(constraint.dofs.begin(), constraint.dofs.end()).size() !=
                constraint.dofs.size())
            issue(out.report.issues, "invalid_dofs", "SPC1 requires unique digits 1 through 6");
    }
    for (const auto& force : model.forces)
        if (!std::isfinite(std::hypot(force.force_n.x, force.force_n.y, force.force_n.z)) ||
            std::hypot(force.force_n.x, force.force_n.y, force.force_n.z) == 0)
            issue(out.report.issues, "invalid_force", "FORCE vector must be finite and nonzero");
    auto exact_real = [&](double value, bool floating = false) {
        if (real_text(value, floating).empty())
            issue(out.report.issues,
                  "precision_unrepresentable",
                  "real value has no exact eight-character free-field spelling");
    };
    for (const auto& n : model.nodes) {
        exact_real(n.position.x);
        exact_real(n.position.y);
        exact_real(n.position.z);
    }
    for (const auto& m : model.materials) {
        exact_real(m.young_modulus_mpa);
        if (m.poisson_ratio)
            exact_real(*m.poisson_ratio);
    }
    for (const auto& s : model.sections) {
        exact_real(s.area_mm2);
        exact_real(s.i1_mm4);
        exact_real(s.i2_mm4);
        exact_real(s.torsion_mm4);
    }
    for (const auto& b : model.beams) {
        exact_real(b.orientation.x, true);
        exact_real(b.orientation.y);
        exact_real(b.orientation.z);
    }
    for (const auto& f : model.forces) {
        exact_real(f.force_n.x);
        exact_real(f.force_n.y);
        exact_real(f.force_n.z);
    }
    if (model.nodes.size() + model.beams.size() + model.sections.size() + model.materials.size() +
            model.forces.size() + model.constraints.size() >
        max_entities)
        issue(out.report.issues, "resource_limit", "entity limit exceeded");
    if (blocked(out.report.issues))
        return out;

    ArtifactPlan plan;
    plan.analysis = analysis_id;
    plan.profile = definition_.reference;
    plan.root_resource = "model.bdf";
    std::map<EntityId, std::string> owner;
    std::map<EntityId, std::pair<std::string, std::uint64_t>> preferred;
    std::map<EntityId, std::string> include_paths;
    std::map<EntityId, std::string> entity_kinds;
    for (const auto& n : model.nodes)
        entity_kinds[n.id] = "GRID";
    for (const auto& m : model.materials)
        entity_kinds[m.id] = "MAT1";
    for (const auto& s : model.sections)
        entity_kinds[s.id] = "PBAR";
    for (const auto& b : model.beams)
        entity_kinds[b.id] = "CBAR";
    for (const auto& f : model.forces)
        entity_kinds[f.id] = "FORCE";
    for (const auto& c : model.constraints)
        entity_kinds[c.id] = "SPC1";
    for (const auto& inc : model.includes) {
        if (!safe_path(inc.path) || (inc.path == plan.root_resource && inc.parent))
            issue(out.report.issues, "include_path", "unsafe include path", inc.path);
        if (!include_paths.emplace(inc.id, inc.path).second)
            issue(out.report.issues, "include_duplicate", "duplicate include ID", inc.path);
        for (const auto& member : inc.members) {
            if (entity_kinds.count(member))
                owner.emplace(member, inc.path);
        }
    }
    if (!model.includes.empty()) {
        const auto roots = std::count_if(model.includes.begin(),
                                         model.includes.end(),
                                         [](const IncludeDocument& i) { return !i.parent; });
        if (roots != 1)
            issue(out.report.issues, "include_root", "exactly one root include document required");
        else
            plan.root_resource = std::find_if(model.includes.begin(),
                                              model.includes.end(),
                                              [](const IncludeDocument& i) { return !i.parent; })
                                     ->path;
    }
    for (const auto& inc : model.includes) {
        std::set<EntityId> seen;
        auto current = &inc;
        std::size_t depth = 0;
        while (current && seen.insert(current->id).second) {
            if (++depth > max_depth) {
                issue(out.report.issues, "resource_limit", "INCLUDE depth exceeded", inc.path);
                break;
            }
            if (!current->parent)
                break;
            const auto parent = std::find_if(
                model.includes.begin(),
                model.includes.end(),
                [&](const IncludeDocument& candidate) { return candidate.id == *current->parent; });
            current = parent == model.includes.end() ? nullptr : &*parent;
        }
    }
    for (const auto& src : model.sources) {
        if (src.profile != definition_.reference) {
            issue(out.report.issues, "source_profile", "source identity profile mismatch");
            continue;
        }
        if (!include_paths.count(src.include)) {
            issue(out.report.issues, "source_include", "source include not found");
            continue;
        }
        const auto kind = entity_kinds.find(src.entity);
        if (kind == entity_kinds.end() || kind->second != src.name_space || src.number == 0 ||
            src.number > max_number) {
            issue(out.report.issues,
                  "source_namespace",
                  "source identity namespace or number is unsupported");
            continue;
        }
        const auto member = owner.find(src.entity);
        if (member == owner.end() || member->second != include_paths.at(src.include)) {
            issue(out.report.issues,
                  "source_include",
                  "source identity does not match include membership");
            continue;
        }
        preferred[src.entity] = {src.name_space, src.number};
    }
    if (blocked(out.report.issues))
        return out;
    std::map<std::string, std::set<std::uint64_t>> used;
    std::map<EntityId, std::uint64_t> reserved;
    std::vector<std::pair<std::string, EntityId>> physical;
    for (const auto& n : model.nodes)
        physical.emplace_back("GRID", n.id);
    for (const auto& m : model.materials)
        physical.emplace_back("MAT1", m.id);
    for (const auto& s : model.sections)
        physical.emplace_back("PBAR", s.id);
    for (const auto& b : model.beams)
        physical.emplace_back("CBAR", b.id);
    for (const auto& f : model.forces)
        physical.emplace_back("FORCE", f.id);
    for (const auto& c : model.constraints)
        physical.emplace_back("SPC1", c.id);
    std::sort(physical.begin(), physical.end());
    for (const auto& [kind, id] : physical) {
        const auto p = preferred.find(id);
        if (p != preferred.end() && p->second.first == kind && p->second.second > 0 &&
            p->second.second <= max_number && used[kind].insert(p->second.second).second)
            reserved[id] = p->second.second;
    }
    auto assign = [&](const EntityId& id, const std::string& kind) {
        std::uint64_t n = 0;
        const auto p = reserved.find(id);
        if (p != reserved.end())
            n = p->second;
        if (!n) {
            n = 1;
            while (used[kind].count(n))
                ++n;
        }
        used[kind].insert(n);
        plan.identities.push_back({id, kind, n});
        return n;
    };
    std::map<EntityId, std::uint64_t> ids;
    for (const auto& n : model.nodes)
        ids[n.id] = assign(n.id, "GRID");
    for (const auto& m : model.materials)
        ids[m.id] = assign(m.id, "MAT1");
    for (const auto& s : model.sections)
        ids[s.id] = assign(s.id, "PBAR");
    for (const auto& b : model.beams)
        ids[b.id] = assign(b.id, "CBAR");
    for (const auto& f : model.forces)
        ids[f.id] = assign(f.id, "FORCE");
    for (const auto& c : model.constraints)
        ids[c.id] = assign(c.id, "SPC1");
    auto lookup = [&](const EntityId& id) { return ids.at(id); };
    std::map<std::string, std::vector<std::string>> lines;
    for (const auto& inc : model.includes)
        lines[inc.path];
    lines[plan.root_resource];
    auto add = [&](const EntityId& id, const std::string& line) {
        lines[owner.count(id) ? owner.at(id) : plan.root_resource].push_back(line);
    };
    for (const auto& n : model.nodes)
        add(n.id,
            "GRID," + std::to_string(lookup(n.id)) + ",," + real_text(n.position.x) + "," +
                real_text(n.position.y) + "," + real_text(n.position.z));
    for (const auto& m : model.materials)
        add(m.id,
            "MAT1," + std::to_string(lookup(m.id)) + "," + real_text(m.young_modulus_mpa) + ",," +
                real_text(*m.poisson_ratio));
    for (const auto& s : model.sections)
        add(s.id,
            "PBAR," + std::to_string(lookup(s.id)) + "," + std::to_string(lookup(s.material)) +
                "," + real_text(s.area_mm2) + "," + real_text(s.i1_mm4) + "," +
                real_text(s.i2_mm4) + "," + real_text(s.torsion_mm4));
    for (const auto& b : model.beams)
        add(b.id,
            "CBAR," + std::to_string(lookup(b.id)) + "," + std::to_string(lookup(b.section)) + "," +
                std::to_string(lookup(b.nodes[0])) + "," + std::to_string(lookup(b.nodes[1])) +
                "," + real_text(b.orientation.x, true) + "," + real_text(b.orientation.y) + "," +
                real_text(b.orientation.z));
    for (const auto& f : model.forces)
        add(f.id,
            "FORCE," + std::to_string(lookup(f.id)) + "," + std::to_string(lookup(f.node)) +
                ",0,1.0," + real_text(f.force_n.x) + "," + real_text(f.force_n.y) + "," +
                real_text(f.force_n.z));
    for (const auto& c : model.constraints) {
        if (c.nodes.size() > 6) {
            issue(out.report.issues,
                  "unsupported_format",
                  "SPC1 exceeds one free-field physical line");
            continue;
        }
        std::string row = "SPC1," + std::to_string(lookup(c.id)) + "," + c.dofs;
        for (const auto& node : c.nodes)
            row += "," + std::to_string(lookup(node));
        add(c.id, row);
    }
    if (blocked(out.report.issues))
        return out;
    for (const auto& inc : model.includes) {
        if (!inc.parent)
            continue;
        const auto p = include_paths.find(*inc.parent);
        if (p == include_paths.end()) {
            issue(out.report.issues, "include_parent", "include parent missing", inc.path);
            continue;
        }
        const auto rel = std::filesystem::path(inc.path)
                             .lexically_relative(std::filesystem::path(p->second).parent_path())
                             .generic_string();
        if (!safe_path(rel)) {
            issue(out.report.issues,
                  "include_path",
                  "include path cannot be represented safely",
                  inc.path);
            continue;
        }
        if (rel.size() + 10 > 72) {
            issue(out.report.issues,
                  "unsupported_format",
                  "INCLUDE statement exceeds 72 characters",
                  inc.path);
            continue;
        }
        lines[p->second].insert(lines[p->second].begin(), "INCLUDE '" + rel + "'");
    }
    if (blocked(out.report.issues))
        return out;
    lines[plan.root_resource].insert(
        lines[plan.root_resource].begin(),
        {"SOL 101",
         "CEND",
         "SUBCASE 1",
         "SPC = " + std::to_string(lookup(model.constraints.front().id)),
         "LOAD = " + std::to_string(lookup(model.forces.front().id)),
         "DISPLACEMENT = ALL",
         "SPCFORCES = ALL",
         "BEGIN BULK"});
    lines[plan.root_resource].push_back("ENDDATA");
    for (const auto& [path, body] : lines) {
        std::string text;
        for (const auto& line : body) {
            if (line.size() > max_line)
                issue(out.report.issues, "resource_limit", "export line exceeds limit", path);
            text += line + "\n";
        }
        plan.resources.push_back({path, text});
    }
    std::size_t total_bytes = 0;
    for (const auto& resource : plan.resources)
        total_bytes += resource.text.size();
    if (plan.resources.size() > max_resources || total_bytes > max_bytes)
        issue(out.report.issues, "resource_limit", "export bundle exceeds limits");
    if (blocked(out.report.issues))
        return out;
    if (!model.parts.empty() || !model.assemblies.empty() || !model.sets.empty())
        issue(out.report.issues,
              "organization_loss",
              "parts, assemblies, and sets are not represented in BDF",
              {},
              0,
              false);
    if (std::any_of(model.materials.begin(),
                    model.materials.end(),
                    [](const auto& m) { return !m.name.empty(); }) ||
        std::any_of(model.sections.begin(),
                    model.sections.end(),
                    [](const auto& s) { return !s.name.empty(); }) ||
        !analysis->name.empty())
        issue(out.report.issues,
              "name_loss",
              "platform names are not represented in BDF",
              {},
              0,
              false);
    issue(out.report.issues,
          "lexical_normalization",
          "comments and original numeric formatting are normalized",
          {},
          0,
          false);
    out.artifact = std::move(plan);
    out.report.complete = true;
    return out;
}
} // namespace qcae
