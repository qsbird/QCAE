// Synthetic contract metadata provider; no Qt callback behavior is proven.
#include <cstdint>
using Callback = void (*)(void*, const char*, unsigned, std::uint64_t);
extern "C" void qcae_qt_sdk_observer_install(Callback, void*) {}
extern "C" const char* qcae_qt_sdk_observer_manifest() {
    return R"QCAEFENCE({"observer_failure_fence":{"balanced_admission_stack_capacity":1024,"callback_abi_unchanged":true,"collector_loss_symbol":"qcae_qt_sdk_observer_collector_loss","concurrent_install_or_context_destruction_during_callbacks":"unvalidated; forbidden by lifecycle precondition","counter_unbounded_stream":"unvalidated; finite overflow remains unknown","depth_symbol":"qcae_qt_sdk_observer_depth","failure_is_unknown":true,"noexcept_callback_boundary":true,"sticky_status_symbol":"qcae_qt_sdk_observer_status","version":1},"qt_version":"6.11.1","schema":1,"scope":"Synthetic ABI negotiation metadata only; no actual Qt callback behavior","sites":[{"site":"synthetic/contract_guard_only"}]})QCAEFENCE";
}
extern "C" std::uint64_t qcae_qt_sdk_observer_status() noexcept {
    return 0;
}
extern "C" unsigned qcae_qt_sdk_observer_depth() noexcept {
    return 0;
}
extern "C" void qcae_qt_sdk_observer_collector_loss() noexcept {}
