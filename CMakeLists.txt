cmake_minimum_required(VERSION 3.24)
project(QCAE VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(QCAE_BUILD_IPC "Build the optional Qt local engine and CLI" OFF)
option(QCAE_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
include(CTest)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

function(qcae_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(QCAE_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(QCAE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${QCAE_GENERATED_DIR}/qcae/operations.hpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_operations.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/contracts/operations.json"
    "${QCAE_GENERATED_DIR}/qcae/operations.hpp"
  DEPENDS tools/generate_operations.py docs/contracts/operations.json
  VERBATIM)
add_library(qcae_core STATIC src/core.cpp "${QCAE_GENERATED_DIR}/qcae/operations.hpp")
target_include_directories(qcae_core PUBLIC include "${QCAE_GENERATED_DIR}")
target_compile_features(qcae_core PUBLIC cxx_std_20)
target_link_libraries(qcae_core PUBLIC Threads::Threads)
qcae_warnings(qcae_core)

if(BUILD_TESTING)
  add_executable(qcae_core_tests tests/core_tests.cpp)
  target_link_libraries(qcae_core_tests PRIVATE qcae_core)
  qcae_warnings(qcae_core_tests)
  add_test(NAME core COMMAND qcae_core_tests)
  add_executable(qcae_contract_tests tests/contract_tests.cpp)
  target_link_libraries(qcae_contract_tests PRIVATE qcae_core)
  qcae_warnings(qcae_contract_tests)
  add_test(NAME contracts COMMAND qcae_contract_tests)
  add_executable(qcae_allocation_tests tests/allocation_tests.cpp)
  target_link_libraries(qcae_allocation_tests PRIVATE qcae_core)
  qcae_warnings(qcae_allocation_tests)
  add_test(NAME allocation_atomicity COMMAND qcae_allocation_tests)
  add_test(NAME design COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_design.py")
  add_test(NAME generated_contract COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_operations.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/contracts/operations.json"
    "${QCAE_GENERATED_DIR}/qcae/operations.hpp" --check)
endif()

if(QCAE_BUILD_IPC)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)
  add_library(qcae_ipc STATIC src/ipc_api.cpp)
  target_include_directories(qcae_ipc PUBLIC include "${QCAE_GENERATED_DIR}")
  target_link_libraries(qcae_ipc PUBLIC qcae_core Qt6::Core Qt6::Network)
  qcae_warnings(qcae_ipc)
  add_executable(qcae-engine apps/engine.cpp)
  target_link_libraries(qcae-engine PRIVATE qcae_ipc)
  qcae_warnings(qcae-engine)
  add_executable(qcae-cli apps/cli.cpp)
  target_link_libraries(qcae-cli PRIVATE Qt6::Core Qt6::Network)
  target_include_directories(qcae-cli PRIVATE include "${QCAE_GENERATED_DIR}")
  add_dependencies(qcae-cli qcae_core)
  qcae_warnings(qcae-cli)
  if(BUILD_TESTING AND UNIX)
    add_test(NAME ipc COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/ipc_tests.py"
      --engine $<TARGET_FILE:qcae-engine> --cli $<TARGET_FILE:qcae-cli>)
    set_tests_properties(ipc PROPERTIES TIMEOUT 60)
  endif()
endif()
