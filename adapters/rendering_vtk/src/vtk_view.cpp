#include "qcae/vtk_view.hpp"

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
#include <unordered_map>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkDataObject.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkHardwareSelector.h>
#include <vtkIdTypeArray.h>
#include <vtkInformation.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSelection.h>
#include <vtkSelectionNode.h>

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
} // namespace

struct VtkView::Impl {
    static constexpr std::size_t block_capacity = 1024;
    enum class Kind { node, beam, geometry };
    struct Location {
        Kind kind;
        std::size_t index;
    };
    struct Endpoint {
        std::size_t block, point;
    };
    struct Block {
        Kind kind;
        std::size_t begin;
        vtkSmartPointer<vtkActor> actor, highlight;
        vtkSmartPointer<vtkPoints> points;
        std::vector<std::size_t> cell_entities;
        std::set<std::size_t> selected;
    };
    QVTKOpenGLNativeWidget* widget{};
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkActor> preview;
    RenderPacket packet;
    std::vector<Block> node_blocks, beam_blocks, geometry_blocks;
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
            if (beam.points[0] >= packet.points.size() || beam.points[1] >= packet.points.size())
                continue;
            const auto& a = packet.points[beam.points[0]];
            const auto& b = packet.points[beam.points[1]];
            if (lineHit(a.position_mm, b.position_mm))
                selected_ids.insert(qs(beam.entity.value));
        }
        for (const auto& line : packet.geometry_lines)
            if (lineHit(line.start_mm, line.end_mm))
                selected_ids.insert(qs(line.entity.value));
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
                   double width) {
        vtkNew<vtkPolyData> geometry;
        geometry->SetPoints(points);
        if (vertices)
            geometry->SetVerts(cells);
        else
            geometry->SetLines(cells);
        vtkNew<vtkPolyDataMapper> mapper;
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
                                             : geometry_blocks[index];
    }
    void refreshHighlight(Block& block) {
        vtkNew<vtkCellArray> cells;
        for (const auto index : block.selected) {
            if (block.kind == Kind::node) {
                if (!packet.points[index].visible)
                    continue;
                const vtkIdType point = static_cast<vtkIdType>(index - block.begin);
                cells->InsertNextCell(1, &point);
            } else {
                if (block.kind == Kind::beam &&
                    (packet.beams[index].points[0] >= packet.points.size() ||
                     packet.beams[index].points[1] >= packet.points.size()))
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
    }
    Block makeBlock(Kind kind, std::size_t begin, std::size_t count) {
        Block block{kind,
                    begin,
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkActor>::New(),
                    vtkSmartPointer<vtkPoints>::New(),
                    {},
                    {}};
        block.points->SetDataTypeToDouble();
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
                cells->InsertNextCell(2, endpoints);
                block.cell_entities.push_back(index);
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
        for (auto* blocks : {&node_blocks, &beam_blocks, &geometry_blocks}) {
            for (auto& block : *blocks) {
                renderer->RemoveActor(block.actor);
                renderer->RemoveActor(block.highlight);
            }
            blocks->clear();
        }
        actors.clear();
        locations.clear();
        locations.reserve(packet.points.size() + packet.beams.size() +
                          packet.geometry_lines.size());
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
        for (auto* blocks : {&beam_blocks, &geometry_blocks, &node_blocks})
            for (auto& block : *blocks) {
                actors.emplace(block.actor, &block);
                renderer->AddActor(block.actor);
                renderer->AddActor(block.highlight);
            }
        stats.node_blocks = node_blocks.size();
        stats.beam_blocks = beam_blocks.size();
        stats.geometry_blocks = geometry_blocks.size();
        window->Render();
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
             delta.revision == delta.base_revision))
            return false;
        std::set<std::size_t> point_indices, geometry_indices;
        std::set<Block*> dirty_nodes, dirty_beams, dirty_geometry;
        // Build every affected-block set before touching either the packet or VTK arrays.
        for (const auto& update : delta.points) {
            if (update.index >= packet.points.size() ||
                !point_indices.insert(update.index).second ||
                update.point.entity != packet.points[update.index].entity ||
                update.point.visible != packet.points[update.index].visible ||
                !finite(update.point.position_mm))
                return false;
            dirty_nodes.insert(&node_blocks[update.index / block_capacity]);
            for (const auto& endpoint : incident_endpoints[update.index])
                dirty_beams.insert(&beam_blocks[endpoint.block]);
        }
        for (const auto& update : delta.geometry_lines) {
            if (update.index >= packet.geometry_lines.size() ||
                !geometry_indices.insert(update.index).second ||
                update.line.entity != packet.geometry_lines[update.index].entity ||
                !finite(update.line.start_mm) || !finite(update.line.end_mm))
                return false;
            dirty_geometry.insert(&geometry_blocks[update.index / block_capacity]);
        }
        for (const auto& update : delta.points) {
            packet.points[update.index].position_mm = update.point.position_mm;
            node_blocks[update.index / block_capacity].points->SetPoint(
                static_cast<vtkIdType>(update.index % block_capacity),
                update.point.position_mm.data());
            stats.coordinate_bytes_copied += 6 * sizeof(double);
            for (const auto& endpoint : incident_endpoints[update.index]) {
                beam_blocks[endpoint.block].points->SetPoint(static_cast<vtkIdType>(endpoint.point),
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
        for (const auto* dirty : {&dirty_nodes, &dirty_beams, &dirty_geometry})
            for (auto* block : *dirty) {
                block->points->Modified();
                stats.dirty_coordinate_array_bytes +=
                    block->points->GetNumberOfPoints() * 3 * sizeof(double);
            }
        stats.node_blocks = dirty_nodes.size();
        stats.beam_blocks = dirty_beams.size();
        stats.geometry_blocks = dirty_geometry.size();
        packet.revision = delta.revision;
        packet.view_revision = delta.view_revision;
        if (!dirty_nodes.empty() || !dirty_beams.empty() || !dirty_geometry.empty())
            window->Render();
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
