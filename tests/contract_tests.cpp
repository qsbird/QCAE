#include "qcae/core.hpp"
#include "qcae/operations.hpp"
#include "qcae/profile_provider.hpp"

#include <iostream>
#include <set>
#include <stdexcept>
#include <type_traits>

static_assert(!std::is_convertible_v<qcae::EntityId, qcae::DocumentId>);
static_assert(!std::is_convertible_v<qcae::DocumentEpoch, qcae::DocumentId>);

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class TestProvider final : public qcae::IProfileProvider {
    qcae::ProfileDefinition value_{{"test-only", "1", "digest-1"}, "test", "static", false, false};
public:
    const qcae::ProfileDefinition& definition() const noexcept override { return value_; }
};
}

int main() {
    try {
        check(qcae::api_version == "1.1", "application contract version");
        std::set<std::string_view> names;
        for (const auto& descriptor : qcae::operations) {
            check(names.insert(descriptor.name).second, "duplicate descriptor");
            check(!descriptor.description.empty() && !descriptor.input_type.empty() && !descriptor.output_type.empty(),
                  "incomplete descriptor");
            if (descriptor.effect == "model_write")
                check(descriptor.requires_document && descriptor.requires_epoch && descriptor.requires_revision &&
                      descriptor.requires_idempotency_key, "unsafe mutation contract");
        }
        check(qcae::find_operation("unknown.operation") == nullptr, "unknown operation resolves");
        check(qcae::find_operation("analysis.start")->requires_profile_match, "analysis target must be explicit");
        check(qcae::find_operation("changes.commit")->target_context == "from_preview", "commit loses frozen target");
        const TestProvider provider;
        auto original = provider.definition().reference;
        auto changed = original;
        changed.definition_digest = "digest-2";
        check(original != changed, "profile equality ignores definition digest");
        check(!provider.definition().configured && !provider.definition().validated, "test metadata claims real solver support");
        qcae::TargetBinding a{original, "static"};
        qcae::TargetBinding b{original, "thermal"};
        check(a != b, "target equality ignores analysis semantics");
        std::cout << "contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
