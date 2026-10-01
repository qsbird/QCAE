#include "sdk_copy_widgets.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTreeWidget>
#include <QtGui/private/qtextdocument_p.h>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
const qcae::SdkCopyObservation& component(const qcae::SdkCopySnapshot& snapshot,
                                          std::string_view name) {
    const auto found = std::find_if(snapshot.components.begin(),
                                    snapshot.components.end(),
                                    [name](const auto& value) { return value.component == name; });
    if (found == snapshot.components.end())
        throw std::runtime_error("Missing actual observer component");
    return *found;
}
QImage paint(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}
void text(QPlainTextEdit& edit) {
    edit.resize(400, 240);
    edit.setReadOnly(true);
    edit.appendPlainText(QStringLiteral("Revision 8"));
    edit.appendPlainText(QStringLiteral("Steel E = 200000 MPa"));
    edit.appendPlainText(QStringLiteral("Unicode: 梁 αβ"));
}
void untouchedOffscreenLayout(const std::shared_ptr<qcae::sdk_copy::Tracker>& copies) {
    QPlainTextEdit original;
    qcae::sdk_copy::ObservedPlainTextEdit observed(copies, nullptr);
    QString contents;
    for (int index = 0; index < 300; ++index) {
        if (index)
            contents += u'\n';
        contents += QStringLiteral("Offscreen block %1").arg(index);
    }
    for (auto* edit : {&original, static_cast<QPlainTextEdit*>(&observed)}) {
        edit->resize(400, 80);
        edit->setReadOnly(true);
        edit->setPlainText(contents);
    }
    check(paint(original) == paint(observed), "The observer changed initial long-document pixels");
    auto original_block = original.document()->findBlockByNumber(250);
    auto observed_block = observed.document()->findBlockByNumber(250);
    check(original_block.isValid() && observed_block.isValid(),
          "The offscreen layout fixture is incomplete");
    // Both controls have completed identical initial layout and geometry work.
    // Reset only the remote block, then inspect its existing pointer through
    // this exact-version test-only SDK view. Calling block.layout() here would
    // create the cache and invalidate the causal guard. Public clearLayout()
    // retains the QTextLayout object; use the owned-pointer destruction from
    // QTextBlockData::free, preserving all other block state and user data.
    for (const auto& block : {original_block, observed_block}) {
        const auto* data = QTextDocumentPrivate::block(block);
        delete data->layout;
        data->layout = nullptr;
    }
    check(QTextDocumentPrivate::block(original_block)->layout == nullptr &&
              QTextDocumentPrivate::block(observed_block)->layout == nullptr,
          "The offscreen layout guard did not start with empty caches");
    const auto original_pixels = paint(original);
    check(QTextDocumentPrivate::block(original_block)->layout == nullptr,
          "The base paint traversed the selected offscreen block");
    const auto observed_pixels = paint(observed);
    check(QTextDocumentPrivate::block(observed_block)->layout == nullptr,
          "The observer initialized a layout that the base paint did not visit");
    check(original_pixels == observed_pixels && original.toPlainText() == observed.toPlainText(),
          "The offscreen guard changed document contents or rendered pixels");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace qcae::sdk_copy;
    try {
        auto copies = std::make_shared<Tracker>();
        QPlainTextEdit original;
        ObservedPlainTextEdit observed(copies, nullptr);
        text(original);
        text(observed);
        check(copies->snapshot().run_id.empty(),
              "An inactive observer manufactured a measured run");
        const auto operation = std::make_shared<qcae::ledger::OperationLedger>(
            qcae::ledger::Identity{"qt-widget-copy-probe", {}, {}, 8});
        qcae::ledger::activate(operation);
        original.appendPlainText(QStringLiteral("Revision 9"));
        observed.appendPlainText(QStringLiteral("Revision 9"));
        check(original.toPlainText() == observed.toPlainText() &&
                  original.document()->characterCount() == observed.document()->characterCount(),
              "The public text observer changed real document contents");
        check(paint(original) == paint(observed),
              "The public text observer changed rendered pixels");
        const auto measured = copies->snapshot();
        check(measured.run_id == "qt-widget-copy-probe" &&
                  component(measured, "qt_log_paint").calls > 0 &&
                  component(measured, "qt_log_block_layout").calls > 0 &&
                  !component(measured, "qt_log_paint_block_text").copy_bytes.has_value() &&
                  !component(measured, "qt_font_glyph_buffers").copy_bytes.has_value(),
              "Actual text hooks lost calls, guarded bounds, or unsupported font coverage");
        untouchedOffscreenLayout(copies);

        QTreeWidget plain_tree;
        QTreeWidget observed_tree;
        auto tree_copies = std::make_shared<Tracker>();
        observed_tree.setItemDelegate(new ObservedTreeDelegate(tree_copies, &observed_tree));
        for (auto* tree : {&plain_tree, &observed_tree}) {
            tree->resize(400, 240);
            tree->setHeaderLabel(QStringLiteral("Entities"));
            new QTreeWidgetItem(tree, {QStringLiteral("Steel")});
            new QTreeWidgetItem(tree, {QStringLiteral("梁 αβ")});
        }
        check(paint(plain_tree) == paint(observed_tree),
              "The public tree observer changed rendered pixels");
        const auto tree_measured = tree_copies->snapshot();
        check(component(tree_measured, "qt_tree_paint").calls > 0 &&
                  component(tree_measured, "qt_tree_style_option").copy_bytes == 0 &&
                  !component(tree_measured, "qt_font_glyph_buffers").copy_bytes.has_value(),
              "Actual tree hooks lost shared display proof or unknown font work");
        observed_tree.topLevelItem(0)->setText(0, QStringLiteral("Steel\nnext line"));
        (void)paint(observed_tree);
        check(!component(tree_copies->snapshot(), "qt_tree_style_option").copy_bytes.has_value(),
              "Newline display detachment incorrectly retained shared-string zero coverage");
        qcae::ledger::activate({});
        check(copies->snapshot().run_id == "qt-widget-copy-probe",
              "A read-only snapshot did not retain its completed operation");
        std::cout
            << "PASS: actual text/tree calls, original pixels, Unicode, inactive observer, "
               "untouched offscreen layout, and honest paint/newline/font unsupported coverage\n";
        return 0;
    } catch (const std::exception& error) {
        qcae::ledger::activate({});
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
