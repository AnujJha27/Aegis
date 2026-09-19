cmake_minimum_required(VERSION 3.24)
project(aegis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

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

option(AEGIS_BUILD_APP "Build the Aegis UI" ON)
if(AEGIS_BUILD_APP)
    add_executable(aegis src/main.cpp)
    target_link_libraries(aegis PRIVATE aegis_core aegis_analysis aegis_session aegis_terminal aegis_ui Qt6::Widgets)
    add_test(NAME aegis_cli_status COMMAND aegis status ${CMAKE_SOURCE_DIR})
    add_test(NAME aegis_cli_verify COMMAND aegis verify ${CMAKE_SOURCE_DIR} -- true)
    add_test(NAME aegis_cli_help COMMAND aegis --help)
endif()

install(TARGETS aegis RUNTIME DESTINATION bin)

enable_testing()
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
