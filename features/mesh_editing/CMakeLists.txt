add_library(qcae_mesh_editing STATIC src/mesh_editing_operations.cpp include/qcae/mesh_editing_operations.hpp)
target_include_directories(qcae_mesh_editing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_mesh_editing PUBLIC cxx_std_20)
target_link_libraries(qcae_mesh_editing PUBLIC qcae_application qcae_operations qcae_parameters)
qcae_warnings(qcae_mesh_editing)
if(BUILD_TESTING AND QCAE_BUILD_IPC AND QCAE_BUILD_STORAGE AND UNIX)
  add_test(NAME batch_translation_ipc COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/batch_translation_ipc_tests.py"
    --engine "$<TARGET_FILE:qcae-engine>" --cli "$<TARGET_FILE:qcae-cli>"
    --evidence-dir "${CMAKE_CURRENT_BINARY_DIR}/batch-translation-evidence")
  set_tests_properties(batch_translation_ipc PROPERTIES TIMEOUT 90)
endif()
