#pragma once
#include "qcae/analysis_input.hpp"
#include <array>

namespace qcae::features::results {
struct FixtureNodeValue {
    std::uint64_t solver_number{};
    std::array<double, 3> value{};
};
struct ResultFixture {
    std::string input_fingerprint;
    std::vector<ExportIdentifier> identities;
    std::string quantity, unit;
    std::vector<std::string> components;
    std::string location, coordinate_basis, case_label, source_kind;
    std::optional<std::uint64_t> frame;
    std::vector<FixtureNodeValue> values;
};
struct EntityVectorValue {
    EntityId entity;
    std::array<double, 3> value{};
};
struct ResultField {
    std::string quantity, unit;
    std::vector<std::string> components;
    std::string location, coordinate_basis, case_label;
    std::uint64_t frame{};
    std::vector<EntityVectorValue> values;
};
struct ResultBundle {
    analysis::FrozenAnalysisInput input;
    ResultField field;
    std::string source_kind{"fixture"};
    std::string reader_version{"qcae.fixture.displacement.v1"};
};
enum class ResultState { current, stale };
class IResultReader {
  public:
    virtual ~IResultReader() = default;
    [[nodiscard]] virtual Result<ResultBundle> read(const analysis::FrozenAnalysisInput&,
                                                    const ResultFixture&) const = 0;
};
// A fixture adapter only. It never labels synthetic data as an external solver run.
class FixtureResultReader final : public IResultReader {
  public:
    [[nodiscard]] Result<ResultBundle> read(const analysis::FrozenAnalysisInput&,
                                            const ResultFixture&) const override;
};
[[nodiscard]] ResultState result_state(const ResultBundle&, const DocumentView&);
[[nodiscard]] std::string encode_result_bundle(const ResultBundle&);
[[nodiscard]] ResultBundle decode_result_bundle(std::string_view);
} // namespace qcae::features::results
