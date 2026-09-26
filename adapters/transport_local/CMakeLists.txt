add_library(qcae_transport_local INTERFACE)
target_include_directories(qcae_transport_local INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(qcae_transport_local INTERFACE cxx_std_20)
target_link_libraries(qcae_transport_local INTERFACE qcae_contracts Qt6::Core Qt6::Network)
