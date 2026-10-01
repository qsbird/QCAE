#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QPainter>
#include <QRawFont>
#include <QSaveFile>
#include <QScreen>
#include <QSysInfo>
#include <QTextLayout>
#include <QUuid>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qrawfont_p.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_MACOS
#include <mach-o/dyld.h>
#endif

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QString sha256(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString fileHash(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Could not open a required font or SDK image");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    check(digest.addData(&file), "Could not hash a required font or SDK image");
    return QString::fromLatin1(digest.result().toHex());
}
double finite(qreal value) {
    check(std::isfinite(value), "A font/layout metric was not finite");
    return value;
}
QJsonArray point(QPointF value) {
    return {finite(value.x()), finite(value.y())};
}
QJsonArray rect(QRectF value) {
    return {finite(value.x()), finite(value.y()), finite(value.width()), finite(value.height())};
}
QString engineName(QFontEngine::Type type) {
    switch (type) {
    case QFontEngine::Box:
        return QStringLiteral("Box");
    case QFontEngine::Multi:
        return QStringLiteral("Multi");
    case QFontEngine::Win:
        return QStringLiteral("Win");
    case QFontEngine::Mac:
        return QStringLiteral("Mac");
    case QFontEngine::Freetype:
        return QStringLiteral("Freetype");
    case QFontEngine::QPF1:
        return QStringLiteral("QPF1");
    case QFontEngine::QPF2:
        return QStringLiteral("QPF2");
    case QFontEngine::Proxy:
        return QStringLiteral("Proxy");
    case QFontEngine::DirectWrite:
        return QStringLiteral("DirectWrite");
    case QFontEngine::TestFontEngine:
        return QStringLiteral("TestFontEngine");
    }
    return QStringLiteral("Unrecognized");
}
QJsonObject fontIdentity(const QRawFont& font) {
    QJsonObject tables;
    for (const char* tag : {"head", "maxp", "name", "cmap", "OS/2", "hhea", "hmtx"}) {
        const auto bytes = font.fontTable(tag);
        tables.insert(
            QString::fromLatin1(tag),
            QJsonObject{{"bytes", static_cast<double>(bytes.size())}, {"sha256", sha256(bytes)}});
    }
    return {{"family", font.familyName()},
            {"style", font.styleName()},
            {"pixel_size", finite(font.pixelSize())},
            {"units_per_em", finite(font.unitsPerEm())},
            {"font_tables", tables}};
}
QJsonArray failedGlyphBitmaps(const QList<QGlyphRun>& runs) {
    QJsonArray observations;
    // Diagnostic calls occur only after the original draw is already blank.
    // They cannot warm another case or repair the failed original image.
    for (const auto& run : runs) {
        const auto font = run.rawFont();
        for (const auto glyph : run.glyphIndexes()) {
            const auto alpha = font.alphaMapForGlyph(glyph, QRawFont::PixelAntialiasing);
            const auto pixels = alpha.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            observations.append(QJsonObject{
                {"family", font.familyName()},
                {"glyph_id", static_cast<double>(glyph)},
                {"glyph_bounding_rect", rect(font.boundingRect(glyph))},
                {"alpha_map_null", alpha.isNull()},
                {"alpha_map_original_format", static_cast<int>(alpha.format())},
                {"width", pixels.width()},
                {"height", pixels.height()},
                {"canonical_format", "ARGB32_Premultiplied"},
                {"canonical_pixel_sha256",
                 sha256(QByteArrayView(reinterpret_cast<const char*>(pixels.constBits()),
                                       pixels.sizeInBytes()))}});
        }
    }
    return observations;
}
QJsonArray loadedLibraries() {
    QJsonArray images;
#ifdef Q_OS_MACOS
    // Read loader facts after shaping/rasterization; do not load another backend.
    std::vector<QString> paths;
    for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const auto path = QString::fromLocal8Bit(_dyld_get_image_name(i));
        if ((path.contains(QStringLiteral("Qt")) && path.contains(QStringLiteral(".framework"))) ||
            path.contains(QStringLiteral("libqcocoa")) ||
            path.contains(QStringLiteral("libfreetype")) ||
            path.contains(QStringLiteral("libharfbuzz")))
            paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths)
        images.append(QJsonObject{{"path", path}, {"sha256", fileHash(path)}});
#endif
    return images;
}
struct FontRequest {
    QString id;
    QFont font;
    QString source;
    QString source_hash;
};
struct TextCase {
    QString id;
    QString text;
    bool ascii;
};
FontRequest registeredFont(const QString& id, const QString& path) {
    const auto font_id = QFontDatabase::addApplicationFont(path);
    if (font_id < 0)
        throw std::runtime_error("The backend could not register required font " +
                                 path.toStdString());
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    check(!families.empty(), "The required registered font had no family");
    return {id, QFont(families.front()), path, fileHash(path)};
}
QJsonObject observeCase(const FontRequest& request,
                        const TextCase& text_case,
                        int pixel_size,
                        const QString& expected_engine) {
    auto font = request.font;
    font.setPixelSize(pixel_size);
    font.setHintingPreference(QFont::PreferDefaultHinting);
    font.setStyleStrategy(QFont::PreferDefault);
    QTextLayout layout(text_case.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    layout.setTextOption(option);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    auto line = layout.createLine();
    check(line.isValid(), "The requested text produced no layout line");
    line.setLineWidth(600);
    line.setPosition({0, 0});
    check(!layout.createLine().isValid(), "The fixed no-wrap input produced another line");
    layout.endLayout();

    QImage image(1280, 320, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(2);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        check(painter.isActive(), "The diagnostic raster image had no painter");
        painter.setPen(Qt::black);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        layout.draw(&painter, {20, 20});
    }
    QJsonArray runs, engines, glyphs, positions, clusters, fonts;
    std::uint64_t missing{}, glyph_count{}, changed_pixels{};
    bool expected_ascii_backend = true;
    const auto glyph_runs = layout.glyphRuns(-1, -1, QTextLayout::RetrieveAll);
    check(!glyph_runs.empty(), "The real layout produced no glyph runs");
    for (const auto& run : glyph_runs) {
        const auto raw_font = run.rawFont();
        check(raw_font.isValid(), "A real glyph run had an invalid raw font");
        const auto* raw_private = QRawFontPrivate::get(raw_font);
        check(raw_private && raw_private->fontEngine, "A real glyph run had no font engine");
        const auto type = raw_private->fontEngine->type();
        const auto name = engineName(type);
        if (text_case.ascii && name != expected_engine)
            expected_ascii_backend = false;
        const auto ids = run.glyphIndexes();
        const auto locations = run.positions();
        const auto indexes = run.stringIndexes();
        check(ids.size() == locations.size() && ids.size() == indexes.size(),
              "Glyph IDs, positions and source clusters had different sizes");
        QJsonArray run_ids, run_positions, run_clusters, advances;
        const auto raw_advances = raw_font.advancesForGlyphIndexes(ids);
        check(raw_advances.size() == ids.size(), "A raw font did not report every glyph advance");
        for (qsizetype i = 0; i < ids.size(); ++i) {
            run_ids.append(static_cast<double>(ids[i]));
            run_positions.append(point(locations[i]));
            run_clusters.append(static_cast<double>(indexes[i]));
            advances.append(point(raw_advances[i]));
            missing += ids[i] == 0;
            ++glyph_count;
        }
        const auto identity = fontIdentity(raw_font);
        fonts.append(identity);
        glyphs.append(run_ids);
        positions.append(run_positions);
        clusters.append(run_clusters);
        engines.append(QJsonObject{{"actual_engine", name},
                                   {"actual_type", static_cast<int>(type)},
                                   {"family", raw_font.familyName()},
                                   {"style", raw_font.styleName()}});
        runs.append(QJsonObject{{"font", identity},
                                {"glyph_ids", run_ids},
                                {"positions", run_positions},
                                {"string_indexes", run_clusters},
                                {"unshaped_raw_advances", advances},
                                {"rtl", run.isRightToLeft()},
                                {"source_string", run.sourceString()},
                                {"bounding_rect", rect(run.boundingRect())}});
    }
    check(glyph_count > 0, "The real layout produced no glyphs");
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            changed_pixels += row[x] != qRgb(255, 255, 255);
    }
    const QFontMetricsF metrics(font);
    QJsonArray scalars;
    for (const auto scalar : text_case.text.toUcs4())
        scalars.append(static_cast<double>(scalar));
    const QJsonObject input{{"font_request_id", request.id},
                            {"requested_family", font.family()},
                            {"registered_font_path", request.source},
                            {"registered_font_sha256", request.source_hash},
                            {"pixel_size", pixel_size},
                            {"text_id", text_case.id},
                            {"text", text_case.text},
                            {"text_utf8_hex", QString::fromLatin1(text_case.text.toUtf8().toHex())},
                            {"unicode_scalars", scalars},
                            {"hinting", "PreferDefaultHinting"},
                            {"font_merging", "enabled"},
                            {"wrap", "NoWrap"},
                            {"line_width", 600},
                            {"draw_origin", QJsonArray{20, 20}}};
    const QJsonObject metric_values{
        {"ascent", finite(metrics.ascent())},
        {"descent", finite(metrics.descent())},
        {"leading", finite(metrics.leading())},
        {"height", finite(metrics.height())},
        {"cap_height", finite(metrics.capHeight())},
        {"x_height", finite(metrics.xHeight())},
        {"advance", finite(metrics.horizontalAdvance(text_case.text))},
        {"bounding_rect", rect(metrics.boundingRect(text_case.text))},
        {"tight_bounding_rect", rect(metrics.tightBoundingRect(text_case.text))},
        {"line_ascent", finite(line.ascent())},
        {"line_descent", finite(line.descent())},
        {"line_leading", finite(line.leading())},
        {"line_height", finite(line.height())},
        {"line_natural_width", finite(line.naturalTextWidth())},
        {"line_horizontal_advance", finite(line.horizontalAdvance())},
        {"layout_bounding_rect", rect(layout.boundingRect())}};
    const QJsonObject pixels{
        {"format", "ARGB32_Premultiplied"},
        {"width", image.width()},
        {"height", image.height()},
        {"device_pixel_ratio", 2},
        {"bytes_per_line", image.bytesPerLine()},
        {"bytes", static_cast<double>(image.sizeInBytes())},
        {"background", "opaque white"},
        {"foreground", "opaque black"},
        {"text_antialiasing", true},
        {"changed_pixels", QString::number(changed_pixels)},
        {"sha256",
         sha256(QByteArrayView(reinterpret_cast<const char*>(image.constBits()),
                               image.sizeInBytes()))}};
    const auto failed_bitmaps = changed_pixels == 0 ? failedGlyphBitmaps(glyph_runs) : QJsonArray{};
    return {{"case_id", request.id + '/' + QString::number(pixel_size) + '/' + text_case.id},
            {"input", input},
            {"actual_engines", engines},
            {"expected_ascii_backend", expected_ascii_backend},
            {"ascii_backend_required", text_case.ascii},
            {"font_identities", fonts},
            {"glyph_ids", glyphs},
            {"positions", positions},
            {"string_indexes", clusters},
            {"glyph_runs", runs},
            {"metrics", metric_values},
            {"pixels", pixels},
            {"failure_only_post_draw_glyph_bitmaps", failed_bitmaps},
            {"glyph_count", QString::number(glyph_count)},
            {"missing_glyph_count", QString::number(missing)}};
}
QJsonObject compareReports(const QJsonObject& baseline, const QJsonObject& current) {
    check(baseline.value("schema") == current.value("schema") &&
              baseline.value("probe_completed").toBool() &&
              baseline.value("backend_verified").toBool() &&
              baseline.value("expected_engine") == QLatin1String("Mac") &&
              current.value("expected_engine") == QLatin1String("Freetype") &&
              baseline.value("platform") == current.value("platform") &&
              baseline.value("qt_version") == current.value("qt_version") &&
              baseline.value("screen") == current.value("screen") &&
              baseline.value("loaded_sdk_images") == current.value("loaded_sdk_images"),
          "The comparison baseline is incomplete or has a different Qt/platform contract");
    const auto originals = baseline.value("cases").toArray();
    const auto observed = current.value("cases").toArray();
    check(!originals.empty() && originals.size() == observed.size(),
          "The comparison does not have the exact same case domain");
    QJsonArray results;
    bool all_equal = true;
    for (qsizetype i = 0; i < originals.size(); ++i) {
        const auto before = originals[i].toObject();
        const auto after = observed[i].toObject();
        check(before.value("case_id") == after.value("case_id"),
              "The comparison case IDs/order differ");
        bool equal = true;
        QJsonObject fields;
        QJsonArray differences;
        for (const char* field : {"input",
                                  "font_identities",
                                  "glyph_ids",
                                  "positions",
                                  "string_indexes",
                                  "glyph_runs",
                                  "metrics",
                                  "pixels",
                                  "glyph_count",
                                  "missing_glyph_count"}) {
            check(before.contains(field) && after.contains(field), "A comparison field is missing");
            const bool matches = before.value(field) == after.value(field);
            fields.insert(QString::fromLatin1(field), matches);
            if (!matches)
                differences.append(QString::fromLatin1(field));
            equal &= matches;
        }
        all_equal &= equal;
        results.append(QJsonObject{{"case_id", before.value("case_id")},
                                   {"exactly_equal", equal},
                                   {"fields_equal", fields},
                                   {"different_fields", differences}});
    }
    return {{"baseline_run_id", baseline.value("run_id")},
            {"baseline_engine", baseline.value("expected_engine")},
            {"current_engine", current.value("expected_engine")},
            {"comparison", "exact values; no metric tolerance or field normalization"},
            {"case_count", observed.size()},
            {"equivalence_passed", all_equal},
            {"cases", results}};
}
void writeReport(const QString& path, const QJsonObject& report) {
    QSaveFile file(path);
    check(file.open(QIODevice::WriteOnly), "Could not create the diagnostic report");
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    check(file.write(bytes) == bytes.size() && file.commit(), "Could not preserve the report");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Independent Cocoa font backend behavior probe"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("output"), QStringLiteral("Diagnostic JSON"), "path"});
    parser.addOption(
        {QStringLiteral("expected-engine"), QStringLiteral("Mac or Freetype"), "type"});
    parser.addOption({QStringLiteral("compare"), QStringLiteral("Default backend JSON"), "path"});
    parser.addOption(
        {QStringLiteral("case"), QStringLiteral("One exact fixed case ID"), "case-id"});
    parser.process(application);
    const auto output = parser.value(QStringLiteral("output"));
    const auto expected = parser.value(QStringLiteral("expected-engine"));
    QJsonObject report{{"schema", "qcae.font-backend-behavior/1"},
                       {"run_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {"qt_version", qVersion()},
                       {"platform", QGuiApplication::platformName()},
                       {"qpa_environment", qEnvironmentVariable("QT_QPA_PLATFORM")},
                       {"os_product", QSysInfo::prettyProductName()},
                       {"os_kernel", QSysInfo::kernelVersion()},
                       {"architecture", QSysInfo::currentCpuArchitecture()},
                       {"expected_engine", expected},
                       {"requested_case", parser.value(QStringLiteral("case"))},
                       {"qt_libraries_path", QLibraryInfo::path(QLibraryInfo::LibrariesPath)},
                       {"product_default_changed", false},
                       {"whole_pipeline_owned_copy_coverage", "unknown"},
                       {"default_coretext_copy_gap_closed", false},
                       {"probe_completed", false},
                       {"backend_verified", false}};
    QJsonArray cases;
    try {
        check(!output.isEmpty() &&
                  (expected == QLatin1String("Mac") || expected == QLatin1String("Freetype")),
              "Use --output report.json --expected-engine Mac|Freetype [--compare baseline.json]");
        check(QLatin1String(qVersion()) == QLatin1String("6.11.1"),
              "Private engine type observation is bound to Qt 6.11.1");
        check(QGuiApplication::platformName() == QLatin1String("cocoa"),
              "This diagnostic requires the actual Cocoa platform");
        const auto* screen = QGuiApplication::primaryScreen();
        check(screen, "The Cocoa environment has no primary screen");
        report.insert("screen",
                      QJsonObject{{"name", screen->name()},
                                  {"geometry", rect(screen->geometry())},
                                  {"logical_dpi", finite(screen->logicalDotsPerInch())},
                                  {"physical_dpi", finite(screen->physicalDotsPerInch())},
                                  {"device_pixel_ratio", finite(screen->devicePixelRatio())}});
        const std::vector<FontRequest> fonts{
            registeredFont(QStringLiteral("SFNS"),
                           QStringLiteral("/System/Library/Fonts/SFNS.ttf")),
            registeredFont(QStringLiteral("SFNSMono"),
                           QStringLiteral("/System/Library/Fonts/SFNSMono.ttf")),
            {QStringLiteral("system_general"),
             QFontDatabase::systemFont(QFontDatabase::GeneralFont),
             QStringLiteral("QFontDatabase::GeneralFont"),
             {}},
            {QStringLiteral("system_fixed"),
             QFontDatabase::systemFont(QFontDatabase::FixedFont),
             QStringLiteral("QFontDatabase::FixedFont"),
             {}}};
        const std::vector<TextCase> texts{
            {QStringLiteral("ascii_material"), QStringLiteral("Steel E = 200000 MPa"), true},
            {QStringLiteral("ascii_revision"), QStringLiteral("Revision 8; node 1234"), true},
            {QStringLiteral("latin_combining"), QStringLiteral("caf\u00e9 A\u030a fi ffi"), false},
            {QStringLiteral("greek_cjk"), QStringLiteral("梁 αβ Ω = 12.5 mm"), false},
            {QStringLiteral("arabic"), QStringLiteral("العربية 123"), false},
            {QStringLiteral("devanagari"), QStringLiteral("नमस्ते 123"), false},
            {QStringLiteral("emoji"), QStringLiteral("Node 🙂 👩‍🔧"), false},
            {QStringLiteral("mixed_rtl"), QStringLiteral("Steel العربية αβ 梁"), false}};
        bool backend_verified = true;
        for (const auto& font : fonts)
            for (const auto pixel_size : {14, 18})
                for (const auto& text_case : texts) {
                    const auto case_id =
                        font.id + '/' + QString::number(pixel_size) + '/' + text_case.id;
                    if (parser.isSet(QStringLiteral("case")) &&
                        parser.value(QStringLiteral("case")) != case_id)
                        continue;
                    const auto observed = observeCase(font, text_case, pixel_size, expected);
                    if (observed.value("pixels").toObject().value("changed_pixels") ==
                        QLatin1String("0")) {
                        report.insert("failed_case", observed);
                        throw std::runtime_error(
                            "The actual font rendered only the white background: " +
                            case_id.toStdString());
                    }
                    backend_verified &= observed.value("expected_ascii_backend").toBool();
                    cases.append(observed);
                }
        check(!cases.empty(), "The requested case ID is outside the fixed input domain");
        report.insert("cases", cases);
        report.insert("case_count", cases.size());
        report.insert("loaded_sdk_images", loadedLibraries());
        report.insert("backend_verified", backend_verified);
        check(backend_verified,
              "Actual ASCII glyph-run engines did not match the requested backend");
        report.insert("probe_completed", true);
        if (parser.isSet(QStringLiteral("compare"))) {
            QFile file(parser.value(QStringLiteral("compare")));
            check(file.open(QIODevice::ReadOnly),
                  "The default backend comparison report is missing");
            QJsonParseError error;
            const auto bytes = file.readAll();
            const auto baseline = QJsonDocument::fromJson(bytes, &error);
            check(error.error == QJsonParseError::NoError && baseline.isObject(),
                  "The default backend comparison report is malformed");
            report.insert("baseline_report_sha256", sha256(bytes));
            report.insert("comparison", compareReports(baseline.object(), report));
        }
        writeReport(output, report);
        std::cout << "Captured " << cases.size() << " actual " << expected.toStdString()
                  << " Cocoa cases; exact comparison and remaining copy gaps are in the report\n";
        // Behavioral differences are evidence, not a failed diagnostic capture.
        return 0;
    } catch (const std::exception& error) {
        report.insert("probe_completed", false);
        report.insert("cases", cases);
        report.insert("case_count", cases.size());
        report.insert("error", QString::fromLocal8Bit(error.what()));
        if (!report.contains("loaded_sdk_images")) {
            try {
                report.insert("loaded_sdk_images", loadedLibraries());
            } catch (const std::exception& loader_error) {
                report.insert("loader_fact_error", QString::fromLocal8Bit(loader_error.what()));
            }
        }
        if (!output.isEmpty()) {
            try {
                writeReport(output, report);
            } catch (const std::exception& write_error) {
                std::cerr << "Report failure: " << write_error.what() << '\n';
            }
        }
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
