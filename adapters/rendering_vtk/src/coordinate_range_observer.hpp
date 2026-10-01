#pragma once

#include "qcae/sdk_copy_tracker.hpp"

#include <limits>
#include <string_view>
#include <vtkDoubleArray.h>
#include <vtkInformation.h>
#include <vtkObjectFactory.h>
#include <vtkSMPTools.h>
#include <vtkVersion.h>

namespace qcae::detail {

// This private adapter keeps VTK's original AOS array, range cache and scanner.
// Only actual virtual calls contribute to the operation's SDK observation.
class ObservedCoordinateArray final : public vtkDoubleArray {
  public:
    static ObservedCoordinateArray* New() {
        VTK_STANDARD_NEW_BODY(ObservedCoordinateArray);
    }
    vtkTypeMacro(ObservedCoordinateArray,
                 vtkDoubleArray) void setCopyTracker(std::shared_ptr<sdk_copy::Tracker> tracker) {
        tracker_ = std::move(tracker);
    }

  protected:
    ObservedCoordinateArray() = default;
    ~ObservedCoordinateArray() override = default;

    void ComputeRange(double output[2],
                      int component,
                      const unsigned char* ghosts,
                      unsigned char ghosts_to_skip) override {
        const bool observe = tracker_ && ledger::current();
        const bool supported = observe && auditedArray() && !ghosts && component >= 0 &&
                               component < GetNumberOfComponents();
        const bool cached = supported && HasInformation() && GetInformation()->Has(PER_COMPONENT());
        // No preflight GetRange or cache write: the superclass remains authoritative.
        this->Superclass::ComputeRange(output, component, ghosts, ghosts_to_skip);
        if (!observe)
            return;
        if (!supported) {
            unsupported("coordinate range requires VTK 9.7, double3, no ghosts and Sequential");
            return;
        }
        constexpr auto range_bytes = 6 * sizeof(double);
        // Two output initializations, value-initialized allCompRanges and two
        // final output assignments. A cold successful call stores all 3 ranges.
        const auto bytes = range_bytes + 4 * sizeof(double) +
                           (!cached && GetNumberOfTuples() > 0 ? range_bytes : 0);
        tracker_->record(
            sdk_copy::Component::vtk_coordinate_ranges, ledger::Stage::vtk_apply, bytes);
    }

    bool ComputeScalarRange(double* output,
                            const unsigned char* ghosts,
                            unsigned char ghosts_to_skip) override {
        const bool observe = tracker_ && ledger::current();
        const bool supported = observe && auditedArray() && !ghosts;
        const auto tuples = GetNumberOfTuples();
        const auto result = this->Superclass::ComputeScalarRange(output, ghosts, ghosts_to_skip);
        if (!observe)
            return result;
        if (!supported || tuples < 0) {
            unsupported(
                "coordinate scalar scan requires VTK 9.7, double3, no ghosts and Sequential");
            return result;
        }
        constexpr auto range_bytes = 6 * sizeof(double);
        constexpr auto fixed = 7 * range_bytes;
        constexpr auto per_tuple = 3 * 3 * sizeof(double);
        const auto count = static_cast<std::uint64_t>(tuples);
        if (count > (std::numeric_limits<std::uint64_t>::max() - fixed) / per_tuple) {
            unsupported("coordinate scalar scan copy bound overflow");
            return result;
        }
        // DoComputeScalarRange initializes the output even when N is zero.
        // Nonempty scans also initialize ReducedRange, TLS storage, copy its
        // exemplar, initialize/reduce it, and copy six final ranges. Each input
        // scalar is read once and can write at most two accumulator scalars.
        tracker_->record(sdk_copy::Component::vtk_coordinate_ranges,
                         ledger::Stage::vtk_apply,
                         count ? fixed + count * per_tuple : range_bytes);
        return result;
    }

  private:
    bool auditedArray() {
        return std::string_view(vtkVersion::GetVTKVersion()) == "9.7.0" &&
               GetNumberOfComponents() == 3 && GetDataType() == VTK_DOUBLE &&
               HasStandardMemoryLayout() &&
               std::string_view(vtkSMPTools::GetBackend()) == "Sequential";
    }
    void unsupported(std::string_view reason) noexcept {
        tracker_->unsupported(
            sdk_copy::Component::vtk_coordinate_ranges, ledger::Stage::vtk_apply, reason);
    }
    std::shared_ptr<sdk_copy::Tracker> tracker_;
};

} // namespace qcae::detail
