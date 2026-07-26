cmake_minimum_required(VERSION 3.28)

set(ARENA_VERSION "0.1.0" CACHE STRING "Suite-compatible Manager version")
set(
  ARENA_TUF_ROOT_SHA256
  ""
  CACHE STRING
  "Pinned SHA-256 of the trusted TUF root metadata for official arena-setup builds"
)
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
option(ARENA_ENABLE_PACKAGING "Enable CPack packaging" ON)

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
  set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
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

find_package(CLI11 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-libharu CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_library(arena_core
  src/domain/uuid.cpp
  src/domain/match_planner.cpp
  src/application/control_plane.cpp
  src/application/local_auth.cpp
  src/application/broker_policy.cpp
  src/application/service_catalog.cpp
  src/application/process_supervisor.cpp
  src/application/installer/setup.cpp
  src/adapters/sqlite_store.cpp
  src/adapters/uci_process.cpp
  src/adapters/report_renderer.cpp
)

target_include_directories(arena_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${chess_library_SOURCE_DIR}/include
)
target_compile_definitions(
  arena_core
  PRIVATE ARENA_TUF_ROOT_SHA256="${ARENA_TUF_ROOT_SHA256}"
)

target_link_libraries(arena_core
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    spdlog::spdlog
    unofficial-sodium::sodium
    unofficial::libharu::hpdf
    zstd::libzstd
    LibArchive::LibArchive
)

if(WIN32)
  target_sources(arena_core PRIVATE
    src/platform/windows/process_platform.cpp
    src/platform/windows/service_control.cpp
  )
  target_link_libraries(arena_core PRIVATE advapi32 bcrypt shell32 userenv)
else()
  target_sources(arena_core PRIVATE
    src/platform/linux/process_platform.cpp
    src/platform/linux/service_control.cpp
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

add_executable(arena apps/cli/main.cpp)
target_link_libraries(arena PRIVATE arena_core CLI11::CLI11 CURL::libcurl)

add_executable(arena-broker apps/broker/main.cpp)
target_link_libraries(arena-broker PRIVATE arena_core CLI11::CLI11)

add_executable(arena-setup apps/setup/main.cpp)
target_link_libraries(arena-setup PRIVATE arena_core CLI11::CLI11 CURL::libcurl)

if(MSVC)
  target_compile_options(arena_core PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-service PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-broker PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-setup PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(arena_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-service PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-broker PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-setup PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  install(TARGETS arena-service arena arena-broker arena-setup RUNTIME DESTINATION bin)
  install(
    DIRECTORY
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
    DESTINATION bin
    CONFIGURATIONS Release RelWithDebInfo MinSizeRel
    FILES_MATCHING PATTERN "*.dll"
  )
  install(
    DIRECTORY
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/"
    DESTINATION bin
    CONFIGURATIONS Debug
    FILES_MATCHING PATTERN "*.dll"
  )
  install(
    DIRECTORY
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/"
    DESTINATION share/vcpkg
    FILES_MATCHING PATTERN "copyright"
  )
else()
  install(TARGETS arena-service arena arena-broker arena-setup RUNTIME DESTINATION bin)
endif()
install(DIRECTORY web/dist/ DESTINATION share/uci-arena/web)
install(DIRECTORY specs/ DESTINATION share/uci-arena/specs)
if(NOT WIN32)
  install(FILES packaging/linux/uci-arena-manager.service DESTINATION lib/systemd/system)
  install(FILES packaging/linux/uci-arena-broker.service DESTINATION lib/systemd/system)
endif()

if(ARENA_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(arena_mock_uci tests/mocks/mock_uci_engine.cpp)
  add_executable(arena_mock_service tests/mocks/mock_service_cli.cpp)
  add_executable(arena_tests
    tests/unit/domain_tests.cpp
    tests/unit/match_planner_tests.cpp
    tests/unit/security_report_tests.cpp
    tests/unit/setup_tests.cpp
    tests/integration/service_output_integration_tests.cpp
    tests/integration/sqlite_store_tests.cpp
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

if(ARENA_ENABLE_PACKAGING)
  set(CPACK_PACKAGE_NAME "uci-arena-manager")
  set(CPACK_PACKAGE_VENDOR "UCI Arena Manager contributors")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_CONTACT "https://github.com/iteathen/uci-arena-manager")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  if(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_PACKAGE_FILE_NAME
      "UCI-Arena-Manager-${PROJECT_VERSION}-windows-x64")
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_UPGRADE_GUID "930B856A-66FB-4A34-B998-8A61684EA345")
  else()
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_RPM_FILE_NAME
      "uci-arena-manager-${PROJECT_VERSION}-1.x86_64.rpm")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libstdc++6, systemd")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm"
    )
    set(CPACK_RPM_PACKAGE_REQUIRES "systemd")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
  endif()
  include(CPack)
endif()
