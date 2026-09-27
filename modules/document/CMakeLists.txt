add_library(qcae_document STATIC src/model.cpp src/model_delta.cpp src/state_codec.cpp include/qcae/model.hpp include/qcae/model_codec.hpp include/qcae/model_delta.hpp include/qcae/state_codec.hpp include/qcae/read_view.hpp)
target_sources(qcae_document PRIVATE
  src/record_registry.cpp src/document_view.cpp src/edit_session.cpp
  src/record_model_bridge.cpp src/records_rules.cpp
  include/qcae/record_registry.hpp include/qcae/document_view.hpp
  include/qcae/edit_session.hpp include/qcae/records.hpp
  include/qcae/records_model_bridge.hpp)
target_include_directories(qcae_document PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_document PUBLIC cxx_std_20)
target_link_libraries(qcae_document PUBLIC qcae_foundation qcae_contracts)
qcae_warnings(qcae_document)
if(BUILD_TESTING)
  add_executable(qcae_record_document_tests "${PROJECT_SOURCE_DIR}/tests/record_document_tests.cpp")
  target_link_libraries(qcae_record_document_tests PRIVATE qcae_document)
  qcae_warnings(qcae_record_document_tests)
  add_test(NAME record_document COMMAND qcae_record_document_tests)
  add_test(NAME generated_entities COMMAND Python3::Interpreter
    "${PROJECT_SOURCE_DIR}/tools/generate_entities.py" --check --permutation-check)
endif()
