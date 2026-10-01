add_library(qcae_material_features STATIC src/material_operations.cpp include/qcae/material_operations.hpp)
target_include_directories(qcae_material_features PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_material_features PUBLIC cxx_std_20)
target_link_libraries(qcae_material_features PUBLIC qcae_application qcae_operations qcae_parameters)
qcae_warnings(qcae_material_features)
if(BUILD_TESTING AND QCAE_BUILD_IPC AND QCAE_BUILD_STORAGE AND UNIX)
  add_test(NAME material_description_ipc COMMAND Python3::Interpreter
    "${PROJECT_SOURCE_DIR}/tests/material_description_ipc_tests.py"
    --engine $<TARGET_FILE:qcae-engine> --cli $<TARGET_FILE:qcae-cli>
    --evidence-dir "${CMAKE_BINARY_DIR}/ext01-ipc-evidence")
  set_tests_properties(material_description_ipc PROPERTIES TIMEOUT 120)
endif()
