#pragma once

#include "qcae/sdk_copy_tracker.hpp"

#include <QAbstractItemModel>
#include <QPaintEvent>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextLayout>
#include <limits>

namespace qcae::sdk_copy {

inline bool auditedQtTextVersion() {
    return QT_VERSION == QT_VERSION_CHECK(6, 11, 1) && std::string_view(qVersion()) == "6.11.1";
}

class ObservedTreeDelegate : public QStyledItemDelegate {
  public:
    ObservedTreeDelegate(std::shared_ptr<Tracker> copies, QObject* parent)
        : QStyledItemDelegate(parent), copies_(std::move(copies)) {}

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        copies_->record(Component::tree_paint, ledger::Stage::socket_receive, 0);
        copies_->unsupported(
            Component::font_glyph_buffers,
            ledger::Stage::socket_receive,
            "native style/font shaping and paint buffers have no internal observer");
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto result = QStyledItemDelegate::sizeHint(option, index);
        copies_->record(Component::tree_size_hint, ledger::Stage::socket_receive, 0);
        copies_->unsupported(
            Component::font_glyph_buffers,
            ledger::Stage::socket_receive,
            "native style/font shaping and size buffers have no internal observer");
        return result;
    }

  protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!ledger::current())
            return;
        const auto value = index.data(Qt::DisplayRole);
        // QString display values without newlines are shared through QVariant
        // and displayText. DisplayRole replaces newlines and may detach.
        // Other conversions cannot use that proof. The downstream style/font
        // work is deliberately kept in a separate unsupported component.
        const bool shared = auditedQtTextVersion() &&
                            value.metaType() == QMetaType::fromType<QString>() &&
                            !value.toString().contains(u'\n');
        copies_->record(Component::tree_style_option,
                        ledger::Stage::socket_receive,
                        shared ? std::optional<std::uint64_t>(0) : std::nullopt,
                        "display conversion/newline detachment or unaudited Qt version");
    }

  private:
    std::shared_ptr<Tracker> copies_;
};

class ObservedPlainTextLayout : public QPlainTextDocumentLayout {
  public:
    ObservedPlainTextLayout(QTextDocument* document, std::shared_ptr<Tracker> copies)
        : QPlainTextDocumentLayout(document), copies_(std::move(copies)),
          block_count_(document->blockCount()) {}

    QRectF blockBoundingRect(const QTextBlock& block) const override {
        const bool needs_layout = block.isValid() && block.layout()->lineCount() == 0;
        if (needs_layout)
            observeBlockLayout(block);
        return QPlainTextDocumentLayout::blockBoundingRect(block);
    }

  protected:
    void documentChanged(int from, int removed, int added) override {
        const auto count = document()->blockCount();
        const auto first = document()->findBlock(from);
        const auto end = static_cast<qint64>(from) + removed + added - 1;
        const bool valid_end = end <= std::numeric_limits<int>::max();
        const auto last = valid_end
                              ? document()->findBlock(static_cast<int>(std::max<qint64>(0, end)))
                              : QTextBlock{};
        // In this exact source branch the base makes one direct layoutBlock
        // call. Its virtual boundingRect calls are observed independently.
        if (first == last && count == block_count_ && first.isValid() && first.length())
            observeBlockLayout(first);
        copies_->record(Component::log_document_change,
                        ledger::Stage::socket_receive,
                        valid_end ? std::optional<std::uint64_t>(0) : std::nullopt,
                        "document-change range overflow");
        block_count_ = count;
        QPlainTextDocumentLayout::documentChanged(from, removed, added);
    }

  private:
    void observeBlockLayout(const QTextBlock& block) const {
        const auto option = document()->defaultTextOption();
        const bool supported = auditedQtTextVersion() &&
                               !(option.flags() & (QTextOption::ShowLineAndParagraphSeparators |
                                                   QTextOption::ShowDocumentTerminator)) &&
                               block.layout()->preeditAreaPosition() == -1;
        // beginLayout invalidates, then itemize/validate obtains one fresh
        // QTextBlock::text QString. The block length includes its separator.
        copies_->record(
            Component::log_block_layout,
            ledger::Stage::socket_receive,
            supported
                ? std::optional<std::uint64_t>(
                      static_cast<std::uint64_t>(std::max(0, block.length() - 1)) * sizeof(QChar))
                : std::nullopt,
            "Qt version/preedit/document-marker layout path is not audited");
        copies_->unsupported(Component::font_glyph_buffers,
                             ledger::Stage::socket_receive,
                             "text-engine glyph and native font buffers have no internal observer");
    }

    std::shared_ptr<Tracker> copies_;
    int block_count_{};
};

class ObservedPlainTextEdit : public QPlainTextEdit {
  public:
    ObservedPlainTextEdit(std::shared_ptr<Tracker> copies, QWidget* parent)
        : QPlainTextEdit(parent), copies_(std::move(copies)) {
        document()->setDocumentLayout(new ObservedPlainTextLayout(document(), copies_));
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        if (ledger::current()) {
            copies_->record(Component::log_paint, ledger::Stage::socket_receive, 0);
            // QTextBlock::layout() lazily creates a layout. Reading preedit
            // through it for offscreen blocks changes the base paint's work.
            // The public hook cannot prove the original traversal or preedit
            // state; only the source observer can measure those copies.
            copies_->record(Component::log_paint_block_text,
                            ledger::Stage::socket_receive,
                            std::nullopt,
                            "original paint traversal/preedit has no non-mutating public observer");
            copies_->unsupported(Component::font_glyph_buffers,
                                 ledger::Stage::socket_receive,
                                 "text/glyph paint internals require a version-bound SDK observer");
        }
        QPlainTextEdit::paintEvent(event);
    }

  private:
    std::shared_ptr<Tracker> copies_;
};

} // namespace qcae::sdk_copy
