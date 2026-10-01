#pragma once

#include "qcae/operation_ledger.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QUuid>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace qcae::transport::json_ledger {
// Source audit: qtbase v6.11.1 qjsonwriter.cpp, qjsonparser.cpp,
// qcborvalue_p.h, qbytearray.cpp and qlocale_tools.cpp. This observer
// uses empty shadow buffers solely to obtain the installed Qt growth
// capacities; no model bytes are copied into observation buffers.
// Bounds cover serializer/parser payload temporaries and growth, excluding
// the terminal JSON output/readAll/adapter copies counted by their callers.
// Construction/mutation of QJsonObject values is a separate caller boundary.
struct CopyBound {
    std::uint64_t payload{}, metadata{};
    bool known{true};
};
// Observe the actual insertion order of a freshly built JSON object/array.
// Nested containers are shared references: their construction is observed at
// their own builder boundary, never charged again by the parent object.
class ObjectCopies;
namespace detail {
struct Buffer {
    QByteArray shadow;
    std::uint64_t model_size{};
    void resize(qsizetype size, CopyBound& bound) {
        const auto old_size = shadow.size();
        const auto old_capacity = shadow.capacity();
        shadow.resize(size);
        if (shadow.capacity() != old_capacity) {
            bound.payload += model_size;
            bound.metadata += static_cast<std::uint64_t>(old_size) - model_size;
        }
    }
    void append(qsizetype size, CopyBound& bound) {
        resize(shadow.size() + size, bound);
        model_size += size;
    }
};
struct Container {
    Buffer bytes;
    QList<std::array<std::uint64_t, 2>> elements;
    std::uint64_t scalar_bytes{};
    void element(CopyBound& bound, std::uint64_t scalar = 0) {
        const auto old_size = elements.size();
        const auto old_capacity = elements.capacity();
        elements.append(std::array<std::uint64_t, 2>{});
        bound.payload += scalar;
        bound.metadata += 16 - scalar;
        if (elements.capacity() != old_capacity) {
            bound.payload += scalar_bytes;
            bound.metadata += static_cast<std::uint64_t>(old_size) * 16 - scalar_bytes;
        }
        scalar_bytes += scalar;
    }
    void string(qsizetype size, CopyBound& bound) {
        // QtCbor::ByteData is a qsizetype length followed by bytes, aligned
        // to qsizetype. addByteDataImpl performs exactly one resize+memcpy.
        const auto aligned =
            (bytes.shadow.size() + sizeof(qsizetype) - 1) & ~(sizeof(qsizetype) - 1);
        bytes.resize(aligned + sizeof(qsizetype) + size, bound);
        bytes.model_size += size;
        bound.payload += size;
        bound.metadata += sizeof(qsizetype);
        element(bound);
    }
};
class Scan {
    QByteArrayView input_;
    qsizetype offset_{};
    bool encoding_;
    Buffer output_;

    char peek() const {
        return offset_ < input_.size() ? input_[offset_] : '\0';
    }
    void whitespace() {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t')
            ++offset_;
    }
    void append(qsizetype size) {
        if (encoding_)
            output_.append(size, bound);
    }
    QByteArrayView string() {
        if (peek() != '"') {
            bound.known = false;
            return {};
        }
        const auto start = ++offset_;
        while (offset_ < input_.size() && peek() != '"') {
            const auto ch = static_cast<unsigned char>(peek());
            // Escaped/non-ASCII strings follow other parser/converter paths.
            // They remain unknown until that path has its own source audit.
            if (ch == '\\' || ch < 0x20 || ch >= 0x80) {
                bound.known = false;
                return {};
            }
            ++offset_;
        }
        if (peek() != '"') {
            bound.known = false;
            return {};
        }
        const auto value = input_.sliced(start, offset_ - start);
        ++offset_;
        if (encoding_) {
            // QCbor stringAt/toString writes a UTF-16 temporary, then
            // escapedString writes UTF-8 with a six-byte free-space guard.
            bound.payload += static_cast<std::uint64_t>(value.size()) * 2;
            Buffer escaped;
            escaped.shadow.reserve(std::max<qsizetype>(value.size(), 16));
            escaped.shadow.resize(std::max<qsizetype>(value.size(), 16));
            qsizetype cursor{};
            for (qsizetype index = 0; index < value.size(); ++index) {
                if (cursor >= escaped.shadow.size() - 6) {
                    escaped.model_size = cursor;
                    CopyBound growth;
                    escaped.resize(escaped.shadow.size() * 2, growth);
                    // Uninitialized free bytes are observer/allocation storage,
                    // not model or transaction metadata.
                    bound.payload += growth.payload;
                }
                ++cursor;
            }
            bound.payload += value.size();
        }
        return value;
    }
    void value(Container* parent, unsigned depth) {
        whitespace();
        if (!bound.known || depth > 64) {
            bound.known = false;
            return;
        }
        const auto token = peek();
        if (token == '{' || token == '[') {
            container(depth, token == '{');
            if (!encoding_ && parent)
                parent->element(bound);
            return;
        }
        if (token == '"') {
            const auto text = string();
            if (encoding_) {
                append(1);
                append(text.size());
                append(1);
            } else if (parent) {
                parent->string(text.size(), bound);
            }
            return;
        }
        const auto start = offset_;
        while (peek() != '\0' && peek() != ',' && peek() != ']' && peek() != '}' && peek() != ' ' &&
               peek() != '\n' && peek() != '\r' && peek() != '\t')
            ++offset_;
        const auto size = offset_ - start;
        if (!size) {
            bound.known = false;
            return;
        }
        if (encoding_) {
            if (token == '-' || (token >= '0' && token <= '9')) {
                // Integer: stack digit buffer plus owned QByteArray. Double:
                // dtoString's shortest buffer is max_digits10+1 and the owned
                // result is reserved once (source asserts no reallocations).
                bound.payload +=
                    static_cast<std::uint64_t>(size) +
                    std::max<std::uint64_t>(size, std::numeric_limits<double>::max_digits10 + 1);
            }
            append(size);
        } else if (parent) {
            const auto scalar = token == '-' || (token >= '0' && token <= '9') ? 8 : 4;
            // Numeric/bool values are owned scalar payload, unlike byte offsets
            // and nested container pointers. Count the temporary plus append.
            bound.payload += scalar;
            parent->element(bound, scalar);
        }
    }
    void container(unsigned depth, bool object) {
        ++offset_;
        append(1);
        Container parsed;
        QByteArrayView previous_key;
        std::size_t members{};
        whitespace();
        const auto closing = object ? '}' : ']';
        while (bound.known && peek() != closing && peek() != '\0') {
            if (object) {
                const auto key = string();
                if (members && previous_key >= key)
                    bound.known = false;
                previous_key = key;
                if (!encoding_)
                    parsed.string(key.size(), bound);
                whitespace();
                if (peek() != ':') {
                    bound.known = false;
                    return;
                }
                ++offset_;
                append(1);
                append(key.size());
                append(2);
            }
            value(&parsed, depth + 1);
            ++members;
            whitespace();
            if (peek() != ',')
                break;
            ++offset_;
            append(1);
            whitespace();
        }
        if (peek() != closing) {
            bound.known = false;
            return;
        }
        ++offset_;
        append(1);
        if (!encoding_ && object && members > 1) {
#ifdef _LIBCPP_VERSION
            // KeyIterator::value_type is two 16-byte trivially copied Elements.
            // libc++ stable_sort uses insertion sort up to 128 sorted members:
            // no relocation. Each comparator can create two 32-byte Values and
            // two 16-byte keys. customAssigningUniqueLast adjacent comparison
            // adds the same bound; duplicate/unsorted keys are rejected above.
            if (members <= 128)
                bound.metadata += 2 * (members - 1) * (2 * 32 + 2 * 16);
            else
                bound.known = false;
#else
            bound.known = false;
#endif
        }
    }

  public:
    CopyBound bound;
    Scan(QByteArrayView input, bool encoding, qsizetype root_elements)
        : input_(input), encoding_(encoding) {
        if (encoding_)
            output_.shadow.reserve(root_elements);
        value(nullptr, 0);
        if (encoding_ && peek() == '\n') {
            ++offset_;
            append(1);
        }
        whitespace();
        if (offset_ != input_.size())
            bound.known = false;
    }
};
inline bool supported() noexcept {
    return QT_VERSION == QT_VERSION_CHECK(6, 11, 1) && std::strcmp(qVersion(), "6.11.1") == 0;
}
inline void record(ledger::Stage stage, const CopyBound& bound) {
    if (bound.known) {
        ledger::add(stage, ledger::Metric::library_internal_copy_bytes, bound.payload);
        ledger::add(stage, ledger::Metric::reference_descriptor_copy_bytes, bound.metadata);
    } else {
        ledger::unknown(stage, ledger::Metric::library_internal_copy_bytes);
    }
}
} // namespace detail

class ObjectCopies {
    ledger::Stage stage_;
    detail::Container container_;
    bool active_{};
    bool retained_{};

    static qsizetype ascii_length(QAnyStringView text, CopyBound& bound) {
        return text.visit([&](auto view) -> qsizetype {
            for (const auto character : view) {
                const auto value = [&] {
                    if constexpr (std::is_same_v<std::decay_t<decltype(character)>, QChar>)
                        return character.unicode();
                    else
                        return static_cast<unsigned char>(character);
                }();
                if (value >= 0x80) {
                    bound.known = false;
                    return 0;
                }
            }
            return view.size();
        });
    }
    void element(CopyBound& bound, bool insertion = true, std::uint64_t scalar = 0) {
        // QList::insert relocates at most all prior 16-byte Elements, in
        // addition to any capacity relocation already observed by element().
        // replaceAt_internal/fromJsonValue copy one Element and QCborValue.
        if (insertion) {
            bound.payload += container_.scalar_bytes;
            bound.metadata += container_.elements.size() * 16 - container_.scalar_bytes;
        }
        bound.payload += scalar * 2;
        bound.metadata += 16 + 32 - scalar * 2;
        container_.element(bound, scalar);
    }
    void bytes(qsizetype size, CopyBound& bound) {
        const auto aligned =
            (container_.bytes.shadow.size() + sizeof(qsizetype) - 1) & ~(sizeof(qsizetype) - 1);
        if (retained_) {
            // Fresh retained containers can have a different insertion order
            // and capacity. Charge the entire direct prefix for each possible
            // relocation rather than infer its private Qt capacity.
            bound.payload += container_.bytes.model_size;
            bound.metadata += container_.bytes.shadow.size() - container_.bytes.model_size;
            CopyBound ignored;
            container_.bytes.resize(aligned + sizeof(qsizetype) + size, ignored);
        } else
            container_.bytes.resize(aligned + sizeof(qsizetype) + size, bound);
        container_.bytes.model_size += size;
        bound.payload += size;
        bound.metadata += sizeof(qsizetype);
    }
    void value(const QJsonValue& value, bool new_string, CopyBound& bound, bool insertion = true) {
        if (value.isString()) {
            const auto size = ascii_length(value.toStringView(), bound);
            if (new_string) {
                // QString -> QCborValue owns a fresh ASCII buffer once.
                bound.payload += size;
                bound.metadata += sizeof(qsizetype) + 16;
            }
            // replaceAt_complex copies the source QCbor bytes into this object.
            bytes(size, bound);
        }
        const auto scalar = value.isDouble() ? 8 : value.isBool() ? 4 : 0;
        element(bound, insertion, scalar);
    }
    void record(const CopyBound& bound) const {
        if (bound.known && detail::supported()) {
            ledger::add(stage_, ledger::Metric::json_object_copy_bytes, bound.payload);
            ledger::add(stage_, ledger::Metric::reference_descriptor_copy_bytes, bound.metadata);
        } else {
            ledger::unknown(stage_, ledger::Metric::json_object_copy_bytes);
        }
    }

  public:
    explicit ObjectCopies(ledger::Stage stage = ledger::Stage::socket_send)
        : stage_(stage), active_(bool(ledger::current())) {}
    void insert(QAnyStringView key,
                const QJsonValue& member,
                bool new_string = true,
                bool new_key = true,
                bool key_at_end = false) {
        if (!active_)
            return;
        CopyBound bound;
        const auto size = ascii_length(key, bound);
        if (new_key)
            bound.payload += size * 2; // initializer's owned UTF-16 QString key
        // insertAt(key) constructs a temporary QCborValue, then inserts it.
        bound.payload += size;
        bound.metadata += sizeof(qsizetype) + 16;
        bytes(size, bound);
        element(bound, !key_at_end);
        value(member, new_string, bound, !key_at_end);
        record(bound);
    }
    void append(const QJsonValue& member, bool new_string = true) {
        if (!active_)
            return;
        CopyBound bound;
        value(member, new_string, bound, false);
        record(bound);
    }
    // A fresh factory object may be borrowed by the caller before mutation.
    // Its direct byte data detaches once; nested containers retain references.
    // Seed only shape/capacity here, without recharging the original builder.
    void retained(const QJsonObject& object) {
        if (!active_)
            return;
        CopyBound bound;
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const auto key_size = ascii_length(it.keyView(), bound);
            const auto value = it.value();
            for (const auto size :
                 {key_size,
                  value.isString() ? ascii_length(value.toStringView(), bound) : qsizetype(-1)}) {
                if (size < 0)
                    continue;
                const auto aligned = (container_.bytes.shadow.size() + sizeof(qsizetype) - 1) &
                                     ~(sizeof(qsizetype) - 1);
                container_.bytes.shadow.resize(aligned + sizeof(qsizetype) + size);
                container_.bytes.model_size += size;
                bound.payload += size;
                bound.metadata += sizeof(qsizetype);
            }
            const auto scalar = value.isDouble() ? 8 : value.isBool() ? 4 : 0;
            container_.elements.append(std::array<std::uint64_t, 2>{});
            container_.elements.append(std::array<std::uint64_t, 2>{});
            container_.scalar_bytes += scalar;
            bound.payload += scalar;
            bound.metadata += 32 - scalar;
        }
        retained_ = true;
        container_.elements.squeeze();
        record(bound);
    }
};
// Strict decimal wire integers can be checked without QString/UTF-8 digit
// temporaries. Views never retain data beyond this call.
inline bool unsigned_decimal(QAnyStringView text, std::uint64_t& result) noexcept {
    std::uint64_t value{};
    const bool valid = text.visit([&](auto view) {
        if (view.isEmpty())
            return false;
        for (const auto character : view) {
            const auto code = [&] {
                if constexpr (std::is_same_v<std::decay_t<decltype(character)>, QChar>)
                    return character.unicode();
                else
                    return static_cast<unsigned char>(character);
            }();
            if (code < '0' || code > '9')
                return false;
            const auto digit = static_cast<std::uint64_t>(code - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
                return false;
            value = value * 10 + digit;
        }
        return true;
    });
    if (valid)
        result = value;
    return valid;
}
// QJsonObject's view overloads avoid an owned QString for an existing key.
// ASCII UTF-8 equals Latin-1; other UTF-8 retains Qt's original conversion.
inline bool insert_borrowed_key(QJsonObject& object, QAnyStringView key, const QJsonValue& value) {
    return key.visit([&](auto view) {
        if constexpr (std::is_same_v<decltype(view), QUtf8StringView>) {
            const bool ascii = std::all_of(view.begin(), view.end(), [](char character) {
                return static_cast<unsigned char>(character) < 0x80;
            });
            if (ascii)
                object.insert(QLatin1StringView(view.data(), view.size()), value);
            else {
                object.insert(QString::fromUtf8(view.data(), view.size()), value);
                return true;
            }
        } else {
            object.insert(view, value);
        }
        return false;
    });
}
inline void remove_prefix(QByteArray& buffer,
                          qsizetype bytes,
                          ledger::Stage stage = ledger::Stage::socket_receive) {
    const bool changes = bytes > 0 && !buffer.isEmpty();
    const bool unique = buffer.capacity() > 0 && buffer.isDetached();
    buffer.remove(0, bytes);
    // Qt 6.11.1 QByteArray::remove/QArrayDataOps<char>::erase advance a unique
    // owned buffer's pointer (or shrink its size). Capacity also excludes raw
    // borrowed storage. Shared data copies only the retained
    // suffix into a fresh allocation. No input view may be used after mutation.
    if (!changes || detail::supported())
        ledger::add(
            stage, ledger::Metric::model_copy_bytes, changes && !unique ? buffer.size() : 0);
    else
        ledger::unknown(stage, ledger::Metric::model_copy_bytes);
}
// This overload is for call sites whose source contains one fresh initializer
// list. Preserve its written order; QJsonObject sorts its Element index, while
// owned byte data follows the insertion order.
inline void object(const QJsonObject& value,
                   std::initializer_list<const char*> insertion_order,
                   ledger::Stage stage = ledger::Stage::socket_send) {
    if (!ledger::current())
        return;
    ObjectCopies observer(stage);
    if (insertion_order.size() != static_cast<std::size_t>(value.size())) {
        ledger::unknown(stage, ledger::Metric::json_object_copy_bytes);
        return;
    }
    for (const auto* text : insertion_order) {
        const QLatin1StringView key(text);
        observer.insert(key, value.value(key));
    }
}
inline QString from_utf8(std::string_view value, ledger::Stage stage = ledger::Stage::socket_send) {
    auto output = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    ledger::add(stage, ledger::Metric::json_object_copy_bytes, output.size() * 2);
    return output;
}
inline QString uuid(ledger::Stage stage = ledger::Stage::socket_send) {
    const auto output = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // QUuid::toString writes a Latin-1 stack buffer, then owns UTF-16 data.
    // Random entropy is newly generated metadata, not copied model payload.
    if (detail::supported())
        ledger::add(stage, ledger::Metric::json_object_copy_bytes, output.size() * 3);
    else
        ledger::unknown(stage, ledger::Metric::json_object_copy_bytes);
    return output;
}
inline std::string sha256_hex(QByteArrayView bytes, ledger::Stage stage) {
    const auto output =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
#if defined(QCAE_C3_QT_RFC6234_SHA256) && QCAE_C3_QT_RFC6234_SHA256
    if (detail::supported()) {
        // Exact installed backend audit: QT_FEATURE_openssl_hash == -1.
        // Qt RFC6234 SHA256Input copies each input byte to Message_Block.
        // finalizeUnchecked copies this ABI-compatible context once. Each
        // block writes sixteen source words, copies A..H, then performs six
        // scalar assignments in each of 64 rounds. Arithmetic-generated words
        // are computation, while these assignments remain in the raw bound.
        struct Context {
            std::uint32_t digest[8], high, low;
            std::int_least16_t index;
            std::uint8_t block[64];
            int computed, corrupted;
        };
        const auto blocks =
            static_cast<std::uint64_t>(bytes.size()) / 64 + ((bytes.size() % 64) >= 56 ? 2 : 1);
        const auto bound = static_cast<std::uint64_t>(bytes.size()) + sizeof(Context) +
                           blocks * (16 * 4 + 8 * 4 + 64 * 6 * 4) + 32 + 64 + 64;
        ledger::add(stage, ledger::Metric::library_internal_copy_bytes, bound);
    } else
        ledger::unknown(stage, ledger::Metric::library_internal_copy_bytes);
#else
    ledger::unknown(stage, ledger::Metric::library_internal_copy_bytes);
#endif
    return output;
}
template <class Integer>
    requires std::is_integral_v<Integer>
inline QString number(Integer value, ledger::Stage stage = ledger::Stage::socket_send) {
    QString output;
    std::uint64_t multiplier = 4;
    if constexpr (std::is_signed_v<Integer>) {
        output = QString::number(static_cast<qlonglong>(value));
        if (value < 0)
            multiplier = 6; // sign insertion can additionally relocate digit data
    } else {
        output = QString::number(static_cast<qulonglong>(value));
    }
    // qulltoa writes UTF-16 digits on its stack, then owns one QString copy.
    // Default C-locale/base-10 formatting needs no group/padding conversion.
    if (detail::supported())
        ledger::add(stage, ledger::Metric::json_object_copy_bytes, output.size() * multiplier);
    else
        ledger::unknown(stage, ledger::Metric::json_object_copy_bytes);
    return output;
}
inline QString owned_string(QString value, ledger::Stage stage = ledger::Stage::socket_receive) {
    ledger::add(stage, ledger::Metric::library_internal_copy_bytes, value.size() * 2);
    return value;
}
inline QString string(const QJsonValue& value,
                      ledger::Stage stage = ledger::Stage::socket_receive) {
    return owned_string(value.toString(), stage);
}
// Piece-table append observation only, not font/layout/paint coverage. The
// instance follows one initially empty QTextDocument and every append, also
// outside active samples. Public character counts guard its tracked shape.
class PlainTextAppendCopies {
    std::uint64_t characters_{1}; // QTextDocument's initial paragraph separator
    bool synchronized_{true};

  public:
    void append(QStringView input,
                bool document_was_empty,
                std::uint64_t before_characters,
                std::uint64_t after_characters,
                ledger::Stage stage = ledger::Stage::socket_receive) noexcept {
        const auto separator = document_was_empty ? 0u : 1u;
        const auto length = static_cast<std::uint64_t>(input.size());
        const auto maximum = std::numeric_limits<std::uint64_t>::max() / 4;
        if (before_characters != characters_ || characters_ > maximum - separator ||
            length > maximum - characters_ - separator)
            synchronized_ = false;
        for (const auto character : input)
            if (character == u'\r' || character == u'\n' ||
                character == QChar::ParagraphSeparator || character.unicode() == 0xfdd0 ||
                character.unicode() == 0xfdd1)
                synchronized_ = false;
        if (!synchronized_ || after_characters != characters_ + separator + length ||
            !detail::supported()) {
            synchronized_ = false;
            ledger::unknown(stage, ledger::Metric::library_internal_copy_bytes);
            return;
        }
        // QWidgetTextControl::append inserts a separator for a nonempty doc;
        // QTextCursor::insertText then appends the full input. Either mutation
        // can detach/grow and relocate the entire old UTF-16 backing prefix.
        // Charge each possible prefix once; capacity is intentionally private.
        std::uint64_t units{};
        if (separator) {
            units += characters_ + 1;
            ++characters_;
        }
        if (length)
            units += characters_ + length;
        characters_ += length;
        ledger::add(stage, ledger::Metric::library_internal_copy_bytes, units * sizeof(QChar));
    }
};
inline std::string utf8(const QString& value, ledger::Stage stage = ledger::Stage::socket_receive) {
    if (ledger::current()) {
        bool ascii = true;
        for (const auto character : value)
            ascii = ascii && character.unicode() < 0x80;
        if (ascii)
            // QString::toStdString owns its UTF-8 QByteArray and std::string.
            ledger::add(stage, ledger::Metric::library_internal_copy_bytes, value.size() * 2);
        else
            ledger::unknown(stage, ledger::Metric::library_internal_copy_bytes);
    }
    return value.toStdString();
}
inline std::string utf8(QAnyStringView value, ledger::Stage stage = ledger::Stage::socket_receive) {
    return value.visit([&](auto view) {
        std::string result;
        result.reserve(view.size());
        for (const auto character : view) {
            const auto code = [&] {
                if constexpr (std::is_same_v<std::decay_t<decltype(character)>, QChar>)
                    return character.unicode();
                else
                    return static_cast<unsigned char>(character);
            }();
            if (code >= 0x80)
                return utf8(owned_string(value.toString(), stage), stage);
            result.push_back(static_cast<char>(code));
        }
        // ASCII is copied directly from a borrowed CBor view into its owned
        // std::string. No intermediate UTF-16/UTF-8 Qt allocation is needed.
        const auto sso_move = result.size() <= std::string{}.capacity() ? result.size() : 0;
        ledger::add(stage, ledger::Metric::library_internal_copy_bytes, result.size() + sso_move);
        return result;
    });
}
inline bool trimmed_empty(QAnyStringView value,
                          ledger::Stage stage = ledger::Stage::socket_receive) {
    return value.visit([&](auto view) {
        bool empty = true;
        for (const auto character : view) {
            const auto code = [&] {
                if constexpr (std::is_same_v<std::decay_t<decltype(character)>, QChar>)
                    return character.unicode();
                else
                    return static_cast<unsigned char>(character);
            }();
            if (code >= 0x80)
                return owned_string(value.toString(), stage).trimmed().isEmpty();
            empty = empty && (code == 0x20 || (code >= 9 && code <= 13));
        }
        return empty;
    });
}
inline void frame(ledger::Stage stage, const QJsonObject& object, std::uint64_t bytes) noexcept {
    if (!ledger::current())
        return;
    try {
        // Diagnostic-only copies of short identifiers belong to observation,
        // never to the business payload ledger. No permission comes from them.
        const auto text = [&](const char* key) {
            return object.value(QLatin1StringView(key)).toString().toStdString();
        };
        const auto operation = text("operation");
        const auto declared_kind = text("frame_type");
        const auto kind = operation.empty()
                              ? (declared_kind.empty() ? std::string("response") : declared_kind)
                              : std::string("request");
        ledger::frame(stage, bytes, kind, operation, text("request_id"), text("event"));
    } catch (...) {
        // A lost diagnostic attribution does not invalidate exact socket bytes.
        // Mark the trace incomplete through a safe no-allocation setter.
        if (const auto operation = ledger::current())
            operation->frame_trace_unknown();
    }
}
inline void encoding(QByteArrayView compact_frame, qsizetype root_elements) {
    if (!ledger::current())
        return;
    if (!detail::supported()) {
        ledger::unknown(ledger::Stage::socket_send, ledger::Metric::library_internal_copy_bytes);
        return;
    }
    detail::record(ledger::Stage::socket_send,
                   detail::Scan(compact_frame, true, root_elements).bound);
}
namespace detail {
// Public views retain the immutable QCbor containers. Only complete frames
// proven equal to Qt's writer reach write(); every other frame uses Qt intact.
class AsciiFrame {
    qsizetype size_{1}; // Terminal protocol newline.
    std::uint64_t copied_{}, descriptors_{};

    static unsigned character(QChar value) {
        return value.unicode();
    }
    static unsigned character(char value) {
        return static_cast<unsigned char>(value);
    }
    static unsigned character(char16_t value) {
        return value;
    }
    static bool ascii(QAnyStringView text) {
        return text.visit([](auto view) {
            return std::all_of(view.begin(), view.end(), [](auto value) {
                const auto ch = character(value);
                return ch >= 0x20 && ch < 0x80 && ch != '"' && ch != '\\';
            });
        });
    }
    bool take(qsizetype count) {
        if (count > std::numeric_limits<qsizetype>::max() - size_)
            return false;
        size_ += count;
        return true;
    }
    void value_copy(const QJsonValue& value) {
        const auto scalar = value.isDouble() ? 8 : value.isBool() ? 4 : 0;
        copied_ += scalar;
        // Conversion from QJsonValueConstRef owns only these scalar bytes;
        // strings/containers retain immutable pointers and offsets.
        descriptors_ += sizeof(QJsonValue) - scalar;
    }
    static bool integer(const QJsonValue& value, qint64& result) {
        const auto number = value.toDouble();
        constexpr double exact_limit = 9007199254740991.0;
        if (!std::isfinite(number) || std::trunc(number) != number || number < -exact_limit ||
            number > exact_limit)
            return false;
        result = static_cast<qint64>(number);
        return true;
    }
    qsizetype digits(qint64 value, std::array<char, 32>& buffer) {
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        const auto length = result.ptr - buffer.data();
        copied_ += length; // Actual stack digit writes, in both passes.
        return length;
    }
    bool text(QAnyStringView value) {
        descriptors_ += sizeof(QAnyStringView);
        return ascii(value) && take(2) && take(value.size());
    }
    bool measure(const QJsonValue& value, unsigned depth) {
        value_copy(value);
        if (depth > 64)
            return false;
        if (value.isString())
            return text(value.toStringView());
        if (value.isNull())
            return take(4);
        if (value.isBool())
            return take(value.toBool() ? 4 : 5);
        if (value.isDouble()) {
            qint64 number{};
            if (!integer(value, number))
                return false;
            std::array<char, 32> buffer;
            const auto length = digits(number, buffer);
            const auto probe = value.toJson(QJsonValue::JsonFormat::Compact);
            // The probe is a real Qt-owned temporary, including its terminal
            // writes; it is not a transmitted frame or a fictitious saving.
            ledger::add(ledger::Stage::socket_send,
                        ledger::Metric::library_internal_copy_bytes,
                        probe.size());
            encoding(probe, 16);
            return QByteArrayView(probe) == QByteArrayView(buffer.data(), length) && take(length);
        }
        if (value.isObject()) {
            const auto object = value.toObject();
            descriptors_ += sizeof(QJsonObject);
            if (!take(2))
                return false;
            bool first = true;
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                if ((!first && !take(1)) || !text(it.keyView()) || !take(1) ||
                    !measure(it.value(), depth + 1))
                    return false;
                first = false;
            }
            return true;
        }
        if (value.isArray()) {
            const auto array = value.toArray();
            descriptors_ += sizeof(QJsonArray);
            if (!take(2))
                return false;
            bool first = true;
            for (auto it = array.constBegin(); it != array.constEnd(); ++it) {
                if ((!first && !take(1)) || !measure(*it, depth + 1))
                    return false;
                first = false;
            }
            return true;
        }
        return false;
    }
    void write_text(QAnyStringView value, char*& output) {
        descriptors_ += sizeof(QAnyStringView);
        *output++ = '"';
        value.visit([&](auto view) {
            for (const auto ch : view)
                *output++ = static_cast<char>(character(ch));
        });
        *output++ = '"';
    }
    void write(const QJsonValue& value, char*& output) {
        value_copy(value);
        if (value.isString())
            write_text(value.toStringView(), output);
        else if (value.isNull() || value.isBool()) {
            const std::string_view literal = value.isNull()   ? "null"
                                             : value.toBool() ? "true"
                                                              : "false";
            std::memcpy(output, literal.data(), literal.size());
            output += literal.size();
        } else if (value.isDouble()) {
            qint64 number{};
            (void)integer(value, number); // Proven by the immutable first pass.
            std::array<char, 32> buffer;
            const auto length = digits(number, buffer);
            std::memcpy(output, buffer.data(), length);
            output += length;
        } else {
            const bool object_value = value.isObject();
            *output++ = object_value ? '{' : '[';
            bool first = true;
            if (object_value) {
                const auto object = value.toObject();
                descriptors_ += sizeof(QJsonObject);
                for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                    if (!first)
                        *output++ = ',';
                    write_text(it.keyView(), output);
                    *output++ = ':';
                    write(it.value(), output);
                    first = false;
                }
            } else {
                const auto array = value.toArray();
                descriptors_ += sizeof(QJsonArray);
                for (auto it = array.constBegin(); it != array.constEnd(); ++it) {
                    if (!first)
                        *output++ = ',';
                    write(*it, output);
                    first = false;
                }
            }
            *output++ = object_value ? '}' : ']';
        }
    }

  public:
    std::optional<QByteArray> frame(const QJsonObject& object) {
        if (!measure(object, 0))
            return {};
        QByteArray result(size_, Qt::Uninitialized);
        auto* output = result.data();
        write(object, output);
        *output = '\n';
        return result;
    }
    ~AsciiFrame() {
        ledger::add(ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, copied_);
        ledger::add(ledger::Stage::socket_send,
                    ledger::Metric::reference_descriptor_copy_bytes,
                    descriptors_);
    }
};
} // namespace detail
inline QByteArray compact_frame(const QJsonObject& object, bool* borrowed_ascii = nullptr) {
    detail::AsciiFrame writer;
    auto candidate = writer.frame(object);
    if (borrowed_ascii)
        *borrowed_ascii = candidate.has_value();
    auto bytes = candidate ? std::move(*candidate)
                           : QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (!candidate)
        encoding(bytes, object.size() * 2);
    else
        ledger::add(ledger::Stage::socket_send, ledger::Metric::library_internal_copy_bytes, 0);
    ledger::cover(ledger::Stage::socket_send);
    ledger::add(ledger::Stage::socket_send, ledger::Metric::encoded_bytes, bytes.size());
    ledger::add(ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, bytes.size());
    return bytes;
}
inline void decoding(QByteArrayView compact_frame) {
    if (!ledger::current())
        return;
    if (!detail::supported()) {
        ledger::unknown(ledger::Stage::socket_receive, ledger::Metric::library_internal_copy_bytes);
        return;
    }
    detail::record(ledger::Stage::socket_receive, detail::Scan(compact_frame, false, 0).bound);
}
inline void socket_read(qsizetype received_bytes) {
    if (!ledger::current())
        return;
#if defined(Q_OS_UNIX)
    if (detail::supported()) {
        CopyBound bound;
        // QAbstractSocketPrivate::readFromSocket reads kernel bytes directly
        // into QRingBuffer::reserve. reserve adds/reuses separate chunks and
        // does not copy old payload. The readAll destination writes are already
        // counted by the caller, so this is only the kernel→Qt buffer copy.
        bound.payload = received_bytes;
        // QLocalSocket::readData forwards to unixSocket.read. Outer QIODevice
        // readAll uses 4096-byte increments on this non-transactional stream,
        // including the final zero-byte read. Only initialized prior bytes
        // are payload; allocator free space and terminators are excluded.
        detail::Buffer result;
        qsizetype read{};
        do {
            result.model_size = read;
            CopyBound growth;
            result.resize(read + 4096, growth);
            bound.payload += growth.payload;
            if (read == received_bytes)
                break;
            read += std::min<qsizetype>(4096, received_bytes - read);
        } while (true);
        detail::record(ledger::Stage::socket_receive, bound);
        return;
    }
#endif
    ledger::unknown(ledger::Stage::socket_receive, ledger::Metric::library_internal_copy_bytes);
}
} // namespace qcae::transport::json_ledger
