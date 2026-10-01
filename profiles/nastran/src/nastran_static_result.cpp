#include "qcae/nastran_static_result.hpp"
#include <charconv>
#include <cmath>
#include <map>
#include <set>

namespace qcae {
namespace {
enum class Table { none, displacement, reaction };
Result<NastranStaticResult> bad(ErrorCode code, const char* message, const char* field) {
    return {code == ErrorCode::missing_input ? Status::needs_input : Status::failed,
            {},
            Diagnostic{code, message, field}};
}
bool space(char value) {
    return value == ' ' || value == '\t' || value == '\r';
}
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        while (!line.empty() && space(line.front()))
            line.remove_prefix(1);
        const auto end = line.find_first_of(" \t\r");
        if (line.empty())
            break;
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
std::optional<std::uint64_t> number(std::string_view value) {
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result
               ? std::optional(result)
               : std::nullopt;
}
std::optional<double> component(std::string_view value) {
    std::string normalized(value);
    for (char& digit : normalized)
        if (digit == 'd' || digit == 'D')
            digit = 'E';
    std::string_view token(normalized);
    if (!token.empty() && token.front() == '+')
        token.remove_prefix(1);
    double result{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
    return !token.empty() && parsed.ec == std::errc{} &&
                   parsed.ptr == token.data() + token.size() && std::isfinite(result)
               ? std::optional(result)
               : std::nullopt;
}
} // namespace

Result<NastranStaticResult> read_nastran_static_f06(std::string_view text,
                                                    const NastranStaticReadContext& context,
                                                    std::span<const ExportIdentifier> identities) {
    if (!context.subcase || context.unit_system.empty() || context.coordinate_basis.empty())
        return bad(ErrorCode::missing_input,
                   "Explicit subcase, units and coordinates are required",
                   "context");
    if (context.unit_system != "mm-N-MPa")
        return bad(ErrorCode::invalid_unit,
                   "The controlled reader requires mm-N-MPa input",
                   "unit_system");
    if (context.coordinate_basis != "basic")
        return bad(ErrorCode::unsupported_capability,
                   "Non-basic nodal coordinates are unsupported",
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
    Table table = Table::none;
    bool header{}, subcase_seen{}, case_active{}, job_finished{};
    std::set<std::uint64_t> displacements, reactions;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.size() > 4096)
            return bad(ErrorCode::invalid_input, "F06 line exceeds the controlled limit", "text");
        // ASA '0' is a print-control prefix on recognized metadata, never a GRID ID.
        if (line.size() > 1 && line.front() == '0' && space(line[1])) {
            const auto metadata = compact(line.substr(1));
            if (metadata == "DISPLACEMENTVECTOR" || metadata == "FORCESOFSINGLE-POINTCONSTRAINT" ||
                metadata == "POINTID.TYPET1T2T3R1R2R3" || metadata.starts_with("SUBCASE"))
                line.remove_prefix(1);
        }
        const auto squeezed = compact(line);
        const auto fields = words(line);
        if (squeezed.find("FATALMESSAGE") != squeezed.npos)
            return bad(
                ErrorCode::invalid_input, "F06 contains a solver fatal message", "execution");
        if (squeezed.find("SUBCASE") != squeezed.npos) {
            if (fields.empty() || fields[0] != "SUBCASE") {
                // Numbered input echoes cannot establish result provenance.
                table = Table::none;
                header = false;
                case_active = false;
                continue;
            }
            if (job_finished || (fields.size() != 2 && !(fields.size() == 3 && fields[1] == "=")))
                return bad(ErrorCode::invalid_input, "F06 subcase header is malformed", "subcase");
            const auto subcase = number(fields.back());
            if (!subcase || *subcase != context.subcase)
                return bad(ErrorCode::invalid_input,
                           "F06 subcase differs from the frozen case",
                           "subcase");
            subcase_seen = true;
            case_active = true;
            table = Table::none;
            header = false;
            continue;
        }
        if (squeezed == "DISPLACEMENTVECTOR" || squeezed == "FORCESOFSINGLE-POINTCONSTRAINT") {
            if (!case_active || job_finished)
                return bad(ErrorCode::invalid_input,
                           "A result table lacks explicit subcase provenance",
                           "subcase");
            table = squeezed == "DISPLACEMENTVECTOR" ? Table::displacement : Table::reaction;
            header = false;
            continue;
        }
        if (squeezed.find("POINTID.") != squeezed.npos) {
            if (table == Table::none || squeezed != "POINTID.TYPET1T2T3R1R2R3")
                return bad(ErrorCode::unsupported_capability,
                           "The nodal result header is unsupported",
                           "components");
            header = true;
            continue;
        }
        if (squeezed.find("PAGE") != squeezed.npos || squeezed.find("ENDOFJOB") != squeezed.npos) {
            table = Table::none;
            header = false;
            case_active = false;
            job_finished = job_finished || squeezed.find("ENDOFJOB") != squeezed.npos;
            continue;
        }
        if (fields.empty())
            continue;
        if (table == Table::none) {
            if (fields.size() > 1 && (fields[1] == "G" || fields[1] == "S" || fields[1] == "H"))
                return bad(ErrorCode::invalid_input,
                           "A nodal row lacks its recognized table and case header",
                           "values");
            continue;
        }
        // Blank and prose lines do not create values. Numeric table rows are strict.
        const auto initial = fields.front().front();
        const bool is_row = (initial >= '0' && initial <= '9') || initial == '+' ||
                            initial == '-' || (fields.size() > 1 && fields[1] == "G");
        if (!is_row) {
            // An unrecognized title/prose ends the active table. Its columns or
            // subsequent rows cannot be merged into displacement/SPC values.
            table = Table::none;
            header = false;
            continue;
        }
        if (!header || fields.size() != 8 || fields[1] != "G")
            return bad(ErrorCode::invalid_input,
                       "The nodal result row is truncated or unsupported",
                       "values");
        const auto id = number(fields[0]);
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
    }
    if (!subcase_seen || displacements.size() != nodes.size() || reactions.empty())
        return bad(ErrorCode::missing_input,
                   "Complete GRID displacement and SPC reaction tables are required",
                   "values");
    return {Status::success, std::move(result), {}};
}
} // namespace qcae
