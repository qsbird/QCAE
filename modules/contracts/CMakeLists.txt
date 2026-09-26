add_library(qcae_contracts INTERFACE)
target_include_directories(qcae_contracts INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_contracts INTERFACE cxx_std_20)
target_link_libraries(qcae_contracts INTERFACE qcae_foundation)
add_custom_command(
  OUTPUT "${QCAE_GENERATED_DIR}/qcae/operations.hpp"
  COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/generate_operations.py"
    "${PROJECT_SOURCE_DIR}/docs/contracts/operations.json"
    "${QCAE_GENERATED_DIR}/qcae/operations.hpp"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_operations.py"
    "${PROJECT_SOURCE_DIR}/docs/contracts/operations.json"
  VERBATIM)
add_custom_target(qcae_generated_contract DEPENDS "${QCAE_GENERATED_DIR}/qcae/operations.hpp")
add_dependencies(qcae_contracts qcae_generated_contract)
target_include_directories(qcae_contracts INTERFACE "${QCAE_GENERATED_DIR}")
