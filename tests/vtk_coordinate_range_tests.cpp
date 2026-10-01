#include "coordinate_range_observer.hpp"

#include <stdexcept>
#include <cmath>
#include <iostream>
#include <vtkNew.h>
#include <vtkOpenGLVertexBufferObject.h>
#include <vtkPoints.h>

namespace {
using namespace qcae;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

auto operation(const char* name) {
    return std::make_shared<ledger::OperationLedger>(
        ledger::Identity{name, "document", "epoch", 1});
}
auto ranges(const sdk_copy::Tracker& tracker) {
    const auto snapshot = tracker.snapshot();
    check(snapshot.components.size() == 1, "snapshot.components.size() == 1");
    check(snapshot.components.front().component == "vtk_coordinate_ranges",
          "snapshot.components.front().component == \"vtk_coordinate_ranges\"");
    return snapshot.components.front();
}
void setCoordinates(vtkDoubleArray* array) {
    array->SetNumberOfComponents(3);
    array->SetNumberOfTuples(2);
    array->SetTuple3(0, 0, 0, 0);
    array->SetTuple3(1, 1, 1, 1);
    array->Modified();
}
void sameRange(vtkDataArray* left, vtkDataArray* right, int component) {
    double a[2], b[2];
    left->GetRange(a, component);
    right->GetRange(b, component);
    check(a[0] == b[0] && a[1] == b[1], "a[0] == b[0] && a[1] == b[1]");
}
} // namespace

int main() {
    try {
        check(std::string_view(vtkVersion::GetVTKVersion()) == "9.7.0",
              "std::string_view(vtkVersion::GetVTKVersion()) == \"9.7.0\"");
        check(vtkSMPTools::SetBackend("Sequential"), "vtkSMPTools::SetBackend(\"Sequential\")");
        auto tracker = std::make_shared<sdk_copy::Tracker>(
            std::initializer_list<sdk_copy::Component>{sdk_copy::Component::vtk_coordinate_ranges});
        vtkNew<detail::ObservedCoordinateArray> observed;
        vtkNew<vtkDoubleArray> original;
        observed->setCopyTracker(tracker);
        setCoordinates(observed);
        setCoordinates(original);
        check(observed->HasStandardMemoryLayout(), "observed->HasStandardMemoryLayout()");
        check(vtkAOSDataArrayTemplate<double>::FastDownCast(observed),
              "vtkAOSDataArrayTemplate<double>::FastDownCast(observed)");

        constexpr std::uint64_t range_call = 10 * sizeof(double);
        constexpr std::uint64_t scan = (42 + 3 * 2 * 3) * sizeof(double);
        constexpr std::uint64_t cache_store = 6 * sizeof(double);
        {
            ledger::Scope scope(operation("cold"));
            for (int component = 0; component < 3; ++component)
                sameRange(observed, original, component);
            const auto observation = ranges(*tracker);
            check(observation.calls == 4, "observation.calls == 4");
            check(observation.copy_bytes == 3 * range_call + scan + cache_store,
                  "observation.copy_bytes == 3 * range_call + scan + cache_store");
        }
        {
            ledger::Scope scope(operation("hot"));
            for (int component = 0; component < 3; ++component)
                sameRange(observed, original, component);
            const auto observation = ranges(*tracker);
            check(observation.calls == 3, "observation.calls == 3");
            check(observation.copy_bytes == 3 * range_call,
                  "observation.copy_bytes == 3 * range_call");
        }

        // Bounds scan bypasses PER_COMPONENT. The next GetRange must still scan.
        vtkNew<vtkPoints> observed_points;
        vtkNew<vtkPoints> original_points;
        observed_points->SetData(observed);
        original_points->SetData(original);
        {
            ledger::Scope scope(operation("bounds-and-range"));
            const auto* a = observed_points->GetBounds();
            const auto* b = original_points->GetBounds();
            for (int index = 0; index < 6; ++index)
                check(a[index] == b[index], "a[index] == b[index]");
            for (int component = 0; component < 3; ++component)
                sameRange(observed, original, component);
            const auto observation = ranges(*tracker);
            check(observation.calls == 5, "observation.calls == 5");
            check(observation.copy_bytes == 2 * scan + 3 * range_call + cache_store,
                  "observation.copy_bytes == 2 * scan + 3 * range_call + cache_store");
        }
        {
            ledger::Scope scope(operation("cached-bounds"));
            observed_points->GetBounds();
            for (int component = 0; component < 3; ++component)
                sameRange(observed, original, component);
            check(ranges(*tracker).calls == 3, "ranges(*tracker).calls == 3");
        }

        // Actual VBO calls are exercised without an OpenGL context or upload.
        vtkNew<vtkOpenGLVertexBufferObject> vbo;
        vbo->SetCoordShiftAndScaleMethod(vtkOpenGLVertexBufferObject::AUTO_SHIFT_SCALE);
        observed->Modified();
        {
            ledger::Scope scope(operation("auto-near"));
            vbo->UpdateShiftScale(observed);
            check(!vbo->GetCoordShiftAndScaleEnabled(), "!vbo->GetCoordShiftAndScaleEnabled()");
            check(ranges(*tracker).calls == 4, "ranges(*tracker).calls == 4");
            check(ranges(*tracker).copy_bytes == 3 * range_call + scan + cache_store,
                  "ranges(*tracker).copy_bytes == 3 * range_call + scan + cache_store");
        }
        observed->SetTuple3(0, 100000, 100000, 100000);
        observed->SetTuple3(1, 100001, 100001, 100001);
        observed->Modified();
        {
            ledger::Scope scope(operation("auto-far"));
            vbo->UpdateShiftScale(observed);
            check(vbo->GetCoordShiftAndScaleEnabled(), "vbo->GetCoordShiftAndScaleEnabled()");
            check(ranges(*tracker).calls == 7, "ranges(*tracker).calls == 7");
            check(ranges(*tracker).copy_bytes == 6 * range_call + scan + cache_store,
                  "ranges(*tracker).copy_bytes == 6 * range_call + scan + cache_store");
        }

        vtkNew<detail::ObservedCoordinateArray> empty;
        empty->setCopyTracker(tracker);
        empty->SetNumberOfComponents(3);
        vtkNew<vtkPoints> empty_points;
        empty_points->SetData(empty);
        {
            ledger::Scope scope(operation("empty"));
            const auto* output = empty_points->GetBounds();
            check(output[0] > output[1], "output[0] > output[1]");
            check(ranges(*tracker).copy_bytes == 6 * sizeof(double),
                  "ranges(*tracker).copy_bytes == 6 * sizeof(double)");
        }
        {
            ledger::Scope scope(operation("unsupported-magnitude"));
            observed->GetRange(-1);
            check(!ranges(*tracker).copy_bytes, "!ranges(*tracker).copy_bytes");
        }
        if (vtkSMPTools::SetBackend("STDThread")) {
            ledger::Scope scope(operation("unsupported-backend"));
            double output[2];
            observed->GetRange(output, 0);
            check(!ranges(*tracker).copy_bytes, "!ranges(*tracker).copy_bytes");
            check(vtkSMPTools::SetBackend("Sequential"), "vtkSMPTools::SetBackend(\"Sequential\")");
        }
        std::cout << "PASS: actual VTK AOS ranges/bounds/cache/VBO branches and unknown backends\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
