#include "shift_scale_copy_observer.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vtkDoubleArray.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkTransform.h>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void checkInverse(vtkOpenGLVertexBufferObject* vbo) {
    vtkNew<vtkTransform> inverse;
    const auto& shift = vbo->GetShift();
    const auto& scale = vbo->GetScale();
    inverse->Identity();
    inverse->Translate(shift[0], shift[1], shift[2]);
    inverse->Scale(1 / scale[0], 1 / scale[1], 1 / scale[2]);
    vtkNew<vtkMatrix4x4> transposed;
    inverse->GetTranspose(transposed);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            const auto expected = row == column ? (row == 3 ? 1 : 1 / scale[row])
                                  : row == 3    ? shift[column]
                                                : 0;
            check(transposed->GetElement(row, column) == expected,
                  "AUTO inverse matrix changed the coordinate reconstruction");
        }
}
} // namespace

int main() {
    try {
        vtkNew<vtkDoubleArray> coordinates;
        coordinates->SetNumberOfComponents(3);
        coordinates->SetNumberOfTuples(2);
        coordinates->SetTuple3(0, 0, 0, 0);
        coordinates->SetTuple3(1, 1, 1, 1);
        vtkNew<vtkOpenGLVertexBufferObject> original;
        vtkNew<vtkOpenGLVertexBufferObject> observed;
        original->SetCoordShiftAndScaleMethod(vtkOpenGLVertexBufferObject::AUTO_SHIFT_SCALE);
        observed->SetCoordShiftAndScaleMethod(vtkOpenGLVertexBufferObject::AUTO_SHIFT_SCALE);
        for (const auto values : {std::array<double, 3>{1, 1, 1},
                                  std::array<double, 3>{1000, 2, 3},
                                  std::array<double, 3>{1e8, 1e8 + 1, 1e8 + 2},
                                  std::array<double, 3>{1, 1, 1}}) {
            coordinates->SetTuple3(1, values[0], values[1], values[2]);
            coordinates->Modified();
            original->UpdateShiftScale(coordinates);
            observed->UpdateShiftScale(coordinates);
            const auto mtime = observed->GetMTime();
            const auto upload_time = observed->GetUploadTime().GetMTime();
            const auto enabled = observed->GetCoordShiftAndScaleEnabled();
            const auto vector_bytes = qcae::detail::autoShiftVectorBytes(observed);
            const auto inverse_bytes = qcae::detail::autoInverseMatrixBytes(observed);
            const auto shader_bytes =
                qcae::detail::autoShaderMatrixBytes(observed, true, true, true, false);
            check(vector_bytes.has_value() && inverse_bytes.has_value() && shader_bytes.has_value(),
                  "The supported AUTO branch has an unknown bound");
            check(*vector_bytes == (enabled ? 192 : 0),
                  "Three-value vector prefix relocation bound is incorrect");
            check(*inverse_bytes == (enabled ? 1696 : 0),
                  "Inverse matrix typed-store bound is incorrect");
            check(*shader_bytes == (enabled ? 512 : 0) + 328,
                  "Shader product/uniform copy bound is incorrect");
            check(original->GetShift() == observed->GetShift() &&
                      original->GetScale() == observed->GetScale() &&
                      original->GetCoordShiftAndScaleEnabled() == enabled,
                  "Read-only bounds changed the original VBO shift or scale");
            check(observed->GetMTime() == mtime &&
                      observed->GetUploadTime().GetMTime() == upload_time &&
                      observed->GetPackedVBO().empty(),
                  "The observer changed a VBO/array/cache modification time");
            if (enabled)
                checkInverse(observed);
            check(!qcae::detail::autoShaderMatrixBytes(observed, false, true, true, false),
                  "Non-identity actor was silently accepted");
            check(!qcae::detail::autoShaderMatrixBytes(observed, true, true, true, true),
                  "Environment matrix path was silently accepted");
        }
        observed->SetCoordShiftAndScaleMethod(vtkOpenGLVertexBufferObject::ALWAYS_AUTO_SHIFT_SCALE);
        observed->UpdateShiftScale(coordinates);
        check(!qcae::detail::autoShiftVectorBytes(observed), "Alternate shift method was accepted");
        observed->SetCoordShiftAndScaleMethod(vtkOpenGLVertexBufferObject::MANUAL_SHIFT_SCALE);
        check(!qcae::detail::autoInverseMatrixBytes(observed), "Manual shift method was accepted");
        std::cout << "PASS: actual AUTO enable/disable/large-span branches preserve VBO state and "
                     "inverse output; unsupported shader/shift paths remain unknown\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
