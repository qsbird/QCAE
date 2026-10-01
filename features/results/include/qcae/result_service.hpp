#pragma once
#include "qcae/result_fixture.hpp"
#include "qcae/record_application.hpp"
#include <functional>

namespace qcae::features::results {
using FrozenInputResolver = std::function<Result<analysis::FrozenAnalysisInput>(
    const Caller&, const DocumentRef&, std::string_view artifact_id)>;
struct StoredFixtureResult {
    std::string id, principal, signature, artifact_id;
    ResultBundle bundle;
};
[[nodiscard]] OwnedRowHandler result_row_handler();
class FixtureResultService {
  public:
    FixtureResultService(RecordApplication&, FrozenInputResolver);
    [[nodiscard]] Result<StoredFixtureResult> read(const Caller&,
                                                   const WriteContext&,
                                                   std::string_view artifact_id,
                                                   const ResultFixture&,
                                                   const std::string& idempotency_key);
    [[nodiscard]] Result<StoredFixtureResult>
    get(const Caller&, const DocumentRef&, std::string_view result_id) const;

  private:
    RecordApplication& app_;
    FrozenInputResolver resolve_;
};
} // namespace qcae::features::results
