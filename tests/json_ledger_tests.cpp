#include "qcae/json_ledger.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::optional<std::uint64_t> metric(const qcae::ledger::OperationLedger& operation,
                                    qcae::ledger::Stage stage,
                                    qcae::ledger::Metric metric) {
    return operation.snapshot()
        .values[static_cast<std::size_t>(stage)][static_cast<std::size_t>(metric)];
}
auto observe(const char* id) {
    auto operation =
        std::make_shared<qcae::ledger::OperationLedger>(qcae::ledger::Identity{id, {}, {}, 0});
    qcae::ledger::activate(operation);
    return operation;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace qcae;
    using namespace transport::json_ledger;
    try {
        std::uint64_t parsed = 123;
        check(unsigned_decimal(QLatin1StringView("18446744073709551615"), parsed) &&
                  parsed == std::numeric_limits<std::uint64_t>::max() &&
                  unsigned_decimal(QStringView(u"00042"), parsed) && parsed == 42 &&
                  unsigned_decimal(QUtf8StringView("0"), parsed) && parsed == 0,
              "Borrowed decimal parsing changed uint64 or leading-zero behavior");
        for (const auto text : {"", "+1", "-0", " 1", "1 ", "1.0", "1e2", "18446744073709551616"}) {
            parsed = 123;
            check(!unsigned_decimal(QLatin1StringView(text), parsed) && parsed == 123,
                  "Invalid decimal strings must fail without changing the previous result");
        }
        check(!unsigned_decimal(QStringView(u"\u0661"), parsed),
              "Non-ASCII digits cannot become a wire revision");
        const std::array<QByteArray, 16> frames{QByteArray("{}"),
                                                QByteArray("{\"a\":1,\"b\":true,\"c\":null}"),
                                                QByteArray("{\"a\":[1.5,2e20,-0,{},[]]}"),
                                                QByteArray("{\"a\":1,\"a\":2}"),
                                                QByteArray("{\"a\":\"\\u4e2d\\ud83d\\ude00\"}"),
                                                QByteArray("{\"a\":\"\xe4\xb8\xad\"}"),
                                                QByteArray(" {\"a\":2} \r"),
                                                QByteArray("\xef\xbb\xbf{}"),
                                                QByteArray("{\"a\":\"\\n\\u0000\"}"),
                                                QByteArray("{\"a\":1}{}"),
                                                QByteArray("{\"a\":NaN}"),
                                                QByteArray("{\"a\":1,}"),
                                                QByteArray("{\"a\":\"\xff\"}"),
                                                QByteArray("[]"),
                                                QByteArray("null"),
                                                QByteArray("1")};
        for (const auto& bytes : frames) {
            QJsonParseError original_error, view_error;
            const auto original = QJsonDocument::fromJson(bytes, &original_error);
            const auto borrowed = QJsonValue::fromJson(QByteArrayView(bytes), &view_error);
            const bool original_valid =
                original_error.error == QJsonParseError::NoError && original.isObject();
            const bool borrowed_valid =
                view_error.error == QJsonParseError::NoError && borrowed.isObject();
            check(original_valid == borrowed_valid &&
                      (!original_valid || original.object() == borrowed.toObject()),
                  "Borrowed parsing changed object-frame acceptance or owned fields");
            if (bytes.startsWith('{') || bytes.startsWith(' '))
                check(original_error.error == view_error.error &&
                          original_error.offset == view_error.offset,
                      "Borrowed parsing changed object-frame parse diagnostics");
        }
        auto input = frames[1];
        const auto owned = QJsonValue::fromJson(QByteArrayView(input)).toObject();
        input.fill('x');
        check(owned.value("a").toInt() == 1,
              "Parsed fields retained invalid references into their input buffer");
        const auto compatible = [](const QJsonObject& object, bool expected_fast) {
            bool fast = !expected_fast;
            const auto result = compact_frame(object, &fast);
            check(result == QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n',
                  "Borrowed ASCII encoding changed the complete protocol bytes");
            check(fast == expected_fast, "Unsupported values did not fall back as a whole frame");
        };
        compatible({}, true);
        compatible(
            {{"ascii", " !#$%&'()*+,-./0123456789:;<=>?@[]^_`{|}~"},
             {"array", QJsonArray{true, false, QJsonValue::Null, 1, -2, "text", QJsonObject{}}}},
            true);
        compatible({{"nested", QJsonObject{{"a", 1}, {"z", "ASCII"}}}, {"empty", ""}}, true);
        compatible({{"ascii", 1}, {"unicode", QString::fromUtf8("\xe4\xb8\xad")}}, false);
        compatible({{QString::fromUtf8("\xc3\xa9"), "ASCII"}}, false);
        for (const auto escaped : {"quote\"", "slash\\", "line\n", "tab\t", "\x1f"})
            compatible({{"a", 1}, {"b", escaped}}, false);
        compatible({{"nul", QString(QChar(0))}}, false);
        compatible({{"fraction", 0.3}}, false);
        compatible({{"infinity", std::numeric_limits<double>::infinity()}}, false);
        compatible({{"nan", std::numeric_limits<double>::quiet_NaN()}}, false);
        compatible({{"too_large", std::numeric_limits<qint64>::max()}}, false);
        for (const auto raw : {"0",
                               "-0",
                               "1",
                               "1.0",
                               "1e6",
                               "-123456",
                               "1e15",
                               "1e16",
                               "9007199254740991",
                               "-9007199254740991"}) {
            const auto value = QJsonValue::fromJson(QByteArrayView(raw));
            const auto number = value.toDouble();
            const bool safe = std::isfinite(number) && std::trunc(number) == number &&
                              number >= -9007199254740991.0 && number <= 9007199254740991.0;
            const bool same = safe && value.toJson(QJsonValue::JsonFormat::Compact) ==
                                          QByteArray::number(static_cast<qint64>(number));
            compatible({{"number", value}}, same);
        }
        QJsonObject deep{{"value", "leaf"}};
        for (unsigned i = 0; i < 66; ++i)
            deep = {{"nested", deep}};
        compatible(deep, false);
        const auto normalized =
            QJsonValue::fromJson(QByteArrayView("{\"z\":1,\"a\":2,\"a\":3}")).toObject();
        compatible(normalized, true);
        // A successful string-only writer owns just its single terminal
        // buffer. It cannot charge the old QString/escaped UTF-8 temporaries.
        const QJsonObject large_ascii{{"payload", QString(4096, QLatin1Char('x'))}};
        const auto ascii_operation = observe("borrowed-ascii-output");
        bool fast{};
        const auto ascii_bytes = compact_frame(large_ascii, &fast);
        check(fast &&
                  metric(*ascii_operation,
                         ledger::Stage::socket_send,
                         ledger::Metric::model_copy_bytes) ==
                      static_cast<std::uint64_t>(ascii_bytes.size()) &&
                  metric(*ascii_operation,
                         ledger::Stage::socket_send,
                         ledger::Metric::library_internal_copy_bytes) == 0 &&
                  metric(*ascii_operation,
                         ledger::Stage::socket_send,
                         ledger::Metric::encoded_bytes) ==
                      static_cast<std::uint64_t>(ascii_bytes.size()),
              "A borrowed string frame omitted or duplicated its actual output writes");
        const auto numeric_operation = observe("actual-numeric-probe");
        const auto numeric_bytes = compact_frame({{"number", 123}}, &fast);
        check(fast &&
                  metric(*numeric_operation,
                         ledger::Stage::socket_send,
                         ledger::Metric::model_copy_bytes)
                          .value_or(0) > static_cast<std::uint64_t>(numeric_bytes.size()) &&
                  metric(*numeric_operation,
                         ledger::Stage::socket_send,
                         ledger::Metric::library_internal_copy_bytes)
                          .value_or(0) >= 3,
              "The actual numeric Qt probe or stack digit writes disappeared from the ledger");
        const auto fallback_operation = observe("whole-frame-fallback");
        const auto fallback_bytes = compact_frame({{"a", 1}, {"fraction", 0.3}}, &fast);
        check(!fast && metric(*fallback_operation,
                              ledger::Stage::socket_send,
                              ledger::Metric::model_copy_bytes)
                               .value_or(0) > static_cast<std::uint64_t>(fallback_bytes.size()),
              "A failed preflight lost its actual work before the original Qt fallback");
        ledger::activate({});
        const auto prefix_operation = observe("prefix-removal");
        QByteArray unique("old\nnext\n");
        const auto* start = unique.constData();
        remove_prefix(unique, 4);
        check(unique == "next\n" && unique.constData() == start + 4 &&
                  metric(*prefix_operation,
                         ledger::Stage::socket_receive,
                         ledger::Metric::model_copy_bytes) == 0,
              "Unique prefix removal must advance its pointer without copying payload");
        const QByteArray read_chunk("old\nnext\n");
        QByteArray shared;
        shared += read_chunk; // A null receive buffer retains a mutable readAll chunk.
        const auto shared_alias = shared;
        remove_prefix(shared, 4);
        check(shared == "next\n" && shared_alias == "old\nnext\n" && read_chunk == "old\nnext\n" &&
                  metric(*prefix_operation,
                         ledger::Stage::socket_receive,
                         ledger::Metric::model_copy_bytes) == 5,
              "Shared prefix removal must count its actual owned suffix and preserve aliases");
        remove_prefix(shared, 0);
        remove_prefix(shared, shared.size());
        check(shared.isEmpty() && metric(*prefix_operation,
                                         ledger::Stage::socket_receive,
                                         ledger::Metric::model_copy_bytes) == 5,
              "No-op/full removals cannot invent retained payload copies");
        const char raw[] = "old\nnext\n";
        auto borrowed = QByteArray::fromRawData(raw, sizeof(raw) - 1);
        remove_prefix(borrowed, 4);
        check(borrowed == "next\n" && QByteArrayView(raw, sizeof(raw) - 1) == "old\nnext\n" &&
                  metric(*prefix_operation,
                         ledger::Stage::socket_receive,
                         ledger::Metric::model_copy_bytes) == 10,
              "Raw borrowed storage must detach without changing or omitting owned suffix bytes");
        ledger::activate({});
        const auto armed = std::make_shared<ledger::OperationLedger>(
            ledger::Identity{"matched-trigger", {}, {}, 0}, "node.move");
        ledger::arm(armed);
        check(!ledger::activate_pending_for("view.update") && !ledger::current() &&
                  ledger::pending() == armed,
              "Background work activated a test-only measurement");
        check(ledger::activate_pending_for("node.move") && ledger::current() == armed &&
                  !ledger::pending(),
              "The declared business trigger did not activate its measurement");
        ledger::activate({});
        // The nested buffer already belongs to an immutable warmed cache.
        // Parent insertion must retain it rather than charge a fictitious MiB.
        const QJsonObject cached{{"bytes", QString(1024 * 1024, QLatin1Char('x'))}};
        const auto parent_operation = observe("nested-reference");
        const QJsonObject parent{{"cache", cached}, {"number", 42}};
        object(parent, {"cache", "number"});
        const auto parent_copy = metric(
            *parent_operation, ledger::Stage::socket_send, ledger::Metric::json_object_copy_bytes);
        check(parent_copy && *parent_copy >= sizeof(double) && *parent_copy < 1024,
              "Nested sharing or owned numeric payload classification is incorrect");
        check(parent.value("cache").toObject() == cached,
              "Observation changed a borrowed nested value");
        frame(ledger::Stage::socket_send,
              QJsonObject{{"operation", "entity.query"}, {"request_id", "same-request"}},
              123);
        frame(ledger::Stage::socket_receive,
              QJsonObject{{"status", "success"}, {"request_id", "same-request"}},
              456);
        const auto trace = parent_operation->snapshot();
        check(trace.frame_trace_complete && trace.frames.size() == 2 &&
                  trace.frames[0].bytes == 123 && trace.frames[1].bytes == 456 &&
                  trace.frames[1].operation == "entity.query",
              "Actual frame lengths or response opcode correlation were lost");

        const auto retained_operation = observe("retained-factory");
        const QJsonObject original{{"name", "owned model text"}, {"number", 123}};
        auto changed = original;
        ObjectCopies mutation;
        mutation.retained(changed);
        const auto revision = number(7);
        changed.insert("revision", revision);
        mutation.insert(QLatin1StringView("revision"), revision);
        check(!original.contains("revision") && changed.value("revision") == "7",
              "Mutation observation changed Qt copy-on-write semantics");
        check(metric(*retained_operation,
                     ledger::Stage::socket_send,
                     ledger::Metric::json_object_copy_bytes)
                      .value_or(0) > sizeof(double) + 16,
              "Retained owned strings/scalars were omitted");

        const auto unicode_operation = observe("unsupported-unicode");
        const QJsonObject unicode{{"name", QString::fromUtf8("\xc3\xa9")}};
        object(unicode, {"name"});
        check(!metric(*unicode_operation,
                      ledger::Stage::socket_send,
                      ledger::Metric::json_object_copy_bytes),
              "An unaudited object string path claimed known coverage");

        const auto malformed_operation = observe("unsorted-parser");
        decoding(QByteArrayView("{\"z\":1,\"a\":2}"));
        check(!metric(*malformed_operation,
                      ledger::Stage::socket_receive,
                      ledger::Metric::library_internal_copy_bytes),
              "An unaudited sorting path claimed known coverage");

        ledger::activate({});
        PlainTextAppendCopies warmed_text;
        warmed_text.append(QStringView(u"ab"), true, 1, 3);
        const auto text_operation = observe("warmed-text-prefix");
        warmed_text.append(QStringView(u"c"), false, 3, 5);
        check(metric(*text_operation,
                     ledger::Stage::socket_receive,
                     ledger::Metric::library_internal_copy_bytes) == 18,
              "A warmed document's owned UTF-16 prefix was omitted");
        warmed_text.append(QStringView(u"x"), false, 4, 6);
        check(!metric(*text_operation,
                      ledger::Stage::socket_receive,
                      ledger::Metric::library_internal_copy_bytes),
              "Unexpected document mutation must invalidate piece-table coverage");
        const auto multiline_operation = observe("unsupported-text-shape");
        PlainTextAppendCopies multiline;
        multiline.append(QStringView(u"a\nb"), true, 1, 4);
        check(!metric(*multiline_operation,
                      ledger::Stage::socket_receive,
                      ledger::Metric::library_internal_copy_bytes),
              "Unaudited multiline/layout behavior cannot claim known coverage");

        for (const auto length : {0, 1, 55, 56, 63, 64, 65, 1024}) {
            const QByteArray input(length, 'q');
            const auto operation = observe("sha256");
            const auto digest = sha256_hex(input, ledger::Stage::resource_decode);
            check(digest == QCryptographicHash::hash(input, QCryptographicHash::Sha256)
                                .toHex()
                                .toStdString(),
                  "SHA observation changed the digest across block/padding boundaries");
#if defined(QCAE_C3_QT_RFC6234_SHA256) && QCAE_C3_QT_RFC6234_SHA256
            check(metric(*operation,
                         ledger::Stage::resource_decode,
                         ledger::Metric::library_internal_copy_bytes)
                          .value_or(0) > static_cast<std::uint64_t>(length),
                  "Audited SHA context/working copies were omitted");
#else
            check(!metric(*operation,
                          ledger::Stage::resource_decode,
                          ledger::Metric::library_internal_copy_bytes),
                  "An unregistered SHA backend claimed known coverage");
#endif
        }
        ledger::activate({});
        std::cout << "PASS: shared references, retained owned payload, guarded text prefixes, "
                     "unsupported paths and unchanged SHA-256 across padding boundaries\n";
        return 0;
    } catch (const std::exception& error) {
        ledger::activate({});
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
