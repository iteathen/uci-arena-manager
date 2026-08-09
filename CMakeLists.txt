cmake_minimum_required(VERSION 3.28)

set(ARENA_VERSION "0.1.0" CACHE STRING "Suite-compatible Manager version")
project(
  uci_arena_manager
  VERSION ${ARENA_VERSION}
  DESCRIPTION "Standalone cross-platform UCI arena control plane"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ARENA_BUILD_TESTS "Build tests" ON)

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
  include(InstallRequiredSystemLibraries)
endif()

include(FetchContent)

FetchContent_Declare(
  chess_library
  GIT_REPOSITORY https://github.com/Disservin/chess-library.git
  GIT_TAG 1f287e0e03e1032ebee76aa0223818e5435c231a
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(chess_library)

set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_MSWEBVIEW2_VERSION "1.0.4078.44" CACHE STRING "" FORCE)
set(WEBVIEW_USE_BUILTIN_MSWEBVIEW2 ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  webview
  GIT_REPOSITORY https://github.com/webview/webview.git
  GIT_TAG 3ab4b5d722438fc8a13e6ca830c5e2372d19a01d
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(webview)

find_package(CLI11 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-libharu CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_library(arena_core
  src/domain/uuid.cpp
  src/domain/match_planner.cpp
  src/application/control_plane.cpp
  src/application/engine_onboarding.cpp
  src/application/engine_option_values.cpp
  src/application/local_auth.cpp
  src/application/broker_policy.cpp
  src/application/service_catalog.cpp
  src/application/process_supervisor.cpp
  src/application/tournament_executor.cpp
  src/adapters/sqlite_store.cpp
  src/adapters/uci_process.cpp
  src/adapters/uci_game_runner.cpp
  src/adapters/report_renderer.cpp
)

target_include_directories(arena_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${chess_library_SOURCE_DIR}/include
)
target_compile_definitions(
  arena_core
  PUBLIC ARENA_VERSION="${ARENA_VERSION}"
)

target_link_libraries(arena_core
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    unofficial::sqlite3::sqlite3
    spdlog::spdlog
    unofficial-sodium::sodium
    unofficial::libharu::hpdf
)

if(WIN32)
  target_sources(arena_core PRIVATE
    src/platform/windows/operator_credential_file.cpp
    src/platform/windows/path_inspection.cpp
    src/platform/windows/process_platform.cpp
    src/platform/windows/service_control.cpp
    src/platform/windows/workspace_permissions.cpp
  )
  target_link_libraries(arena_core PRIVATE advapi32 bcrypt shell32 userenv)
else()
  target_sources(arena_core PRIVATE
    src/platform/linux/operator_credential_file.cpp
    src/platform/linux/path_inspection.cpp
    src/platform/linux/process_platform.cpp
    src/platform/linux/service_control.cpp
    src/platform/linux/workspace_permissions.cpp
  )
  target_link_libraries(arena_core PRIVATE pthread)
endif()

add_executable(arena-service
  apps/service/main.cpp
  apps/service/events_websocket.cpp
)
target_link_libraries(arena-service PRIVATE arena_core Drogon::Drogon)
if(WIN32)
  target_link_libraries(arena-service PRIVATE advapi32)
endif()

add_executable(arena apps/cli/main.cpp apps/cli/ui_picker.cpp)
target_link_libraries(
  arena
  PRIVATE
    arena_core
    CLI11::CLI11
    CURL::libcurl
    Drogon::Drogon
    unofficial-sodium::sodium
)
if(WIN32)
  target_link_libraries(arena PRIVATE ole32)
endif()

add_executable(arena-broker apps/broker/main.cpp)
target_link_libraries(arena-broker PRIVATE arena_core CLI11::CLI11)

if(WIN32)
  add_executable(
    uci-arena-manager
    WIN32
    apps/launcher/main.cpp
    apps/launcher/resources.rc
  )
  target_link_libraries(
    uci-arena-manager
    PRIVATE
      webview::core
      "${MSWebView2_INCLUDE_DIR}/../x64/WebView2LoaderStatic.lib"
      ole32
      shell32
      ws2_32
  )
  target_compile_definitions(
    uci-arena-manager
    PRIVATE
      WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=0
      WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK=0
  )
  target_include_directories(
    uci-arena-manager
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
else()
  add_executable(uci-arena-manager apps/launcher/main.cpp)
  target_link_libraries(uci-arena-manager PRIVATE webview::core)
endif()

if(MSVC)
  target_compile_options(arena_core PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-service PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-broker PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(uci-arena-manager PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(arena_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-service PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-broker PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(uci-arena-manager PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  set(ARENA_MANAGER_RUNTIME_SEARCH_DIRECTORIES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
else()
  set(ARENA_MANAGER_RUNTIME_SEARCH_DIRECTORIES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
endif()
install(
  TARGETS arena-service arena arena-broker uci-arena-manager
  RUNTIME_DEPENDENCIES
    DIRECTORIES ${ARENA_MANAGER_RUNTIME_SEARCH_DIRECTORIES}
    PRE_EXCLUDE_REGEXES
      "api-ms-"
      "ext-ms-"
      "[Aa]zure[Aa]ttest.*"
      "[Hh]vsi[Ff]ile[Tt]rust\\.dll"
      "[Pp]dm[Uu]tilities\\.dll"
      "[Ww]paxholder\\.dll"
    POST_EXCLUDE_REGEXES
      ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
      "^/lib(64)?/.*"
      "^/usr/lib(64)?/.*"
  RUNTIME DESTINATION bin
)
install(DIRECTORY web/dist/ DESTINATION share/uci-arena/web)
install(DIRECTORY specs/ DESTINATION share/uci-arena/specs)
install(FILES LICENSE DESTINATION share/licenses/uci-arena-manager)
install(
  FILES
    packaging/icons/uci-arena-manager.svg
    packaging/icons/uci-arena-manager.png
  DESTINATION share/icons/uci-arena-manager
)
install(
  FILES config/broker-policy.example.json
  DESTINATION share/uci-arena/config
)
install(FILES docs/arena.1 docs/cli.md DESTINATION share/doc/uci-arena-manager)
install(
  FILES "${webview_SOURCE_DIR}/LICENSE"
  DESTINATION share/licenses/webview
  RENAME LICENSE
)
if(WIN32)
  install(
    FILES
      "${MSWebView2_INCLUDE_DIR}/../../../LICENSE.txt"
      "${MSWebView2_INCLUDE_DIR}/../../../NOTICE.txt"
    DESTINATION share/licenses/microsoft-webview2
  )
endif()
file(GLOB ARENA_VCPKG_COPYRIGHTS CONFIGURE_DEPENDS
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
foreach(ARENA_VCPKG_COPYRIGHT IN LISTS ARENA_VCPKG_COPYRIGHTS)
  get_filename_component(ARENA_VCPKG_SHARE_DIR
    "${ARENA_VCPKG_COPYRIGHT}" DIRECTORY)
  get_filename_component(ARENA_VCPKG_PACKAGE
    "${ARENA_VCPKG_SHARE_DIR}" NAME)
  install(FILES "${ARENA_VCPKG_COPYRIGHT}"
    DESTINATION "share/licenses/vcpkg/${ARENA_VCPKG_PACKAGE}")
endforeach()

if(ARENA_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(arena_mock_uci tests/mocks/mock_uci_engine.cpp)
  add_executable(arena_mock_service tests/mocks/mock_service_cli.cpp)
  add_executable(arena_tests
    tests/unit/domain_tests.cpp
    tests/unit/engine_onboarding_tests.cpp
    tests/unit/match_planner_tests.cpp
    tests/unit/security_report_tests.cpp
    tests/integration/service_output_integration_tests.cpp
    tests/integration/sqlite_store_tests.cpp
    tests/integration/tournament_execution_tests.cpp
    tests/integration/uci_probe_tests.cpp
  )
  add_dependencies(arena_tests arena_mock_uci arena_mock_service)
  target_compile_definitions(
    arena_tests
    PRIVATE
      ARENA_MOCK_UCI_PATH="$<TARGET_FILE:arena_mock_uci>"
      ARENA_MOCK_SERVICE_PATH="$<TARGET_FILE:arena_mock_service>"
  )
  target_link_libraries(arena_tests PRIVATE arena_core Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(arena_tests)
endif()
