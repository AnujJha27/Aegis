cmake_minimum_required(VERSION 3.24)
project(aegis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(AEGIS_BUILD_LEGACY "Build the optional Qt prototype and its tests" OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_library(aegis_daemon_session daemon/session/store.cpp daemon/protocol/event_hub.cpp)
target_include_directories(aegis_daemon_session PUBLIC .)
target_link_libraries(aegis_daemon_session PUBLIC SQLite3::SQLite3)

add_library(aegis_daemon_services
    daemon/process/process.cpp
    daemon/repository/git.cpp
    daemon/repository/files.cpp
    daemon/verification/runner.cpp
    daemon/agents/pty_adapter.cpp
    daemon/agents/codex_adapter.cpp
    daemon/agents/manager.cpp)
target_include_directories(aegis_daemon_services PUBLIC .)
target_link_libraries(aegis_daemon_services PUBLIC aegis_daemon_session Threads::Threads util)

add_library(aegis_daemon_api
    daemon/api/server.cpp
    daemon/api/routes.cpp
    daemon/api/routes/agents.cpp
    daemon/api/routes/tasks.cpp
    daemon/api/routes/runs.cpp
    daemon/api/routes/git.cpp
    daemon/api/routes/verification.cpp
    daemon/api/routes/review.cpp
    daemon/api/routes/files.cpp)
target_include_directories(aegis_daemon_api PUBLIC .)
target_compile_definitions(aegis_daemon_api PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(aegis_daemon_api PUBLIC aegis_daemon_session aegis_daemon_services Threads::Threads)

add_executable(aegis_daemon daemon/main.cpp daemon/app.cpp)
target_include_directories(aegis_daemon PRIVATE .)
target_link_libraries(aegis_daemon PRIVATE aegis_daemon_api aegis_daemon_services)

add_executable(aegis src/launcher.cpp)
target_compile_definitions(aegis PRIVATE "AEGIS_SOURCE_WEB_ROOT=\"${CMAKE_SOURCE_DIR}/web/dist\"")
target_link_libraries(aegis PRIVATE Threads::Threads)

enable_testing()
add_executable(daemon_store_test tests/daemon_store_test.cpp)
target_link_libraries(daemon_store_test PRIVATE aegis_daemon_session)
add_test(NAME daemon_store_test COMMAND daemon_store_test)

add_executable(daemon_api_test tests/daemon_api_test.cpp)
target_link_libraries(daemon_api_test PRIVATE aegis_daemon_api)
add_test(NAME daemon_api_test COMMAND daemon_api_test)
set_tests_properties(daemon_api_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(daemon_services_test tests/daemon_services_test.cpp)
target_link_libraries(daemon_services_test PRIVATE aegis_daemon_services)
add_test(NAME daemon_services_test COMMAND daemon_services_test)
set_tests_properties(daemon_services_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(daemon_repository_test tests/daemon_repository_test.cpp)
target_link_libraries(daemon_repository_test PRIVATE aegis_daemon_services)
add_test(NAME daemon_repository_test COMMAND daemon_repository_test)

add_test(NAME aegis_launcher_help COMMAND aegis --help)

if(AEGIS_BUILD_LEGACY)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
    set(CMAKE_AUTOMOC ON)

    add_library(aegis_core src/core.cpp)
    target_include_directories(aegis_core PUBLIC include)
    target_link_libraries(aegis_core PUBLIC Qt6::Core)

    add_library(aegis_analysis src/analysis.cpp)
    target_include_directories(aegis_analysis PUBLIC include)
    target_link_libraries(aegis_analysis PUBLIC Qt6::Core)

    add_library(aegis_session src/session.cpp)
    target_include_directories(aegis_session PUBLIC include)
    target_link_libraries(aegis_session PUBLIC Qt6::Core)

    add_library(aegis_terminal src/terminal.cpp)
    target_include_directories(aegis_terminal PUBLIC include)
    target_link_libraries(aegis_terminal PUBLIC Qt6::Core util)

    add_library(aegis_ui src/ui.cpp)
    target_include_directories(aegis_ui PUBLIC include)
    target_link_libraries(aegis_ui PUBLIC Qt6::Core Qt6::Widgets)

    add_library(aegis_transcript src/transcript.cpp)
    target_include_directories(aegis_transcript PUBLIC include)
    target_link_libraries(aegis_transcript PUBLIC Qt6::Core)

    add_executable(aegis_legacy src/main.cpp src/commands.cpp src/window.cpp)
    target_compile_definitions(aegis_legacy PRIVATE "AEGIS_SOURCE_WEB_ROOT=\"${CMAKE_SOURCE_DIR}/web/dist\"")
    target_link_libraries(aegis_legacy PRIVATE aegis_core aegis_analysis aegis_session aegis_terminal aegis_ui aegis_transcript Qt6::Widgets)

    add_executable(aegis_core_test tests/core_test.cpp)
    target_link_libraries(aegis_core_test PRIVATE aegis_core Qt6::Core)
    add_test(NAME aegis_core_test COMMAND aegis_core_test)
    add_executable(aegis_analysis_test tests/analysis_test.cpp)
    target_link_libraries(aegis_analysis_test PRIVATE aegis_analysis Qt6::Core)
    add_test(NAME aegis_analysis_test COMMAND aegis_analysis_test)
    add_executable(aegis_session_test tests/session_test.cpp)
    target_link_libraries(aegis_session_test PRIVATE aegis_session Qt6::Core)
    add_test(NAME aegis_session_test COMMAND aegis_session_test)
    add_executable(aegis_terminal_test tests/terminal_test.cpp)
    target_link_libraries(aegis_terminal_test PRIVATE aegis_terminal Qt6::Core)
    add_test(NAME aegis_terminal_test COMMAND aegis_terminal_test)
    add_executable(aegis_ui_test tests/ui_test.cpp)
    target_link_libraries(aegis_ui_test PRIVATE aegis_ui Qt6::Core)
    add_test(NAME aegis_ui_test COMMAND aegis_ui_test)
    add_executable(aegis_transcript_test tests/transcript_test.cpp)
    target_link_libraries(aegis_transcript_test PRIVATE aegis_transcript Qt6::Core)
    add_test(NAME aegis_transcript_test COMMAND aegis_transcript_test)

    add_test(NAME aegis_cli_status COMMAND aegis_legacy status ${CMAKE_SOURCE_DIR})
    add_test(NAME aegis_cli_verify COMMAND aegis_legacy verify ${CMAKE_SOURCE_DIR} -- true)
    add_test(NAME aegis_cli_help COMMAND aegis_legacy --help)
    install(TARGETS aegis_legacy RUNTIME DESTINATION bin)
endif()

install(TARGETS aegis aegis_daemon RUNTIME DESTINATION bin)
install(DIRECTORY web/dist/ DESTINATION bin/web)
