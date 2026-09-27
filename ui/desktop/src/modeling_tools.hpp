#pragma once
#include "qcae/desktop_client.hpp"
#include "qcae/render_packet.hpp"
#include <QTimer>
#include <QWidget>
#include <array>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace qcae {
// Disposable input/preview/task UI. Authoritative edits always go through DesktopClient.
class ModelingTools : public QWidget {
  public:
    struct Actions {
        std::function<void(const RenderPreview&)> preview;
        std::function<void()> clear_preview;
        std::function<void()> refresh;
        std::function<void(const QString&, const QJsonObject&)> record;
    };
    ModelingTools(DesktopClient&, Actions, QWidget* parent = nullptr);
    void suspend();
    void setContext(const QJsonObject&);
    void setSelectedGeometry(const QString& id, const QString& label);

  private:
    void invalidatePreview();
    void previewLine();
    void apply();
    void cancel();
    void pollTask();
    void showTask(const QJsonObject&);
    void updateControls();
    void request(const QString&, const QJsonObject&, const QJsonObject&, DesktopClient::Reply);
    [[nodiscard]] bool lineMode() const;
    [[nodiscard]] bool activeTask() const;

    DesktopClient& client_;
    Actions actions_;
    QJsonObject context_, preview_context_, preview_parameters_;
    QString selected_geometry_, task_id_, task_state_;
    QString pending_operation_;
    QJsonObject pending_context_, pending_parameters_;
    std::uint64_t session_{}, task_control_{};
    bool busy_{}, submitting_{}, suspended_{}, polling_{}, outcome_unknown_{}, task_poll_failed_{};
    QTimer task_poll_;
    QComboBox* mode_{};
    QWidget *line_fields_{}, *mesh_fields_{};
    std::array<QDoubleSpinBox*, 6> coordinates_{};
    QSpinBox* segments_{};
    QLabel *geometry_{}, *status_{};
    QProgressBar* progress_{};
    QPushButton *preview_{}, *apply_{}, *cancel_{};
};
} // namespace qcae
