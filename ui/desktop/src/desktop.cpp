#include "qcae/desktop.hpp"
#include "modeling_tools.hpp"
#include "qcae/desktop_client.hpp"
#include "qcae/local_endpoint.hpp"
#include "qcae/render_packet.hpp"
#include "qcae/vtk_view.hpp"

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <array>
#include <cmath>
#include <functional>
#include <optional>

namespace {
using qcae::DesktopClient;
using qcae::VtkView;
using Done = std::function<void(bool)>;

QString errorText(const QJsonObject& response) {
    const auto error = response.value("error").toObject();
    return error.value("code").toString() + ": " + error.value("message").toString();
}
bool succeeded(const QJsonObject& response) {
    return response.value("status").toString() == "success";
}
QString uniqueKey() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString revisionText(const QJsonValue& value) {
    return value.isString() ? value.toString() : QString::number(value.toInteger());
}
QString entityId(const QJsonObject& object) {
    return object.value("entity_id").toString(object.value("entity").toString());
}
QString entityLabel(const QJsonObject& object) {
    const auto name = object.value("name").toString().trimmed();
    if (!name.isEmpty())
        return name;
    auto kind = object.value("kind").toString("Entity");
    if (!kind.isEmpty())
        kind[0] = kind[0].toUpper();
    const auto sources = object.value("sources").toArray();
    for (const auto& value : sources) {
        const auto number = value.toObject().value("number").toString();
        if (!number.isEmpty())
            return kind + " " + number;
    }
    return kind + " · " + entityId(object).left(8);
}

std::optional<qcae::RenderPacket>
decodePacket(const QJsonObject& data, const QString& document, const QString& epoch) {
    if (data.value("document_id").toString(document) != document ||
        data.value("document_epoch").toString(epoch) != epoch)
        return std::nullopt;
    qcae::RenderPacket packet;
    packet.document = {qcae::DocumentId{document.toStdString()},
                       qcae::DocumentEpoch{epoch.toStdString()}};
    packet.revision = revisionText(data.value("revision")).toULongLong();
    packet.view_session_id = data.value("view_session_id").toString().toStdString();
    packet.view_revision = revisionText(data.value("view_revision")).toULongLong();
    if (packet.view_session_id.empty() || !data.value("points").isArray() ||
        !data.value("beams").isArray())
        return std::nullopt;
    for (const auto& value : data.value("points").toArray()) {
        const auto row = value.toObject();
        const auto xyz = row.value("position_mm").toArray();
        const auto id = entityId(row);
        if (id.isEmpty() || xyz.size() != 3)
            return std::nullopt;
        packet.points.push_back({qcae::EntityId{id.toStdString()},
                                 {xyz[0].toDouble(), xyz[1].toDouble(), xyz[2].toDouble()},
                                 row.value("visible").toBool(true)});
    }
    for (const auto& value : data.value("beams").toArray()) {
        const auto row = value.toObject();
        const auto ends = row.value("points").toArray();
        const auto id = entityId(row);
        if (id.isEmpty() || ends.size() != 2 || !ends[0].isDouble() || !ends[1].isDouble())
            return std::nullopt;
        const auto a = ends[0].toInteger(-1);
        const auto b = ends[1].toInteger(-1);
        if (a < 0 || b < 0 || a >= static_cast<qint64>(packet.points.size()) ||
            b >= static_cast<qint64>(packet.points.size()))
            return std::nullopt;
        packet.beams.push_back({qcae::EntityId{id.toStdString()},
                                {static_cast<std::size_t>(a), static_cast<std::size_t>(b)}});
    }
    if (data.contains("geometry_lines") && !data.value("geometry_lines").isArray())
        return std::nullopt;
    for (const auto& value : data.value("geometry_lines").toArray()) {
        const auto row = value.toObject();
        const auto start = row.value("start_mm").toArray(), end = row.value("end_mm").toArray();
        const auto id = entityId(row);
        if (id.isEmpty() || start.size() != 3 || end.size() != 3)
            return std::nullopt;
        qcae::RenderGeometryLine line{qcae::EntityId{id.toStdString()}, {}, {}};
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
        : client_(std::move(options), this), smoke_(smoke),
          screenshot_path_(std::move(screenshot)) {
        setWindowTitle("QCAE");
        resize(1400, 850);
        buildUi();
        connect(&client_, &DesktopClient::readyChanged, this, [this](bool ready) {
            statusBar()->showMessage(ready ? "Connected to local engine"
                                           : "Connecting to local engine");
            if (ready)
                pollCurrent();
            else {
                modeling_->suspend();
                clearDocument(false);
            }
        });
        connect(&client_, &DesktopClient::transportError, this, [this](const QString& message) {
            statusBar()->showMessage(message, 5000);
        });
        poll_.setInterval(500);
        connect(&poll_, &QTimer::timeout, this, [this] { pollCurrent(); });
        poll_.start();
        camera_update_.setSingleShot(true);
        camera_update_.setInterval(200);
        connect(&camera_update_, &QTimer::timeout, this, [this] { updateView(hidden_ids_); });
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

    [[nodiscard]] bool screenshotDone() const {
        return screenshot_done_;
    }

  private:
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
        view_kind_->addItems({"all", "part", "assembly", "set", "include", "material", "property"});
        owner_ = new QComboBox(left_content);
        owner_->setEnabled(false);
        owner_->setToolTip("Owners are listed from the first 1,000 model entities");
        tree_count_ = new QLabel("No active project", left_content);
        tree_ = new QTreeWidget(left_content);
        tree_->setObjectName("entityTree");
        tree_->setHeaderLabels({"Entity", "Kind"});
        left_layout->addWidget(view_kind_);
        left_layout->addWidget(owner_);
        left_layout->addWidget(tree_count_);
        left_layout->addWidget(tree_);
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
        apply_ = new QPushButton("Apply change", right_content);
        form->addRow("Entity", selected_label_);
        form->addRow("Kind", property_kind_);
        form->addRow("X (mm)", property_x_);
        form->addRow("Y (mm)", property_y_);
        form->addRow("Z (mm)", property_z_);
        form->addRow("E (MPa)", property_e_);
        form->addRow(apply_);
        right->setWidget(right_content);
        addDockWidget(Qt::RightDockWidgetArea, right);
        connect(apply_, &QPushButton::clicked, this, [this] { applyProperty(); });

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

        auto* bottom = new QDockWidget("Operations and history", this);
        log_ = new QPlainTextEdit(bottom);
        log_->setReadOnly(true);
        bottom->setWidget(log_);
        addDockWidget(Qt::BottomDockWidgetArea, bottom);
        statusBar()->showMessage("Connecting to local engine");

        auto* file = menuBar()->addMenu("File");
        auto* new_action =
            addActionTo(file, "New project", [this] { createProject(); }, QKeySequence::New);
        auto* open_action =
            addActionTo(file, "Open project…", [this] { openProject(); }, QKeySequence::Open);
        addActionTo(file, "Recover workspace", [this] { recoverProject(); });
        auto* save_action =
            addActionTo(file, "Save", [this] { saveProject(false); }, QKeySequence::Save);
        addActionTo(file, "Save as…", [this] { saveProject(true); }, QKeySequence::SaveAs);
        addActionTo(file, "Import BDF…", [this] { importBdf(); });
        addActionTo(file, "Close project…", [this] { closeProject({}); });
        auto* edit = menuBar()->addMenu("Edit");
        auto* undo_action = addActionTo(
            edit, "Undo", [this] { historyAction("history.undo"); }, QKeySequence::Undo);
        auto* redo_action = addActionTo(
            edit, "Redo", [this] { historyAction("history.redo"); }, QKeySequence::Redo);
        auto* model_menu = menuBar()->addMenu("Model");
        addActionTo(model_menu, "Modeling tools", [modeling_dock] {
            modeling_dock->show();
            modeling_dock->raise();
        });
        auto* display = menuBar()->addMenu("Display");
        auto* fit_action = addActionTo(display, "Fit", [this] { viewport_->fit(); });
        addActionTo(
            display, "Front", [this] { viewport_->standardView(VtkView::StandardView::front); });
        addActionTo(
            display, "Top", [this] { viewport_->standardView(VtkView::StandardView::top); });
        addActionTo(
            display, "Right", [this] { viewport_->standardView(VtkView::StandardView::right); });
        addActionTo(display, "Isometric", [this] {
            viewport_->standardView(VtkView::StandardView::isometric);
        });
        addActionTo(display, "Hide selected", [this] { hideSelected(); });
        addActionTo(display, "Isolate selected", [this] { isolateSelected(); });
        addActionTo(display, "Show all", [this] { updateView({}); });
        auto* workspace_toolbar = addToolBar("Workspace");
        for (auto* action :
             {new_action, open_action, save_action, undo_action, redo_action, fit_action})
            workspace_toolbar->addAction(action);
        auto* toolbar = addToolBar("Selection");
        auto* through = toolbar->addAction("Through selection");
        through->setCheckable(true);
        connect(through, &QAction::toggled, viewport_, &VtkView::setThroughSelection);
        auto* contained = toolbar->addAction("Shift+drag box contained");
        contained->setCheckable(true);
        connect(contained, &QAction::toggled, this, [this](bool checked) {
            viewport_->setBoxMode(checked ? VtkView::BoxMode::contained
                                          : VtkView::BoxMode::intersecting);
        });
    }

    QAction* addActionTo(QMenu* menu,
                         const QString& title,
                         const std::function<void()>& callback,
                         const QKeySequence& shortcut = {}) {
        auto* action = menu->addAction(title);
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, callback);
        return action;
    }
    QJsonObject context(bool write = false) const {
        QJsonObject result{{"document_id", document_id_}, {"document_epoch", epoch_}};
        if (write)
            result.insert("expected_revision", revision_);
        return result;
    }
    void call(const QString& operation,
              const QJsonObject& parameters,
              const QJsonObject& extra,
              DesktopClient::Reply callback) {
        (void)client_.request(operation, parameters, extra, std::move(callback));
    }
    void record(const QString& operation, const QJsonObject& response) {
        const auto message = succeeded(response) ? operation + " succeeded" : errorText(response);
        log_->appendPlainText(message);
        statusBar()->showMessage(message, 5000);
    }
    void invalidateSelectionRequests(bool clear_pending) {
        ++selection_generation_;
        pending_selection_evaluating_ = false;
        if (clear_pending)
            pending_selection_.reset();
    }
    void clearDocument(bool reset_tools = true) {
        invalidateSelectionRequests(true);
        rendered_version_.reset();
        if (reset_tools)
            modeling_->setContext({});
        if (document_id_.isEmpty() && view_id_.isEmpty())
            return;
        document_id_.clear();
        epoch_.clear();
        revision_.clear();
        view_id_.clear();
        view_revision_.clear();
        view_update_pending_ = false;
        queued_hidden_.reset();
        tree_->clear();
        tree_count_->setText("No active project");
        owner_rows_ = {};
        rebuildOwnerChoices();
        selected_ids_.clear();
        hidden_ids_.clear();
        all_ids_.clear();
        selected_property_ = {};
        qcae::RenderPacket empty;
        empty.document = {qcae::DocumentId{}, qcae::DocumentEpoch{}};
        viewport_->setPacket(empty);
        selected_label_->setText("No selection");
    }
    void pollCurrent() {
        if (!client_.ready() || current_pending_)
            return;
        current_pending_ = true;
        call("project.current", {}, {}, [this](const QJsonObject& response) {
            current_pending_ = false;
            if (!client_.ready())
                return;
            if (!succeeded(response)) {
                if (response.value("error").toObject().value("code") == "DOCUMENT_NOT_FOUND")
                    clearDocument();
                return;
            }
            const auto data = response.value("data").toObject();
            const auto id = data.value("document_id").toString();
            const auto epoch = data.value("document_epoch").toString();
            const auto rev = revisionText(data.value("revision"));
            if (id.isEmpty() || epoch.isEmpty())
                return;
            const bool changed = id != document_id_ || epoch != epoch_ || rev != revision_;
            const bool new_document = id != document_id_ || epoch != epoch_;
            document_id_ = id;
            epoch_ = epoch;
            revision_ = rev;
            modeling_->setContext(context(true));
            saved_path_ = data.value("saved_path").toString();
            dirty_ = data.value("dirty").toBool();
            setWindowTitle(data.value("name").toString("QCAE") +
                           (dirty_ ? " * — QCAE" : " — QCAE"));
            if (changed) {
                invalidateSelectionRequests(true);
                rendered_version_.reset();
                selected_ids_.clear();
                viewport_->setSelectedIds({});
                selected_label_->setText("No selection");
                loadSelectedProperty();
                if (new_document) {
                    hidden_ids_.clear();
                    all_ids_.clear();
                    view_id_.clear();
                    view_revision_.clear();
                    owner_rows_ = {};
                    rebuildOwnerChoices();
                }
                owner_->setEnabled(false);
                loadOwners();
                loadHistory();
                if (view_id_.isEmpty())
                    createView();
                else
                    updateView(hidden_ids_);
            }
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
        call(
            "changes.preview", parameters, expected, [this, expected](const QJsonObject& response) {
                record("changes.preview", response);
                if (!succeeded(response))
                    return;
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

    void loadTree(int offset = 0) {
        if (document_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto view = view_kind_->currentText();
        const auto owner_id = owner_->currentData().toString();
        if (view != "all" && owner_id.isEmpty()) {
            tree_->clear();
            tree_count_->setText("Choose a " + view + " owner");
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
                 if (offset == 0)
                     tree_->clear();
                 const auto data = response.value("data").toObject();
                 const auto rows = data.value("entities").toArray();
                 for (const auto& value : rows) {
                     const auto row = value.toObject();
                     auto* item = new QTreeWidgetItem(
                         tree_, {entityLabel(row), row.value("kind").toString()});
                     item->setData(0, Qt::UserRole, entityId(row));
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
            log_->appendPlainText("History at revision " + rev + ": " +
                                  QString::number(rows.size()) + " transactions");
            for (const auto& value : rows) {
                const auto item = value.toObject();
                log_->appendPlainText(QString(item.value("applied").toBool() ? "  ✓ " : "  ○ ") +
                                      item.value("label").toString());
            }
        });
    }
    void createView() {
        if (document_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        QJsonArray hidden;
        for (const auto& id : hidden_ids_)
            hidden.append(id);
        call("view.create",
             {{"hidden_ids", hidden}, {"camera_fingerprint", viewport_->cameraFingerprint()}},
             context(true),
             [this, doc, epoch, rev](const QJsonObject& response) {
                 if (doc != document_id_ || epoch != epoch_ || rev != revision_)
                     return;
                 if (!succeeded(response)) {
                     record("view.create", response);
                     return;
                 }
                 const auto data = response.value("data").toObject();
                 invalidateSelectionRequests(false);
                 rendered_version_.reset();
                 view_id_ = data.value("view_session_id").toString();
                 view_revision_ = revisionText(data.value("view_revision"));
                 renderView();
             });
    }
    void renderView() {
        if (view_id_.isEmpty())
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_, view = view_id_;
        const auto view_rev = view_revision_;
        call("view.render_data",
             {{"view_session_id", view}, {"expected_view_revision", view_rev}},
             context(true),
             [this, doc, epoch, rev, view, view_rev](const QJsonObject& response) {
                 if (doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                     view != view_id_ || view_rev != view_revision_)
                     return;
                 if (!succeeded(response)) {
                     record("view.render_data", response);
                     return;
                 }
                 const auto packet = decodePacket(response.value("data").toObject(), doc, epoch);
                 if (!packet || QString::number(packet->revision) != rev ||
                     QString::fromStdString(packet->view_session_id) != view ||
                     QString::number(packet->view_revision) != view_rev)
                     return;
                 view_revision_ = QString::number(packet->view_revision);
                 viewport_->setPacket(*packet);
                 rendered_version_ = RenderVersion{doc, epoch, rev, view, view_rev};
                 viewport_->setSelectedIds(selected_ids_);
                 all_ids_.clear();
                 for (const auto& point : packet->points)
                     all_ids_.append(QString::fromStdString(point.entity.value));
                 for (const auto& beam : packet->beams)
                     all_ids_.append(QString::fromStdString(beam.entity.value));
                 for (const auto& line : packet->geometry_lines)
                     all_ids_.append(QString::fromStdString(line.entity.value));
                 evaluatePendingSelection();
             });
    }
    void updateView(const QStringList& hidden) {
        if (view_id_.isEmpty())
            return;
        if (view_update_pending_) {
            queued_hidden_ = hidden;
            return;
        }
        view_update_pending_ = true;
        invalidateSelectionRequests(false);
        rendered_version_.reset();
        const auto doc = document_id_, epoch = epoch_, rev = revision_, view = view_id_;
        const auto expected_view = view_revision_;
        const auto fingerprint = viewport_->cameraFingerprint();
        QJsonArray ids;
        for (const auto& id : hidden)
            ids.append(id);
        auto extra = context(true);
        call("view.update",
             {{"view_session_id", view},
              {"expected_view_revision", expected_view},
              {"hidden_ids", ids},
              {"camera_fingerprint", fingerprint}},
             extra,
             [this, doc, epoch, rev, view, hidden, fingerprint](const QJsonObject& response) {
                 view_update_pending_ = false;
                 if (doc != document_id_ || epoch != epoch_ || view != view_id_)
                     return;
                 if (rev != revision_) {
                     const auto queued = queued_hidden_.value_or(hidden);
                     queued_hidden_.reset();
                     updateView(queued);
                     return;
                 }
                 if (!succeeded(response)) {
                     record("view.update", response);
                     const auto code = response.value("error").toObject().value("code").toString();
                     if (code == "ENTITY_NOT_FOUND")
                         updateView({});
                     else if (response.value("status") == "conflict")
                         createView();
                     return;
                 }
                 hidden_ids_ = hidden;
                 view_revision_ =
                     revisionText(response.value("data").toObject().value("view_revision"));
                 renderView();
                 if (queued_hidden_) {
                     const auto queued = *queued_hidden_;
                     queued_hidden_.reset();
                     updateView(queued);
                 } else if (viewport_->cameraFingerprint() != fingerprint)
                     updateView(hidden_ids_);
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
        return client_.ready() && !view_update_pending_ && rendered_version_ &&
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
                 if (generation != selection_generation_ || view != view_id_ ||
                     view_rev != view_revision_ || rev != revision_)
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
                          if (generation != selection_generation_ || view != view_id_ ||
                              view_rev != view_revision_ || rev != revision_)
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
    void loadSelectedProperty() {
        selected_property_ = {};
        modeling_->setSelectedGeometry({}, {});
        property_kind_->clear();
        for (auto* field : {property_x_, property_y_, property_z_, property_e_})
            field->clear();
        if (selected_ids_.size() != 1)
            return;
        const auto doc = document_id_, epoch = epoch_, rev = revision_;
        const auto selected_id = selected_ids_.front();
        call("entity.query",
             {{"ids", QJsonArray{selected_id}}},
             context(),
             [this, doc, epoch, rev, selected_id](const QJsonObject& response) {
                 if (doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                     selected_ids_.size() != 1 || selected_ids_.front() != selected_id ||
                     !succeeded(response))
                     return;
                 const auto rows = response.value("data").toObject().value("entities").toArray();
                 if (rows.size() != 1 || entityId(rows.at(0).toObject()) != selected_id)
                     return;
                 selected_property_ = rows.at(0).toObject();
                 const auto kind = selected_property_.value("kind").toString();
                 property_kind_->setText(kind);
                 if (kind == "geometry")
                     modeling_->setSelectedGeometry(selected_id, entityLabel(selected_property_));
                 const auto xyz = selected_property_.value("position_mm").toArray();
                 if (xyz.size() == 3) {
                     property_x_->setText(QString::number(xyz[0].toDouble(), 'g', 17));
                     property_y_->setText(QString::number(xyz[1].toDouble(), 'g', 17));
                     property_z_->setText(QString::number(xyz[2].toDouble(), 'g', 17));
                 }
                 if (selected_property_.contains("young_modulus_mpa"))
                     property_e_->setText(QString::number(
                         selected_property_.value("young_modulus_mpa").toDouble(), 'g', 17));
                 for (auto* field : {property_x_, property_y_, property_z_})
                     field->setEnabled(kind == "node");
                 property_e_->setEnabled(kind == "material");
             });
    }
    void applyProperty() {
        if (selected_ids_.size() != 1)
            return;
        const auto kind = selected_property_.value("kind").toString();
        QJsonObject command;
        bool ok = false;
        if (kind == "node") {
            const auto x = property_x_->text().toDouble(&ok);
            if (!ok)
                return;
            const auto y = property_y_->text().toDouble(&ok);
            if (!ok)
                return;
            const auto z = property_z_->text().toDouble(&ok);
            if (!ok)
                return;
            command = {{"command", "node.move"},
                       {"entity_id", selected_ids_.front()},
                       {"position_mm", QJsonArray{x, y, z}}};
        } else if (kind == "material") {
            const auto modulus = property_e_->text().toDouble(&ok);
            if (!ok)
                return;
            command = {{"command", "material.set_young_modulus"},
                       {"entity_id", selected_ids_.front()},
                       {"young_modulus", QJsonObject{{"value", modulus}, {"unit", "MPa"}}}};
        }
        if (!command.isEmpty())
            previewAndCommit(command);
    }

    DesktopClient client_;
    bool smoke_{};
    QString screenshot_path_;
    bool screenshot_done_{};
    QTimer poll_;
    QTimer camera_update_;
    QTimer screenshot_retry_;
    bool current_pending_{};
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
    QString document_id_, epoch_, revision_, saved_path_, view_id_, view_revision_;
    bool dirty_{};
    QStringList selected_ids_, hidden_ids_, all_ids_;
    QJsonArray owner_rows_;
    QJsonObject selected_property_;
    VtkView* viewport_{};
    qcae::ModelingTools* modeling_{};
    QComboBox* view_kind_{};
    QComboBox* owner_{};
    QTreeWidget* tree_{};
    QLabel* tree_count_{};
    QLabel* selected_label_{};
    QLabel* property_kind_{};
    QLineEdit *property_x_{}, *property_y_{}, *property_z_{}, *property_e_{};
    QPushButton* apply_{};
    QPlainTextEdit* log_{};
};
} // namespace

QMainWindow* qcae::create_desktop_window(DesktopClient::Options options) {
    return new DesktopWindow(std::move(options), false, {});
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
