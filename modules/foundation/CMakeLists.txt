add_library(qcae_foundation INTERFACE)
target_include_directories(qcae_foundation INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_foundation INTERFACE cxx_std_20)
