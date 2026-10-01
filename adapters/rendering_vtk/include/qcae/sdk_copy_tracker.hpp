#pragma once

#include "qcae/operation_ledger.hpp"
#include "qcae/sdk_copy_observation.hpp"

#include <array>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace qcae::sdk_copy {

enum class Component : std::size_t {
    tree_style_option,
    tree_paint,
    tree_size_hint,
    log_document_change,
    log_block_layout,
    log_paint,
    log_paint_block_text,
    font_glyph_buffers,
    vtk_mapper_check,
    vtk_mapper_build,
    vtk_index_build,
    vtk_cell_texture,
    vtk_coordinate_ranges,
    vtk_camera_shift,
    vtk_coordinate_vbo,
    count
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Component::count)>
    component_names{"qt_tree_style_option",
                    "qt_tree_paint",
                    "qt_tree_size_hint",
                    "qt_log_document_change",
                    "qt_log_block_layout",
                    "qt_log_paint",
                    "qt_log_paint_block_text",
                    "qt_font_glyph_buffers",
                    "vtk_mapper_check",
                    "vtk_mapper_build",
                    "vtk_index_build",
                    "vtk_cell_texture",
                    "vtk_coordinate_ranges",
                    "vtk_camera_shift",
                    "vtk_coordinate_vbo"};

// Observational state is bounded and retains only the most recent operation.
// Allocations made by snapshot() are reporting overhead, not measured payload.
class Tracker {
  public:
    explicit Tracker(std::initializer_list<Component> components = {}) {
        for (const auto component : components)
            enabled_[static_cast<std::size_t>(component)] = true;
    }

    void beginOperation() noexcept {
        const auto operation = ledger::current();
        if (operation && operation != operation_) {
            operation_ = operation;
            cells_ = {};
        }
    }

    void unsupported(Component component, ledger::Stage stage, std::string_view reason) noexcept {
        beginOperation();
        if (!ledger::current())
            return;
        auto& cell = cells_[static_cast<std::size_t>(component)];
        enabled_[static_cast<std::size_t>(component)] = true;
        cell.unsupported = true;
        if (cell.reason.empty())
            cell.reason = reason;
        operation_->unknown(stage, ledger::Metric::library_internal_copy_bytes);
    }

    void record(Component component,
                ledger::Stage stage,
                std::optional<std::uint64_t> bytes,
                std::string_view unsupported_reason = {}) noexcept {
        const auto operation = ledger::current();
        if (!operation)
            return;
        beginOperation();
        enabled_[static_cast<std::size_t>(component)] = true;
        auto& cell = cells_[static_cast<std::size_t>(component)];
        if (cell.calls == std::numeric_limits<std::uint64_t>::max()) {
            unsupported(component, stage, "observer call-count overflow");
            return;
        }
        ++cell.calls;
        if (!bytes || *bytes > std::numeric_limits<std::uint64_t>::max() - cell.bytes) {
            cell.unsupported = true;
            if (cell.reason.empty())
                cell.reason =
                    unsupported_reason.empty() ? "copy bound overflow" : unsupported_reason;
            operation->unknown(stage, ledger::Metric::library_internal_copy_bytes);
            return;
        }
        cell.bytes += *bytes;
        operation->add(stage, ledger::Metric::library_internal_copy_bytes, *bytes);
    }

    [[nodiscard]] SdkCopySnapshot snapshot() const {
        SdkCopySnapshot result;
        if (!operation_)
            return result;
        result.run_id = operation_->identity().run_id;
        for (std::size_t index = 0; index < cells_.size(); ++index) {
            const auto& cell = cells_[index];
            if (enabled_[index])
                result.components.push_back(
                    {std::string(component_names[index]),
                     cell.calls,
                     cell.unsupported ? std::nullopt : std::optional<std::uint64_t>(cell.bytes),
                     std::string(cell.reason)});
        }
        return result;
    }

  private:
    struct Cell {
        std::uint64_t calls{}, bytes{};
        bool unsupported{};
        std::string_view reason;
    };
    std::shared_ptr<ledger::OperationLedger> operation_;
    std::array<Cell, static_cast<std::size_t>(Component::count)> cells_{};
    std::array<bool, static_cast<std::size_t>(Component::count)> enabled_{};
};

} // namespace qcae::sdk_copy
