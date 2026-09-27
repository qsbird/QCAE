add_library(qcae_legacy_api STATIC src/core.cpp src/legacy_state.cpp src/legacy_migration.cpp
  include/qcae/core.hpp include/qcae/legacy_migration.hpp)
target_include_directories(qcae_legacy_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_legacy_api PUBLIC cxx_std_20)
target_link_libraries(qcae_legacy_api PUBLIC qcae_application qcae_document qcae_contracts
  PRIVATE qcae_parameters)
qcae_warnings(qcae_legacy_api)
