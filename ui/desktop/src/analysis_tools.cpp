#include "analysis_tools.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace qcae {
namespace {
bool succeeded(const QJsonObject& reply) {
    return reply.value("status") == "success";
}
QString errorText(const QJsonObject& reply) {
    const auto error = reply.value("error").toObject();
    return error.value("code").toString() + ": " + error.value("message").toString();
}
bool sameDocument(const QJsonObject& first, const QJsonObject& second) {
    return first.value("document_id") == second.value("document_id") &&
           first.value("document_epoch") == second.value("document_epoch");
}
bool unsignedDecimal(const QJsonValue& value, quint64& number) {
    if (!value.isString())
        return false;
    const auto text = value.toString();
    bool valid{};
    number = text.toULongLong(&valid);
    return valid && text == QString::number(number);
}
bool positiveUint32(const QJsonValue& value) {
    quint64 number{};
    return unsignedDecimal(value, number) && number > 0 && number <= UINT32_MAX;
}
bool recordVersion(const QJsonObject& version, quint64& revision) {
    for (const char* name : {"document_id", "document_epoch"})
        if (!version.value(name).isString() || version.value(name).toString().isEmpty())
            return false;
    return unsignedDecimal(version.value("revision"), revision);
}
bool completedCheckReport(const QJsonObject& reply,
                          const QJsonObject& expected,
                          const QString& analysis,
                          const QString& check_id = {}) {
    const auto status = reply.value("status").toString();
    const auto report = reply.value("data").toObject();
    const auto outcome = report.value("outcome").toString();
    const auto input = report.value("input_version").toObject();
    const auto current = report.value("current_version").toObject();
    quint64 input_revision{}, current_revision{};
    // A read succeeds independently of the conclusion saved by analysis.check.
    if ((outcome != "success" && outcome != "needs_input" && outcome != "failed") ||
        (check_id.isEmpty() ? status != outcome : status != "success") ||
        report.value("check_execution") != "completed" || !report.value("check_id").isString() ||
        report.value("check_id").toString().isEmpty() || report.value("analysis_id") != analysis ||
        !recordVersion(input, input_revision) || !recordVersion(current, current_revision) ||
        !sameDocument(current, expected) || input_revision > current_revision ||
        input.value("document_id") != expected.value("document_id") ||
        (report.value("state") != "current" && report.value("state") != "stale") ||
        (report.value("state") == "current" && input != current) ||
        !report.value("issues").isArray())
        return false;
    const auto catalog = report.value("catalog_version").toString();
    if ((catalog != "qcae.analysis.rules.v1" && catalog != "qcae.analysis.rules.v2") ||
        (report.value("state") == "current" && catalog != "qcae.analysis.rules.v2") ||
        report.value("analysis_kind") != "linear_static")
        return false;
    const auto profile = report.value("profile").toObject();
    for (const char* field : {"profile_id", "profile_version", "definition_digest"})
        if (!profile.value(field).isString() || profile.value(field).toString().isEmpty())
            return false;
    if (check_id.isEmpty()) {
        if (catalog != "qcae.analysis.rules.v2" || !sameDocument(input, expected) ||
            input.value("revision") != expected.value("expected_revision"))
            return false;
    } else if (report.value("check_id") != check_id)
        return false;
    const auto issues = report.value("issues").toArray();
    if (outcome != "success" && issues.isEmpty())
        return false;
    QSet<QString> rules;
    for (const auto& value : issues) {
        const auto issue = value.toObject();
        const auto rule = issue.value("rule_id").toString();
        if (!value.isObject() || !issue.value("rule_id").isString() || rule.isEmpty() ||
            rules.contains(rule) || !positiveUint32(issue.value("rule_version")) ||
            !positiveUint32(issue.value("entity_type")) || issue.value("severity") != "error" ||
            !issue.value("entity_id").isString() || issue.value("entity_id").toString().isEmpty() ||
            issue.value("input_version") != report.value("input_version") ||
            issue.value("state") != report.value("state"))
            return false;
        rules.insert(rule);
        for (const char* field : {"field", "actual", "expected", "message"})
            if (!issue.value(field).isString() || issue.value(field).toString().isEmpty())
                return false;
    }
    return true;
}
QString label(const QJsonObject& row) {
    const auto identity = row.value("entity_id").toString();
    auto text = row.value("name").toString();
    if (text.isEmpty())
        text = row.value("kind").toString();
    const auto position = row.value("position_mm").toArray();
    if (position.size() == 3)
        text += QString(" (%1, %2, %3) mm")
                    .arg(position[0].toDouble())
                    .arg(position[1].toDouble())
                    .arg(position[2].toDouble());
    return text + " · " + identity;
}
QString key() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace
AnalysisTools::AnalysisTools(DesktopClient& client, Actions actions, QWidget* parent)
    : QWidget(parent), client_(client), actions_(std::move(actions)) {
    setObjectName("analysisTools");
    auto* layout = new QVBoxLayout(this);
    mode_ = new QComboBox(this);
    mode_->setObjectName("analysisMode");
    for (const auto& [title, operation] :
         {std::pair{"Material", "material.create"},
          {"Beam section", "section.create"},
          {"Assign beam section", "beam.assign_section"},
          {"Nodal force", "force.create"},
          {"Constraint", "constraint.create"},
          {"Load case", "load_case.create"},
          {"Linear static analysis", "analysis.create"},
          {"Repair load case references", "load_case.set_references"},
          {"Part", "part.upsert"},
          {"Assembly", "assembly.upsert"},
          {"Set", "set.upsert"}})
        mode_->addItem(title, operation);
    layout->addWidget(mode_);
    fields_ = new QWidget(this);
    layout->addWidget(fields_);
    status_ = new QLabel("Open or create a project; refresh choices before applying.", this);
    status_->setObjectName("analysisStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    refresh_ = new QPushButton("Refresh choices", this);
    preview_ = new QPushButton("Preview", this);
    submit_ = new QPushButton("Apply", this);
    cancel_ = new QPushButton("Cancel preview", this);
    refresh_->setObjectName("analysisRefresh");
    preview_->setObjectName("analysisPreview");
    submit_->setObjectName("analysisApply");
    cancel_->setObjectName("analysisCancel");
    for (auto* button : {refresh_, preview_, submit_, cancel_})
        buttons->addWidget(button);
    layout->addLayout(buttons);
    analysis_ = new QComboBox(this);
    analysis_->setObjectName("checkAnalysis");
    layout->addWidget(analysis_);
    auto* check_buttons = new QHBoxLayout;
    check_ = new QPushButton("Check analysis", this);
    issues_refresh_ = new QPushButton("Refresh report", this);
    auto* repair = new QPushButton("Repair references", this);
    auto* locate = new QPushButton("Locate issue", this);
    locate->setObjectName("analysisLocateIssue");
    check_->setObjectName("analysisCheck");
    issues_refresh_->setObjectName("analysisIssuesRefresh");
    repair->setObjectName("analysisRepair");
    for (auto* button : {check_, issues_refresh_, repair, locate})
        check_buttons->addWidget(button);
    layout->addLayout(check_buttons);
    check_status_ = new QLabel("No check report", this);
    check_status_->setObjectName("analysisCheckStatus");
    check_status_->setWordWrap(true);
    layout->addWidget(check_status_);
    issues_ = new QTreeWidget(this);
    issues_->setObjectName("analysisIssues");
    issues_->setHeaderLabels({"Rule/version", "Field", "Actual / expected", "Message"});
    layout->addWidget(issues_);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { rebuildFields(); });
    connect(refresh_, &QPushButton::clicked, this, [this] { rebuildFields(); });
    connect(submit_, &QPushButton::clicked, this, [this] { submit(); });
    connect(preview_, &QPushButton::clicked, this, [this] { previewOrganization(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        preview_id_.clear();
        status_->setText("Preview cancelled; project unchanged.");
        updateControls();
    });
    connect(check_, &QPushButton::clicked, this, [this] { runCheck(); });
    connect(issues_refresh_, &QPushButton::clicked, this, [this] { refreshIssues(); });
    connect(repair, &QPushButton::clicked, this, [this] {
        mode_->setCurrentIndex(mode_->findData("load_case.set_references"));
    });
    connect(issues_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (!context_.isEmpty() &&
            sameDocument(report_.value("input_version").toObject(), context_))
            actions_.navigate(item->data(0, Qt::UserRole).toString(), context_);
    });
    connect(locate, &QPushButton::clicked, this, [this] {
        const auto* item = issues_->currentItem();
        if (item && !context_.isEmpty() &&
            sameDocument(report_.value("input_version").toObject(), context_))
            actions_.navigate(item->data(0, Qt::UserRole).toString(), context_);
    });
    rebuildFields();
}
void AnalysisTools::suspend() {
    ++generation_;
    suspended_ = true;
    busy_ = false;
    preview_id_.clear();
    status_->setText(pending_operation_.isEmpty()
                         ? "Connection lost. Waiting for project context."
                         : "Write outcome unknown. Reconnect and retry the same operation.");
    updateControls();
}
void AnalysisTools::setContext(const QJsonObject& context) {
    const bool changed_document = !sameDocument(context_, context);
    if (changed_document) {
        ++generation_;
        pending_operation_.clear();
        pending_context_ = pending_parameters_ = {};
        preview_id_.clear();
        report_ = {};
        issues_->clear();
        setProperty("checkId", QVariant{});
        setProperty("checkState", QVariant{});
        busy_ = false;
    }
    context_ = context;
    suspended_ = context_.isEmpty();
    if (!formCurrent()) {
        preview_id_.clear();
        status_->setText("Model context changed. Refresh choices before applying.");
    }
    if (!report_.isEmpty()) {
        const auto input = report_.value("input_version").toObject();
        const bool current = report_.value("state") == "current" && sameDocument(input, context_) &&
                             input.value("revision") == context_.value("expected_revision");
        setProperty("checkState", current ? "current" : "stale");
        check_status_->setText(QString("Check at revision %1: %2; outcome %3; %4 issue(s).")
                                   .arg(input.value("revision").toString())
                                   .arg(current ? "current" : "stale")
                                   .arg(report_.value("outcome").toString())
                                   .arg(issues_->topLevelItemCount()));
    } else
        check_status_->setText("No check report");
    updateControls();
}
void AnalysisTools::setRowsContext(const QJsonObject& context) {
    rows_context_ = context;
    updateControls();
}
bool AnalysisTools::formCurrent() const {
    return !context_.isEmpty() && context_ == form_context_ && context_ == rows_context_;
}
bool AnalysisTools::organizationMode() const {
    return mode_->currentData().toString().endsWith(".upsert");
}
QComboBox* AnalysisTools::entityChoice(const QString& name, const QString& kind) {
    auto* choice = new QComboBox(fields_);
    choice->setObjectName("analysisField_" + name);
    for (const auto& value : rows_) {
        const auto row = value.toObject();
        if (row.value("kind") == kind)
            choice->addItem(label(row), row.value("entity_id").toString());
    }
    form_->addRow(name, choice);
    return choice;
}
void AnalysisTools::entityList(const QString& name, const QString& title, const QString& kind) {
    auto* list = new QListWidget(fields_);
    list->setObjectName("analysisField_" + name);
    list->setSelectionMode(QAbstractItemView::MultiSelection);
    list->setMaximumHeight(130);
    for (const auto& value : rows_) {
        const auto row = value.toObject();
        if (!kind.isEmpty() && row.value("kind") != kind)
            continue;
        auto* item = new QListWidgetItem(label(row), list);
        item->setData(Qt::UserRole, row.value("entity_id").toString());
    }
    form_->addRow(title, list);
}
void AnalysisTools::numericField(const QString& name, const QString& title, double value) {
    auto* field = new QDoubleSpinBox(fields_);
    field->setObjectName("analysisField_" + name);
    field->setRange(-1e12, 1e12);
    field->setDecimals(6);
    field->setValue(value);
    form_->addRow(title, field);
}
void AnalysisTools::textField(const QString& name, const QString& title, const QString& value) {
    auto* field = new QLineEdit(value, fields_);
    field->setObjectName("analysisField_" + name);
    form_->addRow(title, field);
}
void AnalysisTools::rebuildFields() {
    if (busy_ || !pending_operation_.isEmpty())
        return;
    delete fields_;
    fields_ = new QWidget(this);
    form_ = new QFormLayout(fields_);
    qobject_cast<QVBoxLayout*>(layout())->insertWidget(1, fields_);
    form_context_ = context_;
    rows_ = actions_.rows();
    preview_id_.clear();
    const auto operation = mode_->currentData().toString();
    if (operation == "material.create") {
        textField("name", "Name", "Steel");
        numericField("young_modulus", "Young's modulus (MPa)", 210000);
        numericField("poisson_ratio", "Poisson ratio (1)", 0.3);
    } else if (operation == "section.create") {
        textField("name", "Name", "Beam section");
        entityChoice("material_id", "material");
        numericField("area_mm2", "Area (mm²)", 100);
        numericField("i1_mm4", "I1 (mm⁴)", 833.333333);
        numericField("i2_mm4", "I2 (mm⁴)", 833.333333);
        numericField("torsion_mm4", "J (mm⁴)", 1666.666667);
    } else if (operation == "beam.assign_section") {
        entityChoice("section_id", "section");
        entityList("beam_ids", "Beams", "beam");
    } else if (operation == "force.create") {
        entityChoice("node_id", "node");
        numericField("x", "Fx (N)", 0);
        numericField("y", "Fy (N)", -1000);
        numericField("z", "Fz (N)", 0);
    } else if (operation == "constraint.create") {
        entityList("node_ids", "Nodes", "node");
        textField("dofs", "Fixed degrees of freedom", "123456");
    } else if (operation == "load_case.create" || operation == "load_case.set_references") {
        if (operation == "load_case.create")
            textField("name", "Name", "LC1");
        else
            entityChoice("load_case_id", "load_case");
        entityList("force_ids", "Referenced forces (empty permitted)", "force");
        entityList("constraint_ids", "Referenced constraints (empty permitted)", "constraint");
    } else if (operation == "analysis.create") {
        textField("name", "Name", "Cantilever linear static");
        entityList("load_case_ids", "Load cases (empty permitted)", "load_case");
    } else {
        textField("name", "Name", mode_->currentText());
        if (operation == "assembly.upsert")
            entityList("children", "Parts / assemblies", "part");
        else
            entityList("members", "Members", {});
    }
    const auto selected_analysis = analysis_->currentData();
    analysis_->clear();
    for (const auto& value : rows_) {
        const auto row = value.toObject();
        if (row.value("kind") == "analysis")
            analysis_->addItem(label(row), row.value("entity_id").toString());
    }
    const auto previous = analysis_->findData(selected_analysis);
    if (previous >= 0)
        analysis_->setCurrentIndex(previous);
    for (auto* field : fields_->findChildren<QDoubleSpinBox*>())
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] {
            preview_id_.clear();
            updateControls();
        });
    for (auto* field : fields_->findChildren<QLineEdit*>())
        connect(field, &QLineEdit::textChanged, this, [this] {
            preview_id_.clear();
            updateControls();
        });
    for (auto* list : fields_->findChildren<QListWidget*>())
        connect(list, &QListWidget::itemSelectionChanged, this, [this] {
            preview_id_.clear();
            updateControls();
        });
    status_->setText("Choices use the first 1,000 model entities and stable IDs; edits are "
                     "validated by the engine.");
    // Input widgets retain IDs without sharing the cached JSON row index. This
    // avoids an array detach when the ordinary local tree updater changes a row.
    rows_ = {};
    updateControls();
}
QJsonObject AnalysisTools::parameters() const {
    QJsonObject result;
    for (auto* field : fields_->findChildren<QLineEdit*>())
        if (field->objectName().startsWith("analysisField_"))
            result.insert(field->objectName().mid(14), field->text());
    for (auto* field : fields_->findChildren<QDoubleSpinBox*>()) {
        const auto name = field->objectName().mid(14);
        if (name == "young_modulus" || name == "x" || name == "y" || name == "z")
            result.insert(name,
                          QJsonObject{{"value", field->value()},
                                      {"unit", name == "young_modulus" ? "MPa" : "N"}});
        else
            result.insert(name, field->value());
    }
    for (auto* choice : fields_->findChildren<QComboBox*>())
        result.insert(choice->objectName().mid(14), choice->currentData().toString());
    for (auto* list : fields_->findChildren<QListWidget*>()) {
        QJsonArray identities;
        for (auto* item : list->selectedItems())
            identities.append(item->data(Qt::UserRole).toString());
        result.insert(list->objectName().mid(14), identities);
    }
    return result;
}
void AnalysisTools::updateControls() {
    const bool active = client_.ready() && !suspended_ && !context_.isEmpty() && !busy_;
    const bool pending = !pending_operation_.isEmpty();
    mode_->setEnabled(!busy_ && !pending);
    fields_->setEnabled(active && !pending);
    refresh_->setEnabled(active && !pending && context_ == rows_context_);
    preview_->setVisible(organizationMode());
    cancel_->setVisible(organizationMode());
    preview_->setEnabled(active && !pending && formCurrent());
    cancel_->setEnabled(active && !pending && !preview_id_.isEmpty());
    submit_->setText(pending ? "Retry same operation" : "Apply");
    submit_->setEnabled(
        active && (pending ? sameDocument(context_, pending_context_)
                           : formCurrent() && (!organizationMode() || !preview_id_.isEmpty())));
    check_->setEnabled(active && !pending && analysis_->count() > 0 && context_ == rows_context_);
    issues_refresh_->setEnabled(active && !report_.isEmpty());
}
void AnalysisTools::request(const QString& operation,
                            const QJsonObject& parameters,
                            const QJsonObject& context,
                            DesktopClient::Reply callback) {
    const auto generation = generation_;
    const QPointer<AnalysisTools> guard(this);
    (void)client_.request(operation,
                          parameters,
                          context,
                          [guard, generation, callback = std::move(callback)](const auto& reply) {
                              if (guard && generation == guard->generation_)
                                  callback(reply);
                          });
}
void AnalysisTools::previewOrganization() {
    if (!formCurrent() || busy_)
        return;
    auto input = parameters();
    input.insert("command", mode_->currentData().toString());
    busy_ = true;
    updateControls();
    const auto expected = form_context_;
    request("changes.preview", input, expected, [this, expected](const auto& reply) {
        busy_ = false;
        actions_.record("changes.preview", reply);
        if (expected != context_)
            status_->setText("Preview input is stale. Refresh choices.");
        else if (succeeded(reply)) {
            const auto value = reply.value("data").toObject();
            preview_id_ = value.value("preview_id").toString();
            status_->setText(
                QString("Preview at revision %1: %2. Apply or cancel before editing.")
                    .arg(value.value("revision").toString())
                    .arg(value.value("creates_entity").toBool()
                             ? "create " + mode_->currentText()
                             : "update " + value.value("affected_entity_id").toString()));
        } else
            status_->setText(errorText(reply));
        updateControls();
    });
}
void AnalysisTools::submit() {
    if (busy_ || suspended_ || context_.isEmpty())
        return;
    if (!pending_operation_.isEmpty()) {
        applyPending();
        return;
    }
    if (!formCurrent() || (organizationMode() && preview_id_.isEmpty()))
        return;
    pending_operation_ =
        organizationMode() ? QString("changes.commit") : mode_->currentData().toString();
    pending_parameters_ =
        organizationMode() ? QJsonObject{{"preview_id", preview_id_}} : parameters();
    pending_context_ = form_context_;
    pending_context_.insert("idempotency_key", key());
    if (pending_operation_ != "analysis.create") {
        applyPending();
        return;
    }
    busy_ = true;
    updateControls();
    request("nastran.ui", {}, {}, [this](const auto& reply) {
        busy_ = false;
        if (!succeeded(reply)) {
            pending_operation_.clear();
            status_->setText(errorText(reply));
            updateControls();
            return;
        }
        pending_context_.insert("expected_profile",
                                reply.value("data").toObject().value("profile"));
        applyPending();
    });
}
void AnalysisTools::applyPending() {
    if (!client_.ready() || !sameDocument(context_, pending_context_))
        return;
    busy_ = true;
    updateControls();
    status_->setText("Applying " + pending_operation_);
    const auto operation = pending_operation_;
    request(operation, pending_parameters_, pending_context_, [this, operation](const auto& reply) {
        busy_ = false;
        actions_.record(operation, reply);
        setProperty("lastOperation", operation);
        setProperty("lastStatus", reply.value("status").toString());
        const auto code = reply.value("error").toObject().value("code").toString();
        const bool unknown =
            code == "TIMEOUT" || code == "CONNECTION_LOST" || code == "NOT_CONNECTED";
        if (!unknown) {
            pending_operation_.clear();
            preview_id_.clear();
        }
        status_->setText(succeeded(reply)
                             ? "Committed through the engine. Refresh choices for the next edit."
                             : errorText(reply));
        if (succeeded(reply)) {
            setProperty("lastReceipt", reply.value("data").toObject());
            actions_.refresh();
        }
        updateControls();
    });
}
void AnalysisTools::runCheck() {
    if (busy_ || suspended_ || analysis_->currentData().toString().isEmpty())
        return;
    busy_ = true;
    updateControls();
    auto expected = context_;
    expected.insert("idempotency_key", key());
    const auto analysis = analysis_->currentData().toString();
    request("analysis.check",
            {{"analysis_id", analysis}},
            expected,
            [this, expected, analysis](const auto& reply) {
                busy_ = false;
                actions_.record("analysis.check", reply);
                if (completedCheckReport(reply, expected, analysis))
                    showReport(reply.value("data").toObject());
                else
                    check_status_->setText(errorText(reply));
                updateControls();
            });
}
void AnalysisTools::refreshIssues() {
    if (busy_ || report_.isEmpty())
        return;
    busy_ = true;
    updateControls();
    const auto expected = context_;
    const auto analysis = report_.value("analysis_id").toString();
    const auto check_id = report_.value("check_id").toString();
    request("analysis.get_issues",
            {{"check_id", check_id}},
            expected,
            [this, expected, analysis, check_id](const auto& reply) {
                busy_ = false;
                if (completedCheckReport(reply, expected, analysis, check_id))
                    showReport(reply.value("data").toObject());
                else
                    check_status_->setText(errorText(reply));
                updateControls();
            });
}
void AnalysisTools::showReport(const QJsonObject& report) {
    report_ = report;
    setProperty("checkId", report.value("check_id").toString());
    issues_->clear();
    for (const auto& value : report.value("issues").toArray()) {
        const auto issue = value.toObject();
        auto* item = new QTreeWidgetItem(
            issues_,
            {issue.value("rule_id").toString() + "/" + issue.value("rule_version").toString(),
             issue.value("field").toString(),
             issue.value("actual").toString() + " / " + issue.value("expected").toString(),
             issue.value("message").toString()});
        item->setData(0, Qt::UserRole, issue.value("entity_id").toString());
        item->setToolTip(0,
                         "Entity: " + issue.value("entity_id").toString() +
                             "; state: " + issue.value("state").toString());
    }
    setContext(context_);
}
} // namespace qcae
