#pragma once
#include "qcae/document_view.hpp"
#include "qcae/render_packet.hpp"
#include "qcae/view_session.hpp"
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <variant>

namespace qcae {
struct RenderLine2 {
    EntityId entity;
    std::array<EntityId, 2> nodes;
};
struct RenderCellSource {
    EntityId entity;
    RenderCellKind kind{RenderCellKind::polyline};
    std::vector<EntityId> nodes;
};
using RenderItem = std::variant<RenderPoint, RenderLine2, RenderGeometryLine, RenderCellSource>;
struct RenderContribution {
    RecordTypeId record_type;
    std::string topology;
    std::function<RenderItem(const Record&)> project;
};
// Trusted static contributions; persistent types remain owned by RecordRegistry.
class RenderContributions {
  public:
    void add(RenderContribution);
    void freeze();
    bool frozen() const noexcept;
    const RenderContribution* find(RecordTypeId) const;
    const std::map<RecordTypeId, RenderContribution>& entries() const;

  private:
    std::map<RecordTypeId, RenderContribution> entries_;
    bool frozen_{};
};
RenderContributions default_render_contributions();

struct DirectedRenderChange {
    Revision base_revision{}, revision{};
    const RecordChangeSet* changes{};
    RecordDirection direction{RecordDirection::forward};
};
struct RenderProjectionStats {
    std::uint64_t full_rebuilds{}, projected_records{}, model_bytes_copied{}, dirty_blocks{};
};
struct RenderUpdate {
    std::optional<RenderPacket> full;
    RenderDelta delta;
    RenderProjectionStats work;
    bool legacy_line2_compatible{true};
};
// A disposable display index. Ordinary updates inspect only committed changed records.
class RenderProjector {
  public:
    explicit RenderProjector(RenderContributions = default_render_contributions(),
                             std::size_t max_entities = 200000);
    Result<RenderUpdate> update(const DocumentView&,
                                const ViewSession&,
                                std::span<const DirectedRenderChange> = {},
                                bool force_full = false);
    void clear();
    std::optional<RecordVersion> version() const;
    RenderProjectionStats stats() const noexcept;

  private:
    Result<RenderUpdate> rebuild(const DocumentView&, const ViewSession&);
    RenderContributions contributions_;
    std::size_t max_entities_;
    std::optional<ViewSession> view_;
    std::map<std::string, std::size_t> point_indices_, cell_indices_, geometry_indices_;
    RenderProjectionStats stats_;
    bool legacy_line2_compatible_{true};
};
} // namespace qcae
