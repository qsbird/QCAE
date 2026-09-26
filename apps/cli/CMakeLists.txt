add_executable(qcae-cli main.cpp)
target_link_libraries(qcae-cli PRIVATE qcae_transport_local qcae_contracts)
qcae_warnings(qcae-cli)
