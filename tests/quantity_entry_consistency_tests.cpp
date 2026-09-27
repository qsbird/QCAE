#include "qcae/core.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace qcae;
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    check(result.ok(), "Expected a successful application result");
    return std::move(*result.value);
}
void accepted(Quantity quantity, double expected) {
    MemoryApplication app;
    const Caller caller{"quantity-consistency"};
    const auto initial = good(app.create_document(caller, "Units", "create"));
    const WriteContext context{initial.document, initial.revision};
    const auto preview = good(app.preview(caller, context, CreateMaterial{"Steel", quantity}));
    check(preview.normalized_modulus_mpa == expected, "Canonical pressure differs");
    check(good(app.current_document()).revision == initial.revision,
          "Preview must not publish a model revision");
    good(app.commit(caller, context, preview.id, "commit"));
    const auto snapshot = good(app.snapshot(initial.document));
    check(snapshot.materials.size() == 1 &&
              snapshot.materials.front().young_modulus_mpa == expected,
          "Commit must retain the canonical material value");
    check(snapshot.info.revision == initial.revision + 1, "Exactly one material commit");
}
void rejected(Quantity quantity, ErrorCode code, Status status, const char* field) {
    MemoryApplication app;
    const Caller caller{"quantity-consistency"};
    const auto initial = good(app.create_document(caller, "Units", "create"));
    const auto result = app.preview(
        caller, {initial.document, initial.revision}, CreateMaterial{"Steel", quantity});
    check(!result.ok() && result.error && result.error->code == code && result.status == status &&
              result.error->field == field,
          "Rejected quantity must retain its structured diagnostic");
    const auto snapshot = good(app.snapshot(initial.document));
    check(snapshot.materials.empty() && snapshot.info.revision == initial.revision &&
              !snapshot.info.dirty,
          "Rejected quantity must leave the model and revision unchanged");
}
} // namespace
int main() {
    try {
        accepted({210, "GPa"}, 210000);
        accepted({210000000, "kPa"}, 210000);
        accepted({210000000000, "Pa"}, 210000);
        accepted({210000, "MPa"}, 210000);
        accepted({std::numeric_limits<double>::min(), "MPa"}, std::numeric_limits<double>::min());
        accepted({std::numeric_limits<double>::max(), "MPa"}, std::numeric_limits<double>::max());
        rejected({1e-307, "Pa"}, ErrorCode::invalid_input, Status::failed, "young_modulus.value");
        rejected({std::numeric_limits<double>::min() / 2, "GPa"},
                 ErrorCode::invalid_input,
                 Status::failed,
                 "young_modulus.value");
        rejected({std::numeric_limits<double>::max(), "GPa"},
                 ErrorCode::invalid_input,
                 Status::failed,
                 "young_modulus.value");
        for (const double value : {0.0, -1.0, std::numeric_limits<double>::infinity()})
            rejected(
                {value, "MPa"}, ErrorCode::invalid_input, Status::failed, "young_modulus.value");
        rejected({210000, ""}, ErrorCode::missing_input, Status::needs_input, "young_modulus.unit");
        rejected({210000, "N"}, ErrorCode::invalid_unit, Status::failed, "young_modulus.unit");
        std::cout << "PASS: legacy material entry uses shared pressure normalization and numeric "
                     "bounds\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
