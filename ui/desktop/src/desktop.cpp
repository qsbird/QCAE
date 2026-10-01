#include "qcae/desktop.hpp"
#include "modeling_tools.hpp"
#include "analysis_tools.hpp"
#include "shortcut_preferences.hpp"
#include "sdk_copy_widgets.hpp"
#include "qcae/desktop_client.hpp"
#include "qcae/local_endpoint.hpp"
#include "qcae/render_packet.hpp"
#include "qcae/render_wire.hpp"
#include "qcae/resource_client.hpp"
#include "qcae/vtk_view.hpp"
#include "qcae/operation_ledger.hpp"
#include "qcae/json_ledger.hpp"

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QScrollArea>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTimer>
#include <QTextDocument>
#include <QToolBar>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace {
using qcae::DesktopClient;
using qcae::DocumentEpoch;
using qcae::DocumentId;
using qcae::ResourceClient;
using qcae::ResourceVersion;
using qcae::Result;
namespace transport = qcae::transport;
namespace json_ledger = transport::json_ledger;
using qcae::VtkView;
using Done = std::function<void(bool)>;

QString errorText(const QJsonObject& response) {
    const auto error = response.value("error").toObject();
    return json_ledger::owned_string(json_ledger::string(error.value("code")) + ": " +
                                     json_ledger::string(error.value("message")));
}
bool succeeded(const QJsonObject& response) {
    return response.value("status") == "success";
}
bool finiteVector(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() != 3)
        return false;
    for (const auto& component : value.toArray())
        if (!component.isDouble() || !std::isfinite(component.toDouble()))
            return false;
    return true;
}
QString physicalNumber(const QJsonValue& value) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return "unavailable";
    return json_ledger::owned_string(QString::number(value.toDouble(), 'g', 12));
}
QString selectedPhysicalDetails(const QJsonObject& row) {
    const auto kind = row.value("kind").toStringView();
    if (kind == QLatin1StringView("section"))
        return json_ledger::owned_string(
            "A = " + physicalNumber(row.value("area_mm2")) +
            " mm²\nI1 = " + physicalNumber(row.value("i1_mm4")) +
            " mm⁴\nI2 = " + physicalNumber(row.value("i2_mm4")) +
            " mm⁴\nJ = " + physicalNumber(row.value("torsion_mm4")) +
            " mm⁴\nMaterial: " + json_ledger::string(row.value("material_id")));
    if (kind == QLatin1StringView("force")) {
        if (!finiteVector(row.value("force_n")))
            return "Force (global) = unavailable";
        const auto force = row.value("force_n").toArray();
        return json_ledger::owned_string("Force (global) = (" + physicalNumber(force[0]) + ", " +
                                         physicalNumber(force[1]) + ", " +
                                         physicalNumber(force[2]) +
                                         ") N\nNode: " + json_ledger::string(row.value("node_id")));
    }
    return {};
}
std::pair<QString, QJsonObject> propertyPreviewRequest(QJsonObject parameters) {
    if (parameters.value("command") == QJsonValue("force.set_vector")) {
        parameters.remove("command");
        return {"force.preview_vector", std::move(parameters)};
    }
    return {"changes.preview", std::move(parameters)};
}
QString uniqueKey() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString revisionText(const QJsonValue& value) {
    return value.isString() ? json_ledger::string(value) : json_ledger::number(value.toInteger());
}
std::optional<std::uint64_t> unsignedRevision(const QJsonValue& value) {
    if (!value.isString())
        return std::nullopt;
    const auto text = json_ledger::utf8(json_ledger::string(value));
    if (text.empty() || text.size() > 20)
        return std::nullopt;
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return result;
}
bool propertyReplyMatches(const QJsonObject& response,
                          const QString& document,
                          const QString& epoch,
                          const QString& revision) {
    const auto matches_revision = [&](const QJsonValue& value) {
        if (value.isString())
            return value.toStringView() == revision;
        // Older engine metadata used JSON numbers. Only exactly representable
        // nonnegative integers can establish a version; never truncate a value.
        if (!value.isDouble())
            return false;
        const auto number = value.toDouble();
        constexpr auto max_exact = 9007199254740991ULL;
        bool valid = false;
        const auto expected = revision.toULongLong(&valid);
        return valid && expected <= max_exact && std::isfinite(number) && number >= 0 &&
               std::trunc(number) == number && number <= static_cast<double>(max_exact) &&
               static_cast<std::uint64_t>(number) == expected;
    };
    if (!succeeded(response) || !response.value("data").isObject())
        return false;
    const auto data = response.value("data").toObject();
    // Both entity.query and preview contracts carry data.revision. An optional
    // envelope version or identity must agree rather than override that source.
    if (!matches_revision(data.value("revision")))
        return false;
    for (const auto& object : {response, data}) {
        if ((object.contains("revision") && !matches_revision(object.value("revision"))) ||
            (object.contains("document_id") && object.value("document_id") != document) ||
            (object.contains("document_epoch") && object.value("document_epoch") != epoch))
            return false;
    }
    return true;
}
bool boundedString(const QJsonValue& value, qsizetype limit, bool empty = true) {
    if (!value.isString())
        return false;
    const auto text = json_ledger::string(value);
    return text.size() <= limit && (empty || !text.isEmpty()) && !text.contains(QChar(0));
}
bool validDocumentSummary(const QJsonObject& value) {
    if (value.size() != 11 || !unsignedRevision(value.value("revision")) ||
        !value.value("dirty").isBool() || !value.value("durable").isBool() ||
        !value.value("material_count").isDouble() ||
        value.value("material_count").toInteger(-1) < 0)
        return false;
    for (const auto* key : {"document_id", "document_epoch"})
        if (!boundedString(value.value(key), 1024, false))
            return false;
    for (const auto* key : {"name", "saved_path"})
        if (!boundedString(value.value(key), 4096))
            return false;
    for (const auto* key : {"content_state", "project_id", "saved_content_state"})
        if (!boundedString(value.value(key), 1024))
            return false;
    return true;
}
bool withinJsonBudget(const QJsonValue& value, std::uint64_t& remaining, unsigned depth = 0) {
    if (depth > 64)
        return false;
    const auto take = [&remaining](std::uint64_t amount) {
        if (amount > remaining)
            return false;
        remaining -= amount;
        return true;
    };
    if (value.isString()) {
        const auto count = static_cast<std::uint64_t>(value.toStringView().size());
        return count <= remaining / 6 && take(2 + count * 6);
    }
    if (value.isDouble())
        return std::isfinite(value.toDouble()) && take(64);
    if (value.isArray()) {
        const auto array = value.toArray();
        if (!take(2 + (array.isEmpty() ? 0 : array.size() - 1)))
            return false;
        for (const auto& child : array)
            if (!withinJsonBudget(child, remaining, depth + 1))
                return false;
        return true;
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        if (!take(2 + (object.isEmpty() ? 0 : object.size() - 1)))
            return false;
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const auto size = static_cast<std::uint64_t>(it.keyView().size());
            if (size > remaining / 6 || !take(3 + size * 6) ||
                !withinJsonBudget(it.value(), remaining, depth + 1))
                return false;
        }
        return true;
    }
    return take(5);
}
bool validChangedIds(const QJsonValue& value) {
    if (!value.isArray())
        return false;
    const auto ids = value.toArray();
    if (ids.size() > 1000)
        return false;
    QSet<QString> unique;
    for (const auto& id : ids) {
        if (!boundedString(id, 1024, false))
            return false;
        const auto text = json_ledger::string(id);
        if (unique.contains(text))
            return false;
        unique.insert(text);
    }
    return true;
}
bool validChangedRows(const QJsonObject& data,
                      const QString& doc,
                      const QString& epoch,
                      const QString& revision,
                      const QString& view,
                      const QString& view_revision) {
    if (data.value("rows_complete") != QJsonValue(true) || !data.value("rows_version").isObject() ||
        !data.value("changed_rows").isArray() || !validChangedIds(data.value("changed_ids")))
        return false;
    const auto version = data.value("rows_version").toObject();
    if (version.size() != 5 || version.value("document_id") != doc ||
        version.value("document_epoch") != epoch || version.value("revision") != revision ||
        version.value("view_session_id") != view || version.value("view_revision") != view_revision)
        return false;
    const auto rows = data.value("changed_rows").toArray();
    const auto changed = data.value("changed_ids").toArray();
    if (rows.size() > 1000 || rows.size() != changed.size())
        return false;
    std::uint64_t budget = 65536;
    if (!withinJsonBudget(rows, budget))
        return false;
    QSet<QString> ids;
    for (const auto& id : changed) {
        if (!boundedString(id, 1024, false))
            return false;
        const auto text = json_ledger::string(id);
        if (ids.contains(text))
            return false;
        ids.insert(text);
    }
    for (const auto& value : rows) {
        if (!value.isObject())
            return false;
        const auto row = value.toObject();
        if (!boundedString(row.value("entity_id"), 1024, false) ||
            !boundedString(row.value("kind"), 128, false) ||
            !boundedString(row.value("name"), 4096) || !row.value("sources").isArray() ||
            (row.contains("fields") && !row.value("fields").isObject()))
            return false;
        if (!ids.remove(json_ledger::string(row.value("entity_id"))))
            return false;
        for (const auto* key : {"position_mm", "start_mm", "end_mm"})
            if (row.contains(key)) {
                const auto coordinates = row.value(key).toArray();
                if (!row.value(key).isArray() || coordinates.size() != 3)
                    return false;
                for (const auto& coordinate : coordinates)
                    if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()))
                        return false;
            }
        for (const auto& source : row.value("sources").toArray()) {
            if (!source.isObject())
                return false;
            const auto entry = source.toObject();
            const auto number = unsignedRevision(entry.value("number"));
            if (!boundedString(entry.value("source_model_id"), 1024, false) ||
                !boundedString(entry.value("namespace"), 1024, false) ||
                !boundedString(entry.value("include_id"), 1024) || !number || !*number)
                return false;
        }
    }
    return ids.isEmpty();
}
void noBinaryPayload(qcae::ledger::Stage stage) {
    for (const auto metric : {qcae::ledger::Metric::model_copy_bytes,
                              qcae::ledger::Metric::metadata_copy_bytes,
                              qcae::ledger::Metric::encoded_bytes,
                              qcae::ledger::Metric::decoded_bytes,
                              qcae::ledger::Metric::library_internal_copy_bytes})
        qcae::ledger::add(stage, metric, 0);
    qcae::ledger::cover(stage);
}
std::optional<qcae::RenderDelta> decodeAcknowledgement(const QJsonObject& value,
                                                       const ResourceVersion& expected) {
    if (value.size() != 7)
        return std::nullopt;
    const auto text = [&](const char* key) -> std::optional<std::string> {
        const auto field = value.value(QLatin1String(key));
        if (!field.isString())
            return std::nullopt;
        auto result =
            json_ledger::utf8(json_ledger::string(field, qcae::ledger::Stage::render_decode),
                              qcae::ledger::Stage::render_decode);
        if (result.empty() || result.size() > 1024 || result.find('\0') != std::string::npos)
            return std::nullopt;
        return result;
    };
    const auto number = [&](const char* key) -> std::optional<std::uint64_t> {
        const auto input = text(key);
        if (!input)
            return std::nullopt;
        std::uint64_t result{};
        const auto parsed = std::from_chars(input->data(), input->data() + input->size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != input->data() + input->size())
            return std::nullopt;
        return result;
    };
    auto doc = text("document_id"), epoch = text("document_epoch"), view = text("view_session_id");
    const auto revision = number("revision"), view_revision = number("view_revision"),
               base_revision = number("base_revision"),
               base_view_revision = number("base_view_revision");
    if (!doc || !epoch || !view || !revision || !view_revision || !base_revision ||
        !base_view_revision || *doc != expected.document.id.value ||
        *epoch != expected.document.epoch.value || *view != expected.view_session_id ||
        *revision != expected.revision || *view_revision != expected.view_revision)
        return std::nullopt;
    qcae::ledger::add(qcae::ledger::Stage::render_decode,
                      qcae::ledger::Metric::metadata_copy_bytes,
                      3 * (doc->size() + epoch->size() + view->size()) + 4 * sizeof(std::uint64_t));
    qcae::ledger::add(
        qcae::ledger::Stage::render_decode, qcae::ledger::Metric::model_copy_bytes, 0);
    qcae::ledger::add(qcae::ledger::Stage::render_decode, qcae::ledger::Metric::decoded_bytes, 0);
    qcae::ledger::cover(qcae::ledger::Stage::render_decode);
    noBinaryPayload(qcae::ledger::Stage::resource_decode);
    return qcae::RenderDelta{{DocumentId(std::move(*doc)), DocumentEpoch(std::move(*epoch))},
                             *base_revision,
                             *revision,
                             std::move(*view),
                             *base_view_revision,
                             *view_revision,
                             {},
                             {},
                             {}};
}
QString entityId(const QJsonObject& object) {
    return json_ledger::string(
        object.value(object.value("entity_id").isString() ? "entity_id" : "entity"));
}
QString entityLabel(const QJsonObject& object) {
    const auto name = json_ledger::string(object.value("name")).trimmed();
    if (!name.isEmpty())
        return name;
    auto kind = object.value("kind").isString() ? json_ledger::string(object.value("kind"))
                                                : QString("Entity");
    if (!kind.isEmpty())
        kind[0] = kind[0].toUpper();
    const auto sources = object.value("sources").toArray();
    for (const auto& value : sources) {
        const auto number = json_ledger::string(value.toObject().value("number"));
        if (!number.isEmpty())
            return json_ledger::owned_string(kind + " " + number);
    }
    const auto prefix = json_ledger::owned_string(entityId(object).left(8));
    return json_ledger::owned_string(kind + " · " + prefix);
}

std::optional<qcae::RenderPacket>
decodePacket(const QJsonObject& data, const QString& document, const QString& epoch) {
    const auto legacy_revision = [](const QJsonValue& value) -> std::optional<std::uint64_t> {
        if (value.isDouble()) {
            const auto integer = value.toInteger(-1);
            if (integer >= 0)
                return static_cast<std::uint64_t>(integer);
            return std::nullopt;
        }
        return unsignedRevision(value);
    };
    const auto revision = legacy_revision(data.value("revision"));
    const auto view_revision = legacy_revision(data.value("view_revision"));
    if (data.value("document_id") != document || data.value("document_epoch") != epoch ||
        !revision || !view_revision || !*view_revision ||
        !boundedString(data.value("view_session_id"), 1024, false))
        return std::nullopt;
    qcae::RenderPacket packet;
    packet.document = {qcae::DocumentId{document.toStdString()},
                       qcae::DocumentEpoch{epoch.toStdString()}};
    packet.revision = *revision;
    packet.view_session_id = data.value("view_session_id").toString().toStdString();
    packet.view_revision = *view_revision;
    if (packet.view_session_id.empty() || !data.value("points").isArray() ||
        !data.value("beams").isArray())
        return std::nullopt;
    QSet<QString> identities;
    const auto identity = [&identities](const QJsonObject& row) -> std::optional<QString> {
        const auto field = row.value(row.contains("entity_id") ? "entity_id" : "entity");
        if (!boundedString(field, 1024, false) ||
            (row.contains("visible") && !row.value("visible").isBool()))
            return std::nullopt;
        const auto id = json_ledger::string(field);
        if (identities.contains(id))
            return std::nullopt;
        identities.insert(id);
        return id;
    };
    for (const auto& value : data.value("points").toArray()) {
        const auto row = value.toObject();
        const auto xyz = row.value("position_mm").toArray();
        const auto id = identity(row);
        if (!id || !row.value("position_mm").isArray() || xyz.size() != 3)
            return std::nullopt;
        for (const auto& coordinate : xyz)
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()))
                return std::nullopt;
        packet.points.push_back({qcae::EntityId{id->toStdString()},
                                 {xyz[0].toDouble(), xyz[1].toDouble(), xyz[2].toDouble()},
                                 row.value("visible").toBool(true)});
    }
    for (const auto& value : data.value("beams").toArray()) {
        const auto row = value.toObject();
        const auto ends = row.value("points").toArray();
        const auto id = identity(row);
        if (!id || ends.size() != 2 || !ends[0].isDouble() || !ends[1].isDouble())
            return std::nullopt;
        const auto a = ends[0].toInteger(-1);
        const auto b = ends[1].toInteger(-1);
        if (a < 0 || b < 0 || a >= static_cast<qint64>(packet.points.size()) ||
            b >= static_cast<qint64>(packet.points.size()))
            return std::nullopt;
        packet.beams.push_back({qcae::EntityId{id->toStdString()},
                                {static_cast<std::size_t>(a), static_cast<std::size_t>(b)},
                                row.value("visible").toBool(true)});
    }
    if (data.contains("geometry_lines") && !data.value("geometry_lines").isArray())
        return std::nullopt;
    for (const auto& value : data.value("geometry_lines").toArray()) {
        const auto row = value.toObject();
        const auto start = row.value("start_mm").toArray(), end = row.value("end_mm").toArray();
        const auto id = identity(row);
        if (!id || start.size() != 3 || end.size() != 3)
            return std::nullopt;
        qcae::RenderGeometryLine line{
            qcae::EntityId{id->toStdString()}, {}, {}, row.value("visible").toBool(true)};
        for (int i = 0; i < 3; ++i) {
            if (!start[i].isDouble() || !end[i].isDouble() || !std::isfinite(start[i].toDouble()) ||
                !std::isfinite(end[i].toDouble()))
                return std::nullopt;
            line.start_mm[i] = start[i].toDouble();
            line.end_mm[i] = end[i].toDouble();
        }
        packet.geometry_lines.push_back(line);
    }
    return packet;
}

// The codec accepts a resource bundle. Resolve INCLUDEs under the selected deck directory.
bool readDeck(const QString& path,
              const QString& root,
              QSet<QString>& visited,
              QJsonArray& resources,
              QString& error) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    const QString prefix = root + QDir::separator();
    if (canonical.isEmpty() || !(canonical == root || canonical.startsWith(prefix))) {
        error = "INCLUDE escapes the selected deck directory: " + path;
        return false;
    }
    if (visited.contains(canonical))
        return true;
    if (visited.size() >= 64) {
        error = "Too many INCLUDE resources";
        return false;
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 900000) {
        error = "Cannot read resource or resource is too large: " + canonical;
        return false;
    }
    const auto bytes = file.readAll();
    if (bytes.contains('\0')) {
        error = "Binary resource is not a supported BDF text deck";
        return false;
    }
    visited.insert(canonical);
    const auto relative = QDir(root).relativeFilePath(canonical);
    resources.append(QJsonObject{{"path", relative}, {"text", QString::fromUtf8(bytes)}});
    const auto lines = QString::fromUtf8(bytes).split('\n');
    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();
        if (!trimmed.startsWith("INCLUDE", Qt::CaseInsensitive))
            continue;
        auto include = trimmed.mid(7).trimmed();
        if (include.size() < 2 || !QStringLiteral("'\"").contains(include.front()) ||
            include.back() != include.front()) {
            error = "Unsupported INCLUDE syntax: " + line;
            return false;
        }
        include = include.mid(1, include.size() - 2);
        if (include.isEmpty() || QDir::isAbsolutePath(include) ||
            !readDeck(QDir(QFileInfo(canonical).absolutePath()).filePath(include),
                      root,
                      visited,
                      resources,
                      error))
            return false;
    }
    return true;
}

class DesktopWindow : public QMainWindow {
  public:
    explicit DesktopWindow(DesktopClient::Options options, bool smoke, QString screenshot)
        : client_(std::move(options), this), resources_(client_, this), smoke_(smoke),
          screenshot_path_(std::move(screenshot)) {
        setWindowTitle("QCAE");
        resize(1400, 850);
        buildUi();
        if (!smoke_) {
            QSettings settings("QCAE", "Desktop");
            restoreGeometry(settings.value("layout/geometry").toByteArray());
            restoreState(settings.value("layout/state").toByteArray(), 1);
        }
        connect(&client_, &DesktopClient::readyChanged, this, [this](bool ready) {
            statusBar()->showMessage(ready ? "Connected to local engine"
                                           : "Connecting to local engine");
            if (ready) {
                poll_.setInterval(client_.supportsEvents() ? 2000 : 500);
                if (client_.supportsEvents())
                    subscribeEvents();
                else
                    pollCurrent();
            } else {
                ++subscription_generation_;
                subscription_pending_ = false;
                modeling_->suspend();
                analysis_tools_->suspend();
                clearDocument(false);
            }
        });
        connect(&client_, &DesktopClient::transportError, this, [this](const QString& message) {
            statusBar()->showMessage(message, 5000);
        });
        connect(&client_, &DesktopClient::eventReceived, this, [this](const QJsonObject& event) {
            const auto kind = json_ledger::string(event.value("event"));
            if (kind == "JobChanged")
                modeling_->refreshTask();
            if (kind == "HistoryChanged")
                appendLog(json_ledger::owned_string("History changed at revision " +
                                                    json_ledger::string(event.value("revision"))));
            const auto event_data = event.value("data").toObject();
            const bool rebase = kind == "DocumentChanged" && canRebaseModel();
            if (kind == "DocumentChanged" && event_data.value("resync_required") == true)
                force_full_render_ = true;
            if (kind == "DocumentChanged" &&
                (json_ledger::string(event.value("document_id")) != document_id_ ||
                 json_ledger::string(event.value("document_epoch")) != epoch_ ||
                 json_ledger::string(event.value("revision")) != revision_ ||
                 event_data.value("active") == QJsonValue(false) ||
                 event_data.value("resync_required") == QJsonValue(true))) {
                // Fence pending replies immediately; the authoritative query may still be queued.
                invalidateModelContext();
            }
            if (kind == "DocumentChanged" && client_.supportsEventsDocumentSummary() &&
                !force_full_render_ && event_data.value("resync_required") == QJsonValue(false) &&
                event_data.value("active") != QJsonValue(false) &&
                event_data.value("base_revision") == revision_ &&
                event.value("document_id") == document_id_ &&
                event.value("document_epoch") == epoch_ &&
                event_data.value("document_summary").isObject()) {
                const auto summary = event_data.value("document_summary").toObject();
                const auto next = unsignedRevision(event.value("revision"));
                const auto base = unsignedRevision(QJsonValue(revision_));
                if (validDocumentSummary(summary) && next && base &&
                    *base < std::numeric_limits<std::uint64_t>::max() && *next == *base + 1 &&
                    summary.value("document_id") == event.value("document_id") &&
                    summary.value("document_epoch") == event.value("document_epoch") &&
                    summary.value("revision") == event.value("revision")) {
                    applyDocumentInfo(summary, rebase);
                    return;
                }
            }
            if (kind == "DocumentChanged" && !context_refresh_required_)
                invalidateModelContext();
            if (kind == "DocumentChanged" || kind == "ProjectMetadataChanged")
                pollCurrent();
        });
        connect(&client_, &DesktopClient::eventGap, this, [this](const QString& reason) {
            statusBar()->showMessage(reason, 5000);
            force_full_render_ = true;
            invalidateModelContext();
            modeling_->suspend();
            analysis_tools_->suspend();
            subscribeEvents();
        });
        poll_.setInterval(500);
        connect(&poll_, &QTimer::timeout, this, [this] { pollCurrent(); });
        poll_.start();
        camera_update_.setSingleShot(true);
        camera_update_.setInterval(200);
        connect(&camera_update_, &QTimer::timeout, this, [this] { requestViewRefresh(); });
        if (!screenshot_path_.isEmpty()) {
            screenshot_retry_.setInterval(200);
            connect(&screenshot_retry_, &QTimer::timeout, this, [this] { captureSmoke(); });
            screenshot_retry_.start();
        }
        client_.start();
        if (smoke_)
            QTimer::singleShot(
                1500, this, [this] { statusBar()->showMessage("Desktop smoke ready"); });
    }

    bool pipelineIdle() const {
        if (!client_.ready() || client_.pendingRequests() || subscription_pending_ ||
            current_pending_ || current_queued_ || pending_display_callbacks_ ||
            view_update_pending_ || render_pending_ || queued_hidden_ || pending_selection_ ||
            pending_selection_evaluating_ || camera_update_.isActive() ||
            screenshot_retry_.isActive() || viewport_->pendingCameraUpdate())
            return false;
        if (document_id_.isEmpty())
            return true;
        return !context_refresh_required_ && viewportMatchesContext() &&
               property("treeDocumentId") == document_id_ &&
               property("treeDocumentEpoch") == epoch_ && property("treeRevision") == revision_;
    }

    void closeEvent(QCloseEvent* event) override {
        if (!smoke_) {
            QSettings settings("QCAE", "Desktop");
            settings.setValue("layout/geometry", saveGeometry());
            settings.setValue("layout/state", saveState(1));
        }
        QMainWindow::closeEvent(event);
    }
    [[nodiscard]] bool screenshotDone() const {
        return screenshot_done_;
    }

  private:
    void scheduleDisplayCallback(int milliseconds, std::function<void()> callback) {
        ++pending_display_callbacks_;
        QTimer::singleShot(milliseconds, this, [this, callback = std::move(callback)] {
            --pending_display_callbacks_;
            callback();
        });
    }
    void captureSmoke() {
        if (screenshot_done_ || !client_.ready() || !viewport_->hasPacket() || view_update_pending_)
            return;
        screenshot_done_ = grab().save(screenshot_path_, "PNG");
        if (screenshot_done_) {
            screenshot_retry_.stop();
            statusBar()->showMessage("Desktop smoke screenshot saved: " + screenshot_path_);
        }
    }
    void buildUi() {
        viewport_ = new VtkView(this);
        setCentralWidget(viewport_);
        connect(viewport_, &VtkView::picked, this, [this](const QStringList& ids, bool through) {
            if (viewportMatchesContext())
                validateSelection(ids, through);
        });
        connect(viewport_, &VtkView::cameraChanged, this, [this] {
            if (!view_id_.isEmpty())
                camera_update_.start();
        });

        auto* left = new QDockWidget("Model views", this);
        auto* left_content = new QWidget(left);
        auto* left_layout = new QVBoxLayout(left_content);
        view_kind_ = new QComboBox(left_content);
        view_kind_->setObjectName("entityViewKind");
        view_kind_->addItems({"all", "part", "assembly", "set", "include", "material", "property"});
        owner_ = new QComboBox(left_content);
        owner_->setObjectName("entityViewOwner");
        owner_->setEnabled(false);
        owner_->setToolTip("Owners are listed from the first 1,000 model entities");
        tree_count_ = new QLabel("No active project", left_content);
        tree_ = new QTreeWidget(left_content);
        tree_->setItemDelegate(new qcae::sdk_copy::ObservedTreeDelegate(sdk_copies_, tree_));
        tree_->setObjectName("entityTree");
        tree_->setHeaderLabels({"Entity", "Kind"});
        left_layout->addWidget(view_kind_);
        left_layout->addWidget(owner_);
        left_layout->addWidget(tree_count_);
        left_layout->addWidget(tree_);
        left->setObjectName("modelViewsDock");
        left->setWidget(left_content);
        addDockWidget(Qt::LeftDockWidgetArea, left);
        connect(view_kind_, &QComboBox::currentTextChanged, this, [this] {
            rebuildOwnerChoices();
            loadTree();
        });
        connect(owner_, &QComboBox::currentIndexChanged, this, [this] { loadTree(); });
        connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
            const auto selected = tree_->selectedItems();
            if (!selected.isEmpty() &&
                !selected.front()->data(0, Qt::UserRole).toString().isEmpty())
                validateSelection({selected.front()->data(0, Qt::UserRole).toString()}, true);
        });

        auto* right = new QDockWidget("Selection properties", this);
        auto* right_content = new QWidget(right);
        auto* form = new QFormLayout(right_content);
        selected_label_ = new QLabel("No selection", right_content);
        selected_label_->setObjectName("selectedEntities");
        property_kind_ = new QLabel(right_content);
        property_x_ = new QLineEdit(right_content);
        property_y_ = new QLineEdit(right_content);
        property_z_ = new QLineEdit(right_content);
        property_e_ = new QLineEdit(right_content);
        property_details_ = new QLabel(right_content);
        property_details_->setObjectName("propertyDetails");
        property_details_->setTextFormat(Qt::PlainText);
        property_details_->setWordWrap(true);
        property_details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        for (std::size_t index = 0; index < property_coordinate_labels_.size(); ++index) {
            property_coordinate_labels_[index] = new QLabel(right_content);
            property_coordinate_labels_[index]->setObjectName(
                QString("property%1Unit").arg(QChar('X' + static_cast<int>(index))));
        }
        setPropertyCoordinateUnits(false);
        apply_ = new QPushButton("Apply change", right_content);
        apply_->setObjectName("propertyApply");
        auto* property_preview = new QPushButton("Preview change", right_content);
        property_preview->setObjectName("propertyPreview");
        property_cancel_ = new QPushButton("Cancel preview", right_content);
        property_cancel_->setObjectName("propertyCancel");
        property_cancel_->setEnabled(false);
        form->addRow("Entity", selected_label_);
        form->addRow("Kind", property_kind_);
        form->addRow(property_coordinate_labels_[0], property_x_);
        form->addRow(property_coordinate_labels_[1], property_y_);
        form->addRow(property_coordinate_labels_[2], property_z_);
        form->addRow("E (MPa)", property_e_);
        form->addRow("Physical values", property_details_);
        form->addRow(apply_);
        form->addRow(property_preview);
        form->addRow(property_cancel_);
        right->setObjectName("selectionPropertiesDock");
        right->setWidget(right_content);
        addDockWidget(Qt::RightDockWidgetArea, right);
        connect(apply_, &QPushButton::clicked, this, [this] { applyProperty(); });
        connect(property_preview, &QPushButton::clicked, this, [this] { previewProperty(); });
        connect(property_cancel_, &QPushButton::clicked, this, [this] {
            clearPropertyPreview();
            statusBar()->showMessage("Property preview cancelled; project unchanged.", 5000);
        });
        for (auto* field : {property_x_, property_y_, property_z_, property_e_})
            connect(field, &QLineEdit::textChanged, this, [this] { clearPropertyPreview(); });

        auto* modeling_dock = new QDockWidget("Modeling", this);
        modeling_dock->setObjectName("modelingDock");
        modeling_ =
            new qcae::ModelingTools(client_,
                                    {[this](const qcae::RenderPreview& preview) {
                                         viewport_->setPreview(preview);
                                         viewport_->fit();
                                     },
                                     [this] { viewport_->clearPreview(); },
                                     [this] { pollCurrent(); },
                                     [this](const QString& operation, const QJsonObject& response) {
                                         record(operation, response);
                                     }},
                                    modeling_dock);
        modeling_dock->setWidget(modeling_);
        addDockWidget(Qt::RightDockWidgetArea, modeling_dock);
        tabifyDockWidget(right, modeling_dock);
        modeling_dock->raise();

        auto* analysis_dock = new QDockWidget("Analysis and organization", this);
        analysis_dock->setObjectName("analysisDock");
        analysis_tools_ =
            new qcae::AnalysisTools(client_,
                                    {[this] { pollCurrent(); },
                                     [this](const QString& operation, const QJsonObject& response) {
                                         record(operation, response);
                                     },
                                     [this](const QString& identity, const QJsonObject& expected) {
                                         navigateIssue(identity, expected);
                                     },
                                     [this] { return owner_rows_; }},
                                    analysis_dock);
        auto* analysis_scroll = new QScrollArea(analysis_dock);
        analysis_scroll->setWidgetResizable(true);
        analysis_scroll->setWidget(analysis_tools_);
        analysis_dock->setWidget(analysis_scroll);
        addDockWidget(Qt::RightDockWidgetArea, analysis_dock);
        tabifyDockWidget(modeling_dock, analysis_dock);
        analysis_dock->hide();

        auto* bottom = new QDockWidget("Operations and history", this);
        log_ = new qcae::sdk_copy::ObservedPlainTextEdit(sdk_copies_, bottom);
        log_->setReadOnly(true);
        bottom->setObjectName("historyDock");
        bottom->setWidget(log_);
        addDockWidget(Qt::BottomDockWidgetArea, bottom);
        statusBar()->showMessage("Connecting to local engine");

        auto* file = menuBar()->addMenu("File");
        auto* new_action = addActionTo(
            file,
            "projectNewAction",
            "New project",
            [this] { createProject(); },
            QKeySequence::New);
        auto* open_action = addActionTo(
            file,
            "projectOpenAction",
            "Open project…",
            [this] { openProject(); },
            QKeySequence::Open);
        addActionTo(
            file, "projectRecoverAction", "Recover workspace", [this] { recoverProject(); });
        auto* save_action = addActionTo(
            file, "projectSaveAction", "Save", [this] { saveProject(false); }, QKeySequence::Save);
        addActionTo(
            file,
            "projectSaveAsAction",
            "Save as…",
            [this] { saveProject(true); },
            QKeySequence::SaveAs);
        addActionTo(file, "nastranImportAction", "Import BDF…", [this] { importBdf(); });
        auto* export_action = addActionTo(
            file, "nastranExportAction", "Export Nastran files…", [this] { exportNastran(); });
        export_action->setObjectName("nastranExportAction");
        addActionTo(file, "projectCloseAction", "Close project…", [this] { closeProject({}); });
        auto* edit = menuBar()->addMenu("Edit");
        auto* undo_action = addActionTo(
            edit,
            "historyUndoAction",
            "Undo",
            [this] { historyAction("history.undo"); },
            QKeySequence::Undo);
        auto* redo_action = addActionTo(
            edit,
            "historyRedoAction",
            "Redo",
            [this] { historyAction("history.redo"); },
            QKeySequence::Redo);
        addActionTo(edit, "shortcutPreferencesAction", "Keyboard shortcuts…", [this] {
            qcae::show_shortcut_preferences(*this);
        });
        auto* model_menu = menuBar()->addMenu("Model");
        addActionTo(model_menu, "modelingToolsAction", "Modeling tools", [modeling_dock] {
            modeling_dock->show();
            modeling_dock->raise();
        });
        auto* analysis_action = addActionTo(
            model_menu, "analysisToolsAction", "Analysis and organization tools", [analysis_dock] {
                analysis_dock->show();
                analysis_dock->raise();
            });
        analysis_action->setObjectName("analysisToolsAction");
        auto* display = menuBar()->addMenu("Display");
        auto* fit_action =
            addActionTo(display, "displayFitAction", "Fit", [this] { viewport_->fit(); });
        addActionTo(display, "displayFrontAction", "Front", [this] {
            viewport_->standardView(VtkView::StandardView::front);
        });
        addActionTo(display, "displayTopAction", "Top", [this] {
            viewport_->standardView(VtkView::StandardView::top);
        });
        addActionTo(display, "displayRightAction", "Right", [this] {
            viewport_->standardView(VtkView::StandardView::right);
        });
        addActionTo(display, "displayIsometricAction", "Isometric", [this] {
            viewport_->standardView(VtkView::StandardView::isometric);
        });
        addActionTo(display, "displayHideAction", "Hide selected", [this] { hideSelected(); });
        addActionTo(
            display, "displayIsolateAction", "Isolate selected", [this] { isolateSelected(); });
        addActionTo(display, "displayShowAllAction", "Show all", [this] { updateView({}); });
        auto* workspace_toolbar = addToolBar("Workspace");
        workspace_toolbar->setObjectName("workspaceToolbar");
        for (auto* action :
             {new_action, open_action, save_action, undo_action, redo_action, fit_action})
            workspace_toolbar->addAction(action);
        auto* toolbar = addToolBar("Selection");
        toolbar->setObjectName("selectionToolbar");
        auto* through = toolbar->addAction("Through selection");
        through->setObjectName("selectionThroughAction");
        qcae::restore_action_shortcut(*through, {});
        through->setCheckable(true);
        connect(through, &QAction::toggled, viewport_, &VtkView::setThroughSelection);
        auto* contained = toolbar->addAction("Shift+drag box contained");
        contained->setObjectName("selectionContainedAction");
        qcae::restore_action_shortcut(*contained, {});
        contained->setCheckable(true);
        connect(contained, &QAction::toggled, this, [this](bool checked) {
            viewport_->setBoxMode(checked ? VtkView::BoxMode::contained
                                          : VtkView::BoxMode::intersecting);
        });
    }

    QAction* addActionTo(QMenu* menu,
                         const QString& identity,
                         const QString& title,
                         const std::function<void()>& callback,
                         const QKeySequence& shortcut = {}) {
        auto* action = menu->addAction(title);
        action->setObjectName(identity);
        qcae::restore_action_shortcut(*action, shortcut);
        connect(action, &QAction::triggered, this, callback);
        return action;
    }
    QJsonObject context(bool write = false, bool resource = false) const {
        QJsonObject result{{"document_id", document_id_}, {"document_epoch", epoch_}};
        json_ledger::ObjectCopies copies;
        copies.insert("document_id", result.value("document_id"));
        copies.insert("document_epoch", result.value("document_epoch"));
        if (write) {
            result.insert("expected_revision", revision_);
            copies.insert("expected_revision", result.value("expected_revision"));
        }
        if (resource) {
            result.insert("requested_version", 1);
            copies.insert("requested_version", result.value("requested_version"));
        }
        return result;
    }
    void call(const QString& operation,
              const QJsonObject& parameters,
              const QJsonObject& extra,
              DesktopClient::Reply callback) {
        (void)client_.request(operation, parameters, extra, std::move(callback));
    }
    void record(const QString& operation, const QJsonObject& response) {
        const auto message = succeeded(response)
                                 ? json_ledger::owned_string(operation + " succeeded")
                                 : errorText(response);
        appendLog(message);
        statusBar()->showMessage(message, 5000);
    }
    void appendLog(const QString& message) {
        const auto* document = log_->document();
        const bool empty = document->isEmpty();
        const auto before = document->characterCount();
        log_->appendPlainText(message);
        // Observe every append, including warm-up and failed operations. This
        // covers the text piece table only; font/layout coverage remains separate.
        log_copies_.append(message, empty, before, document->characterCount());
    }
    void invalidateSelectionRequests(bool clear_pending) {
        ++selection_generation_;
        pending_selection_evaluating_ = false;
        if (clear_pending)
            pending_selection_.reset();
    }
    void invalidateModelContext() {
        scene_invalidated_ = true;
        context_refresh_required_ = true;
        ++event_generation_;
        ++render_generation_;
        resources_.clear();
        render_pending_ = false;
        invalidateSelectionRequests(true);
        clearPropertyPreview();
        clearPropertyFields();
        modeling_->setSelectedGeometry({}, {});
    }
    bool propertyRepliesBlocked() const {
        return !client_.ready() || document_id_.isEmpty() || context_refresh_required_ ||
               scene_invalidated_;
    }
    void subscribeEvents() {
        if (!client_.ready() || subscription_pending_)
            return;
        subscription_pending_ = true;
        const auto generation = ++subscription_generation_;
        QJsonObject parameters{{"engine_instance_id", client_.engineInstanceId()},
                               {"after_sequence", json_ledger::number(client_.eventSequence())}};
        json_ledger::ObjectCopies copies;
        copies.insert("engine_instance_id", parameters.value("engine_instance_id"));
        copies.insert("after_sequence", parameters.value("after_sequence"));
        if (client_.supportsEventsDocumentSummary()) {
            parameters.insert("include_document_summary", true);
            copies.insert("include_document_summary", parameters.value("include_document_summary"));
        }
        call("events.subscribe",
             parameters,
             {{"requested_version", 1}},
             [this, generation](const QJsonObject& response) {
                 if (generation != subscription_generation_)
                     return;
                 subscription_pending_ = false;
                 if (!client_.ready())
                     return;
                 if (succeeded(response)) {
                     const auto data = response.value("data").toObject();
                     client_.setEventCursor(data.value("engine_instance_id").toString(),
                                            data.value("next_sequence").toString().toULongLong());
                     force_full_render_ =
                         force_full_render_ || data.value("resync_required").toBool();
                 } else {
                     scheduleDisplayCallback(1000, [this, generation] {
                         if (generation == subscription_generation_)
                             subscribeEvents();
                     });
                 }
                 pollCurrent();
                 modeling_->refreshTask();
             });
    }
    void clearDocument(bool reset_tools = true) {
        resources_.clear();
        ++render_generation_;
        render_pending_ = false;
        ++view_update_generation_;
        view_update_pending_ = false;
        queued_hidden_.reset();
        force_full_render_ = true;
        invalidateSelectionRequests(true);
        rendered_version_.reset();
        if (reset_tools) {
            modeling_->setContext({});
            analysis_tools_->setContext({});
        }
        clearPropertyPreview();
        clearPropertyFields();
        if (document_id_.isEmpty() && view_id_.isEmpty())
            return;
        document_id_.clear();
        epoch_.clear();
        revision_.clear();
        view_id_.clear();
        view_revision_.clear();
        confirmed_camera_.clear();
        view_update_pending_ = false;
        queued_hidden_.reset();
        tree_->clear();
        tree_items_.clear();
        tree_count_->setText("No active project");
        owner_rows_ = {};
        owner_row_indices_.clear();
        rebuildOwnerChoices();
        selected_ids_.clear();
        hidden_ids_.clear();
        all_ids_.clear();
        qcae::RenderPacket empty;
        empty.document = {qcae::DocumentId{}, qcae::DocumentEpoch{}};
        viewport_->setPacket(empty);
        selected_label_->setText("No selection");
    }
    bool canRebaseModel() const {
        const auto view = unsignedRevision(QJsonValue(view_revision_));
        return client_.supportsRenderModelRebase() && client_.supportsResources() && view &&
               *view < std::numeric_limits<std::uint64_t>::max() && pipelineIdle() &&
               !confirmed_camera_.isEmpty() && viewport_->cameraFingerprint() == confirmed_camera_;
    }
    void applyDocumentInfo(const QJsonObject& data, bool model_rebase = false) {
        const auto id = json_ledger::string(data.value("document_id"));
        const auto epoch = json_ledger::string(data.value("document_epoch"));
        const auto rev = revisionText(data.value("revision"));
        if (id.isEmpty() || epoch.isEmpty())
            return;
        context_refresh_required_ = false;
        const bool changed = id != document_id_ || epoch != epoch_ || rev != revision_;
        const bool new_document = id != document_id_ || epoch != epoch_;
        document_id_ = id;
        epoch_ = epoch;
        revision_ = rev;
        modeling_->setContext(context(true));
        analysis_tools_->setContext(context(true));
        saved_path_ = json_ledger::string(data.value("saved_path"));
        dirty_ = data.value("dirty").toBool();
        const auto name = data.value("name").isString() ? json_ledger::string(data.value("name"))
                                                        : QString("QCAE");
        setWindowTitle(json_ledger::owned_string(name + (dirty_ ? " * — QCAE" : " — QCAE")));
        if (!changed && render_pending_)
            return; // Polling the same version must not restart its resource transfer.
        if (changed || force_full_render_ || scene_invalidated_) {
            resources_.clear();
            ++render_generation_;
            render_pending_ = false;
            invalidateSelectionRequests(true);
            selected_ids_.clear();
            viewport_->setSelectedIds({});
            selected_label_->setText("No selection");
            loadSelectedProperty();
            if (new_document) {
                ++view_update_generation_;
                view_update_pending_ = false;
                queued_hidden_.reset();
                rendered_version_.reset();
                force_full_render_ = true;
                hidden_ids_.clear();
                all_ids_.clear();
                view_id_.clear();
                view_revision_.clear();
                confirmed_camera_.clear();
                owner_rows_ = {};
                owner_row_indices_.clear();
                rebuildOwnerChoices();
            }
            if (new_document || !client_.supportsResources() || force_full_render_) {
                owner_->setEnabled(false);
                loadOwners();
                loadHistory();
            }
            if (view_id_.isEmpty()) {
                if (!view_update_pending_)
                    createView();
            } else if (model_rebase && !new_document)
                renderResource(true);
            else
                requestViewRefresh();
        }
    }
    void pollCurrent() {
        if (!client_.ready())
            return;
        if (current_pending_) {
            current_queued_ = true;
            return;
        }
        current_pending_ = true;
        const auto event_generation = event_generation_;
        call("project.current", {}, {}, [this, event_generation](const QJsonObject& response) {
            current_pending_ = false;
            if (current_queued_) {
                current_queued_ = false;
                scheduleDisplayCallback(0, [this] { pollCurrent(); });
            }
            if (!client_.ready())
                return;
            if (event_generation != event_generation_)
                return; // An event queued a newer authoritative query while this one was in flight.
            if (!succeeded(response)) {
                if (response.value("error").toObject().value("code") == "DOCUMENT_NOT_FOUND")
                    clearDocument();
                return;
            }
            applyDocumentInfo(response.value("data").toObject());
        });
    }

    void createProject() {
        bool accepted = false;
        const auto name = QInputDialog::getText(
            this, "New project", "Name", QLineEdit::Normal, "Untitled", &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        withClosedDocument([this, name] {
            call("project.create",
                 {{"name", name}},
                 {{"idempotency_key", uniqueKey()}},
                 [this](const QJsonObject& response) {
                     record("project.create", response);
                     if (succeeded(response))
                         pollCurrent();
                 });
        });
    }
    void openProject() {
        const auto path =
            QFileDialog::getOpenFileName(this, "Open QCAE project", {}, "QCAE (*.qcae)");
        if (path.isEmpty())
            return;
        withClosedDocument([this, path] {
            call("project.open",
                 {{"mode", "normal"}, {"path", path}},
                 {{"idempotency_key", uniqueKey()}},
                 [this](const QJsonObject& response) {
                     record("project.open", response);
                     if (succeeded(response))
                         pollCurrent();
                 });
        });
    }
    void recoverProject() {
        withClosedDocument([this] {
            call("project.open",
                 {{"mode", "recover"}},
                 {{"idempotency_key", uniqueKey()}},
                 [this](const QJsonObject& response) {
                     record("project.open recover", response);
                     if (succeeded(response))
                         pollCurrent();
                 });
        });
    }
    void withClosedDocument(const std::function<void()>& next) {
        if (document_id_.isEmpty()) {
            next();
            return;
        }
        closeProject([next](bool closed) {
            if (closed)
                next();
        });
    }
    void closeProject(Done done) {
        if (document_id_.isEmpty()) {
            if (done)
                done(true);
            return;
        }
        QMessageBox prompt(this);
        prompt.setWindowTitle("Close active project");
        prompt.setText(dirty_ ? "Choose what happens to unsaved work."
                              : "Close the active project?");
        auto* keep = prompt.addButton("Keep recovery", QMessageBox::AcceptRole);
        auto* discard = prompt.addButton("Discard", QMessageBox::DestructiveRole);
        prompt.addButton(QMessageBox::Cancel);
        prompt.exec();
        if (prompt.clickedButton() != keep && prompt.clickedButton() != discard) {
            if (done)
                done(false);
            return;
        }
        const auto policy = prompt.clickedButton() == keep ? "keep_recovery" : "discard";
        auto extra = context(true);
        extra.insert("idempotency_key", uniqueKey());
        call("project.close",
             {{"policy", policy}},
             extra,
             [this, done](const QJsonObject& response) {
                 record("project.close", response);
                 if (succeeded(response)) {
                     clearDocument();
                     pollCurrent();
                 }
                 if (done)
                     done(succeeded(response));
             });
    }
    void saveProject(bool as) {
        if (document_id_.isEmpty())
            return;
        QString path = as || saved_path_.isEmpty()
                           ? QFileDialog::getSaveFileName(
                                 this, "Save QCAE project", saved_path_, "QCAE (*.qcae)")
                           : saved_path_;
        if (path.isEmpty())
            return;
        if (!path.endsWith(".qcae", Qt::CaseInsensitive))
            path += ".qcae";
        auto extra = context(true);
        extra.insert("idempotency_key", uniqueKey());
        call(as ? "project.save_as" : "project.save",
             {{"path", path}},
             extra,
             [this](const QJsonObject& response) {
                 record("project.save", response);
                 if (succeeded(response))
                     pollCurrent();
             });
    }
    void historyAction(const QString& operation) {
        if (document_id_.isEmpty())
            return;
        auto extra = context(true);
        extra.insert("idempotency_key", uniqueKey());
        call(operation, {}, extra, [this, operation](const QJsonObject& response) {
            record(operation, response);
            if (succeeded(response))
                pollCurrent();
        });
    }

    void importBdf() {
        if (document_id_.isEmpty())
            return;
        const auto path = QFileDialog::getOpenFileName(
            this, "Import Nastran BDF", {}, "Nastran (*.bdf *.dat *.nas)");
        if (path.isEmpty())
            return;
        const auto root = QFileInfo(path).absoluteDir().canonicalPath();
        QSet<QString> visited;
        QJsonArray resources;
        QString error;
        if (!readDeck(path, root, visited, resources, error)) {
            QMessageBox::warning(this, "Import", error);
            return;
        }
        call("capabilities.list",
             {},
             {},
             [this, resources, path, root](const QJsonObject& response) {
                 if (!succeeded(response)) {
                     record("capabilities.list", response);
                     return;
                 }
                 const auto profiles =
                     response.value("data").toObject().value("declared_solver_profiles").toArray();
                 if (profiles.isEmpty()) {
                     QMessageBox::warning(
                         this, "Import", "No Nastran profile is declared by engine");
                     return;
                 }
                 const auto profile = profiles.at(0).toObject().value("profile_ref").toObject();
                 QJsonObject parameters{{"command", "model.import"},
                                        {"root_resource", QDir(root).relativeFilePath(path)},
                                        {"resources", resources},
                                        {"source_profile_ref", profile},
                                        {"unit_system", "mm-N-MPa"}};
                 previewAndCommit(parameters);
             });
    }
    void previewAndCommit(const QJsonObject& parameters) {
        const auto expected = context(true);
        const auto generation = property_preview_generation_;
        const bool property_request = parameters.value("command") != QJsonValue("model.import");
        if (context_refresh_required_ || (property_request && propertyRepliesBlocked()))
            return;
        const auto [operation, body] = propertyPreviewRequest(parameters);
        call(
            operation,
            body,
            expected,
            [this, expected, operation, generation, property_request](const QJsonObject& response) {
                if (context_refresh_required_ || expected != context(true) ||
                    (property_request &&
                     (propertyRepliesBlocked() || generation != property_preview_generation_)))
                    return;
                if (!succeeded(response)) {
                    record(operation, response);
                    return;
                }
                if (!propertyReplyMatches(response,
                                          expected.value("document_id").toString(),
                                          expected.value("document_epoch").toString(),
                                          expected.value("expected_revision").toString()))
                    return;
                record(operation, response);
                const auto data = response.value("data").toObject();
                const auto preview = data.value("preview_id").toString();
                if (preview.isEmpty())
                    return;
                const auto report = data.value("import_report").toObject();
                if (!report.isEmpty() && !report.value("complete").toBool()) {
                    QMessageBox::warning(
                        this, "Import", "Import report contains blocking diagnostics");
                    return;
                }
                auto extra = expected;
                extra.insert("idempotency_key", uniqueKey());
                call("changes.commit",
                     {{"preview_id", preview}},
                     extra,
                     [this](const QJsonObject& commit) {
                         record("changes.commit", commit);
                         if (succeeded(commit))
                             pollCurrent();
                     });
            });
    }

    void exportNastran() {
        if (document_id_.isEmpty())
            return;
        const auto expected = context(true);
        call("nastran.ui", {}, {}, [this, expected](const QJsonObject& response) {
            if (!succeeded(response)) {
                record("nastran.ui", response);
                return;
            }
            const auto contribution = response.value("data").toObject();
            if (contribution.value("operation") != "model.export" ||
                contribution.value("units") != "mm-N-MPa")
                return;
            auto* dialog = new QDialog(this);
            dialog->setObjectName("nastranExportForm");
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(contribution.value("label").toString());
            auto* layout = new QFormLayout(dialog);
            auto* analysis = new QComboBox(dialog);
            analysis->setObjectName("nastranExportAnalysis");
            for (const auto& value : owner_rows_) {
                const auto row = value.toObject();
                if (row.value("kind") == "analysis")
                    analysis->addItem(entityLabel(row), entityId(row));
            }
            auto* directory = new QLineEdit(dialog);
            directory->setObjectName("nastranExportDirectory");
            directory->setText(
                QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                    .filePath("nastran-" + uniqueKey()));
            auto* status =
                new QLabel("A new directory will contain verified BDF/INCLUDE files.", dialog);
            status->setObjectName("nastranExportStatus");
            status->setWordWrap(true);
            auto* run = new QPushButton("Export files", dialog);
            run->setObjectName("nastranExportRun");
            run->setEnabled(analysis->count() > 0);
            auto* read_fixture = new QPushButton("Read result fixture file…", dialog);
            read_fixture->setObjectName("nastranReadFixture");
            read_fixture->setEnabled(false);
            auto* refresh_result = new QPushButton("Refresh result provenance", dialog);
            refresh_result->setObjectName("nastranRefreshResult");
            refresh_result->setEnabled(false);
            auto* result_values = new QTreeWidget(dialog);
            result_values->setObjectName("nastranResultValues");
            result_values->setHeaderLabels({"Entity", "X (mm)", "Y (mm)", "Z (mm)"});
            result_values->setMaximumHeight(150);
            layout->addRow("Analysis", analysis);
            layout->addRow("New output directory", directory);
            layout->addRow(status);
            layout->addRow(run);
            layout->addRow(read_fixture);
            layout->addRow(refresh_result);
            layout->addRow(result_values);
            const QPointer<QDialog> guard(dialog);
            connect(
                read_fixture, &QPushButton::clicked, dialog, [this, guard, status, refresh_result] {
                    readResultFixture(guard, status, refresh_result);
                });
            connect(refresh_result, &QPushButton::clicked, dialog, [this, guard, status] {
                if (!guard || guard->property("resultId").toString().isEmpty())
                    return;
                call("results.get",
                     {{"result_id", guard->property("resultId").toString()}},
                     context(),
                     [guard, status](const QJsonObject& result) {
                         showFixtureResult(guard, status, result);
                     });
            });
            connect(run,
                    &QPushButton::clicked,
                    dialog,
                    [this, guard, analysis, directory, status, run, expected, contribution] {
                        auto request_context = expected;
                        request_context.insert("idempotency_key", uniqueKey());
                        request_context.insert("expected_profile", contribution.value("profile"));
                        run->setEnabled(false);
                        status->setText("Exporting frozen input…");
                        call("model.export",
                             {{"analysis_id", analysis->currentData().toString()},
                              {"output_directory", directory->text()}},
                             request_context,
                             [this, guard, status, run, expected](const QJsonObject& exported) {
                                 if (!guard)
                                     return;
                                 if (!succeeded(exported)) {
                                     status->setText(errorText(exported));
                                     run->setEnabled(true);
                                     return;
                                 }
                                 const auto task =
                                     exported.value("data").toObject().value("task_id").toString();
                                 guard->setProperty("taskId", task);
                                 pollExportTask(guard, status, expected, task);
                             });
                    });
            dialog->show();
        });
    }
    void pollExportTask(const QPointer<QDialog>& dialog,
                        QLabel* status,
                        const QJsonObject& expected,
                        const QString& task) {
        if (!dialog)
            return;
        call(
            "task.status",
            {{"task_id", task}},
            expected,
            [this, dialog, status, expected, task](const QJsonObject& response) {
                if (!dialog)
                    return;
                if (!succeeded(response)) {
                    status->setText(errorText(response));
                    return;
                }
                const auto result = response.value("data").toObject();
                const auto state = result.value("state").toString();
                dialog->setProperty("taskState", state);
                status->setText("Export task: " + state);
                if (state == "succeeded") {
                    call(
                        "artifact.get",
                        {{"artifact_id", "artifact-" + task}},
                        expected,
                        [dialog, status](const QJsonObject& artifact) {
                            if (!dialog)
                                return;
                            if (!succeeded(artifact) ||
                                !artifact.value("data").toObject().value("verified").toBool()) {
                                status->setText(errorText(artifact));
                                return;
                            }
                            const auto path =
                                artifact.value("data").toObject().value("manifest_path").toString();
                            dialog->setProperty("manifestPath", path);
                            dialog->setProperty(
                                "artifactId",
                                artifact.value("data").toObject().value("artifact_id").toString());
                            dialog->findChild<QPushButton*>("nastranReadFixture")->setEnabled(true);
                            status->setText("Verified files published. Manifest: " + path);
                        });
                } else if (state == "queued" || state == "running" || state == "committing" ||
                           state == "cancel_requested") {
                    QTimer::singleShot(100, this, [this, dialog, status, expected, task] {
                        pollExportTask(dialog, status, expected, task);
                    });
                } else if (result.contains("diagnostic")) {
                    status->setText(
                        result.value("diagnostic").toObject().value("message").toString());
                }
            });
    }

    static void
    showFixtureResult(const QPointer<QDialog>& dialog, QLabel* status, const QJsonObject& result) {
        if (!dialog)
            return;
        if (!succeeded(result)) {
            status->setText(errorText(result));
            return;
        }
        const auto data = result.value("data").toObject();
        dialog->setProperty("resultId", data.value("result_id").toString());
        dialog->setProperty("resultState", data.value("state").toString());
        dialog->setProperty("resultValue", data);
        const auto field = data.value("field").toObject();
        auto* values = dialog->findChild<QTreeWidget*>("nastranResultValues");
        values->clear();
        for (const auto& sample : field.value("values").toArray()) {
            const auto row = sample.toObject();
            const auto components = row.value("value").toArray();
            if (components.size() != 3)
                continue;
            new QTreeWidgetItem(values,
                                {row.value("entity_id").toString(),
                                 QString::number(components[0].toDouble(), 'g', 17),
                                 QString::number(components[1].toDouble(), 'g', 17),
                                 QString::number(components[2].toDouble(), 'g', 17)});
        }
        status->setText(
            QString("Result fixture (%1), input revision %2: %3; %4 %5 at %6, case %7. Fixture "
                    "values are not solver validation.")
                .arg(data.value("source_kind").toString())
                .arg(data.value("input_version").toObject().value("revision").toString())
                .arg(data.value("state").toString())
                .arg(field.value("quantity").toString())
                .arg(field.value("unit").toString())
                .arg(field.value("location").toString())
                .arg(field.value("case").toString()));
    }
    void readResultFixture(const QPointer<QDialog>& dialog, QLabel* status, QPushButton* refresh) {
        if (!dialog || dialog->property("artifactId").toString().isEmpty())
            return;
        const auto path = QFileDialog::getOpenFileName(
            dialog, "Read result fixture", {}, "Fixture JSON (*.json)");
        if (path.isEmpty() || !dialog)
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
            status->setText("Cannot read fixture file within the 64 KiB limit.");
            return;
        }
        const auto bytes = file.readAll();
        auto expected = context(true);
        expected.insert("idempotency_key", uniqueKey());
        call("results.read_fixture",
             {{"artifact_id", dialog->property("artifactId").toString()},
              {"fixture_json", QString::fromUtf8(bytes)}},
             expected,
             [dialog, status, refresh](const QJsonObject& result) {
                 if (!dialog)
                     return;
                 showFixtureResult(dialog, status, result);
                 refresh->setEnabled(succeeded(result));
             });
    }

    void loadOwners() {
        if (document_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        call("entity.query",
             {{"view", "all"}, {"offset", 0}, {"limit", 1000}},
             context(),
             [this, doc, epoch, rev](const QJsonObject& response) {
                 if (doc != document_id_ || epoch != epoch_ || rev != revision_)
                     return;
                 if (!succeeded(response)) {
                     record("entity.query owners", response);
                     return;
                 }
                 owner_rows_ = response.value("data").toObject().value("entities").toArray();
                 owner_row_indices_.clear();
                 for (int index = 0; index < owner_rows_.size(); ++index)
                     owner_row_indices_.insert(entityId(owner_rows_[index].toObject()), index);
                 analysis_tools_->setRowsContext(context(true));
                 rebuildOwnerChoices();
                 loadTree();
             });
    }
    void rebuildOwnerChoices() {
        const auto view = view_kind_->currentText();
        const auto previous_id = owner_->currentData().toString();
        const QSignalBlocker block(owner_);
        owner_->clear();
        if (view == "all") {
            owner_->addItem("All entities");
            owner_->setEnabled(false);
            return;
        }
        const auto kind = view == "property" ? QString("section") : view;
        owner_->addItem("Select a " + kind + "…");
        for (const auto& value : owner_rows_) {
            const auto row = value.toObject();
            if (row.value("kind").toString() != kind)
                continue;
            const auto id = entityId(row);
            owner_->addItem(entityLabel(row) + " · " + id.left(8), id);
        }
        const auto prior_index = owner_->findData(previous_id);
        owner_->setCurrentIndex(prior_index > 0 ? prior_index : 0);
        owner_->setEnabled(owner_->count() > 1);
    }

    void treeRefreshCompleted() {
        setProperty("treeDocumentId", document_id_);
        setProperty("treeDocumentEpoch", epoch_);
        setProperty("treeRevision", revision_);
    }
    void loadTree(int offset = 0) {
        if (document_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto view = view_kind_->currentText();
        const auto owner_id = owner_->currentData().toString();
        if (view != "all" && owner_id.isEmpty()) {
            tree_->clear();
            tree_items_.clear();
            tree_count_->setText("Choose a " + view + " owner");
            treeRefreshCompleted();
            return;
        }
        QJsonObject params{{"view", view}, {"offset", offset}, {"limit", 500}};
        if (view != "all")
            params.insert("owner_id", owner_id);
        call("entity.query",
             params,
             context(),
             [this, doc, epoch, rev, view, owner_id, offset](const QJsonObject& response) {
                 if (doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                     view != view_kind_->currentText() ||
                     owner_id != owner_->currentData().toString())
                     return;
                 if (!succeeded(response)) {
                     record("entity.query", response);
                     return;
                 }
                 if (offset == 0) {
                     tree_->clear();
                     tree_items_.clear();
                 }
                 const auto data = response.value("data").toObject();
                 const auto rows = data.value("entities").toArray();
                 for (const auto& value : rows) {
                     const auto row = value.toObject();
                     auto* item = new QTreeWidgetItem(
                         tree_, {entityLabel(row), row.value("kind").toString()});
                     const auto identity = entityId(row);
                     item->setData(0, Qt::UserRole, identity);
                     tree_items_.insert(identity, item);
                 }
                 const auto total = data.value("total").toInt();
                 const auto shown = offset + rows.size();
                 if (shown < total && shown < 1000 && !rows.isEmpty()) {
                     loadTree(offset + rows.size());
                     return;
                 }
                 tree_count_->setText(shown < total
                                          ? "Showing first " + QString::number(shown) + " of " +
                                                QString::number(total) + " entities"
                                          : "Showing " + QString::number(shown) + " entities");
                 treeRefreshCompleted();
             });
    }
    void loadHistory() {
        if (document_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        call("history.list", {}, context(), [this, doc, epoch, rev](const QJsonObject& response) {
            if (doc != document_id_ || epoch != epoch_ || rev != revision_ || !succeeded(response))
                return;
            const auto rows = response.value("data").toObject().value("items").toArray();
            appendLog(json_ledger::owned_string("History at revision " + rev + ": " +
                                                json_ledger::number(rows.size()) +
                                                " transactions"));
            for (const auto& value : rows) {
                const auto item = value.toObject();
                appendLog(json_ledger::owned_string(
                    QString(item.value("applied").toBool() ? "  ✓ " : "  ○ ") +
                    json_ledger::string(item.value("label"))));
            }
        });
    }
    void createView(std::optional<QStringList> intended_hidden = std::nullopt) {
        if (document_id_.isEmpty())
            return;
        const auto generation = ++view_update_generation_;
        view_update_pending_ = true;
        resources_.clear();
        ++render_generation_;
        render_pending_ = false;
        force_full_render_ = true;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto fingerprint = viewport_->cameraFingerprint();
        const auto desired = intended_hidden.value_or(queued_hidden_.value_or(hidden_ids_));
        queued_hidden_.reset();
        QJsonArray hidden;
        for (const auto& id : desired)
            hidden.append(id);
        call(
            "view.create",
            {{"hidden_ids", hidden}, {"camera_fingerprint", fingerprint}},
            context(true),
            [this, doc, epoch, rev, fingerprint, desired, generation](const QJsonObject& response) {
                if (generation != view_update_generation_)
                    return;
                view_update_pending_ = false;
                if (doc != document_id_ || epoch != epoch_)
                    return;
                if (rev != revision_) {
                    createView(queued_hidden_.value_or(desired));
                    return;
                }
                if (!succeeded(response)) {
                    record("view.create", response);
                    if (response.value("error").toObject().value("code") == "ENTITY_NOT_FOUND") {
                        queued_hidden_.reset();
                        createView(QStringList{});
                        return;
                    }
                    if (!queued_hidden_)
                        queued_hidden_ = desired;
                    scheduleDisplayCallback(1000, [this, doc, epoch, generation] {
                        if (client_.ready() && doc == document_id_ && epoch == epoch_ &&
                            generation == view_update_generation_)
                            createView();
                    });
                    return;
                }
                const auto data = response.value("data").toObject();
                invalidateSelectionRequests(false);
                rendered_version_.reset();
                view_id_ = data.value("view_session_id").toString();
                view_revision_ = revisionText(data.value("view_revision"));
                confirmed_camera_ = fingerprint;
                hidden_ids_ = desired;
                renderView();
            });
    }
    void applyRows(const QJsonArray& rows) {
        bool owner_label_changed = false;
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const auto id = entityId(row);
            if (auto* item = tree_items_.value(id, nullptr))
                item->setText(0, entityLabel(row));
            const auto owner_index = owner_row_indices_.constFind(id);
            if (owner_index != owner_row_indices_.cend()) {
                const auto index = owner_index.value();
                owner_label_changed =
                    owner_label_changed ||
                    entityLabel(owner_rows_[index].toObject()) != entityLabel(row);
                owner_rows_[index] = row;
                // Qt replaces one shared nested QCbor container reference;
                // it does not copy the row's owned strings or other rows.
                qcae::ledger::add(qcae::ledger::Stage::socket_receive,
                                  qcae::ledger::Metric::metadata_copy_bytes,
                                  64);
            }
        }
        if (owner_label_changed)
            rebuildOwnerChoices();
        analysis_tools_->setRowsContext(context(true));
        treeRefreshCompleted();
    }
    void refreshChangedRows(const QJsonArray& ids) {
        if (ids.isEmpty()) {
            treeRefreshCompleted();
            return;
        }
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto view = view_kind_->currentText(), owner_id = owner_->currentData().toString();
        const auto operation_ledger = qcae::ledger::current();
        const QJsonObject parameters{{"ids", ids}, {"limit", 1000}};
        json_ledger::object(parameters, {"ids", "limit"});
        call(
            "entity.query",
            parameters,
            context(),
            [this, doc, epoch, rev, view, owner_id, operation_ledger](const QJsonObject& response) {
                qcae::ledger::Scope scope(operation_ledger);
                if (doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                    view != view_kind_->currentText() ||
                    owner_id != owner_->currentData().toString() || !succeeded(response))
                    return;
                const auto rows = response.value("data").toObject().value("entities").toArray();
                applyRows(rows);
            });
    }
    void finishRenderResource(const QJsonObject& data,
                              const QString& doc,
                              const QString& epoch,
                              const QString& rev,
                              const QString& view,
                              const QString& view_rev) {
        rendered_version_ = RenderVersion{doc, epoch, rev, view, view_rev};
        force_full_render_ = false;
        scene_invalidated_ = false;
        render_pending_ = false;
        viewport_->setSelectedIds(selected_ids_);
        if (data.value("refresh_tree").toBool())
            loadOwners();
        else if (client_.supportsRenderChangedRows() &&
                 validChangedRows(data, doc, epoch, rev, view, view_rev))
            applyRows(data.value("changed_rows").toArray());
        else if (validChangedIds(data.value("changed_ids")))
            refreshChangedRows(data.value("changed_ids").toArray());
        else
            loadOwners();
        drainViewIntents();
        evaluatePendingSelection();
    }
    void drainViewIntents() {
        if (queued_hidden_) {
            const auto queued = *queued_hidden_;
            queued_hidden_.reset();
            updateView(queued);
        } else if (viewport_->cameraFingerprint() != confirmed_camera_)
            requestViewRefresh();
    }
    void renderResource(bool model_rebase = false) {
        if (view_id_.isEmpty() || context_refresh_required_)
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_, view = view_id_,
                   initial_view_rev = view_revision_;
        const auto previous_view = unsignedRevision(QJsonValue(initial_view_rev));
        if (model_rebase &&
            (!previous_view || *previous_view == std::numeric_limits<std::uint64_t>::max())) {
            requestViewRefresh();
            return;
        }
        const auto view_rev =
            model_rebase ? json_ledger::number(*previous_view + 1) : initial_view_rev;
        const auto generation = ++render_generation_;
        render_pending_ = true;
        const auto operation_ledger = qcae::ledger::current();
        const auto admitted = std::make_shared<bool>(!model_rebase);
        const auto matches =
            [this, doc, epoch, rev, view, view_rev, initial_view_rev, generation, admitted] {
                return generation == render_generation_ && doc == document_id_ && epoch == epoch_ &&
                       rev == revision_ && view == view_id_ &&
                       (*admitted ? view_rev : initial_view_rev) == view_revision_;
            };
        const auto accept_version = [this, admitted, view_rev] {
            view_revision_ = view_rev;
            *admitted = true;
        };
        const ResourceVersion expected{
            {DocumentId(json_ledger::utf8(doc)), DocumentEpoch(json_ledger::utf8(epoch))},
            rev.toULongLong(),
            json_ledger::utf8(view),
            view_rev.toULongLong()};
        resources_.setContext(expected);
        QJsonObject parameters{{"view_session_id", view},
                               {"expected_view_revision", initial_view_rev}};
        json_ledger::ObjectCopies parameters_copies;
        parameters_copies.insert("view_session_id", parameters.value("view_session_id"));
        parameters_copies.insert("expected_view_revision",
                                 parameters.value("expected_view_revision"));
        if (client_.renderWireVersion() == 3) {
            parameters.insert("render_wire_version", 3);
            parameters_copies.insert("render_wire_version",
                                     parameters.value("render_wire_version"));
        }
        if (client_.supportsInlineEmptyRender()) {
            parameters.insert("allow_inline_empty", true);
            parameters_copies.insert("allow_inline_empty", parameters.value("allow_inline_empty"));
        }
        if (model_rebase) {
            parameters.insert("allow_model_rebase", true);
            parameters_copies.insert("allow_model_rebase", parameters.value("allow_model_rebase"));
        }
        if (client_.supportsRenderChangedRows()) {
            parameters.insert("include_changed_rows", true);
            parameters_copies.insert("include_changed_rows",
                                     parameters.value("include_changed_rows"));
        }
        if (rendered_version_ && !force_full_render_ && rendered_version_->document == doc &&
            rendered_version_->epoch == epoch && rendered_version_->view == view) {
            parameters.insert("base_revision", rendered_version_->revision);
            parameters_copies.insert("base_revision", parameters.value("base_revision"));
            parameters.insert("base_view_revision", rendered_version_->view_revision);
            parameters_copies.insert("base_view_revision", parameters.value("base_view_revision"));
        }
        const auto request_context = context(true, true);
        call("view.render_resource",
             parameters,
             request_context,
             [this,
              matches,
              accept_version,
              expected,
              doc,
              epoch,
              rev,
              view,
              view_rev,
              model_rebase,
              operation_ledger](const QJsonObject& response) {
                 qcae::ledger::Scope scope(operation_ledger);
                 const auto data = response.value("data").toObject();
                 if (data.value("mode") == "version_only") {
                     if (!matches())
                         return;
                     const auto delta =
                         data.value("acknowledgement").isObject()
                             ? decodeAcknowledgement(data.value("acknowledgement").toObject(),
                                                     expected)
                             : std::nullopt;
                     if (succeeded(response) && delta)
                         accept_version();
                     if (!succeeded(response) || !delta || !viewport_->applyDelta(*delta)) {
                         render_pending_ = false;
                         const bool already_full = force_full_render_;
                         force_full_render_ = true;
                         statusBar()->showMessage(
                             "Display acknowledgement has an invalid version; refreshing", 5000);
                         if (model_rebase && (!succeeded(response) || !delta))
                             createView();
                         else if (!already_full)
                             renderResource();
                         return;
                     }
                     finishRenderResource(data, doc, epoch, rev, view, view_rev);
                     return;
                 }
                 const auto manifest =
                     ResourceClient::parseManifest(data.value("manifest").toObject());
                 const auto release = [this, &manifest] {
                     if (manifest.ok()) {
                         const QJsonObject parameters{
                             {"resource_id", json_ledger::from_utf8(manifest.value->resource_id)}};
                         const QJsonObject requested{{"requested_version", 1}};
                         json_ledger::object(parameters, {"resource_id"});
                         json_ledger::object(requested, {"requested_version"});
                         call("resources.release", parameters, requested, [](const auto&) {});
                     }
                 };
                 if (!matches()) {
                     release();
                     return;
                 }
                 if (!succeeded(response)) {
                     render_pending_ = false;
                     record("view.render_resource", response);
                     force_full_render_ = true;
                     if (model_rebase)
                         createView();
                     else
                         scheduleDisplayCallback(250, [this] { pollCurrent(); });
                     return;
                 }
                 if (!manifest.ok() || !same_resource_version(manifest.value->version, expected)) {
                     release();
                     render_pending_ = false;
                     force_full_render_ = true;
                     statusBar()->showMessage("Display manifest has an invalid version; refreshing",
                                              5000);
                     if (model_rebase)
                         createView();
                     return;
                 }
                 const auto media = manifest.value->media_type;
                 if (media != "qcae.render.packet.v1" && media != "qcae.render.delta.v1" &&
                     media != "qcae.render.packet.v2" && media != "qcae.render.delta.v2" &&
                     media != "qcae.render.packet.v3" && media != "qcae.render.delta.v3") {
                     release();
                     render_pending_ = false;
                     force_full_render_ = true;
                     statusBar()->showMessage("Unsupported display resource format", 5000);
                     if (model_rebase)
                         createView();
                     return;
                 }
                 accept_version();
                 resources_.fetch(
                     *manifest.value,
                     [this,
                      matches,
                      expected,
                      media,
                      data,
                      doc,
                      epoch,
                      rev,
                      view,
                      view_rev,
                      operation_ledger](Result<QByteArray> bytes) {
                         qcae::ledger::Scope scope(operation_ledger);
                         if (!matches())
                             return;
                         if (!bytes.ok()) {
                             render_pending_ = false;
                             force_full_render_ = true;
                             statusBar()->showMessage(
                                 "Display transfer interrupted; refreshing current state", 5000);
                             pollCurrent();
                             return;
                         }
                         const auto version_matches = [&expected](const auto& packet) {
                             qcae::ledger::add(qcae::ledger::Stage::socket_receive,
                                               qcae::ledger::Metric::metadata_copy_bytes,
                                               packet.document.id.value.size() +
                                                   packet.document.epoch.value.size() +
                                                   packet.view_session_id.size());
                             return same_resource_version({packet.document,
                                                           packet.revision,
                                                           packet.view_session_id,
                                                           packet.view_revision},
                                                          expected);
                         };
                         bool installed = false;
                         const auto encoding = transport::render_resource_version(*bytes.value);
                         const auto expected_encoding = media.ends_with(".v3")   ? 3u
                                                        : media.ends_with(".v2") ? 2u
                                                                                 : 1u;
                         if (encoding != expected_encoding) {
                             render_pending_ = false;
                             force_full_render_ = true;
                             statusBar()->showMessage(
                                 "Display encoding does not match its manifest", 5000);
                             return;
                         }
                         if (media == "qcae.render.packet.v1" || media == "qcae.render.packet.v2" ||
                             media == "qcae.render.packet.v3") {
                             auto packet = transport::decode_render_packet(*bytes.value);
                             if (packet.ok() && version_matches(*packet.value)) {
                                 viewport_->setPacket(*packet.value);
                                 all_ids_.clear();
                                 for (const auto& point : packet.value->points)
                                     all_ids_.append(json_ledger::from_utf8(point.entity.value));
                                 for (const auto& beam : packet.value->beams)
                                     all_ids_.append(json_ledger::from_utf8(beam.entity.value));
                                 for (const auto& line : packet.value->geometry_lines)
                                     all_ids_.append(json_ledger::from_utf8(line.entity.value));
                                 for (const auto& cell : packet.value->cells)
                                     all_ids_.append(json_ledger::from_utf8(cell.entity.value));
                                 installed = true;
                             }
                         } else {
                             const auto delta = transport::decode_render_delta(*bytes.value);
                             installed = delta.ok() && version_matches(*delta.value) &&
                                         viewport_->applyDelta(*delta.value);
                         }
                         if (!installed) {
                             render_pending_ = false;
                             // A rejected delta cannot mutate VTK. The next request has no
                             // baseline.
                             const bool already_full = force_full_render_;
                             force_full_render_ = true;
                             if (!already_full)
                                 renderResource();
                             else
                                 statusBar()->showMessage(
                                     "Invalid display resource; scene was not replaced", 5000);
                             return;
                         }
                         finishRenderResource(data, doc, epoch, rev, view, view_rev);
                     });
             });
    }
    void renderView() {
        if (context_refresh_required_)
            return;
        if (client_.supportsResources()) {
            renderResource();
            return;
        }
        if (view_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_, view = view_id_;
        const auto view_rev = view_revision_;
        const auto generation = ++render_generation_;
        render_pending_ = true;
        call("view.render_data",
             {{"view_session_id", view}, {"expected_view_revision", view_rev}},
             context(true),
             [this, doc, epoch, rev, view, view_rev, generation](const QJsonObject& response) {
                 if (generation != render_generation_ || doc != document_id_ || epoch != epoch_ ||
                     rev != revision_ || view != view_id_ || view_rev != view_revision_)
                     return;
                 const auto recover = [this, doc, epoch, generation] {
                     render_pending_ = false;
                     force_full_render_ = true;
                     scene_invalidated_ = true;
                     scheduleDisplayCallback(250, [this, doc, epoch, generation] {
                         if (generation == render_generation_ && doc == document_id_ &&
                             epoch == epoch_)
                             pollCurrent();
                     });
                 };
                 if (!succeeded(response)) {
                     recover();
                     record("view.render_data", response);
                     return;
                 }
                 const auto packet = decodePacket(response.value("data").toObject(), doc, epoch);
                 if (!packet || QString::number(packet->revision) != rev ||
                     QString::fromStdString(packet->view_session_id) != view ||
                     QString::number(packet->view_revision) != view_rev) {
                     recover();
                     return;
                 }
                 view_revision_ = QString::number(packet->view_revision);
                 viewport_->setPacket(*packet);
                 rendered_version_ = RenderVersion{doc, epoch, rev, view, view_rev};
                 force_full_render_ = false;
                 scene_invalidated_ = false;
                 render_pending_ = false;
                 viewport_->setSelectedIds(selected_ids_);
                 all_ids_.clear();
                 for (const auto& point : packet->points)
                     all_ids_.append(QString::fromStdString(point.entity.value));
                 for (const auto& beam : packet->beams)
                     all_ids_.append(QString::fromStdString(beam.entity.value));
                 for (const auto& line : packet->geometry_lines)
                     all_ids_.append(QString::fromStdString(line.entity.value));
                 drainViewIntents();
                 evaluatePendingSelection();
             });
    }
    void requestViewRefresh() {
        // A model/camera refresh must not replace a user's pending hidden set.
        // The active callback or display installation drains the latest intent.
        if (view_update_pending_ || render_pending_)
            return;
        if (view_id_.isEmpty()) {
            createView();
            return;
        }
        const auto desired = queued_hidden_.value_or(hidden_ids_);
        queued_hidden_.reset();
        updateView(desired);
    }
    void updateView(const QStringList& hidden) {
        if (view_id_.isEmpty())
            return;
        // Keep the server projection and installed display baseline aligned. A camera
        // change may arrive while a large resource is still being transferred.
        if (view_update_pending_ || render_pending_) {
            queued_hidden_ = hidden;
            return;
        }
        view_update_pending_ = true;
        const auto generation = ++view_update_generation_;
        invalidateSelectionRequests(false);
        resources_.clear();
        ++render_generation_;
        const auto doc = document_id_, epoch = epoch_, rev = revision_, view = view_id_;
        const auto expected_view = view_revision_;
        const auto fingerprint = viewport_->cameraFingerprint();
        QJsonArray ids;
        json_ledger::ObjectCopies ids_copies;
        for (const auto& id : hidden) {
            ids.append(id);
            ids_copies.append(ids.last());
        }
        auto extra = context(true);
        const QJsonObject parameters{{"view_session_id", view},
                                     {"expected_view_revision", expected_view},
                                     {"hidden_ids", ids},
                                     {"camera_fingerprint", fingerprint}};
        json_ledger::object(
            parameters,
            {"view_session_id", "expected_view_revision", "hidden_ids", "camera_fingerprint"});
        call("view.update",
             parameters,
             extra,
             [this, doc, epoch, rev, view, hidden, fingerprint, expected_view, generation](
                 const QJsonObject& response) {
                 if (generation != view_update_generation_)
                     return;
                 view_update_pending_ = false;
                 if (doc != document_id_ || epoch != epoch_ || view != view_id_)
                     return;
                 if (rev != revision_) {
                     const auto queued = queued_hidden_.value_or(hidden);
                     queued_hidden_.reset();
                     const auto data = response.value("data").toObject();
                     const auto actual = unsignedRevision(data.value("view_revision"));
                     const auto previous = unsignedRevision(QJsonValue(expected_view));
                     if (succeeded(response) && data.value("view_session_id") == view && actual &&
                         previous &&
                         (*actual == *previous ||
                          (*previous < std::numeric_limits<std::uint64_t>::max() &&
                           *actual == *previous + 1))) {
                         // This old-model acknowledgement still proves the server's
                         // disposable view version; it cannot publish a display.
                         hidden_ids_ = hidden;
                         confirmed_camera_ = fingerprint;
                         view_revision_ = revisionText(data.value("view_revision"));
                         updateView(queued);
                     } else
                         createView(queued);
                     return;
                 }
                 if (!succeeded(response)) {
                     record("view.update", response);
                     force_full_render_ = true;
                     const auto code = response.value("error").toObject().value("code").toString();
                     if (code == "ENTITY_NOT_FOUND")
                         updateView({});
                     else if (response.value("status") == "conflict")
                         createView(queued_hidden_.value_or(hidden));
                     else
                         scheduleDisplayCallback(1000, [this] { pollCurrent(); });
                     return;
                 }
                 hidden_ids_ = hidden;
                 view_revision_ =
                     revisionText(response.value("data").toObject().value("view_revision"));
                 confirmed_camera_ = fingerprint;
                 renderView();
                 if (queued_hidden_) {
                     const auto queued = *queued_hidden_;
                     queued_hidden_.reset();
                     updateView(queued);
                 } else if (viewport_->cameraFingerprint() != fingerprint)
                     requestViewRefresh();
             });
    }
    void hideSelected() {
        auto hidden = hidden_ids_;
        for (const auto& id : selected_ids_)
            if (!hidden.contains(id))
                hidden.append(id);
        updateView(hidden);
    }
    void isolateSelected() {
        auto hidden = hidden_ids_;
        for (const auto& id : selected_ids_)
            hidden.removeAll(id);
        for (const auto& id : all_ids_)
            if (!selected_ids_.contains(id) && !hidden.contains(id))
                hidden.append(id);
        updateView(hidden);
    }
    bool viewportMatchesContext() const {
        return client_.ready() && !scene_invalidated_ && !force_full_render_ &&
               !view_update_pending_ && !render_pending_ && !queued_hidden_ &&
               !camera_update_.isActive() && !viewport_->pendingCameraUpdate() &&
               rendered_version_ && viewport_->cameraFingerprint() == confirmed_camera_ &&
               rendered_version_->document == document_id_ && rendered_version_->epoch == epoch_ &&
               rendered_version_->revision == revision_ && rendered_version_->view == view_id_ &&
               rendered_version_->view_revision == view_revision_;
    }
    void validateSelection(const QStringList& ids, bool through) {
        if (document_id_.isEmpty())
            return;
        invalidateSelectionRequests(false);
        pending_selection_ = PendingSelection{ids, through};
        evaluatePendingSelection();
    }
    void evaluatePendingSelection() {
        if (!pending_selection_ || pending_selection_evaluating_ || !viewportMatchesContext())
            return;
        const auto generation = selection_generation_;
        const auto through = pending_selection_->through;
        QJsonArray array;
        for (const auto& id : pending_selection_->ids)
            array.append(id);
        pending_selection_evaluating_ = true;
        auto extra = context(true);
        const auto view = view_id_, view_rev = view_revision_, rev = revision_;
        call("selection.evaluate",
             {{"view_session_id", view},
              {"expected_view_revision", view_rev},
              {"predicate", QJsonObject{{"op", "ids"}, {"ids", array}}},
              {"scope",
               QJsonObject{{"candidate_ids", array},
                           {"include_hidden", through},
                           {"visibility", through ? "through" : "picker_candidates"},
                           {"invert", false}}}},
             extra,
             [this, view, view_rev, rev, generation](const QJsonObject& response) {
                 if (!viewportMatchesContext() || generation != selection_generation_ ||
                     view != view_id_ || view_rev != view_revision_ || rev != revision_)
                     return;
                 if (!succeeded(response)) {
                     pending_selection_evaluating_ = false;
                     record("selection.evaluate", response);
                     return;
                 }
                 const auto handle =
                     response.value("data").toObject().value("selection_handle").toString();
                 if (handle.isEmpty()) {
                     pending_selection_evaluating_ = false;
                     return;
                 }
                 call("selection.get",
                      {{"selection_handle", handle}, {"offset", 0}, {"limit", 1000}},
                      context(),
                      [this, view, view_rev, rev, generation](const QJsonObject& result) {
                          if (!viewportMatchesContext() || generation != selection_generation_ ||
                              view != view_id_ || view_rev != view_revision_ || rev != revision_)
                              return;
                          if (!succeeded(result)) {
                              pending_selection_evaluating_ = false;
                              record("selection.get", result);
                              return;
                          }
                          QStringList selected;
                          for (const auto& id :
                               result.value("data").toObject().value("entity_ids").toArray())
                              selected.append(id.toString());
                          pending_selection_.reset();
                          pending_selection_evaluating_ = false;
                          selected_ids_ = selected;
                          viewport_->setSelectedIds(selected);
                          selected_label_->setText(selected.isEmpty() ? "No selection"
                                                                      : selected.join(", "));
                          loadSelectedProperty();
                      });
             });
    }
    void setPropertyCoordinateUnits(bool force) {
        for (std::size_t index = 0; index < property_coordinate_labels_.size(); ++index)
            property_coordinate_labels_[index]->setText(
                QString("%1 (%2)")
                    .arg(QChar('X' + static_cast<int>(index)))
                    .arg(force ? "N" : "mm"));
    }
    void clearPropertyFields() {
        selected_property_ = {};
        property_kind_->clear();
        property_details_->clear();
        property_details_->setMinimumHeight(0);
        setPropertyCoordinateUnits(false);
        for (auto* field : {property_x_, property_y_, property_z_, property_e_}) {
            field->clear();
            field->setEnabled(false);
        }
    }
    void loadSelectedProperty() {
        clearPropertyPreview();
        clearPropertyFields();
        modeling_->setSelectedGeometry({}, {});
        if (selected_ids_.size() != 1 || propertyRepliesBlocked())
            return;
        const auto generation = property_preview_generation_;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto selected_id = selected_ids_.front();
        call(
            "entity.query",
            {{"ids", QJsonArray{selected_id}}},
            context(),
            [this, doc, epoch, rev, selected_id, generation](const QJsonObject& response) {
                if (propertyRepliesBlocked() || generation != property_preview_generation_ ||
                    doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                    selected_ids_.size() != 1 || selected_ids_.front() != selected_id ||
                    !propertyReplyMatches(response, doc, epoch, rev))
                    return;
                const auto rows = response.value("data").toObject().value("entities").toArray();
                if (rows.size() != 1 || entityId(rows.at(0).toObject()) != selected_id)
                    return;
                selected_property_ = rows.at(0).toObject();
                const auto kind = selected_property_.value("kind").toString();
                property_kind_->setText(kind);
                property_details_->setText(selectedPhysicalDetails(selected_property_));
                // Preserve the wrapped reference identity as well as the value
                // when QFormLayout computes its compact row height.
                property_details_->setMinimumHeight(std::max(
                    0, property_details_->heightForWidth(std::max(1, property_details_->width()))));
                setPropertyCoordinateUnits(kind == "force");
                if (kind == "geometry")
                    modeling_->setSelectedGeometry(selected_id, entityLabel(selected_property_));
                const auto vector =
                    selected_property_.value(kind == "force" ? "force_n" : "position_mm");
                const auto xyz = vector.toArray();
                const bool coordinates_valid = finiteVector(vector);
                if (coordinates_valid) {
                    property_x_->setText(QString::number(xyz[0].toDouble(), 'g', 17));
                    property_y_->setText(QString::number(xyz[1].toDouble(), 'g', 17));
                    property_z_->setText(QString::number(xyz[2].toDouble(), 'g', 17));
                }
                const auto modulus = selected_property_.value("young_modulus_mpa");
                const bool modulus_valid = modulus.isDouble() && std::isfinite(modulus.toDouble());
                if (modulus_valid)
                    property_e_->setText(QString::number(
                        selected_property_.value("young_modulus_mpa").toDouble(), 'g', 17));
                for (auto* field : {property_x_, property_y_, property_z_})
                    field->setEnabled((kind == "node" || kind == "force") && coordinates_valid);
                property_e_->setEnabled(kind == "material" && modulus_valid);
            });
    }
    QJsonObject propertyCommand() const {
        if (selected_ids_.size() != 1 || propertyRepliesBlocked())
            return {};
        const auto kind = selected_property_.value("kind").toString();
        QJsonObject command;
        bool ok = false;
        if (kind == "node" || kind == "force") {
            const auto x = property_x_->text().toDouble(&ok);
            if (!ok)
                return {};
            const auto y = property_y_->text().toDouble(&ok);
            if (!ok)
                return {};
            const auto z = property_z_->text().toDouble(&ok);
            if (!ok)
                return {};
            if (kind == "node")
                command = {{"command", "node.move"},
                           {"entity_id", selected_ids_.front()},
                           {"position_mm", QJsonArray{x, y, z}}};
            else
                command = {{"command", "force.set_vector"},
                           {"force_id", selected_ids_.front()},
                           {"x", QJsonObject{{"value", x}, {"unit", "N"}}},
                           {"y", QJsonObject{{"value", y}, {"unit", "N"}}},
                           {"z", QJsonObject{{"value", z}, {"unit", "N"}}}};
        } else if (kind == "material") {
            const auto modulus = property_e_->text().toDouble(&ok);
            if (!ok)
                return {};
            command = {{"command", "material.set_young_modulus"},
                       {"entity_id", selected_ids_.front()},
                       {"young_modulus", QJsonObject{{"value", modulus}, {"unit", "MPa"}}}};
        }
        return command;
    }
    void clearPropertyPreview() {
        ++property_preview_generation_;
        property_preview_id_.clear();
        property_preview_context_ = {};
        property_cancel_->setEnabled(false);
        setProperty("propertyPreviewId", QVariant{});
    }
    void previewProperty() {
        const auto command = propertyCommand();
        if (command.isEmpty())
            return;
        clearPropertyPreview();
        const auto generation = property_preview_generation_;
        const auto expected = context(true);
        const auto [operation, parameters] = propertyPreviewRequest(command);
        call(operation,
             parameters,
             expected,
             [this, generation, expected, operation](const QJsonObject& response) {
                 if (propertyRepliesBlocked() || generation != property_preview_generation_ ||
                     expected != context(true))
                     return;
                 if (!succeeded(response)) {
                     record(operation, response);
                     return;
                 }
                 if (!propertyReplyMatches(response,
                                           expected.value("document_id").toString(),
                                           expected.value("document_epoch").toString(),
                                           expected.value("expected_revision").toString()))
                     return;
                 record(operation, response);
                 property_preview_id_ =
                     response.value("data").toObject().value("preview_id").toString();
                 property_preview_context_ = expected;
                 setProperty("propertyPreviewId", property_preview_id_);
                 property_cancel_->setEnabled(!property_preview_id_.isEmpty());
                 statusBar()->showMessage("Property preview ready. Apply or cancel.", 5000);
             });
    }
    void applyProperty() {
        if (propertyRepliesBlocked()) {
            clearPropertyPreview();
            clearPropertyFields();
            return;
        }
        if (!property_preview_id_.isEmpty()) {
            if (property_preview_context_ != context(true)) {
                clearPropertyPreview();
                return;
            }
            const auto preview = property_preview_id_;
            auto expected = property_preview_context_;
            expected.insert("idempotency_key", uniqueKey());
            clearPropertyPreview();
            call("changes.commit",
                 {{"preview_id", preview}},
                 expected,
                 [this](const QJsonObject& response) {
                     record("changes.commit", response);
                     if (succeeded(response))
                         pollCurrent();
                 });
            return;
        }
        const auto command = propertyCommand();
        if (!command.isEmpty())
            previewAndCommit(command);
    }

    void navigateIssue(const QString& identity, const QJsonObject& expected) {
        if (identity.isEmpty() || expected != context(true))
            return;
        call("entity.query",
             {{"ids", QJsonArray{identity}}},
             expected,
             [this, identity, expected](const QJsonObject& response) {
                 if (expected != context(true) || !succeeded(response))
                     return;
                 const auto rows = response.value("data").toObject().value("entities").toArray();
                 if (rows.size() != 1 || entityId(rows.at(0).toObject()) != identity)
                     return;
                 view_kind_->setCurrentText("all");
                 for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                     auto* item = tree_->topLevelItem(index);
                     if (item->data(0, Qt::UserRole).toString() == identity) {
                         tree_->setCurrentItem(item);
                         tree_->scrollToItem(item);
                         break;
                     }
                 }
                 validateSelection({identity}, true);
             });
    }

    DesktopClient client_;
    ResourceClient resources_;
    std::uint64_t render_generation_{}, subscription_generation_{}, event_generation_{};
    std::uint64_t view_update_generation_{};
    bool force_full_render_{true}, scene_invalidated_{}, subscription_pending_{}, current_queued_{};
    bool render_pending_{};
    bool context_refresh_required_{};
    bool smoke_{};
    QString screenshot_path_;
    bool screenshot_done_{};
    QTimer poll_;
    QTimer camera_update_;
    QTimer screenshot_retry_;
    bool current_pending_{};
    std::size_t pending_display_callbacks_{};
    bool view_update_pending_{};
    std::optional<QStringList> queued_hidden_;
    struct RenderVersion {
        QString document, epoch, revision, view, view_revision;
    };
    std::optional<RenderVersion> rendered_version_;
    struct PendingSelection {
        QStringList ids;
        bool through;
    };
    std::optional<PendingSelection> pending_selection_;
    bool pending_selection_evaluating_{};
    std::uint64_t selection_generation_{};
    std::uint64_t property_preview_generation_{};
    QString property_preview_id_;
    QJsonObject property_preview_context_;
    QString document_id_, epoch_, revision_, saved_path_, view_id_, view_revision_,
        confirmed_camera_;
    bool dirty_{};
    QStringList selected_ids_, hidden_ids_, all_ids_;
    QJsonArray owner_rows_;
    QHash<QString, int> owner_row_indices_;
    QHash<QString, QTreeWidgetItem*> tree_items_;
    QJsonObject selected_property_;
    VtkView* viewport_{};
    qcae::ModelingTools* modeling_{};
    qcae::AnalysisTools* analysis_tools_{};
    QComboBox* view_kind_{};
    QComboBox* owner_{};
    QTreeWidget* tree_{};
    QLabel* tree_count_{};
    QLabel* selected_label_{};
    QLabel* property_kind_{};
    QLabel* property_details_{};
    std::array<QLabel*, 3> property_coordinate_labels_{};
    QLineEdit *property_x_{}, *property_y_{}, *property_z_{}, *property_e_{};
    QPushButton* apply_{};
    QPushButton* property_cancel_{};
    QPlainTextEdit* log_{};
    json_ledger::PlainTextAppendCopies log_copies_;
    std::shared_ptr<qcae::sdk_copy::Tracker> sdk_copies_ =
        std::make_shared<qcae::sdk_copy::Tracker>(std::initializer_list<qcae::sdk_copy::Component>{
            qcae::sdk_copy::Component::tree_style_option,
            qcae::sdk_copy::Component::tree_paint,
            qcae::sdk_copy::Component::tree_size_hint,
            qcae::sdk_copy::Component::log_document_change,
            qcae::sdk_copy::Component::log_block_layout,
            qcae::sdk_copy::Component::log_paint,
            qcae::sdk_copy::Component::log_paint_block_text,
            qcae::sdk_copy::Component::font_glyph_buffers});

  public:
    qcae::SdkCopySnapshot sdkCopyObservation() const {
        return sdk_copies_->snapshot();
    }
};
} // namespace

QMainWindow* qcae::create_desktop_window(DesktopClient::Options options) {
    return new DesktopWindow(std::move(options), false, {});
}

bool qcae::desktop_pipeline_idle(const QMainWindow& window) {
    const auto* desktop = dynamic_cast<const DesktopWindow*>(&window);
    return desktop && desktop->pipelineIdle();
}

qcae::SdkCopySnapshot qcae::desktop_sdk_copy_observation(const QMainWindow& window) {
    const auto* desktop = dynamic_cast<const DesktopWindow*>(&window);
    return desktop ? desktop->sdkCopyObservation() : SdkCopySnapshot{};
}

int qcae::run_desktop(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("QCAE");
    QCommandLineParser parser;
    parser.setApplicationDescription("QCAE local desktop");
    parser.addHelpOption();
    parser.addOption({{"s", "socket"}, "Local engine endpoint", "path"});
    parser.addOption({"workspace", "Local SQLite recovery database", "path"});
    parser.addOption({"engine", "Engine executable", "path"});
    parser.addOption({"smoke", "Open workspace for UI smoke test"});
    parser.addOption({"screenshot", "Save full desktop PNG after a real packet arrives", "path"});
    parser.addOption({"quit-after-ms", "Exit smoke mode after this many milliseconds", "ms"});
    parser.process(app);
    const auto endpoint =
        parser.isSet("socket") ? parser.value("socket") : qcae::transport::default_endpoint();
    const auto workspace =
        parser.isSet("workspace")
            ? parser.value("workspace")
            : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                  "/working.sqlite";
    if (endpoint.isEmpty() || !QFileInfo(endpoint).isAbsolute() || workspace.isEmpty() ||
        !QFileInfo(workspace).isAbsolute() || !QDir().mkpath(QFileInfo(workspace).absolutePath()))
        return 2;
    const auto screenshot = parser.value("screenshot");
    bool valid_delay = false;
    const auto quit_after_ms = parser.value("quit-after-ms").toInt(&valid_delay);
    if ((!screenshot.isEmpty() &&
         (!parser.isSet("smoke") || !QFileInfo(screenshot).isAbsolute())) ||
        (parser.isSet("quit-after-ms") &&
         (!parser.isSet("smoke") || !valid_delay || quit_after_ms <= 0)))
        return 2;
    DesktopWindow window(
        {endpoint, workspace, parser.value("engine"), true}, parser.isSet("smoke"), screenshot);
    window.show();
    if (parser.isSet("quit-after-ms"))
        QTimer::singleShot(quit_after_ms, &app, [&app, &window, screenshot] {
            app.exit(screenshot.isEmpty() || window.screenshotDone() ? 0 : 5);
        });
    return app.exec();
}
