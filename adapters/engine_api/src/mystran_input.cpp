#include "mystran_input.hpp"
#include "qcae/record_registry.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace qcae::ipc::detail {
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw RecordError(ErrorCode::invalid_input, message, "solver_input");
}
std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == text.npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
std::string upper(std::string_view text) {
    std::string result(text);
    for (auto& ch : result)
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
    return result;
}
bool top_level_bdf(std::string_view name) {
    return name.size() > 4 && name.size() <= 1024 &&
           name.find_first_of("/\\:'$\r\n") == name.npos &&
           std::none_of(name.begin(),
                        name.end(),
                        [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }) &&
           name.ends_with(".bdf");
}
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    for (;;) {
        const auto comma = line.find(',');
        result.push_back(trim(line.substr(0, comma)));
        require(result.size() <= 9, "MYSTRAN input exceeds the single-line free-field subset");
        if (comma == line.npos)
            return result;
        line.remove_prefix(comma + 1);
    }
}
std::string_view field(const std::vector<std::string_view>& row, std::size_t index) {
    return index < row.size() ? row[index] : std::string_view{};
}
void integer(std::string_view value) {
    require(!value.empty() && value.size() <= 8,
            "MYSTRAN input contains an unsupported integer field");
    std::uint64_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number > 0,
            "MYSTRAN input contains an unsupported integer field");
}
double real(std::string_view value) {
    require(!value.empty() && value.size() <= 8 && value.find('.') != value.npos,
            "MYSTRAN real fields require an exact supported spelling with a decimal point");
    double number{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), number, std::chars_format::general);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                std::isfinite(number),
            "MYSTRAN real fields require an exact supported spelling with a decimal point");
    return number;
}
void real_fields(const std::vector<std::string_view>& row, std::size_t first, std::size_t last) {
    for (auto index = first; index <= last; ++index)
        if (!field(row, index).empty())
            (void)real(field(row, index));
}
void zero_coordinate(std::string_view value) {
    require(value.empty() || value == "0",
            "MYSTRAN input requires basic coordinates and no GRID flags");
}
void card(std::string_view line) {
    require(line.find(',') != line.npos && line.front() != '+' && line.front() != '*',
            "MYSTRAN input requires the codec's single-line comma-separated cards");
    const auto row = fields(line);
    const auto name = upper(row.front());
    if (name == "GRID") {
        require(row.size() >= 6, "MYSTRAN GRID is truncated");
        integer(field(row, 1));
        real_fields(row, 3, 5);
        for (const auto index : {2U, 6U, 7U, 8U})
            zero_coordinate(field(row, index));
    } else if (name == "CBAR") {
        require(row.size() >= 8 && field(row, 8).empty(), "MYSTRAN CBAR fields are unsupported");
        for (std::size_t index = 1; index <= 4; ++index)
            integer(field(row, index));
        real_fields(row, 5, 7);
    } else if (name == "MAT1") {
        require(row.size() >= 5, "MYSTRAN MAT1 is truncated");
        integer(field(row, 1));
        real_fields(row, 2, 8);
        for (std::size_t index = 5; index <= 8; ++index)
            if (!field(row, index).empty())
                require(real(field(row, index)) == 0, "MYSTRAN MAT1 extra physics is unsupported");
    } else if (name == "PBAR") {
        require(row.size() >= 7 && field(row, 8).empty(), "MYSTRAN PBAR fields are unsupported");
        integer(field(row, 1));
        integer(field(row, 2));
        real_fields(row, 3, 7);
        if (!field(row, 7).empty())
            require(real(field(row, 7)) == 0, "MYSTRAN PBAR nonstructural mass is unsupported");
    } else if (name == "FORCE") {
        require(row.size() >= 8 && field(row, 8).empty(), "MYSTRAN FORCE fields are unsupported");
        integer(field(row, 1));
        integer(field(row, 2));
        zero_coordinate(field(row, 3));
        real_fields(row, 4, 7);
    } else if (name == "SPC1") {
        require(row.size() >= 4, "MYSTRAN SPC1 is truncated");
        integer(field(row, 1));
        const auto dofs = field(row, 2);
        require(!dofs.empty() && std::all_of(dofs.begin(),
                                             dofs.end(),
                                             [](char ch) { return ch >= '1' && ch <= '6'; }),
                "MYSTRAN SPC1 component fields are unsupported");
        for (std::size_t index = 3; index < row.size(); ++index)
            integer(row[index]);
    } else {
        require(false, "MYSTRAN input contains a bulk card outside the verified codec subset");
    }
}
bool control(std::string_view line) {
    const auto normalized = upper(line);
    if (normalized == "SOL 101" || normalized == "CEND" || normalized == "SUBCASE 1" ||
        normalized == "DISPLACEMENT = ALL" || normalized == "SPCFORCES = ALL")
        return true;
    const auto equal = normalized.find('=');
    if (equal == normalized.npos)
        return false;
    const auto key = trim(std::string_view(normalized).substr(0, equal));
    if (key != "SPC" && key != "LOAD")
        return false;
    integer(trim(std::string_view(normalized).substr(equal + 1)));
    return true;
}
} // namespace

void validate_mystran_input(std::string_view root_resource,
                            std::span<const TextResource> resources) {
    require(top_level_bdf(root_resource) && !resources.empty() && resources.size() <= 128,
            "MYSTRAN input requires a top-level BDF root and bounded shared-cwd resources");
    std::set<std::string_view> names;
    std::size_t total{};
    for (const auto& resource : resources) {
        require(top_level_bdf(resource.path) && names.insert(resource.path).second,
                "MYSTRAN resources must be unique top-level BDF files in one working directory");
        require(resource.text.size() <= 1048576 - total &&
                    resource.text.find('\0') == resource.text.npos,
                "MYSTRAN input is oversized or contains NUL");
        total += resource.text.size();
    }
    require(names.contains(root_resource), "MYSTRAN root is absent from its verified resource set");
    std::size_t lines{};
    for (const auto& resource : resources) {
        const bool root = resource.path == root_resource;
        bool bulk = !root, ended = false;
        std::string_view remaining(resource.text);
        while (!remaining.empty()) {
            const auto newline = remaining.find('\n');
            auto line = remaining.substr(0, newline);
            remaining =
                newline == remaining.npos ? std::string_view{} : remaining.substr(newline + 1);
            require(++lines <= 100000 && line.size() <= 4096,
                    "MYSTRAN input exceeds the controlled record/line budget");
            line = trim(line);
            if (line.empty() || line.front() == '$')
                continue;
            require(!ended && line.find('$') == line.npos,
                    "MYSTRAN trailing data or inline comment is unsupported");
            const auto normalized = upper(line);
            if (!bulk) {
                if (normalized == "BEGIN BULK")
                    bulk = true;
                else
                    require(control(line), "MYSTRAN control statement is outside the codec subset");
                continue;
            }
            if (normalized == "ENDDATA") {
                require(root, "MYSTRAN ENDDATA must occur in the root");
                ended = true;
            } else if (normalized.starts_with("INCLUDE")) {
                const auto path = trim(line.substr(7));
                require(line.size() <= 72 && path.size() >= 3 && path.front() == '\'' &&
                            path.back() == '\'',
                        "MYSTRAN INCLUDE requires one controlled single-quoted filename");
                const auto target = path.substr(1, path.size() - 2);
                require(top_level_bdf(target) && names.contains(target),
                        "MYSTRAN INCLUDE must resolve to an existing top-level shared-cwd BDF");
            } else {
                card(line);
            }
        }
        require(!root || (bulk && ended), "MYSTRAN root lacks its complete bulk section");
    }
}
} // namespace qcae::ipc::detail
