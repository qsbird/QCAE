add_library(qcae_nastran STATIC src/nastran_codec.cpp include/qcae/nastran_codec.hpp)
target_include_directories(qcae_nastran PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_nastran PUBLIC cxx_std_20)
target_link_libraries(qcae_nastran PUBLIC qcae_document qcae_contracts)
qcae_warnings(qcae_nastran)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/qcae/nastran_digest.hpp"
  COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/generate_profile.py"
    "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated/qcae/nastran_digest.hpp"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_profile.py"
    "${PROJECT_SOURCE_DIR}/modules/source-migration.json"
    "${PROJECT_SOURCE_DIR}/modules/foundation/include/qcae/types.hpp"
    "${PROJECT_SOURCE_DIR}/modules/document/include/qcae/model.hpp"
    "${PROJECT_SOURCE_DIR}/modules/document/include/qcae/model_codec.hpp"
    "${PROJECT_SOURCE_DIR}/modules/contracts/include/qcae/profile_provider.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/qcae/nastran_codec.hpp"
    "${PROJECT_SOURCE_DIR}/modules/document/src/model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nastran_codec.cpp"
    "${PROJECT_SOURCE_DIR}/docs/implementation/nastran-subset.md"
  VERBATIM)
target_sources(qcae_nastran PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/qcae/nastran_digest.hpp")
target_include_directories(qcae_nastran PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
