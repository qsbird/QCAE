#include "shortcut_preferences.hpp"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSettings>

namespace qcae {
namespace {
bool valid(const QKeySequence& sequence) {
    for (int index = 0; index < sequence.count(); ++index) {
        const auto key = sequence[index].key();
        if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift ||
            key == Qt::Key_Alt || key == Qt::Key_Meta || key == 0)
            return false;
    }
    return sequence.count() <= 4;
}
} // namespace
void restore_action_shortcut(QAction& action, const QKeySequence& default_shortcut) {
    QSettings settings("QCAE", "Desktop");
    const auto saved = settings.value("shortcuts/" + action.objectName());
    const auto sequence =
        saved.isValid() ? QKeySequence::fromString(saved.toString(), QKeySequence::PortableText)
                        : default_shortcut;
    action.setShortcut(valid(sequence) ? sequence : default_shortcut);
}
void show_shortcut_preferences(QWidget& window) {
    auto* dialog = new QDialog(&window);
    dialog->setObjectName("shortcutPreferencesForm");
    dialog->setWindowTitle("Keyboard shortcuts");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QFormLayout(dialog);
    auto* actions = new QComboBox(dialog);
    actions->setObjectName("shortcutAction");
    const auto candidates = window.findChildren<QAction*>();
    for (auto* action : candidates)
        if (!action->objectName().isEmpty() && !action->text().isEmpty())
            actions->addItem(action->text(), action->objectName());
    auto* sequence = new QKeySequenceEdit(dialog);
    sequence->setObjectName("shortcutSequence");
    auto* status =
        new QLabel("Choose an action and record its shortcut. Empty clears a binding.", dialog);
    status->setObjectName("shortcutStatus");
    status->setWordWrap(true);
    auto* apply = new QPushButton("Apply shortcut", dialog);
    apply->setObjectName("shortcutApply");
    layout->addRow("Action", actions);
    layout->addRow("Shortcut", sequence);
    layout->addRow(status);
    layout->addRow(apply);
    const auto selected = [actions, candidates] {
        for (auto* action : candidates)
            if (action->objectName() == actions->currentData())
                return action;
        return static_cast<QAction*>(nullptr);
    };
    const auto load = [selected, sequence] {
        if (const auto* action = selected())
            sequence->setKeySequence(action->shortcut());
    };
    QObject::connect(actions, &QComboBox::currentIndexChanged, dialog, load);
    QObject::connect(
        apply, &QPushButton::clicked, dialog, [selected, sequence, status, candidates] {
            auto* action = selected();
            if (!action)
                return;
            const auto shortcut = sequence->keySequence();
            if (!valid(shortcut)) {
                status->setText("Invalid shortcut: record a key with optional modifiers.");
                return;
            }
            for (const auto* candidate : candidates)
                if (candidate != action && !shortcut.isEmpty() &&
                    candidate->shortcut() == shortcut) {
                    status->setText("Shortcut already belongs to " + candidate->text());
                    return;
                }
            action->setShortcut(shortcut);
            QSettings settings("QCAE", "Desktop");
            settings.setValue("shortcuts/" + action->objectName(),
                              shortcut.toString(QKeySequence::PortableText));
            settings.sync();
            status->setText("Saved shortcut for " + action->text());
        });
    load();
    dialog->show();
}
} // namespace qcae
