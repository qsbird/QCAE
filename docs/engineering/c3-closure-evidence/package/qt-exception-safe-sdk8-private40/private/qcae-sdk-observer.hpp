#pragma once
#include "/Users/qs/Documents/ChatGPT/QCAE/build-c3-qt-observed/tranche2/qtbase-everywhere-src-6.11.1/qcae-sdk-observer.hpp"

#if !defined(QT_BOOTSTRAPPED)
extern "C" {
Q_CORE_EXPORT std::uint64_t qcae_qt_sdk_observer_status() noexcept;
Q_CORE_EXPORT unsigned qcae_qt_sdk_observer_depth() noexcept;
Q_CORE_EXPORT void qcae_qt_sdk_observer_collector_loss() noexcept;
}
#else
inline std::uint64_t qcae_qt_sdk_observer_status() noexcept { return 0; }
inline unsigned qcae_qt_sdk_observer_depth() noexcept { return 0; }
inline void qcae_qt_sdk_observer_collector_loss() noexcept {}
#endif
// The original C declarations and legacy scope layout are unchanged.
// SDK8's implementations fence every callback; these entry/exit wrappers are
// explicitly noexcept without conflicting with reused SDK7 declarations.
struct QcaeQtSdkNoexceptScope {
    explicit QcaeQtSdkNoexceptScope(const char *site) noexcept { qcae_qt_sdk_observer_enter(site); }
    ~QcaeQtSdkNoexceptScope() noexcept { qcae_qt_sdk_observer_leave(); }
    QcaeQtSdkNoexceptScope(const QcaeQtSdkNoexceptScope &) = delete;
    QcaeQtSdkNoexceptScope &operator=(const QcaeQtSdkNoexceptScope &) = delete;
};
