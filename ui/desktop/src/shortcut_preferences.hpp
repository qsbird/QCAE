#pragma once
#include <QKeySequence>

class QAction;
class QWidget;
namespace qcae {
void restore_action_shortcut(QAction&, const QKeySequence& default_shortcut);
void show_shortcut_preferences(QWidget&);
} // namespace qcae
