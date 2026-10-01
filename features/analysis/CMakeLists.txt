add_library(qcae_analysis_features STATIC
  src/analysis_operations.cpp src/analysis_checks.cpp src/cantilever_checks.cpp src/cantilever_checks.hpp
  src/check_report.cpp src/analysis_input.cpp src/analysis_input_codec.cpp
  include/qcae/analysis_features.hpp include/qcae/analysis_input.hpp)
target_include_directories(qcae_analysis_features PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_analysis_features PUBLIC cxx_std_20)
target_link_libraries(qcae_analysis_features PUBLIC qcae_application qcae_operations qcae_parameters)
qcae_warnings(qcae_analysis_features)
if(BUILD_TESTING)
  add_executable(qcae_analysis_check_tests "${PROJECT_SOURCE_DIR}/tests/analysis_check_tests.cpp")
  target_link_libraries(qcae_analysis_check_tests PRIVATE qcae_analysis_features qcae_legacy_api)
  qcae_warnings(qcae_analysis_check_tests)
  add_test(NAME analysis_checks COMMAND qcae_analysis_check_tests)
endif()
