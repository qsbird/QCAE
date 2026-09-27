file(GLOB QCAE_OPERATION_SCHEMAS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/operations/*.json")
# Membership is a dependency too: removing a schema must invalidate the generated header.
string(REPLACE ";" "\n" QCAE_OPERATION_SCHEMA_MEMBERSHIP "${QCAE_OPERATION_SCHEMAS}")
set(QCAE_OPERATION_SCHEMA_LIST "${CMAKE_CURRENT_BINARY_DIR}/operation-schemas.txt")
file(GENERATE OUTPUT "${QCAE_OPERATION_SCHEMA_LIST}"
  CONTENT "${QCAE_OPERATION_SCHEMA_MEMBERSHIP}\n")
add_custom_command(
  OUTPUT "${QCAE_GENERATED_DIR}/qcae/operation_inputs.hpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_operation_contracts.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/operations" "${QCAE_GENERATED_DIR}/qcae/operation_inputs.hpp"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_operation_contracts.py"
    ${QCAE_OPERATION_SCHEMAS} "${QCAE_OPERATION_SCHEMA_LIST}"
  VERBATIM)
add_custom_target(qcae_generated_operation_inputs
  DEPENDS "${QCAE_GENERATED_DIR}/qcae/operation_inputs.hpp")
add_library(qcae_operations STATIC src/operation_registry.cpp src/canonical_value.cpp src/change_receipt_value.cpp include/qcae/operation_registry.hpp)
target_include_directories(qcae_operations PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${QCAE_GENERATED_DIR}")
target_compile_features(qcae_operations PUBLIC cxx_std_20)
target_link_libraries(qcae_operations PUBLIC qcae_foundation qcae_contracts)
add_dependencies(qcae_operations qcae_generated_operation_inputs)
qcae_warnings(qcae_operations)
