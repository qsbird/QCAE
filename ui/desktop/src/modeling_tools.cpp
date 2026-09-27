#include "modeling_tools.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace qcae {
namespace {
bool succeeded(const QJsonObject& reply) {
    return reply.value("status") == "success";
}
QString errorText(const QJsonObject& reply) {
    return reply.value("error").toObject().value("message").toString("Operation failed");
}
bool terminal(const QString& state) {
    return state == "succeeded" || state == "cancelled" || state == "conflicted" ||
           state == "failed" || state == "interrupted" || state == "outcome_unknown";
}
} // namespace
ModelingTools::ModelingTools(DesktopClient& client, Actions actions, QWidget* parent)
    : QWidget(parent), client_(client), actions_(std::move(actions)) {
    setObjectName("modelingTools");
    auto* layout = new QVBoxLayout(this);
    mode_ = new QComboBox(this);
    mode_->setObjectName("modelingMode");
    mode_->addItems({"Create line", "Generate line mesh"});
    layout->addWidget(mode_);
    line_fields_ = new QWidget(this);
    auto* form = new QFormLayout(line_fields_);
    const std::array names{"startX", "startY", "startZ", "endX", "endY", "endZ"};
    const std::array labels{
        "Start X (mm)", "Start Y (mm)", "Start Z (mm)", "End X (mm)", "End Y (mm)", "End Z (mm)"};
    for (std::size_t i = 0; i < coordinates_.size(); ++i) {
        auto* field = new QDoubleSpinBox(line_fields_);
        coordinates_[i] = field;
        field->setObjectName(names[i]);
        field->setDecimals(6);
        field->setRange(-1e9, 1e9);
        field->setValue(i == 3 ? 1000.0 : 0.0);
        form->addRow(labels[i], field);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { invalidatePreview(); });
    }
    layout->addWidget(line_fields_);
    mesh_fields_ = new QWidget(this);
    auto* mesh = new QFormLayout(mesh_fields_);
    geometry_ = new QLabel("Select a geometry line in the model or viewport", mesh_fields_);
    geometry_->setWordWrap(true);
    geometry_->setObjectName("meshGeometry");
    segments_ = new QSpinBox(mesh_fields_);
    segments_->setObjectName("meshSegments");
    segments_->setRange(1, 100000);
    segments_->setValue(10);
    mesh->addRow("Geometry", geometry_);
    mesh->addRow("Segments", segments_);
    layout->addWidget(mesh_fields_);
    status_ = new QLabel("Open or create a project to start", this);
    status_->setObjectName("modelingStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName("meshProgress");
    progress_->setRange(0, 100);
    progress_->setValue(0);
    layout->addWidget(progress_);
    auto* buttons = new QHBoxLayout;
    preview_ = new QPushButton("Preview", this);
    apply_ = new QPushButton("Apply", this);
    cancel_ = new QPushButton("Cancel", this);
    preview_->setObjectName("modelingPreview");
    apply_->setObjectName("modelingApply");
    cancel_->setObjectName("modelingCancel");
    buttons->addWidget(preview_);
    buttons->addWidget(apply_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        invalidatePreview();
        status_->setText(lineMode() ? "Preview the line before applying"
                                    : "Generate a new uniform mesh from the selected line");
        updateControls();
    });
    connect(preview_, &QPushButton::clicked, this, [this] { previewLine(); });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { cancel(); });
    task_poll_.setInterval(100);
    connect(&task_poll_, &QTimer::timeout, this, [this] { pollTask(); });
    updateControls();
}
bool ModelingTools::lineMode() const {
    return mode_->currentIndex() == 0;
}
bool ModelingTools::activeTask() const {
    return !task_id_.isEmpty() && !terminal(task_state_);
}
void ModelingTools::suspend() {
    // Transport loss is not project closure. Keep the admitted task or immutable write
    // intent until project.current identifies the reconnected document and epoch.
    ++session_;
    suspended_ = true;
    outcome_unknown_ = outcome_unknown_ || submitting_;
    busy_ = submitting_ = polling_ = false;
    task_poll_.stop();
    status_->setText("Connection lost. Waiting for the active project.");
    invalidatePreview();
}
void ModelingTools::setContext(const QJsonObject& context) {
    if (context_ == context && !suspended_)
        return;
    suspended_ = false;
    const bool session_changed =
        context_.value("document_id") != context.value("document_id") ||
        context_.value("document_epoch") != context.value("document_epoch");
    context_ = context;
    if (session_changed) {
        ++session_;
        busy_ = submitting_ = polling_ = outcome_unknown_ = task_poll_failed_ = false;
        pending_operation_.clear();
        pending_context_ = pending_parameters_ = {};
        task_poll_.stop();
        task_id_.clear();
        task_state_.clear();
        selected_geometry_.clear();
        geometry_->setText("Select a geometry line in the model or viewport");
        status_->setText(context_.isEmpty() ? "Open or create a project to start"
                         : lineMode()       ? "Preview the line before applying"
                                            : "Select a geometry line to generate a mesh");
    } else if (outcome_unknown_) {
        status_->setText("No response received. Retry checks the same request.");
    } else if (activeTask()) {
        task_poll_.start();
        pollTask();
    } else if (!preview_context_.isEmpty() && !busy_ && !outcome_unknown_) {
        status_->setText("The model changed. Preview the line again before applying.");
    }
    invalidatePreview();
}
void ModelingTools::setSelectedGeometry(const QString& id, const QString& label) {
    selected_geometry_ = id;
    geometry_->setText(id.isEmpty() ? "Select a geometry line in the model or viewport" : label);
    updateControls();
}
void ModelingTools::invalidatePreview() {
    preview_context_ = {};
    preview_parameters_ = {};
    actions_.clear_preview();
    updateControls();
}
void ModelingTools::previewLine() {
    if (context_.isEmpty() || suspended_ || busy_ || outcome_unknown_)
        return;
    QJsonArray start, end;
    RenderPreview preview;
    std::array<double, 3> first{}, last{};
    for (std::size_t i = 0; i < 3; ++i) {
        first[i] = coordinates_[i]->value();
        last[i] = coordinates_[i + 3]->value();
        if (!std::isfinite(first[i]) || !std::isfinite(last[i]))
            return;
        start.append(first[i]);
        end.append(last[i]);
    }
    if (first == last) {
        invalidatePreview();
        status_->setText("Start and end must be different");
        return;
    }
    preview.points = {first, last};
    preview.lines = {{0, 1}};
    preview_context_ = context_;
    preview_parameters_ = {{"start_mm", start}, {"end_mm", end}};
    actions_.preview(preview);
    status_->setText("Line preview — Apply adds it to the project");
    updateControls();
}
void ModelingTools::request(const QString& operation,
                            const QJsonObject& parameters,
                            const QJsonObject& context,
                            DesktopClient::Reply callback) {
    QPointer<ModelingTools> guard(this);
    const auto session = session_;
    (void)client_.request(operation,
                          parameters,
                          context,
                          [guard, session, callback = std::move(callback)](const auto& reply) {
                              if (guard && guard->session_ == session)
                                  callback(reply);
                          });
}
void ModelingTools::apply() {
    if (busy_ || suspended_ || context_.isEmpty())
        return;
    if (activeTask()) {
        if (task_poll_failed_) {
            task_poll_failed_ = false;
            task_poll_.start();
            pollTask();
            updateControls();
        }
        return;
    }
    if (!outcome_unknown_) {
        if (lineMode()) {
            if (preview_context_.isEmpty() || preview_context_ != context_)
                return;
            pending_operation_ = "geometry.create_line";
            pending_parameters_ = preview_parameters_;
            pending_context_ = preview_context_;
        } else {
            if (selected_geometry_.isEmpty())
                return;
            pending_operation_ = "mesh.generate_line";
            pending_parameters_ = {{"geometry_id", selected_geometry_},
                                   {"segments", segments_->value()}};
            pending_context_ = context_;
        }
        pending_context_.insert("requested_version", 1);
        pending_context_.insert("idempotency_key",
                                QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    busy_ = submitting_ = true;
    updateControls();
    request(pending_operation_, pending_parameters_, pending_context_, [this](const auto& reply) {
        busy_ = submitting_ = false;
        actions_.record(pending_operation_, reply);
        if (!succeeded(reply)) {
            const auto code = reply.value("error").toObject().value("code").toString();
            outcome_unknown_ = code == "TIMEOUT" || code == "CONNECTION_LOST";
            status_->setText(outcome_unknown_
                                 ? "No response received. Retry checks the same request."
                                 : errorText(reply));
            if (!outcome_unknown_)
                invalidatePreview();
            updateControls();
            return;
        }
        outcome_unknown_ = false;
        const auto data = reply.value("data").toObject();
        if (pending_operation_ == "mesh.generate_line") {
            task_id_ = data.value("task_id").toString();
            showTask(data);
            if (activeTask())
                task_poll_.start();
        } else {
            invalidatePreview();
            status_->setText("Line created");
            actions_.refresh();
        }
        updateControls();
    });
}
void ModelingTools::cancel() {
    if (busy_ || suspended_ || outcome_unknown_)
        return;
    if (!activeTask()) {
        invalidatePreview();
        status_->setText("Preview cancelled — project unchanged");
        return;
    }
    busy_ = true;
    ++task_control_; // An earlier status reply must not overwrite this control's outcome.
    updateControls();
    request("task.cancel", {{"task_id", task_id_}}, context_, [this](const auto& reply) {
        busy_ = false;
        actions_.record("task.cancel", reply);
        if (succeeded(reply)) {
            showTask(reply.value("data").toObject());
            if (activeTask())
                task_poll_.start();
        } else
            status_->setText(errorText(reply));
        updateControls();
    });
}
void ModelingTools::pollTask() {
    if (polling_ || suspended_ || busy_ || !activeTask() || context_.isEmpty())
        return;
    polling_ = true;
    const auto control = task_control_;
    request("task.status", {{"task_id", task_id_}}, context_, [this, control](const auto& reply) {
        polling_ = false;
        if (control != task_control_)
            return;
        if (succeeded(reply))
            showTask(reply.value("data").toObject());
        else {
            task_poll_.stop();
            task_poll_failed_ = true;
            status_->setText(errorText(reply));
            updateControls();
        }
    });
}
void ModelingTools::showTask(const QJsonObject& task) {
    task_poll_failed_ = false;
    task_state_ = task.value("state").toString();
    progress_->setValue(qRound(std::clamp(task.value("progress").toDouble(), 0.0, 1.0) * 100));
    const QMap<QString, QString> labels{
        {"queued", "Mesh queued"},
        {"running", "Generating mesh"},
        {"cancel_requested", "Cancellation requested"},
        {"committing", "Applying mesh"},
        {"succeeded", "Mesh created"},
        {"cancelled", "Mesh cancelled — project unchanged"},
        {"conflicted", "Model changed — mesh was not applied"},
        {"failed", "Mesh generation failed"},
        {"interrupted", "Mesh generation interrupted"},
        {"outcome_unknown", "Mesh outcome requires workspace recovery"}};
    status_->setText(labels.value(task_state_, "Unknown task state"));
    if (task.contains("diagnostic"))
        status_->setText(status_->text() + ": " +
                         task.value("diagnostic").toObject().value("message").toString());
    if (terminal(task_state_)) {
        task_poll_.stop();
        actions_.refresh();
    }
    updateControls();
}
void ModelingTools::updateControls() {
    const bool available =
        !suspended_ && !context_.isEmpty() && !busy_ && !outcome_unknown_ && !activeTask();
    mode_->setEnabled(available);
    line_fields_->setVisible(lineMode());
    mesh_fields_->setVisible(!lineMode());
    line_fields_->setEnabled(available);
    mesh_fields_->setEnabled(available);
    preview_->setVisible(lineMode());
    preview_->setEnabled(available);
    apply_->setText(task_poll_failed_  ? "Refresh task"
                    : outcome_unknown_ ? "Retry request"
                    : lineMode()       ? "Apply"
                                       : "Generate mesh");
    apply_->setEnabled(!suspended_ && !busy_ && !context_.isEmpty() &&
                       (activeTask()
                            ? task_poll_failed_
                            : outcome_unknown_ || (lineMode() ? !preview_context_.isEmpty()
                                                              : !selected_geometry_.isEmpty())));
    cancel_->setText(activeTask() ? "Cancel task" : "Cancel");
    cancel_->setEnabled(!suspended_ && !busy_ && !outcome_unknown_ &&
                        (activeTask()
                             ? task_state_ != "cancel_requested" && task_state_ != "committing"
                             : !preview_context_.isEmpty()));
    progress_->setVisible(!task_id_.isEmpty() && !lineMode());
}
} // namespace qcae
