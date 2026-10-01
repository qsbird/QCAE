#pragma once
#include "qcae/desktop_client.hpp"
#include <QJsonArray>
#include <QWidget>
#include <functional>

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace qcae {
// Forms keep disposable inputs. The shared engine validates and commits every edit.
class AnalysisTools : public QWidget {
  public:
    struct Actions {
        std::function<void()> refresh;
        std::function<void(const QString&, const QJsonObject&)> record;
        std::function<void(const QString&, const QJsonObject&)> navigate;
        std::function<QJsonArray()> rows;
    };
    AnalysisTools(DesktopClient&, Actions, QWidget* parent = nullptr);
    void setContext(const QJsonObject&);
    void setRowsContext(const QJsonObject&);
    void suspend();

  private:
    void rebuildFields();
    void updateControls();
    void submit();
    void previewOrganization();
    void runCheck();
    void refreshIssues();
    void showReport(const QJsonObject&);
    void applyPending();
    void request(const QString&, const QJsonObject&, const QJsonObject&, DesktopClient::Reply);
    [[nodiscard]] QJsonObject parameters() const;
    [[nodiscard]] bool formCurrent() const;
    [[nodiscard]] bool organizationMode() const;
    QComboBox* entityChoice(const QString&, const QString&);
    void entityList(const QString&, const QString&, const QString&);
    void numericField(const QString&, const QString&, double);
    void textField(const QString&, const QString&, const QString&);

    DesktopClient& client_;
    Actions actions_;
    QJsonObject context_, rows_context_, form_context_, pending_context_, pending_parameters_;
    QJsonArray rows_;
    QJsonObject report_;
    QString pending_operation_, preview_id_;
    std::uint64_t generation_{};
    bool suspended_{true}, busy_{};
    QComboBox *mode_{}, *analysis_{};
    QWidget* fields_{};
    QFormLayout* form_{};
    QLabel *status_{}, *check_status_{};
    QPushButton *refresh_{}, *submit_{}, *preview_{}, *cancel_{}, *check_{}, *issues_refresh_{};
    QTreeWidget* issues_{};
};
} // namespace qcae
