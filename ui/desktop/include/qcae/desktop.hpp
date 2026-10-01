#pragma once
#include "qcae/desktop_client.hpp"
#include "qcae/sdk_copy_observation.hpp"

class QMainWindow;
class QJsonObject;
namespace qcae {
// Caller owns the real desktop window; the application event loop remains caller-owned.
QMainWindow* create_desktop_window(DesktopClient::Options);
// Observe the created window without draining events or sending engine requests.
// Deferred camera/view work and tree/scene installation must also be complete.
[[nodiscard]] bool desktop_pipeline_idle(const QMainWindow&);
// Read-only failure diagnostics; this does not drain events or send requests.
[[nodiscard]] QJsonObject desktop_pipeline_diagnostics(const QMainWindow&);
[[nodiscard]] SdkCopySnapshot desktop_sdk_copy_observation(const QMainWindow&);
int run_desktop(int argc, char** argv);
} // namespace qcae
