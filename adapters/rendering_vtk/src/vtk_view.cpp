#include "qcae/vtk_view.hpp"

#include <QCryptographicHash>
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <set>
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
    QVTKOpenGLNativeWidget* widget{};
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkActor> nodes;
    vtkSmartPointer<vtkActor> beams;
    vtkSmartPointer<vtkActor> geometry_lines;
    vtkSmartPointer<vtkActor> selected_nodes;
    vtkSmartPointer<vtkActor> selected_beams;
    vtkSmartPointer<vtkActor> selected_geometry_lines;
    vtkSmartPointer<vtkActor> preview;
    RenderPacket packet;
    QStringList selected;
    std::vector<std::size_t> node_cell_ids;
    std::vector<std::size_t> beam_cell_ids;
    std::vector<std::size_t> geometry_cell_ids;
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
            if (inside(screenPoint(point.position_mm), rect))
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
            for (vtkIdType j = 0; j < cells->GetNumberOfValues(); ++j) {
                const auto index = static_cast<std::size_t>(cells->GetValue(j));
                if (prop == nodes && index < node_cell_ids.size())
                    ids.insert(qs(packet.points[node_cell_ids[index]].entity.value));
                if (prop == beams && index < beam_cell_ids.size())
                    ids.insert(qs(packet.beams[beam_cell_ids[index]].entity.value));
                if (prop == geometry_lines && index < geometry_cell_ids.size())
                    ids.insert(qs(packet.geometry_lines[geometry_cell_ids[index]].entity.value));
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

    void rebuild() {
        node_cell_ids.clear();
        beam_cell_ids.clear();
        geometry_cell_ids.clear();
        vtkNew<vtkPoints> points;
        vtkNew<vtkCellArray> point_cells;
        vtkNew<vtkCellArray> line_cells;
        vtkNew<vtkCellArray> selected_point_cells;
        vtkNew<vtkCellArray> selected_line_cells;
        vtkNew<vtkPoints> geometry_points;
        vtkNew<vtkCellArray> geometry_cells;
        vtkNew<vtkCellArray> selected_geometry_cells;
        for (std::size_t index = 0; index < packet.points.size(); ++index) {
            const auto& point = packet.points[index];
            const auto id = points->InsertNextPoint(point.position_mm.data());
            if (!point.visible)
                continue;
            point_cells->InsertNextCell(1, &id);
            node_cell_ids.push_back(index);
            if (selected.contains(qs(point.entity.value)))
                selected_point_cells->InsertNextCell(1, &id);
        }
        for (std::size_t index = 0; index < packet.beams.size(); ++index) {
            const auto& beam = packet.beams[index];
            if (beam.points[0] >= packet.points.size() || beam.points[1] >= packet.points.size())
                continue;
            const vtkIdType ends[]{static_cast<vtkIdType>(beam.points[0]),
                                   static_cast<vtkIdType>(beam.points[1])};
            line_cells->InsertNextCell(2, ends);
            beam_cell_ids.push_back(index);
            if (selected.contains(qs(beam.entity.value)))
                selected_line_cells->InsertNextCell(2, ends);
        }
        // Geometry endpoints are display coordinates, never mesh nodes or pick identities.
        for (std::size_t index = 0; index < packet.geometry_lines.size(); ++index) {
            const auto& line = packet.geometry_lines[index];
            const vtkIdType ends[]{geometry_points->InsertNextPoint(line.start_mm.data()),
                                   geometry_points->InsertNextPoint(line.end_mm.data())};
            geometry_cells->InsertNextCell(2, ends);
            geometry_cell_ids.push_back(index);
            if (selected.contains(qs(line.entity.value)))
                selected_geometry_cells->InsertNextCell(2, ends);
        }
        const double node_color[]{0.22, 0.7, 0.95};
        const double beam_color[]{0.8, 0.85, 0.91};
        const double geometry_color[]{0.42, 0.87, 0.68};
        const double selected_color[]{1.0, 0.62, 0.13};
        makeActor(nodes, points, point_cells, true, node_color, 7);
        makeActor(beams, points, line_cells, false, beam_color, 3);
        makeActor(selected_nodes, points, selected_point_cells, true, selected_color, 13);
        makeActor(selected_beams, points, selected_line_cells, false, selected_color, 7);
        makeActor(geometry_lines, geometry_points, geometry_cells, false, geometry_color, 3);
        makeActor(selected_geometry_lines,
                  geometry_points,
                  selected_geometry_cells,
                  false,
                  selected_color,
                  7);
        selected_nodes->PickableOff();
        selected_beams->PickableOff();
        selected_geometry_lines->PickableOff();
        window->Render();
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
    impl_->nodes = vtkSmartPointer<vtkActor>::New();
    impl_->beams = vtkSmartPointer<vtkActor>::New();
    impl_->geometry_lines = vtkSmartPointer<vtkActor>::New();
    impl_->selected_nodes = vtkSmartPointer<vtkActor>::New();
    impl_->selected_beams = vtkSmartPointer<vtkActor>::New();
    impl_->selected_geometry_lines = vtkSmartPointer<vtkActor>::New();
    impl_->preview = vtkSmartPointer<vtkActor>::New();
    impl_->preview->PickableOff();
    for (auto* actor : {impl_->beams.GetPointer(),
                        impl_->geometry_lines.GetPointer(),
                        impl_->nodes.GetPointer(),
                        impl_->selected_beams.GetPointer(),
                        impl_->selected_geometry_lines.GetPointer(),
                        impl_->selected_nodes.GetPointer(),
                        impl_->preview.GetPointer()})
        impl_->renderer->AddActor(actor);
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
    impl_->selected = ids;
    impl_->rebuild();
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
