cmake_minimum_required(VERSION 3.24)
project(aegis VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(AEGIS_BUILD_LEGACY "Build the optional Qt prototype and its tests" OFF)
option(AEGIS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(AEGIS_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE AEGIS_GIT_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(AEGIS_GIT_ROOT STREQUAL CMAKE_SOURCE_DIR)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE AEGIS_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/.aegis-git")
        execute_process(COMMAND ${GIT_EXECUTABLE} "--git-dir=${CMAKE_SOURCE_DIR}/.aegis-git" "--work-tree=${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
            OUTPUT_VARIABLE AEGIS_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()

function(aegis_quality target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(target MATCHES "_test$")
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
        if(AEGIS_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(AEGIS_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(AEGIS_SQLITE_TARGET SQLite::SQLite3)
else()
    if(NOT SQLite3_LIBRARIES)
        message(FATAL_ERROR "FindSQLite3 provided neither an imported target nor SQLite3_LIBRARIES")
    endif()
    set(AEGIS_SQLITE_TARGET ${SQLite3_LIBRARIES})
    set(AEGIS_SQLITE_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
endif()

add_library(aegis_daemon_session daemon/session/store.cpp daemon/protocol/event_hub.cpp)
target_include_directories(aegis_daemon_session PUBLIC .)
if(AEGIS_SQLITE_INCLUDE_DIRS)
    target_include_directories(aegis_daemon_session PUBLIC ${AEGIS_SQLITE_INCLUDE_DIRS})
endif()
target_link_libraries(aegis_daemon_session PUBLIC ${AEGIS_SQLITE_TARGET})

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
    daemon/api/static_files.cpp
    daemon/api/routes.cpp
    daemon/api/routes/agents.cpp
    daemon/api/routes/tasks.cpp
    daemon/api/routes/runs.cpp
    daemon/api/routes/git.cpp
    daemon/api/routes/verification.cpp
    daemon/api/routes/review.cpp
    daemon/api/routes/files.cpp
    daemon/api/routes/findings.cpp)
target_include_directories(aegis_daemon_api PUBLIC .)
target_compile_definitions(aegis_daemon_api PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(aegis_daemon_api PUBLIC aegis_daemon_session aegis_daemon_services Threads::Threads)
target_compile_definitions(aegis_daemon_api PRIVATE AEGIS_VERSION="${PROJECT_VERSION}" AEGIS_GIT_COMMIT="${AEGIS_GIT_COMMIT}")

add_executable(aegis_daemon daemon/main.cpp daemon/app.cpp)
target_include_directories(aegis_daemon PRIVATE .)
target_link_libraries(aegis_daemon PRIVATE aegis_daemon_api aegis_daemon_services)

add_executable(aegis src/launcher.cpp)
target_compile_definitions(aegis PRIVATE "AEGIS_SOURCE_WEB_ROOT=\"${CMAKE_SOURCE_DIR}/web/dist\"")
target_compile_definitions(aegis PRIVATE AEGIS_VERSION="${PROJECT_VERSION}" AEGIS_GIT_COMMIT="${AEGIS_GIT_COMMIT}")
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
add_test(NAME aegis_launcher_version COMMAND aegis --version)

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

set(AEGIS_QUALITY_TARGETS aegis_daemon_session aegis_daemon_services aegis_daemon_api aegis_daemon aegis daemon_store_test daemon_api_test daemon_services_test daemon_repository_test)
if(AEGIS_BUILD_LEGACY)
    list(APPEND AEGIS_QUALITY_TARGETS aegis_core aegis_analysis aegis_session aegis_terminal aegis_ui aegis_transcript aegis_legacy aegis_core_test aegis_analysis_test aegis_session_test aegis_terminal_test aegis_ui_test aegis_transcript_test)
endif()
foreach(target IN LISTS AEGIS_QUALITY_TARGETS)
    aegis_quality(${target})
endforeach()

install(TARGETS aegis aegis_daemon RUNTIME DESTINATION bin)
install(DIRECTORY web/dist/ DESTINATION bin/web)
