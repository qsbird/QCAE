#include "qcae/nastran_static_result.hpp"
#include <charconv>
#include <cmath>
#include <map>
#include <set>

namespace qcae {
namespace {
enum class Table { none, displacement, reaction };
enum class Stage { none, basis, columns, coordinate_label, rows, summary };

Result<NastranStaticResult> bad(ErrorCode code, const char* message, const char* field) {
    return {code == ErrorCode::missing_input ? Status::needs_input : Status::failed,
            {},
            Diagnostic{code, message, field}};
}
bool space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\f';
}
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        while (!line.empty() && space(line.front()))
            line.remove_prefix(1);
        if (line.empty())
            break;
        const auto end = line.find_first_of(" \t\r\f");
        result.push_back(line.substr(0, end));
        if (end == std::string_view::npos)
            break;
        line.remove_prefix(end);
    }
    return result;
}
std::string compact(std::string_view line) {
    std::string result;
    for (char value : line)
        if (!space(value))
            result.push_back(value >= 'a' && value <= 'z' ? value - 'a' + 'A' : value);
    return result;
}
std::optional<std::uint64_t> integer(std::string_view token) {
    std::uint64_t result{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
    return !token.empty() && parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()
               ? std::optional(result)
               : std::nullopt;
}
std::optional<double> component(std::string_view token) {
    std::string normalized(token);
    for (char& value : normalized)
        if (value == 'd' || value == 'D')
            value = 'E';
    token = normalized;
    if (!token.empty() && token.front() == '+')
        token.remove_prefix(1);
    double result{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
    return !token.empty() && parsed.ec == std::errc{} &&
                   parsed.ptr == token.data() + token.size() && std::isfinite(result)
               ? std::optional(result)
               : std::nullopt;
}
bool numeric_start(std::string_view token) {
    return !token.empty() && ((token.front() >= '0' && token.front() <= '9') ||
                              token.front() == '+' || token.front() == '-');
}
bool solver_error(std::string_view squeezed) {
    if (squeezed.find("FATAL") != squeezed.npos)
        return true;
    // The successful solver also prints an informational EPSILON ERROR ESTIMATE.
    // Only diagnostic ERROR markers are execution failures.
    while (!squeezed.empty() && squeezed.front() == '*')
        squeezed.remove_prefix(1);
    return squeezed.starts_with("ERROR");
}
} // namespace

Result<NastranStaticResult> read_mystran_static_f06(std::string_view text,
                                                    const NastranStaticReadContext& context,
                                                    std::span<const ExportIdentifier> identities) {
    if (!context.subcase || context.unit_system.empty() || context.coordinate_basis.empty())
        return bad(ErrorCode::missing_input,
                   "Explicit subcase, units and coordinates are required",
                   "context");
    if (context.subcase != 1)
        return bad(ErrorCode::unsupported_capability,
                   "The controlled MYSTRAN reader supports only subcase 1",
                   "subcase");
    if (context.unit_system != "mm-N-MPa")
        return bad(ErrorCode::invalid_unit,
                   "The controlled MYSTRAN reader requires mm-N-MPa input",
                   "unit_system");
    if (context.coordinate_basis != "basic")
        return bad(ErrorCode::unsupported_capability,
                   "Non-basic MYSTRAN nodal coordinates are unsupported",
                   "coordinate_basis");
    if (text.empty() || text.size() > 64 * 1024 * 1024 || text.find('\0') != text.npos)
        return bad(
            ErrorCode::invalid_input, "F06 text is empty, oversized or contains NUL", "text");
    std::map<std::uint64_t, EntityId> nodes;
    std::set<EntityId> entities;
    for (const auto& identity : identities)
        if (identity.name_space == "GRID") {
            if (!identity.number || identity.entity.value.empty() ||
                !nodes.emplace(identity.number, identity.entity).second ||
                !entities.insert(identity.entity).second)
                return bad(
                    ErrorCode::invalid_input, "The frozen GRID mapping is ambiguous", "identities");
        }
    if (nodes.empty())
        return bad(ErrorCode::missing_input, "The frozen GRID mapping is missing", "identities");

    NastranStaticResult result;
    result.subcase = context.subcase;
    result.coordinate_basis = context.coordinate_basis;
    result.reader_version = "qcae.mystran.static-f06.v1";
    Table table{Table::none};
    Stage stage{Stage::none};
    bool version_seen{}, output_seen{}, case_header{}, job_finished{};
    std::size_t segment_rows{};
    std::set<std::uint64_t> displacements, reactions;
    const auto complete_segment = [&] {
        return table == Table::none ||
               ((stage == Stage::rows || stage == Stage::summary) && segment_rows);
    };

    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.size() > 4096)
            return bad(ErrorCode::invalid_input, "F06 line exceeds the controlled limit", "text");
        const auto squeezed = compact(line);
        const auto fields = words(line);
        if (solver_error(squeezed))
            return bad(ErrorCode::invalid_input,
                       "MYSTRAN output contains a solver error or fatal diagnostic",
                       "execution");
        if (fields.empty())
            continue;
        if (job_finished)
            return bad(ErrorCode::invalid_input,
                       "MYSTRAN output continues after its completion marker",
                       "execution");
        if (squeezed.starts_with("MSCNASTRAN"))
            return bad(ErrorCode::unsupported_capability,
                       "MYSTRAN output contains a conflicting solver banner",
                       "solver_version");
        if (fields[0] == "MYSTRAN" && fields.size() >= 2 && fields[1] == "Version") {
            if (fields.size() < 3 || fields[2] != "19.0.0" || version_seen || output_seen)
                return bad(ErrorCode::unsupported_capability,
                           "The result requires the MYSTRAN 19.0.0 banner",
                           "solver_version");
            version_seen = true;
            continue;
        }
        if (squeezed.starts_with("OUTPUTFORSUBCASE")) {
            if (!version_seen || !complete_segment() || case_header || fields.size() != 4 ||
                fields[0] != "OUTPUT" || fields[1] != "FOR" || fields[2] != "SUBCASE" ||
                integer(fields[3]) != context.subcase)
                return bad(ErrorCode::invalid_input,
                           "MYSTRAN output subcase is missing, malformed or differs from the run",
                           "subcase");
            output_seen = true;
            case_header = true;
            table = Table::none;
            stage = Stage::none;
            continue;
        }
        if (squeezed == "DISPLACEMENTS" || squeezed == "SPCFORCES") {
            if (!case_header || table != Table::none)
                return bad(ErrorCode::invalid_input,
                           "A MYSTRAN table lacks explicit output subcase provenance",
                           "subcase");
            case_header = false;
            table = squeezed == "DISPLACEMENTS" ? Table::displacement : Table::reaction;
            stage = Stage::basis;
            segment_rows = 0;
            continue;
        }
        if (squeezed == "***ENDOFJOB***" || squeezed == "ENDOFJOB") {
            if (!complete_segment() || case_header)
                return bad(ErrorCode::invalid_input,
                           "MYSTRAN output ends inside an incomplete result table",
                           "values");
            job_finished = true;
            table = Table::none;
            stage = Stage::none;
            continue;
        }
        if (table == Table::none) {
            if (case_header || squeezed.starts_with("GRIDCOORD") ||
                (output_seen && numeric_start(fields[0])))
                return bad(ErrorCode::invalid_input,
                           "An unsupported MYSTRAN table or unscoped nodal row was found",
                           "values");
            continue;
        }
        if (stage == Stage::basis) {
            if (squeezed != "(INGLOBALCOORDINATESYSTEMATEACHGRID)")
                return bad(ErrorCode::unsupported_capability,
                           "The MYSTRAN result coordinate declaration is unsupported",
                           "coordinate_basis");
            stage = Stage::columns;
            continue;
        }
        if (stage == Stage::columns) {
            if (squeezed != "GRIDCOORDT1T2T3R1R2R3")
                return bad(ErrorCode::unsupported_capability,
                           "The MYSTRAN nodal component header is unsupported",
                           "components");
            stage = Stage::coordinate_label;
            continue;
        }
        if (stage == Stage::coordinate_label) {
            if (squeezed != "SYS")
                return bad(ErrorCode::invalid_input,
                           "The MYSTRAN coordinate-system column label is missing",
                           "components");
            stage = Stage::rows;
            continue;
        }
        if (squeezed == ">>LINK9END") {
            if (!complete_segment())
                return bad(ErrorCode::invalid_input, "A MYSTRAN table has no values", "values");
            table = Table::none;
            stage = Stage::none;
            continue;
        }
        if (squeezed.find_first_not_of('-') == squeezed.npos) {
            if (!segment_rows)
                return bad(ErrorCode::invalid_input, "A MYSTRAN table has no values", "values");
            stage = Stage::summary;
            continue;
        }
        if (stage == Stage::summary) {
            if (squeezed == "*FOROUTPUTSET")
                continue;
            if (fields.size() != 8 ||
                (fields[0] != "MAX*" && fields[0] != "MIN*" && fields[0] != "ABS*") ||
                fields[1] != ":")
                return bad(ErrorCode::invalid_input,
                           "The MYSTRAN table summary is malformed or contains extra rows",
                           "values");
            for (std::size_t index = 2; index < fields.size(); ++index)
                if (!component(fields[index]))
                    return bad(ErrorCode::invalid_input,
                               "A MYSTRAN summary component is malformed or nonfinite",
                               "values");
            continue;
        }
        if (fields.size() != 8)
            return bad(ErrorCode::invalid_input,
                       "The MYSTRAN nodal row is truncated or unsupported",
                       "values");
        const auto id = integer(fields[0]);
        const auto coordinate = integer(fields[1]);
        if (!coordinate || *coordinate != 0)
            return bad(ErrorCode::unsupported_capability,
                       "The MYSTRAN nodal row is not in basic coordinates",
                       "coordinate_basis");
        auto& seen = table == Table::displacement ? displacements : reactions;
        if (!id || !nodes.contains(*id) || !seen.insert(*id).second)
            return bad(ErrorCode::invalid_input,
                       "Result GRID is absent or duplicated in the frozen map",
                       "solver_number");
        NastranStaticGridValues row{nodes.at(*id), *id, {}};
        for (std::size_t index = 0; index < row.components.size(); ++index) {
            const auto value = component(fields[index + 2]);
            if (!value)
                return bad(ErrorCode::invalid_input,
                           "Result component is malformed or nonfinite",
                           "values");
            row.components[index] = *value;
        }
        auto& rows = table == Table::displacement ? result.displacements : result.spc_reactions;
        rows.push_back(std::move(row));
        ++segment_rows;
    }
    if (!version_seen || !output_seen || !job_finished || displacements.size() != nodes.size() ||
        reactions.empty())
        return bad(ErrorCode::missing_input,
                   "Complete MYSTRAN GRID displacement, SPC reaction and completion records are "
                   "required",
                   "values");
    return {Status::success, std::move(result), {}};
}
} // namespace qcae
