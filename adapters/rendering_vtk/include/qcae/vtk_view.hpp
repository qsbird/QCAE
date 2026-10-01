#pragma once

#include "qcae/render_packet.hpp"
#include "qcae/resource.hpp"
#include "qcae/sdk_copy_observation.hpp"

#include <QStringList>
#include <QWidget>
#include <memory>

namespace qcae {

struct VtkUpdateStats {
    std::uint64_t full_rebuilds{}, node_blocks{}, beam_blocks{}, geometry_blocks{};
    std::uint64_t highlight_blocks{}, highlight_cells_written{};
    // Coordinates written to retained packet/VTK buffers, and complete VTK arrays marked dirty.
    // These are not a measurement of graphics-driver allocation or GPU transfer bytes.
    std::uint64_t coordinate_bytes_copied{}, dirty_coordinate_array_bytes{};
    std::uint64_t cell_blocks{};
};

class VtkView : public QWidget {
    Q_OBJECT
  public:
    enum class StandardView { front, top, right, isometric };
    enum class BoxMode { contained, intersecting };

    explicit VtkView(QWidget* parent = nullptr);
    ~VtkView() override;
    void setPacket(const RenderPacket& packet);
    [[nodiscard]] bool applyDelta(const RenderDelta& delta);
    // Most recent packet, delta or selection update; camera/preview work is excluded.
    [[nodiscard]] VtkUpdateStats lastUpdateStats() const;
    void setSelectedIds(const QStringList& ids);
    void setPreview(const RenderPreview& preview);
    void clearPreview();
    void fit();
    void standardView(StandardView view);
    void setBoxMode(BoxMode mode);
    void setThroughSelection(bool enabled);
    [[nodiscard]] QString cameraFingerprint() const;
    [[nodiscard]] bool hasPacket() const;
    [[nodiscard]] bool pendingCameraUpdate() const;
    [[nodiscard]] std::optional<ResourceVersion> installedVersion() const;
    [[nodiscard]] SdkCopySnapshot sdkCopyObservation() const;

  signals:
    // IDs are stable display entities from RenderPacket, never VTK cell IDs.
    void picked(const QStringList& ids, bool through);
    void cameraChanged(const QString& fingerprint);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qcae
