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
add_library(qcae_core STATIC src/core.cpp src/model.cpp "${QCAE_GENERATED_DIR}/qcae/operations.hpp")
target_include_directories(qcae_core PUBLIC include "${QCAE_GENERATED_DIR}")
target_compile_features(qcae_core PUBLIC cxx_std_20)
target_link_libraries(qcae_core PUBLIC Threads::Threads)
qcae_warnings(qcae_core)

add_custom_command(
  OUTPUT "${QCAE_GENERATED_DIR}/qcae/nastran_digest.hpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_profile.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${QCAE_GENERATED_DIR}/qcae/nastran_digest.hpp"
  DEPENDS tools/generate_profile.py include/qcae/types.hpp include/qcae/model.hpp
    include/qcae/model_codec.hpp include/qcae/profile_provider.hpp include/qcae/nastran_codec.hpp
    src/model.cpp src/nastran_codec.cpp docs/implementation/nastran-subset.md
  VERBATIM)
add_library(qcae_nastran STATIC src/nastran_codec.cpp "${QCAE_GENERATED_DIR}/qcae/nastran_digest.hpp")
target_link_libraries(qcae_nastran PUBLIC qcae_core)
qcae_warnings(qcae_nastran)

if(BUILD_TESTING)
  foreach(suite model nastran)
    add_executable(qcae_${suite}_tests tests/${suite}_tests.cpp)
    if(suite STREQUAL "nastran")
      target_link_libraries(qcae_${suite}_tests PRIVATE qcae_nastran)
    else()
      target_link_libraries(qcae_${suite}_tests PRIVATE qcae_core)
    endif()
    target_compile_definitions(qcae_${suite}_tests PRIVATE QCAE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    qcae_warnings(qcae_${suite}_tests)
    add_test(NAME ${suite} COMMAND qcae_${suite}_tests)
  endforeach()
  add_test(NAME generated_profile COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_profile.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${QCAE_GENERATED_DIR}/qcae/nastran_digest.hpp" --check)
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
  add_library(qcae_ipc STATIC src/ipc_api.cpp src/ipc_model.cpp)
  target_include_directories(qcae_ipc PUBLIC include "${QCAE_GENERATED_DIR}")
  target_link_libraries(qcae_ipc PUBLIC qcae_core Qt6::Core Qt6::Network)
  qcae_warnings(qcae_ipc)
  add_executable(qcae-engine apps/engine.cpp)
  target_link_libraries(qcae-engine PRIVATE qcae_ipc qcae_nastran)
  qcae_warnings(qcae-engine)
  add_executable(qcae-cli apps/cli.cpp)
  target_link_libraries(qcae-cli PRIVATE Qt6::Core Qt6::Network)
  target_include_directories(qcae-cli PRIVATE include "${QCAE_GENERATED_DIR}")
  add_dependencies(qcae-cli qcae_core)
  qcae_warnings(qcae-cli)
  if(BUILD_TESTING AND UNIX)
    add_test(NAME ipc COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/ipc_tests.py"
      --engine $<TARGET_FILE:qcae-engine> --cli $<TARGET_FILE:qcae-cli>)
    add_test(NAME m1_ipc COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/m1_ipc_tests.py"
      --engine $<TARGET_FILE:qcae-engine> --cli $<TARGET_FILE:qcae-cli>)
    set_tests_properties(ipc m1_ipc PROPERTIES TIMEOUT 60)
  endif()
endif()
