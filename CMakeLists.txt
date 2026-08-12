cmake_minimum_required(VERSION 3.25)

project(
  arena_core_server
  VERSION 1.0.1
  DESCRIPTION "Jingyu deterministic 1v1 game server"
  LANGUAGES CXX
)

option(ARENA_BUILD_TESTS "Build arena-core tests" ON)
option(ARENA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ARENA_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ARENA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ARENA_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ArenaCompiler)

find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system thread)
find_package(Protobuf MODULE REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
find_package(Lua 5.4 REQUIRED)
find_path(MYSQLCPPCONN_INCLUDE_DIR mysql_driver.h REQUIRED)
find_library(MYSQLCPPCONN_LIBRARY NAMES mysqlcppconn REQUIRED)

set(ARENA_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/arena_core.proto")
protobuf_generate_cpp(ARENA_PROTO_SOURCES ARENA_PROTO_HEADERS "${ARENA_PROTO_FILE}")

add_library(arena_core_protocol STATIC
  ${ARENA_PROTO_SOURCES}
  ${ARENA_PROTO_HEADERS}
  src/protocol/frame.cpp
)
add_library(arena_core::protocol ALIAS arena_core_protocol)
target_include_directories(arena_core_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(arena_core_protocol PUBLIC protobuf::libprotobuf)
arena_core_apply_compiler_policy(arena_core_protocol)

add_library(arena_core_foundation STATIC
  src/core/config.cpp
  src/core/error.cpp
  src/core/logging.cpp
  src/domain/identifiers.cpp
  src/domain/match_ticket.cpp
  src/domain/room.cpp
  src/domain/session.cpp
)
add_library(arena_core::foundation ALIAS arena_core_foundation)

target_include_directories(arena_core_foundation
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(arena_core_foundation PUBLIC cxx_std_17)
target_compile_definitions(arena_core_foundation PUBLIC BOOST_ASIO_NO_DEPRECATED)
target_link_libraries(arena_core_foundation
  PUBLIC
    Boost::system
    Boost::thread
    protobuf::libprotobuf
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Threads::Threads
)
arena_core_apply_compiler_policy(arena_core_foundation)

add_library(arena_core_observer STATIC
  src/observer/observer_store.cpp
  src/observer/http_server.cpp
)
add_library(arena_core::observer ALIAS arena_core_observer)
target_include_directories(arena_core_observer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(arena_core_observer
  PUBLIC arena_core::foundation nlohmann_json::nlohmann_json Boost::system
)
arena_core_apply_compiler_policy(arena_core_observer)

# Frame validation uses the shared stable error catalog.
target_link_libraries(arena_core_protocol PUBLIC arena_core::foundation)

add_library(arena_core_rules STATIC src/rules/rule_engine.cpp)
add_library(arena_core::rules ALIAS arena_core_rules)
target_include_directories(arena_core_rules
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${LUA_INCLUDE_DIR}
)
target_link_libraries(arena_core_rules PUBLIC arena_core::foundation PRIVATE ${LUA_LIBRARIES})
arena_core_apply_compiler_policy(arena_core_rules)

add_library(arena_core_rank STATIC src/rank/rank_cache.cpp)
add_library(arena_core::rank ALIAS arena_core_rank)
target_include_directories(arena_core_rank PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(arena_core_rank PUBLIC arena_core::foundation PkgConfig::HIREDIS)
arena_core_apply_compiler_policy(arena_core_rank)

add_library(arena_core_storage STATIC
  src/storage/storage_executor.cpp
  src/storage/mysql_settlement_repository.cpp
)
add_library(arena_core::storage ALIAS arena_core_storage)
target_include_directories(arena_core_storage
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR}
)
target_link_libraries(arena_core_storage
  PUBLIC arena_core::foundation arena_core::rank
  PRIVATE ${MYSQLCPPCONN_LIBRARY}
)
arena_core_apply_compiler_policy(arena_core_storage)

add_library(arena_core_settlement STATIC
  src/settlement/in_memory_settlement_gateway.cpp
  src/settlement/persistent_settlement_service.cpp
)
add_library(arena_core::settlement ALIAS arena_core_settlement)
target_include_directories(arena_core_settlement PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(arena_core_settlement
  PUBLIC
    arena_core::foundation
    arena_core::rank
    arena_core::rules
    arena_core::storage
    nlohmann_json::nlohmann_json
    Boost::system
)
arena_core_apply_compiler_policy(arena_core_settlement)

add_library(arena_core_session STATIC
  src/session/session.cpp
  src/session/session_registry.cpp
  src/session/tcp_server.cpp
  src/session/token_generator.cpp
)
add_library(arena_core::session ALIAS arena_core_session)
target_include_directories(arena_core_session
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(arena_core_session
  PUBLIC
    arena_core::foundation
    arena_core::observer
    arena_core::protocol
    Boost::system
    Boost::thread
    spdlog::spdlog
)
arena_core_apply_compiler_policy(arena_core_session)

add_library(arena_core_game STATIC
  src/game/game_service.cpp
  src/game/room_actor.cpp
  src/game/trusted_result.cpp
  src/game/trusted_result_http_server.cpp
)
add_library(arena_core::game ALIAS arena_core_game)
target_include_directories(arena_core_game
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(arena_core_game
  PUBLIC
    arena_core::session
    arena_core::protocol
    arena_core::foundation
    arena_core::settlement
    Boost::system
    Boost::thread
)
arena_core_apply_compiler_policy(arena_core_game)

add_executable(arena-core-server apps/server/main.cpp)
target_link_libraries(arena-core-server PRIVATE arena_core::game arena_core::observer)
arena_core_apply_compiler_policy(arena-core-server)

add_library(arena_core_qa STATIC
  src/qa/demo_battle_adapter.cpp
  src/qa/robot_scenario.cpp
)
add_library(arena_core::qa ALIAS arena_core_qa)
target_include_directories(arena_core_qa
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(arena_core_qa
  PUBLIC arena_core::protocol arena_core::foundation nlohmann_json::nlohmann_json Boost::system Threads::Threads
)
arena_core_apply_compiler_policy(arena_core_qa)

add_executable(arena-core-robot apps/robot/main.cpp)
target_link_libraries(arena-core-robot PRIVATE arena_core::qa)
arena_core_apply_compiler_policy(arena-core-robot)

install(TARGETS arena-core-server arena-core-robot RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION config)
install(DIRECTORY rules/ DESTINATION rules)
install(DIRECTORY migrations/ DESTINATION migrations)
install(DIRECTORY web/ DESTINATION web)
install(DIRECTORY scenarios/ DESTINATION scenarios)

if(ARENA_BUILD_TESTS)
  include(CTest)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)

  add_executable(arena_core_unit_tests
    tests/unit/config_test.cpp
    tests/unit/domain_test.cpp
    tests/unit/error_test.cpp
    tests/unit/version_test.cpp
  )
  target_link_libraries(arena_core_unit_tests
    PRIVATE
      arena_core::foundation
      GTest::gtest_main
  )
  arena_core_apply_compiler_policy(arena_core_unit_tests)

  include(GoogleTest)
  gtest_discover_tests(arena_core_unit_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "unit"
  )

  add_executable(arena_core_protocol_tests tests/protocol/frame_test.cpp)
  target_link_libraries(arena_core_protocol_tests PRIVATE arena_core::protocol GTest::gtest_main)
  arena_core_apply_compiler_policy(arena_core_protocol_tests)
  gtest_discover_tests(arena_core_protocol_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "protocol" TIMEOUT 10
  )

  add_executable(arena_core_session_tests tests/component/session_server_test.cpp)
  target_link_libraries(arena_core_session_tests PRIVATE arena_core::session GTest::gtest_main)
  arena_core_apply_compiler_policy(arena_core_session_tests)
  gtest_discover_tests(arena_core_session_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "network" TIMEOUT 10
  )

  add_executable(arena_core_game_tests
    tests/game/game_flow_test.cpp
    tests/game/room_settlement_test.cpp
    tests/game/trusted_result_http_test.cpp
  )
  target_link_libraries(arena_core_game_tests PRIVATE arena_core::game GTest::gtest_main)
  arena_core_apply_compiler_policy(arena_core_game_tests)
  gtest_discover_tests(arena_core_game_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "game" TIMEOUT 15
  )

  add_executable(arena_core_data_tests
    tests/data/rule_engine_test.cpp
    tests/data/storage_executor_test.cpp
    tests/data/settlement_service_test.cpp
  )
  target_link_libraries(arena_core_data_tests PRIVATE arena_core::settlement GTest::gtest_main)
  target_compile_definitions(arena_core_data_tests PRIVATE ARENA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  arena_core_apply_compiler_policy(arena_core_data_tests)
  gtest_discover_tests(arena_core_data_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "data" TIMEOUT 15
  )

  add_executable(arena_core_data_integration_tests tests/integration/data_integration_test.cpp)
  target_link_libraries(arena_core_data_integration_tests PRIVATE arena_core::settlement GTest::gtest_main)
  target_compile_definitions(arena_core_data_integration_tests PRIVATE ARENA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  arena_core_apply_compiler_policy(arena_core_data_integration_tests)
  gtest_discover_tests(arena_core_data_integration_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "data" TIMEOUT 30
  )

  add_executable(arena_core_observer_tests
    tests/observer/observer_store_test.cpp
    tests/observer/http_server_test.cpp
  )
  target_link_libraries(arena_core_observer_tests PRIVATE arena_core::observer GTest::gtest_main)
  target_compile_definitions(arena_core_observer_tests PRIVATE ARENA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  arena_core_apply_compiler_policy(arena_core_observer_tests)
  gtest_discover_tests(arena_core_observer_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "observer" TIMEOUT 15
  )

  add_executable(arena_core_qa_tests tests/qa/robot_scenario_test.cpp)
  target_link_libraries(arena_core_qa_tests PRIVATE arena_core::qa GTest::gtest_main)
  arena_core_apply_compiler_policy(arena_core_qa_tests)
  gtest_discover_tests(arena_core_qa_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "qa" TIMEOUT 15
  )

  add_test(NAME arena_core_server_self_check COMMAND arena-core-server --self-check)
  add_test(NAME arena_core_robot_self_check COMMAND arena-core-robot --self-check)
  set_tests_properties(
    arena_core_server_self_check
    arena_core_robot_self_check
    PROPERTIES LABELS "smoke"
  )

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(
      NAME arena_core_rejects_cpp20
      COMMAND ${CMAKE_COMMAND}
        -DARENA_COMPILER=${CMAKE_CXX_COMPILER}
        -DARENA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DARENA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyCpp17Guard.cmake
    )
    set_tests_properties(arena_core_rejects_cpp20 PROPERTIES LABELS "contract")
  endif()
endif()
