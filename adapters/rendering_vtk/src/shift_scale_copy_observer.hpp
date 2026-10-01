#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vtkOpenGLVertexBufferObject.h>
#include <vtkVersion.h>

namespace qcae::detail {

// Read-only bounds for the single AOS double3 coordinate path in VTK 9.7.0.
// Call sites observe actual uploads / mapper virtual calls. These helpers never
// call UpdateShiftScale, GetRange, GetMatrix or a lazy shader lookup themselves.
inline bool auditedAutoShift(vtkOpenGLVertexBufferObject* vbo) {
    if (!vbo || std::string_view(vtkVersion::GetVTKVersion()) != "9.7.0")
        return false;
    const auto method = vbo->GetCoordShiftAndScaleMethod();
    const bool enabled = vbo->GetCoordShiftAndScaleEnabled();
    if (method == vtkOpenGLVertexBufferObject::DISABLE_SHIFT_SCALE)
        return !enabled;
    if (method != vtkOpenGLVertexBufferObject::AUTO_SHIFT_SCALE)
        return false;
    if (!enabled)
        return true;
    const auto& shift = vbo->GetShift();
    const auto& scale = vbo->GetScale();
    if (shift.size() != 3 || scale.size() != 3)
        return false;
    for (std::size_t index = 0; index < 3; ++index)
        if (!std::isfinite(shift[index]) || !std::isfinite(scale[index]) || scale[index] <= 0)
            return false;
    return true;
}

inline std::optional<std::uint64_t> autoShiftVectorBytes(vtkOpenGLVertexBufferObject* vbo) {
    if (!auditedAutoShift(vbo))
        return std::nullopt;
    if (!vbo->GetCoordShiftAndScaleEnabled())
        return 0;
    // UpdateShiftScale creates two 3-double vectors and SetShift/SetScale may
    // each replace one retained vector. Three pushes write 3 values and can
    // relocate prefixes of 1 and 2 values, regardless of growth policy.
    // Equality/capacity reuse can only reduce this conservative bound.
    return 4 * (3 + 1 + 2) * sizeof(double);
}

inline std::optional<std::uint64_t> autoInverseMatrixBytes(vtkOpenGLVertexBufferObject* vbo) {
    if (!auditedAutoShift(vbo))
        return std::nullopt;
    if (!vbo->GetCoordShiftAndScaleEnabled())
        return 0;
    // The mapper-owned transform has no input, stack, inverse or user edits.
    // Identity can write 16 doubles. Translate/Scale each create an identity
    // matrix, replace 3 fields and concatenate (16 temporary + 16 destination
    // doubles). The first concatenate creates one identity matrix. Internal
    // update writes an identity and one product. Transpose has 10 iterations,
    // each with a temporary and two output stores (including the diagonal).
    // Transform-list entries retain references; they do not copy matrices.
    constexpr std::uint64_t values = 16 + 2 * (16 + 3 + 32) + 16 + 16 + 32 + 10 + 20;
    return values * sizeof(double);
}

inline std::optional<std::uint64_t> autoShaderMatrixBytes(vtkOpenGLVertexBufferObject* vbo,
                                                          bool identity_actor,
                                                          bool model_view_uniform,
                                                          bool normal_uniform,
                                                          bool environment_uniform) {
    if (!auditedAutoShift(vbo) || !identity_actor || environment_uniform)
        return std::nullopt;
    const std::uint64_t matrices = 1 + static_cast<std::uint64_t>(model_view_uniform);
    // The enabled branch multiplies one model->display matrix and optionally
    // one model->view matrix. Multiply4x4 writes 16 temporary and 16 destination
    // doubles. Uniform conversion writes each float once; its owned cache may
    // copy it once. The outer vector moves reference-owning vector descriptors
    // without copying cached payloads. Cached-equal values reduce actual writes.
    const auto products = vbo->GetCoordShiftAndScaleEnabled() ? matrices * 32 * sizeof(double) : 0;
    const auto uniform_values = matrices * 16 + (normal_uniform ? 9 : 0);
    return products + 2 * uniform_values * sizeof(float);
}

} // namespace qcae::detail
