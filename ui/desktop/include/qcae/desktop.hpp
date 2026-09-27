#pragma once
#include "qcae/desktop_client.hpp"

class QMainWindow;
namespace qcae {
// Caller owns the real desktop window; the application event loop remains caller-owned.
QMainWindow* create_desktop_window(DesktopClient::Options);
int run_desktop(int argc, char** argv);
} // namespace qcae
