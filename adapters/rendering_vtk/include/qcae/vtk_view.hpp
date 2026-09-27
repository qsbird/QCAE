#pragma once

#include "qcae/render_packet.hpp"

#include <QStringList>
#include <QWidget>
#include <memory>

namespace qcae {

class VtkView : public QWidget {
    Q_OBJECT
  public:
    enum class StandardView { front, top, right, isometric };
    enum class BoxMode { contained, intersecting };

    explicit VtkView(QWidget* parent = nullptr);
    ~VtkView() override;
    void setPacket(const RenderPacket& packet);
    void setSelectedIds(const QStringList& ids);
    void setPreview(const RenderPreview& preview);
    void clearPreview();
    void fit();
    void standardView(StandardView view);
    void setBoxMode(BoxMode mode);
    void setThroughSelection(bool enabled);
    [[nodiscard]] QString cameraFingerprint() const;
    [[nodiscard]] bool hasPacket() const;

  signals:
    // IDs are real node/beam/geometry entities from RenderPacket, never VTK cell IDs.
    void picked(const QStringList& ids, bool through);
    void cameraChanged(const QString& fingerprint);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qcae
