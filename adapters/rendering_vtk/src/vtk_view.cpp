#include "qcae/vtk_view.hpp"
#include "qcae/operation_ledger.hpp"
#include "qcae/sdk_copy_tracker.hpp"
#include "coordinate_range_observer.hpp"
#include "shift_scale_copy_observer.hpp"

#include <QCryptographicHash>
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkDataObject.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkHardwareSelector.h>
#include <vtkIdTypeArray.h>
#include <vtkInformation.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLIndexBufferObject.h>
#include <vtkOpenGLHelper.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkOpenGLVertexBufferObject.h>
#include <vtkOpenGLVertexBufferObjectGroup.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSelection.h>
#include <vtkSelectionNode.h>
#include <vtkShaderProgram.h>
#include <vtkVersion.h>

namespace qcae {
namespace {
QString qs(const std::string& value) {
    return QString::fromStdString(value);
}
bool inside(const QPointF& point, const QRectF& rectangle) {
    return rectangle.contains(point);
}
double orientation(const QPointF& a, const QPointF& b, const QPointF& c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}
bool intersects(const QPointF& a, const QPointF& b, const QPointF& c, const QPointF& d) {
    const auto ab_c = orientation(a, b, c);
    const auto ab_d = orientation(a, b, d);
    const auto cd_a = orientation(c, d, a);
    const auto cd_b = orientation(c, d, b);
    constexpr double epsilon = 1e-9;
    const auto between = [](double value, double first, double second) {
        return value >= std::min(first, second) - 1e-9 && value <= std::max(first, second) + 1e-9;
    };
    const auto on_segment = [&](const QPointF& p, const QPointF& q, const QPointF& r) {
        return between(q.x(), p.x(), r.x()) && between(q.y(), p.y(), r.y());
    };
    if (std::abs(ab_c) <= epsilon && on_segment(a, c, b))
        return true;
    if (std::abs(ab_d) <= epsilon && on_segment(a, d, b))
        return true;
    if (std::abs(cd_a) <= epsilon && on_segment(c, a, d))
        return true;
    if (std::abs(cd_b) <= epsilon && on_segment(c, b, d))
        return true;
    return (ab_c > 0) != (ab_d > 0) && (cd_a > 0) != (cd_b > 0);
}
bool segmentIntersectsRect(const QPointF& a, const QPointF& b, const QRectF& r) {
    if (inside(a, r) || inside(b, r))
        return true;
    const QPointF tl = r.topLeft(), tr = r.topRight(), br = r.bottomRight(), bl = r.bottomLeft();
    return intersects(a, b, tl, tr) || intersects(a, b, tr, br) || intersects(a, b, br, bl) ||
           intersects(a, b, bl, tl);
}
bool polygonContains(const std::vector<QPointF>& points, const QPointF& point) {
    bool contained{};
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto& a = points[i];
        const auto& b = points[j];
        if ((a.y() > point.y()) != (b.y() > point.y()) &&
            point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
            contained = !contained;
    }
    return contained;
}

// The installed OpenGL mapper still performs every operation. Overrides observe
// actual virtual entry points and cache decisions without changing VTK state.
class ObservedPolyDataMapper : public vtkOpenGLPolyDataMapper {
  public:
    static ObservedPolyDataMapper* New();
    vtkTypeMacro(ObservedPolyDataMapper, vtkOpenGLPolyDataMapper) void setCopyTracker(
        std::shared_ptr<sdk_copy::Tracker> tracker) {
        tracker_ = std::move(tracker);
    }

  protected:
    ObservedPolyDataMapper() = default;
    ~ObservedPolyDataMapper() override = default;

    bool GetNeedToRebuildBufferObjects(vtkRenderer* renderer, vtkActor* actor) override {
        const auto rebuild = this->Superclass::GetNeedToRebuildBufferObjects(renderer, actor);
        if (observing()) {
            // Three appended MTime scalars plus possible initialized-prefix
            // moves, and the three-scalar state assignment on a cache miss.
            tracker_->record(sdk_copy::Component::vtk_mapper_check,
                             ledger::Stage::vtk_apply,
                             (6 + (rebuild ? 3 : 0)) * sizeof(vtkMTimeType));
        }
        return rebuild;
    }

    void BuildBufferObjects(vtkRenderer* renderer, vtkActor* actor) override {
        this->Superclass::BuildBufferObjects(renderer, actor);
        if (!observing())
            return;
        // TempState contains six MTimes and two integer properties. Each
        // append can relocate its preceding initialized bytes, and assigning
        // the completed state can copy its entire payload once more.
        tracker_->record(sdk_copy::Component::vtk_mapper_build,
                         ledger::Stage::vtk_apply,
                         30 * sizeof(vtkMTimeType) + 19 * sizeof(int));
        if (auto* vbo = this->VBOs->GetVBO("vertexMC"))
            tracker_->record(sdk_copy::Component::vtk_mapper_build,
                             ledger::Stage::vtk_apply,
                             detail::autoInverseMatrixBytes(vbo),
                             "inverse matrices require the audited AUTO/DISABLE coordinate path");
        if (this->Colors || this->ColorCoordinates || this->ColorTextureMap ||
            !this->ExtraAttributes.empty() || this->GetSelection() ||
            (this->CurrentInput && (this->CurrentInput->GetPointData()->GetNormals() ||
                                    this->CurrentInput->GetPointData()->GetTCoords() ||
                                    this->CurrentInput->GetPointData()->GetTangents() ||
                                    this->CurrentInput->GetCellData()->GetNormals())))
            tracker_->unsupported(sdk_copy::Component::vtk_mapper_build,
                                  ledger::Stage::vtk_apply,
                                  "scalar/normal/texture/selection buffers are not audited");
    }

    void BuildCellTextures(vtkRenderer* renderer,
                           vtkActor* actor,
                           vtkCellArray* primitives[4],
                           int representation) override {
        this->Superclass::BuildCellTextures(renderer, actor, primitives, representation);
        if (observing())
            tracker_->record(sdk_copy::Component::vtk_cell_texture,
                             ledger::Stage::vtk_apply,
                             this->HaveCellScalars || this->HaveCellNormals
                                 ? std::nullopt
                                 : std::optional<std::uint64_t>(0),
                             "cell scalar/normal arrays require their own observer");
    }

    void BuildIBO(vtkRenderer* renderer, vtkActor* actor, vtkPolyData* poly) override {
        if (!observing()) {
            this->Superclass::BuildIBO(renderer, actor, poly);
            return;
        }
        vtkCellArray* primitives[]{
            poly->GetVerts(), poly->GetLines(), poly->GetPolys(), poly->GetStrips()};
        const auto representation = actor->GetProperty()->GetRepresentation();
        auto* edge_flags = poly->GetPointData()->GetAttribute(vtkDataSetAttributes::EDGEFLAG);
        // This state is diagnostic overhead. It reproduces the installed
        // source key only to observe whether the original call rebuilds.
        vtkStateStorage expected;
        for (auto* primitive : primitives)
            expected.Append(primitive->GetNumberOfCells() ? primitive->GetMTime() : 0, "primitive");
        expected.Append(representation, "representation");
        expected.Append(actor->GetProperty()->GetVertexVisibility(), "vertices");
        expected.Append(edge_flags ? edge_flags->GetMTime() : 0, "edges");
        expected.Append(actor->GetProperty()->GetEdgeVisibility() && representation == VTK_SURFACE,
                        "surface edges");
        const bool rebuild = this->IBOBuildState != expected;
        this->Superclass::BuildIBO(renderer, actor, poly);
        // State append writes and possible prefix relocation; state assignment
        // copies one completed payload only when the original cache changes.
        const auto state_bytes = 28 * sizeof(vtkMTimeType) + 8 * sizeof(int) + 2 * sizeof(bool);
        const auto assigned = 5 * sizeof(vtkMTimeType) + sizeof(int) + 2 * sizeof(bool);
        if (!rebuild) {
            tracker_->record(
                sdk_copy::Component::vtk_index_build, ledger::Stage::vtk_apply, state_bytes);
            return;
        }
        if (primitives[2]->GetNumberOfCells() || primitives[3]->GetNumberOfCells() || edge_flags ||
            actor->GetProperty()->GetVertexVisibility() ||
            actor->GetProperty()->GetEdgeVisibility() ||
            (representation != VTK_SURFACE && representation != VTK_WIREFRAME)) {
            tracker_->record(
                sdk_copy::Component::vtk_index_build,
                ledger::Stage::vtk_apply,
                std::nullopt,
                "polygon/strip/edge/vertex/point-representation indices are not audited");
            ledger::unknown(ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes);
            return;
        }
        const auto points = primitives[0]->GetNumberOfConnectivityIds();
        const auto connectivity = primitives[1]->GetNumberOfConnectivityIds();
        const auto cells = primitives[1]->GetNumberOfCells();
        const auto lines = 2 * (connectivity - cells);
        if (points < 0 || lines < 0 ||
            this->Primitives[PrimitivePoints].IBO->IndexCount != static_cast<std::size_t>(points) ||
            this->Primitives[PrimitiveLines].IBO->IndexCount != static_cast<std::size_t>(lines)) {
            tracker_->record(sdk_copy::Component::vtk_index_build,
                             ledger::Stage::vtk_apply,
                             std::nullopt,
                             "installed index counts do not match the audited source path");
            ledger::unknown(ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes);
            return;
        }
        // Point indices reserve their complete size. Polyline indices reserve
        // when arity exceeds two. Line2 uses libc++ geometric growth; twice
        // the final size bounds all prior initialized-prefix relocations.
        const auto index_writes = static_cast<std::uint64_t>(points + lines) * sizeof(unsigned int);
        const auto line_moves = connectivity > 2 * cells
                                    ? 0
                                    : 2 * static_cast<std::uint64_t>(lines) * sizeof(unsigned int);
        tracker_->record(sdk_copy::Component::vtk_index_build,
                         ledger::Stage::vtk_apply,
                         state_bytes + assigned + index_writes + line_moves);
        ledger::add(ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes, index_writes);
    }

    void UpdateCameraShiftScale(vtkRenderer* renderer, vtkActor* actor) override {
        this->Superclass::UpdateCameraShiftScale(renderer, actor);
        if (!observing())
            return;
        const bool camera_shift =
            this->ShiftScaleMethod == ShiftScaleMethodType::NEAR_PLANE_SHIFT_SCALE ||
            this->ShiftScaleMethod == ShiftScaleMethodType::FOCAL_POINT_SHIFT_SCALE;
        tracker_->record(
            sdk_copy::Component::vtk_camera_shift,
            ledger::Stage::vtk_apply,
            camera_shift ? std::nullopt : std::optional<std::uint64_t>(0),
            "camera shift matrices and transformed coordinate buffers are not audited");
    }

    void SetCameraShaderParameters(vtkOpenGLHelper& cell,
                                   vtkRenderer* renderer,
                                   vtkActor* actor) override {
        this->Superclass::SetCameraShaderParameters(cell, renderer, actor);
        if (!observing())
            return;
        auto* vbo = this->VBOs->GetVBO("vertexMC");
        if (!cell.Program || !actor->GetIsIdentity() || !vbo) {
            tracker_->record(sdk_copy::Component::vtk_camera_shift,
                             ledger::Stage::vtk_apply,
                             std::nullopt,
                             "shader matrices require an identity actor and coordinate VBO");
            return;
        }
        // The original identity-actor branch has already looked up all four
        // names. Post-call queries borrow that cache; they cannot create a new
        // lazy uniform entry or alter the shader's original output.
        const bool display = cell.Program->IsUniformUsed("MCDCMatrix");
        const bool model_view = cell.Program->IsUniformUsed("MCVCMatrix");
        const bool normal = cell.Program->IsUniformUsed("normalMatrix");
        const bool environment = cell.Program->IsUniformUsed("envMatrix");
        tracker_->record(
            sdk_copy::Component::vtk_camera_shift,
            ledger::Stage::vtk_apply,
            display ? detail::autoShaderMatrixBytes(vbo, true, model_view, normal, environment)
                    : std::nullopt,
            "environment/absent-display/alternate coordinate shader paths are not audited");
    }

  private:
    bool observing() const {
        if (!tracker_ || !ledger::current())
            return false;
        if (std::string_view(vtkVersion::GetVTKVersion()) == "9.7.0")
            return true;
        tracker_->unsupported(sdk_copy::Component::vtk_mapper_build,
                              ledger::Stage::vtk_apply,
                              "VTK version differs from the 9.7.0 source audit");
        return false;
    }
    std::shared_ptr<sdk_copy::Tracker> tracker_;
};
vtkStandardNewMacro(ObservedPolyDataMapper);
} // namespace

struct VtkView::Impl {
    static constexpr std::size_t block_capacity = 1024;
    enum class Kind { node, beam, geometry, cell };
    struct Location {
        Kind kind;
        std::size_t index;
    };
    struct Endpoint {
        std::size_t block, point;
        Kind kind{Kind::beam};
    };
    struct Block {
        Kind kind;
        std::size_t begin;
        vtkSmartPointer<vtkActor> actor, highlight;
        vtkSmartPointer<vtkPoints> points;
        std::vector<std::size_t> cell_entities;
        std::set<std::size_t> selected;
        std::vector<vtkIdType> point_offsets{};
    };
    QVTKOpenGLNativeWidget* widget{};
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkActor> preview;
    RenderPacket packet;
    std::vector<Block> node_blocks, beam_blocks, geometry_blocks, cell_blocks;
    std::unordered_map<std::string, Location> locations;
    std::map<const vtkProp*, Block*> actors;
    std::vector<std::vector<Endpoint>> incident_endpoints;
    std::set<std::string> selected;
    VtkUpdateStats stats;
    bool delta_ready{};
    QPoint press;
    bool mouse_down{false};
    bool box_active{false};
    bool through{false};
    struct ObservedVbo {
        vtkSmartPointer<vtkOpenGLVertexBufferObject> buffer;
        vtkMTimeType upload_time{};
    };
    // Diagnostic references never copy geometry or change VBO/array modification times.
    std::map<vtkOpenGLVertexBufferObject*, ObservedVbo> observed_vbos;
    std::shared_ptr<ledger::OperationLedger> render_ledger;
    unsigned render_depth{};
    unsigned long render_start_observer{}, render_end_observer{};
    std::shared_ptr<sdk_copy::Tracker> sdk_copies = std::make_shared<sdk_copy::Tracker>(
        std::initializer_list<sdk_copy::Component>{sdk_copy::Component::vtk_mapper_check,
                                                   sdk_copy::Component::vtk_mapper_build,
                                                   sdk_copy::Component::vtk_index_build,
                                                   sdk_copy::Component::vtk_cell_texture,
                                                   sdk_copy::Component::vtk_coordinate_ranges,
                                                   sdk_copy::Component::vtk_camera_shift,
                                                   sdk_copy::Component::vtk_coordinate_vbo});

    ~Impl() {
        if (window) {
            window->RemoveObserver(render_start_observer);
            window->RemoveObserver(render_end_observer);
        }
    }
    void unknownModelBufferObservation() noexcept {
        if (!render_ledger)
            return;
        render_ledger->unknown(ledger::Stage::vtk_apply,
                               ledger::Metric::library_internal_copy_bytes);
        render_ledger->unknown(ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes);
        sdk_copies->unsupported(sdk_copy::Component::vtk_coordinate_vbo,
                                ledger::Stage::vtk_apply,
                                "coordinate VBO observation was unsupported or incomplete");
    }
    template <class Visitor> void visitModelVbos(Visitor visit) {
        for (const auto* blocks : {&node_blocks, &beam_blocks, &geometry_blocks, &cell_blocks})
            for (const auto& block : *blocks)
                for (const auto& actor : {block.actor, block.highlight}) {
                    auto* mapper = vtkOpenGLPolyDataMapper::SafeDownCast(actor->GetMapper());
                    if (!mapper) {
                        unknownModelBufferObservation();
                        continue;
                    }
                    if (auto* vbo = mapper->GetVBOs()->GetVBO("vertexMC"))
                        visit(vbo);
                }
    }
    void beginModelBufferObservation() noexcept {
        ++render_depth;
        if (render_depth != 1) {
            unknownModelBufferObservation();
            return;
        }
        render_ledger = ledger::current();
        if (!render_ledger)
            return;
        try {
            observed_vbos.clear();
            visitModelVbos([&](vtkOpenGLVertexBufferObject* vbo) {
                observed_vbos.try_emplace(vbo, ObservedVbo{vbo, vbo->GetUploadTime().GetMTime()});
            });
        } catch (...) {
            unknownModelBufferObservation();
        }
    }
    void endModelBufferObservation() noexcept {
        if (render_depth == 0 || --render_depth != 0 || !render_ledger)
            return;
        try {
            // Source audit is limited to the installed VTK 9.7.0 coordinate VBO path:
            // UploadDataArray resizes PackedVBO then converts double points to float.
            // Two destination-buffer writes conservatively include resize initialization.
            // Other VTK model buffers remain separate, explicitly unaudited boundaries.
            if (std::string_view(vtkVersion::GetVTKVersion()) != "9.7.0") {
                unknownModelBufferObservation();
            } else {
                std::set<vtkOpenGLVertexBufferObject*> counted;
                visitModelVbos([&](vtkOpenGLVertexBufferObject* vbo) {
                    if (!counted.insert(vbo).second)
                        return;
                    const auto previous = observed_vbos.find(vbo);
                    if (previous != observed_vbos.end() &&
                        previous->second.upload_time == vbo->GetUploadTime().GetMTime())
                        return;
                    if (vbo->GetDataType() != VTK_FLOAT || vbo->GetNumberOfComponents() != 3 ||
                        vbo->GetStride() != 3 * sizeof(float) || !vbo->GetPackedVBO().empty()) {
                        unknownModelBufferObservation();
                        return;
                    }
                    const auto bytes =
                        static_cast<std::uint64_t>(vbo->GetNumberOfTuples()) * vbo->GetStride();
                    sdk_copies->record(sdk_copy::Component::vtk_coordinate_vbo,
                                       ledger::Stage::vtk_apply,
                                       2 * bytes);
                    // CacheDataArray supplies exactly one AOS double3 input;
                    // BuildAllVBOs makes one UploadDataArray call on an actual
                    // upload-time change. Shared VBOs are deduplicated above.
                    // AUTO's worker writes the same PackedVBO float3 values
                    // already charged by 2*bytes, not a second transform array.
                    sdk_copies->record(sdk_copy::Component::vtk_coordinate_ranges,
                                       ledger::Stage::vtk_apply,
                                       detail::autoShiftVectorBytes(vbo),
                                       "shift vectors require the audited AUTO/DISABLE path");
                    // This is the actual OpenGL upload argument length, not GPU memory traffic.
                    render_ledger->add(
                        ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes, bytes);
                });
            }
        } catch (...) {
            unknownModelBufferObservation();
        }
        observed_vbos.clear();
        render_ledger.reset();
    }
    BoxMode box_mode{BoxMode::intersecting};
    QTimer camera_timer;
    QString last_camera;

    QPointF screenPoint(const std::array<double, 3>& xyz) const {
        renderer->SetWorldPoint(xyz[0], xyz[1], xyz[2], 1.0);
        renderer->WorldToDisplay();
        const auto* point = renderer->GetDisplayPoint();
        return {point[0], widget->height() * widget->devicePixelRatioF() - point[1]};
    }

    QStringList projectedBox(const QRectF& rect, BoxMode mode) const {
        std::set<QString> selected_ids;
        const auto lineHit = [&](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            const auto pa = screenPoint(a);
            const auto pb = screenPoint(b);
            return mode == BoxMode::contained ? inside(pa, rect) && inside(pb, rect)
                                              : segmentIntersectsRect(pa, pb, rect);
        };
        for (const auto& point : packet.points)
            if (point.visible && inside(screenPoint(point.position_mm), rect))
                selected_ids.insert(qs(point.entity.value));
        for (const auto& beam : packet.beams) {
            if (!beam.visible || beam.points[0] >= packet.points.size() ||
                beam.points[1] >= packet.points.size())
                continue;
            const auto& a = packet.points[beam.points[0]];
            const auto& b = packet.points[beam.points[1]];
            if (lineHit(a.position_mm, b.position_mm))
                selected_ids.insert(qs(beam.entity.value));
        }
        for (const auto& line : packet.geometry_lines)
            if (line.visible && lineHit(line.start_mm, line.end_mm))
                selected_ids.insert(qs(line.entity.value));
        for (const auto& cell : packet.cells) {
            if (!cell.visible)
                continue;
            std::vector<QPointF> projected;
            for (const auto index : cell.points)
                projected.push_back(screenPoint(packet.points[index].position_mm));
            bool hit = std::all_of(projected.begin(), projected.end(), [&](const auto& point) {
                return inside(point, rect);
            });
            if (mode == BoxMode::intersecting) {
                for (std::size_t i = 1; i < projected.size(); ++i)
                    hit = hit || segmentIntersectsRect(projected[i - 1], projected[i], rect);
                if (cell.kind == RenderCellKind::polygon) {
                    hit = hit || segmentIntersectsRect(projected.back(), projected.front(), rect);
                    for (const auto& corner :
                         {rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()})
                        hit = hit || polygonContains(projected, corner);
                }
            }
            if (hit)
                selected_ids.insert(qs(cell.entity.value));
        }
        return QStringList(selected_ids.begin(), selected_ids.end());
    }

    QStringList hardwareBox(const QRect& area) const {
        vtkNew<vtkHardwareSelector> selector;
        selector->SetRenderer(renderer);
        selector->SetFieldAssociation(vtkDataObject::FIELD_ASSOCIATION_CELLS);
        const auto height = widget->height() * widget->devicePixelRatioF();
        selector->SetArea(area.left(), height - area.bottom(), area.right(), height - area.top());
        auto result = vtkSmartPointer<vtkSelection>::Take(selector->Select());
        std::set<QString> ids;
        if (!result)
            return {};
        for (unsigned int i = 0; i < result->GetNumberOfNodes(); ++i) {
            auto* node = result->GetNode(i);
            const auto* prop =
                vtkProp::SafeDownCast(node->GetProperties()->Get(vtkSelectionNode::PROP()));
            const auto* cells = vtkIdTypeArray::SafeDownCast(node->GetSelectionList());
            if (!cells)
                continue;
            const auto found = actors.find(prop);
            if (found == actors.end())
                continue;
            const auto& block = *found->second;
            for (vtkIdType j = 0; j < cells->GetNumberOfValues(); ++j) {
                const auto index = static_cast<std::size_t>(cells->GetValue(j));
                if (index >= block.cell_entities.size())
                    continue;
                const auto entity = block.cell_entities[index];
                if (block.kind == Kind::node)
                    ids.insert(qs(packet.points[entity].entity.value));
                else if (block.kind == Kind::beam)
                    ids.insert(qs(packet.beams[entity].entity.value));
                else if (block.kind == Kind::cell)
                    ids.insert(qs(packet.cells[entity].entity.value));
                else
                    ids.insert(qs(packet.geometry_lines[entity].entity.value));
            }
        }
        return QStringList(ids.begin(), ids.end());
    }

    QStringList visibleBox(const QRect& area, BoxMode mode) const {
        const auto visible = hardwareBox(area);
        const auto projected = projectedBox(area, mode);
        QStringList result;
        for (const auto& id : visible)
            if (projected.contains(id))
                result.append(id);
        return result;
    }

    void makeActor(vtkActor* actor,
                   vtkPoints* points,
                   vtkCellArray* cells,
                   bool vertices,
                   const double color[3],
                   double width,
                   vtkCellArray* polygons = nullptr) {
        vtkNew<vtkPolyData> geometry;
        geometry->SetPoints(points);
        if (vertices)
            geometry->SetVerts(cells);
        else
            geometry->SetLines(cells);
        if (polygons)
            geometry->SetPolys(polygons);
        vtkNew<ObservedPolyDataMapper> mapper;
        mapper->setCopyTracker(sdk_copies);
        mapper->SetInputData(geometry);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(color[0], color[1], color[2]);
        if (vertices)
            actor->GetProperty()->SetPointSize(width);
        else
            actor->GetProperty()->SetLineWidth(width);
    }

    static bool finite(const std::array<double, 3>& point) {
        return std::all_of(
            point.begin(), point.end(), [](double value) { return std::isfinite(value); });
    }
    Block& blockAt(const Location& location) {
        const auto index = location.index / block_capacity;
        return location.kind == Kind::node   ? node_blocks[index]
               : location.kind == Kind::beam ? beam_blocks[index]
               : location.kind == Kind::cell ? cell_blocks[index]
                                             : geometry_blocks[index];
    }
    Block& endpointBlock(const Endpoint& endpoint) {
        return endpoint.kind == Kind::cell ? cell_blocks[endpoint.block]
                                           : beam_blocks[endpoint.block];
    }
    void cellArrays(Block& block, vtkCellArray* lines, vtkCellArray* polygons, bool highlight) {
        std::vector<std::size_t> line_entities, polygon_entities;
        const auto count = static_cast<vtkIdType>(block.point_offsets.size() - 1);
        const auto indices = block.point_offsets.back();
        lines->AllocateExact(count, indices);
        polygons->AllocateExact(count, indices);
        line_entities.reserve(static_cast<std::size_t>(count));
        polygon_entities.reserve(static_cast<std::size_t>(count));
        std::uint64_t copied{};
        for (std::size_t local = 0; local + 1 < block.point_offsets.size(); ++local) {
            const auto index = block.begin + local;
            const auto& cell = packet.cells[index];
            if (!cell.visible || (highlight && !block.selected.contains(index)))
                continue;
            const auto first = block.point_offsets[local], end = block.point_offsets[local + 1];
            auto* array = cell.kind == RenderCellKind::polyline ? lines : polygons;
            array->InsertNextCell(end - first);
            for (auto point = first; point < end; ++point)
                array->InsertCellPoint(point);
            (cell.kind == RenderCellKind::polyline ? line_entities : polygon_entities)
                .push_back(index);
            copied += static_cast<std::uint64_t>(end - first + 1) * sizeof(vtkIdType);
            if (highlight)
                ++stats.highlight_cells_written;
        }
        if (!highlight) {
            block.cell_entities = std::move(line_entities);
            block.cell_entities.insert(
                block.cell_entities.end(), polygon_entities.begin(), polygon_entities.end());
            copied += 3 * block.cell_entities.size() * sizeof(std::size_t);
        }
        ledger::add(ledger::Stage::vtk_apply, ledger::Metric::array_write_bytes, copied);
    }
    void refreshHighlight(Block& block) {
        if (block.kind == Kind::cell) {
            vtkNew<vtkCellArray> lines;
            vtkNew<vtkCellArray> polygons;
            cellArrays(block, lines, polygons, true);
            const double color[]{1.0, 0.62, 0.13};
            makeActor(block.highlight, block.points, lines, false, color, 7, polygons);
            block.highlight->PickableOff();
            ++stats.highlight_blocks;
            return;
        }
        vtkNew<vtkCellArray> cells;
        cells->AllocateEstimate(static_cast<vtkIdType>(block.selected.size()),
                                block.kind == Kind::node ? 1 : 2);
        const auto prior_written = stats.highlight_cells_written;
        for (const auto index : block.selected) {
            if (block.kind == Kind::node) {
                if (!packet.points[index].visible)
                    continue;
                const vtkIdType point = static_cast<vtkIdType>(index - block.begin);
                cells->InsertNextCell(1, &point);
            } else {
                if ((block.kind == Kind::beam &&
                     (!packet.beams[index].visible ||
                      packet.beams[index].points[0] >= packet.points.size() ||
                      packet.beams[index].points[1] >= packet.points.size())) ||
                    (block.kind == Kind::geometry && !packet.geometry_lines[index].visible))
                    continue;
                const auto first = static_cast<vtkIdType>((index - block.begin) * 2);
                const vtkIdType endpoints[]{first, first + 1};
                cells->InsertNextCell(2, endpoints);
            }
            ++stats.highlight_cells_written;
        }
        const double color[]{1.0, 0.62, 0.13};
        makeActor(block.highlight,
                  block.points,
                  cells,
                  block.kind == Kind::node,
                  color,
                  block.kind == Kind::node ? 13 : 7);
        block.highlight->PickableOff();
        ++stats.highlight_blocks;
        ledger::add(ledger::Stage::vtk_apply,
                    ledger::Metric::array_write_bytes,
                    (stats.highlight_cells_written - prior_written) *
                            (block.kind == Kind::node ? 2 : 3) * sizeof(vtkIdType) +
                        sizeof(vtkIdType));
    }
    void refreshVisibility(Block& block) {
        if (block.kind == Kind::cell) {
            vtkNew<vtkCellArray> lines;
            vtkNew<vtkCellArray> polygons;
            cellArrays(block, lines, polygons, false);
            auto* geometry = vtkPolyData::SafeDownCast(block.actor->GetMapper()->GetInput());
            geometry->SetLines(lines);
            geometry->SetPolys(polygons);
            refreshHighlight(block);
            return;
        }
        vtkNew<vtkCellArray> cells;
        block.cell_entities.clear();
        const auto count = static_cast<std::size_t>(block.points->GetNumberOfPoints()) /
                           (block.kind == Kind::node ? 1 : 2);
        block.cell_entities.reserve(count);
        cells->AllocateEstimate(static_cast<vtkIdType>(count), block.kind == Kind::node ? 1 : 2);
        for (std::size_t local = 0; local < count; ++local) {
            const auto index = block.begin + local;
            const bool visible = block.kind == Kind::node   ? packet.points[index].visible
                                 : block.kind == Kind::beam ? packet.beams[index].visible
                                                            : packet.geometry_lines[index].visible;
            if (!visible)
                continue;
            const vtkIdType first =
                static_cast<vtkIdType>(local * (block.kind == Kind::node ? 1 : 2));
            const vtkIdType endpoints[]{first, first + 1};
            cells->InsertNextCell(block.kind == Kind::node ? 1 : 2, endpoints);
            block.cell_entities.push_back(index);
        }
        auto* geometry = vtkPolyData::SafeDownCast(block.actor->GetMapper()->GetInput());
        if (block.kind == Kind::node)
            geometry->SetVerts(cells);
        else
            geometry->SetLines(cells);
        ledger::add(
            ledger::Stage::vtk_apply,
            ledger::Metric::array_write_bytes,
            block.cell_entities.size() *
                (sizeof(std::size_t) + (block.kind == Kind::node ? 2 : 3) * sizeof(vtkIdType)));
        refreshHighlight(block);
    }
    void finishDisplayUpdate() {
        if (ledger::current()) {
            sdk_copies->beginOperation();
            // A measured run completes at GPU synchronization, not at Render() return.
            window->WaitForCompletion();
            // A version-only delta performs no Render/coordinate upload. Known zero
            // here describes this component, not complete VTK SDK copy coverage.
            ledger::add(ledger::Stage::vtk_apply, ledger::Metric::library_internal_copy_bytes, 0);
            ledger::add(ledger::Stage::vtk_apply, ledger::Metric::gpu_upload_bytes, 0);
            ledger::cover(ledger::Stage::vtk_complete);
        }
    }
    void installCoordinateObserver(vtkPoints* points) {
        vtkNew<detail::ObservedCoordinateArray> coordinates;
        coordinates->SetNumberOfComponents(3);
        coordinates->setCopyTracker(sdk_copies);
        points->SetData(coordinates);
    }
    Block makeCellBlock(std::size_t begin, std::size_t count) {
        Block block{Kind::cell,
                    begin,
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkPoints>::New(),
                    {},
                    {}};
        installCoordinateObserver(block.points);
        block.point_offsets.reserve(count + 1);
        block.point_offsets.push_back(0);
        for (std::size_t local = 0; local < count; ++local)
            block.point_offsets.push_back(
                block.point_offsets.back() +
                static_cast<vtkIdType>(packet.cells[begin + local].points.size()));
        block.points->SetNumberOfPoints(block.point_offsets.back());
        for (std::size_t local = 0; local < count; ++local) {
            const auto index = begin + local;
            const auto& cell = packet.cells[index];
            for (std::size_t j = 0; j < cell.points.size(); ++j) {
                const auto source = cell.points[j];
                const auto destination = block.point_offsets[local] + static_cast<vtkIdType>(j);
                block.points->SetPoint(destination, packet.points[source].position_mm.data());
                incident_endpoints[source].push_back(
                    {begin / block_capacity, static_cast<std::size_t>(destination), Kind::cell});
            }
            if (cell.entity.value.empty() ||
                !locations.emplace(cell.entity.value, Location{Kind::cell, index}).second)
                delta_ready = false;
            if (selected.contains(cell.entity.value))
                block.selected.insert(index);
        }
        vtkNew<vtkCellArray> lines;
        vtkNew<vtkCellArray> polygons;
        cellArrays(block, lines, polygons, false);
        const double color[]{0.8, 0.85, 0.91};
        makeActor(block.actor, block.points, lines, false, color, 3, polygons);
        refreshHighlight(block);
        const auto bytes =
            static_cast<std::uint64_t>(block.point_offsets.back()) * 3 * sizeof(double);
        stats.coordinate_bytes_copied += bytes;
        stats.dirty_coordinate_array_bytes += bytes;
        return block;
    }
    Block makeBlock(Kind kind, std::size_t begin, std::size_t count) {
        if (kind == Kind::cell)
            return makeCellBlock(begin, count);
        Block block{kind,
                    begin,
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkPoints>::New(),
                    {},
                    {}};
        installCoordinateObserver(block.points);
        block.points->SetNumberOfPoints(
            static_cast<vtkIdType>(count * (kind == Kind::node ? 1 : 2)));
        vtkNew<vtkCellArray> cells;
        for (std::size_t local = 0; local < count; ++local) {
            const auto index = begin + local;
            const EntityId* entity{};
            if (kind == Kind::node) {
                const auto& point = packet.points[index];
                entity = &point.entity;
                delta_ready = delta_ready && finite(point.position_mm);
                block.points->SetPoint(static_cast<vtkIdType>(local), point.position_mm.data());
                if (point.visible) {
                    const auto cell = static_cast<vtkIdType>(local);
                    cells->InsertNextCell(1, &cell);
                    block.cell_entities.push_back(index);
                }
            } else {
                std::array<double, 3> first{}, last{};
                if (kind == Kind::beam) {
                    const auto& beam = packet.beams[index];
                    entity = &beam.entity;
                    if (beam.points[0] >= packet.points.size() ||
                        beam.points[1] >= packet.points.size()) {
                        delta_ready = false;
                        continue;
                    }
                    first = packet.points[beam.points[0]].position_mm;
                    last = packet.points[beam.points[1]].position_mm;
                    incident_endpoints[beam.points[0]].push_back(
                        {begin / block_capacity, local * 2});
                    incident_endpoints[beam.points[1]].push_back(
                        {begin / block_capacity, local * 2 + 1});
                } else {
                    const auto& line = packet.geometry_lines[index];
                    entity = &line.entity;
                    first = line.start_mm;
                    last = line.end_mm;
                    delta_ready = delta_ready && finite(first) && finite(last);
                }
                const auto cell = static_cast<vtkIdType>(local * 2);
                block.points->SetPoint(cell, first.data());
                block.points->SetPoint(cell + 1, last.data());
                const vtkIdType endpoints[]{cell, cell + 1};
                const bool visible = kind == Kind::beam ? packet.beams[index].visible
                                                        : packet.geometry_lines[index].visible;
                if (visible) {
                    cells->InsertNextCell(2, endpoints);
                    block.cell_entities.push_back(index);
                }
            }
            if (entity->value.empty() ||
                !locations.emplace(entity->value, Location{kind, index}).second)
                delta_ready = false;
            if (selected.contains(entity->value))
                block.selected.insert(index);
        }
        const double node_color[]{0.22, 0.7, 0.95};
        const double beam_color[]{0.8, 0.85, 0.91};
        const double geometry_color[]{0.42, 0.87, 0.68};
        makeActor(block.actor,
                  block.points,
                  cells,
                  kind == Kind::node,
                  kind == Kind::node   ? node_color
                  : kind == Kind::beam ? beam_color
                                       : geometry_color,
                  kind == Kind::node ? 7 : 3);
        refreshHighlight(block);
        stats.coordinate_bytes_copied += block.points->GetNumberOfPoints() * 3 * sizeof(double);
        stats.dirty_coordinate_array_bytes +=
            block.points->GetNumberOfPoints() * 3 * sizeof(double);
        return block;
    }
    void rebuild() {
        stats = {};
        stats.full_rebuilds = 1;
        stats.coordinate_bytes_copied = packet.points.size() * 3 * sizeof(double) +
                                        packet.geometry_lines.size() * 6 * sizeof(double);
        for (auto* blocks : {&node_blocks, &beam_blocks, &geometry_blocks, &cell_blocks}) {
            for (auto& block : *blocks) {
                renderer->RemoveActor(block.actor);
                renderer->RemoveActor(block.highlight);
            }
            blocks->clear();
        }
        actors.clear();
        locations.clear();
        locations.reserve(packet.points.size() + packet.beams.size() +
                          packet.geometry_lines.size() + packet.cells.size());
        incident_endpoints.clear();
        incident_endpoints.resize(packet.points.size());
        delta_ready = !packet.view_session_id.empty();
        const auto build = [&](std::vector<Block>& blocks, Kind kind, std::size_t size) {
            blocks.reserve((size + block_capacity - 1) / block_capacity);
            for (std::size_t begin = 0; begin < size; begin += block_capacity)
                blocks.push_back(makeBlock(kind, begin, std::min(block_capacity, size - begin)));
        };
        build(node_blocks, Kind::node, packet.points.size());
        build(beam_blocks, Kind::beam, packet.beams.size());
        build(geometry_blocks, Kind::geometry, packet.geometry_lines.size());
        build(cell_blocks, Kind::cell, packet.cells.size());
        for (auto* blocks : {&beam_blocks, &geometry_blocks, &cell_blocks, &node_blocks})
            for (auto& block : *blocks) {
                actors.emplace(block.actor, &block);
                renderer->AddActor(block.actor);
                renderer->AddActor(block.highlight);
            }
        stats.node_blocks = node_blocks.size();
        stats.beam_blocks = beam_blocks.size();
        stats.geometry_blocks = geometry_blocks.size();
        stats.cell_blocks = cell_blocks.size();
        window->Render();
        finishDisplayUpdate();
    }
    void select(const QStringList& ids) {
        stats = {};
        std::set<std::string> next;
        for (const auto& id : ids)
            if (locations.contains(id.toStdString()))
                next.insert(id.toStdString());
        std::set<Block*> dirty;
        for (const auto& id : selected)
            if (!next.contains(id)) {
                const auto found = locations.find(id);
                if (found == locations.end())
                    continue;
                auto& block = blockAt(found->second);
                block.selected.erase(found->second.index);
                dirty.insert(&block);
            }
        for (const auto& id : next)
            if (!selected.contains(id)) {
                const auto location = locations.at(id);
                auto& block = blockAt(location);
                block.selected.insert(location.index);
                dirty.insert(&block);
            }
        selected = std::move(next);
        for (auto* block : dirty)
            refreshHighlight(*block);
        if (!dirty.empty())
            window->Render();
    }
    bool apply(const RenderDelta& delta) {
        stats = {};
        if (!delta_ready || delta.document.id != packet.document.id ||
            delta.document.epoch != packet.document.epoch ||
            delta.view_session_id != packet.view_session_id ||
            delta.base_revision != packet.revision ||
            delta.base_view_revision != packet.view_revision ||
            delta.revision < delta.base_revision ||
            delta.view_revision < delta.base_view_revision ||
            ((!delta.points.empty() || !delta.geometry_lines.empty()) &&
             delta.revision == delta.base_revision) ||
            (!delta.visibility.empty() && delta.view_revision == delta.base_view_revision))
            return false;
        std::set<std::size_t> point_indices, geometry_indices;
        std::set<Block*> dirty_nodes, dirty_beams, dirty_geometry, dirty_cells, visibility_blocks;
        // Validate every update and build affected sets before changing retained state.
        for (const auto& update : delta.points) {
            if (update.index >= packet.points.size() ||
                !point_indices.insert(update.index).second ||
                update.point.entity != packet.points[update.index].entity ||
                update.point.visible != packet.points[update.index].visible ||
                !finite(update.point.position_mm))
                return false;
            dirty_nodes.insert(&node_blocks[update.index / block_capacity]);
            for (const auto& endpoint : incident_endpoints[update.index])
                (endpoint.kind == Kind::cell ? dirty_cells : dirty_beams)
                    .insert(&endpointBlock(endpoint));
        }
        for (const auto& update : delta.geometry_lines) {
            if (update.index >= packet.geometry_lines.size() ||
                !geometry_indices.insert(update.index).second ||
                update.line.entity != packet.geometry_lines[update.index].entity ||
                update.line.visible != packet.geometry_lines[update.index].visible ||
                !finite(update.line.start_mm) || !finite(update.line.end_mm))
                return false;
            dirty_geometry.insert(&geometry_blocks[update.index / block_capacity]);
        }
        std::set<std::pair<RenderPrimitive, std::size_t>> visibility_indices;
        std::set<std::string> visibility_ids;
        for (const auto& update : delta.visibility) {
            const auto found = locations.find(update.entity.value);
            const auto kind = update.primitive == RenderPrimitive::point  ? Kind::node
                              : update.primitive == RenderPrimitive::beam ? Kind::beam
                              : update.primitive == RenderPrimitive::cell ? Kind::cell
                                                                          : Kind::geometry;
            if (update.primitive < RenderPrimitive::point ||
                update.primitive > RenderPrimitive::cell || found == locations.end() ||
                found->second.kind != kind || found->second.index != update.index ||
                !visibility_indices.emplace(update.primitive, update.index).second ||
                !visibility_ids.insert(update.entity.value).second)
                return false;
            visibility_blocks.insert(&blockAt(found->second));
        }
        for (const auto& update : delta.points) {
            packet.points[update.index].position_mm = update.point.position_mm;
            node_blocks[update.index / block_capacity].points->SetPoint(
                static_cast<vtkIdType>(update.index % block_capacity),
                update.point.position_mm.data());
            stats.coordinate_bytes_copied += 6 * sizeof(double);
            for (const auto& endpoint : incident_endpoints[update.index]) {
                endpointBlock(endpoint).points->SetPoint(static_cast<vtkIdType>(endpoint.point),
                                                         update.point.position_mm.data());
                stats.coordinate_bytes_copied += 3 * sizeof(double);
            }
        }
        for (const auto& update : delta.geometry_lines) {
            auto& line = packet.geometry_lines[update.index];
            line.start_mm = update.line.start_mm;
            line.end_mm = update.line.end_mm;
            auto& points = geometry_blocks[update.index / block_capacity].points;
            const auto first = static_cast<vtkIdType>((update.index % block_capacity) * 2);
            points->SetPoint(first, line.start_mm.data());
            points->SetPoint(first + 1, line.end_mm.data());
            stats.coordinate_bytes_copied += 12 * sizeof(double);
        }
        for (const auto& update : delta.visibility) {
            if (update.primitive == RenderPrimitive::point)
                packet.points[update.index].visible = update.visible;
            else if (update.primitive == RenderPrimitive::beam)
                packet.beams[update.index].visible = update.visible;
            else if (update.primitive == RenderPrimitive::cell)
                packet.cells[update.index].visible = update.visible;
            else
                packet.geometry_lines[update.index].visible = update.visible;
        }
        for (const auto* dirty : {&dirty_nodes, &dirty_beams, &dirty_geometry, &dirty_cells})
            for (auto* block : *dirty) {
                block->points->Modified();
                stats.dirty_coordinate_array_bytes +=
                    block->points->GetNumberOfPoints() * 3 * sizeof(double);
            }
        for (auto* block : visibility_blocks) {
            refreshVisibility(*block);
            if (block->kind == Kind::node)
                dirty_nodes.insert(block);
            else if (block->kind == Kind::beam)
                dirty_beams.insert(block);
            else if (block->kind == Kind::cell)
                dirty_cells.insert(block);
            else
                dirty_geometry.insert(block);
        }
        stats.node_blocks = dirty_nodes.size();
        stats.beam_blocks = dirty_beams.size();
        stats.geometry_blocks = dirty_geometry.size();
        stats.cell_blocks = dirty_cells.size();
        packet.revision = delta.revision;
        packet.view_revision = delta.view_revision;
        ledger::add(ledger::Stage::vtk_apply,
                    ledger::Metric::model_copy_bytes,
                    stats.coordinate_bytes_copied + delta.visibility.size() * sizeof(bool));
        ledger::add(ledger::Stage::vtk_apply,
                    ledger::Metric::array_write_bytes,
                    stats.coordinate_bytes_copied);
        ledger::add(ledger::Stage::vtk_apply,
                    ledger::Metric::array_invalidated_bytes,
                    stats.dirty_coordinate_array_bytes);
        // Index payloads and identity strings are local. Allocator bookkeeping and
        // graphics-driver buffers are not in-process model-data copies.
        std::uint64_t metadata =
            (point_indices.size() + geometry_indices.size() + visibility_indices.size() +
             dirty_nodes.size() + dirty_beams.size() + dirty_geometry.size() + dirty_cells.size()) *
            4 * sizeof(std::size_t);
        for (const auto& id : visibility_ids)
            metadata += 3 * id.size();
        ledger::add(ledger::Stage::vtk_apply, ledger::Metric::metadata_copy_bytes, metadata);
        ledger::cover(ledger::Stage::vtk_apply);
        if (!dirty_nodes.empty() || !dirty_beams.empty() || !dirty_geometry.empty() ||
            !dirty_cells.empty())
            window->Render();
        finishDisplayUpdate();
        return true;
    }

    void setPreview(const RenderPreview& value) {
        vtkNew<vtkPoints> points;
        vtkNew<vtkCellArray> cells;
        for (const auto& point : value.points)
            points->InsertNextPoint(point.data());
        for (const auto& line : value.lines) {
            if (line[0] >= value.points.size() || line[1] >= value.points.size())
                continue;
            const vtkIdType ends[]{static_cast<vtkIdType>(line[0]),
                                   static_cast<vtkIdType>(line[1])};
            cells->InsertNextCell(2, ends);
        }
        const double color[]{0.82, 0.38, 1.0};
        makeActor(preview, points, cells, false, color, 5);
        preview->PickableOff();
        window->Render();
    }
};

VtkView::VtkView(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>()) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    impl_->widget = new QVTKOpenGLNativeWidget(this);
    layout->addWidget(impl_->widget);
    impl_->window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    impl_->renderer = vtkSmartPointer<vtkRenderer>::New();
    impl_->window->AddRenderer(impl_->renderer);
    vtkNew<vtkCallbackCommand> render_observer;
    render_observer->SetClientData(impl_.get());
    render_observer->SetCallback([](vtkObject*, unsigned long event, void* state, void*) {
        auto* implementation = static_cast<Impl*>(state);
        if (event == vtkCommand::StartEvent)
            implementation->beginModelBufferObservation();
        else
            implementation->endModelBufferObservation();
    });
    impl_->render_start_observer =
        impl_->window->AddObserver(vtkCommand::StartEvent, render_observer);
    impl_->render_end_observer = impl_->window->AddObserver(vtkCommand::EndEvent, render_observer);
    impl_->widget->setRenderWindow(impl_->window);
    vtkNew<vtkInteractorStyleTrackballCamera> style;
    impl_->widget->interactor()->SetInteractorStyle(style);
    impl_->renderer->SetBackground(0.08, 0.11, 0.16);
    impl_->renderer->SetBackground2(0.16, 0.2, 0.26);
    impl_->renderer->GradientBackgroundOn();
    impl_->preview = vtkSmartPointer<vtkActor>::New();
    impl_->preview->PickableOff();
    impl_->renderer->AddActor(impl_->preview);
    impl_->widget->installEventFilter(this);
    impl_->camera_timer.setSingleShot(true);
    impl_->camera_timer.setInterval(150);
    connect(&impl_->camera_timer, &QTimer::timeout, this, [this] {
        const auto fingerprint = cameraFingerprint();
        if (fingerprint != impl_->last_camera) {
            impl_->last_camera = fingerprint;
            emit cameraChanged(fingerprint);
        }
    });
}

VtkView::~VtkView() = default;

void VtkView::setPacket(const RenderPacket& packet) {
    std::set<std::string_view> identities;
    for (const auto& point : packet.points)
        identities.insert(point.entity.value);
    for (const auto& beam : packet.beams)
        identities.insert(beam.entity.value);
    for (const auto& line : packet.geometry_lines)
        identities.insert(line.entity.value);
    for (const auto& cell : packet.cells) {
        if (cell.entity.value.empty() || !identities.insert(cell.entity.value).second ||
            (cell.kind != RenderCellKind::polyline && cell.kind != RenderCellKind::polygon) ||
            cell.points.size() < (cell.kind == RenderCellKind::polyline ? 2U : 3U) ||
            cell.points.size() > render_cell_point_limit)
            return;
        std::set<std::size_t> unique;
        for (const auto index : cell.points)
            if (index >= packet.points.size() || !unique.insert(index).second ||
                !Impl::finite(packet.points[index].position_mm))
                return;
    }
    impl_->packet = packet;
    impl_->rebuild();
    if (!impl_->last_camera.size())
        fit();
}

void VtkView::setSelectedIds(const QStringList& ids) {
    impl_->select(ids);
}

bool VtkView::applyDelta(const RenderDelta& delta) {
    return impl_->apply(delta);
}

bool VtkView::pendingCameraUpdate() const {
    return impl_->camera_timer.isActive();
}

VtkUpdateStats VtkView::lastUpdateStats() const {
    return impl_->stats;
}

void VtkView::setPreview(const RenderPreview& preview) {
    impl_->setPreview(preview);
}

void VtkView::clearPreview() {
    impl_->setPreview({});
}

void VtkView::fit() {
    impl_->renderer->ResetCamera();
    impl_->renderer->ResetCameraClippingRange();
    impl_->window->Render();
    impl_->camera_timer.start();
}

void VtkView::standardView(StandardView view) {
    auto* camera = impl_->renderer->GetActiveCamera();
    const auto* focal = camera->GetFocalPoint();
    const auto distance = camera->GetDistance();
    double direction[3]{1, 1, 1};
    double up[3]{0, 0, 1};
    if (view == StandardView::front)
        direction[0] = 0, direction[1] = -1, direction[2] = 0;
    else if (view == StandardView::top)
        direction[0] = 0, direction[1] = 0, direction[2] = 1, up[1] = 1, up[2] = 0;
    else if (view == StandardView::right)
        direction[0] = 1, direction[1] = 0, direction[2] = 0;
    const auto norm = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                direction[2] * direction[2]);
    camera->SetPosition(focal[0] + distance * direction[0] / norm,
                        focal[1] + distance * direction[1] / norm,
                        focal[2] + distance * direction[2] / norm);
    camera->SetViewUp(up);
    camera->ParallelProjectionOn();
    impl_->renderer->ResetCameraClippingRange();
    impl_->window->Render();
    impl_->camera_timer.start();
}

void VtkView::setBoxMode(BoxMode mode) {
    impl_->box_mode = mode;
}
void VtkView::setThroughSelection(bool enabled) {
    impl_->through = enabled;
}
bool VtkView::hasPacket() const {
    return !impl_->packet.view_session_id.empty();
}
std::optional<ResourceVersion> VtkView::installedVersion() const {
    if (!impl_->delta_ready)
        return std::nullopt;
    return ResourceVersion{impl_->packet.document,
                           impl_->packet.revision,
                           impl_->packet.view_session_id,
                           impl_->packet.view_revision};
}

SdkCopySnapshot VtkView::sdkCopyObservation() const {
    return impl_->sdk_copies->snapshot();
}

QString VtkView::cameraFingerprint() const {
    auto* camera = impl_->renderer->GetActiveCamera();
    QByteArray bytes;
    const auto append = [&bytes](const double* values, int count) {
        for (int i = 0; i < count; ++i)
            bytes += QByteArray::number(values[i], 'g', 17) + ',';
    };
    append(camera->GetPosition(), 3);
    append(camera->GetFocalPoint(), 3);
    append(camera->GetViewUp(), 3);
    bytes += QByteArray::number(camera->GetParallelScale(), 'g', 17);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool VtkView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == impl_->widget) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                impl_->press = mouse->pos();
                impl_->mouse_down = true;
                impl_->box_active = mouse->modifiers().testFlag(Qt::ShiftModifier);
                if (impl_->box_active)
                    return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && impl_->mouse_down && hasPacket()) {
                impl_->mouse_down = false;
                const auto rect = QRect(impl_->press, mouse->pos()).normalized();
                const auto dpr = impl_->widget->devicePixelRatioF();
                const QRect pixels(QPoint(rect.left() * dpr, rect.top() * dpr),
                                   QPoint(rect.right() * dpr, rect.bottom() * dpr));
                if (impl_->box_active || (rect.width() < 4 && rect.height() < 4)) {
                    const bool click = rect.width() < 4 && rect.height() < 4;
                    const auto mode = click ? BoxMode::intersecting : impl_->box_mode;
                    const auto area = click ? pixels.adjusted(-3, -3, 3, 3) : pixels;
                    const auto ids = impl_->through ? impl_->projectedBox(area, mode)
                                                    : impl_->visibleBox(area, mode);
                    emit picked(ids, impl_->through);
                    if (impl_->box_active) {
                        impl_->box_active = false;
                        return true;
                    }
                }
            }
        } else if (event->type() == QEvent::MouseMove || event->type() == QEvent::Wheel) {
            if (impl_->box_active && event->type() == QEvent::MouseMove)
                return true;
            impl_->camera_timer.start();
        }
    }
    return QWidget::eventFilter(watched, event);
}
} // namespace qcae
