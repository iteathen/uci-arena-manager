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
option(
  ARENA_WINDOWS_PER_USER_INSTALL
  "Build the fixed-scope non-elevated Windows per-user package"
  OFF
)
option(
  ARENA_DEVELOPMENT_SAME_VERSION_UPGRADES
  "Allow replacement of an installed Complete development candidate at the same version"
  OFF
)

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
  src/application/engine_onboarding.cpp
  src/application/local_auth.cpp
  src/application/broker_policy.cpp
  src/application/service_catalog.cpp
  src/application/process_supervisor.cpp
  src/application/tournament_executor.cpp
  src/application/installer/setup.cpp
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
    src/platform/windows/workspace_permissions.cpp
  )
  target_link_libraries(arena_core PRIVATE advapi32 bcrypt shell32 userenv)
else()
  target_sources(arena_core PRIVATE
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

add_executable(arena-setup apps/setup/main.cpp)
target_link_libraries(arena-setup PRIVATE arena_core CLI11::CLI11 CURL::libcurl)

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
  target_compile_options(arena-setup PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(uci-arena-manager PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(arena_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-service PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-broker PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-setup PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(uci-arena-manager PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  install(TARGETS arena-service arena arena-broker arena-setup uci-arena-manager RUNTIME DESTINATION bin)
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
  file(GLOB ARENA_VCPKG_COPYRIGHTS CONFIGURE_DEPENDS
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
  foreach(ARENA_VCPKG_COPYRIGHT IN LISTS ARENA_VCPKG_COPYRIGHTS)
    get_filename_component(ARENA_VCPKG_SHARE_DIR
      "${ARENA_VCPKG_COPYRIGHT}" DIRECTORY)
    get_filename_component(ARENA_VCPKG_PACKAGE
      "${ARENA_VCPKG_SHARE_DIR}" NAME)
    install(FILES "${ARENA_VCPKG_COPYRIGHT}"
      DESTINATION "share/vcpkg/${ARENA_VCPKG_PACKAGE}")
  endforeach()
else()
  install(TARGETS arena-service arena arena-broker arena-setup uci-arena-manager RUNTIME DESTINATION bin)
endif()
install(DIRECTORY web/dist/ DESTINATION share/uci-arena/web)
install(DIRECTORY specs/ DESTINATION share/uci-arena/specs)
if(WIN32)
  if(ARENA_WINDOWS_PER_USER_INSTALL)
    set(ARENA_WINDOWS_INSTALL_SCOPE "per-user")
  else()
    set(ARENA_WINDOWS_INSTALL_SCOPE "per-machine")
  endif()
  configure_file(
    packaging/windows/install-scope.txt.in
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/windows/install-scope.txt"
    @ONLY
  )
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/windows/install-scope.txt"
    DESTINATION share/uci-arena
  )
endif()
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
if(NOT WIN32)
  find_program(ARENA_GZIP_EXECUTABLE gzip REQUIRED)
  set(ARENA_GENERATED_LINUX_DOC_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/linux")
  file(MAKE_DIRECTORY "${ARENA_GENERATED_LINUX_DOC_DIR}")
  configure_file(
    docs/arena.1
    "${ARENA_GENERATED_LINUX_DOC_DIR}/arena.1"
    COPYONLY
  )
  configure_file(
    docs/arena-setup.1
    "${ARENA_GENERATED_LINUX_DOC_DIR}/arena-setup.1"
    COPYONLY
  )
  configure_file(
    packaging/linux/changelog.in
    "${ARENA_GENERATED_LINUX_DOC_DIR}/changelog"
    @ONLY
  )
  foreach(ARENA_LINUX_DOC arena.1 arena-setup.1 changelog)
    execute_process(
      COMMAND
        "${ARENA_GZIP_EXECUTABLE}" -9 -n -c
        "${ARENA_GENERATED_LINUX_DOC_DIR}/${ARENA_LINUX_DOC}"
      OUTPUT_FILE
        "${ARENA_GENERATED_LINUX_DOC_DIR}/${ARENA_LINUX_DOC}.gz"
      COMMAND_ERROR_IS_FATAL ANY
    )
  endforeach()

  install(FILES packaging/linux/uci-arena-manager.service DESTINATION lib/systemd/system)
  install(FILES packaging/linux/uci-arena-broker.service DESTINATION lib/systemd/system)
  install(FILES packaging/linux/uci-arena-manager.desktop DESTINATION share/applications)
  install(FILES packaging/icons/uci-arena-manager.svg DESTINATION share/icons/hicolor/scalable/apps)
  install(FILES packaging/icons/uci-arena-manager.png DESTINATION share/icons/hicolor/256x256/apps)
  install(
    FILES
      "${ARENA_GENERATED_LINUX_DOC_DIR}/arena.1.gz"
      "${ARENA_GENERATED_LINUX_DOC_DIR}/arena-setup.1.gz"
    DESTINATION share/man/man1
  )
  install(
    FILES
      packaging/linux/copyright
      "${ARENA_GENERATED_LINUX_DOC_DIR}/changelog.gz"
    DESTINATION share/doc/uci-arena-manager
  )
  install(
    FILES packaging/linux/uci-arena-manager.lintian-overrides
    DESTINATION share/lintian/overrides
    RENAME uci-arena-manager
  )
  file(GLOB ARENA_VCPKG_COPYRIGHTS CONFIGURE_DEPENDS
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
  foreach(ARENA_VCPKG_COPYRIGHT IN LISTS ARENA_VCPKG_COPYRIGHTS)
    get_filename_component(ARENA_VCPKG_SHARE_DIR
      "${ARENA_VCPKG_COPYRIGHT}" DIRECTORY)
    get_filename_component(ARENA_VCPKG_PACKAGE
      "${ARENA_VCPKG_SHARE_DIR}" NAME)
    install(FILES "${ARENA_VCPKG_COPYRIGHT}"
      DESTINATION "share/doc/uci-arena-manager/third-party/${ARENA_VCPKG_PACKAGE}")
  endforeach()
endif()
install(FILES docs/cli.md DESTINATION share/doc/uci-arena-manager)

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
    tests/unit/setup_tests.cpp
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

if(ARENA_ENABLE_PACKAGING)
  set(CPACK_PACKAGE_NAME "uci-arena-manager")
  set(CPACK_PACKAGE_VENDOR "UCI Arena Manager contributors")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_CONTACT
    "UCI Arena Manager contributors <iteathen@users.noreply.github.com>")
  set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/iteathen/uci-arena-manager")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Local-first UCI chess arena control plane")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  if(WIN32)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
      COPYONLY
    )
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
    set(CPACK_GENERATOR "WIX;ZIP")
    if(ARENA_WINDOWS_PER_USER_INSTALL)
      if(CMAKE_VERSION VERSION_LESS 4.3)
        message(FATAL_ERROR
          "Per-user MSI generation requires CMake 4.3 or newer for HKCU component key paths")
      endif()
      set(CPACK_PACKAGE_FILE_NAME
        "UCI-Arena-Manager-${PROJECT_VERSION}-windows-x64-user")
      set(CPACK_WIX_INSTALL_SCOPE "perUser")
      set(CPACK_WIX_ROOT_FOLDER_ID "LocalAppDataFolder")
      set(CPACK_PACKAGE_INSTALL_DIRECTORY "Programs/UCI Arena Manager")
      set(CPACK_WIX_COMPONENT_KEYS_REGISTRY_PATH
        "Software\\\\UCI Arena Manager\\\\Installer\\\\Components")
      set(CPACK_WIX_EXTRA_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/UserFolders.wxs")
      set(CPACK_WIX_UPGRADE_GUID "143E64C2-4153-4E8B-8C93-6FB590779E9E")
      set(CPACK_WIX_PATCH_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/UserPatch.xml")
    else()
      set(CPACK_PACKAGE_FILE_NAME
        "UCI-Arena-Manager-${PROJECT_VERSION}-windows-x64-machine")
      set(CPACK_ARCHIVE_FILE_NAME
        "UCI-Arena-Manager-${PROJECT_VERSION}-windows-x64")
      set(CPACK_WIX_INSTALL_SCOPE "perMachine")
      set(CPACK_PACKAGE_INSTALL_DIRECTORY "UCI Arena Manager")
      set(CPACK_WIX_UPGRADE_GUID "930B856A-66FB-4A34-B998-8A61684EA345")
      set(CPACK_WIX_PATCH_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/ServicePatch.xml")
    endif()
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_PRODUCT_ICON
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/uci-arena-manager.ico")
    set(CPACK_WIX_TEMPLATE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Package.wxs.in")
    if(ARENA_DEVELOPMENT_SAME_VERSION_UPGRADES)
      set(CPACK_WIX_ALLOW_SAME_VERSION_UPGRADES "yes")
    else()
      set(CPACK_WIX_ALLOW_SAME_VERSION_UPGRADES "no")
    endif()
  else()
    set(CPACK_GENERATOR "DEB;RPM;TGZ")
    set(CPACK_STRIP_FILES TRUE)
    set(CPACK_PACKAGE_FILE_NAME
      "uci-arena-manager-${PROJECT_VERSION}-linux-x86_64")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "games")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"Local-first UCI chess arena control plane
 UCI Arena Manager provides the native service, authenticated CLI, desktop
 launcher, and typed installer for local UCI engine matches.
 .
 It manages engines, optional arena services, immutable component receipts,
 and retained operator data through one loopback-only control plane.")
    set(CPACK_RPM_FILE_NAME
      "uci-arena-manager-${PROJECT_VERSION}-1.x86_64.rpm")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "libc6, libstdc++6, systemd, init-system-helpers, passwd, acl, libwebkit2gtk-4.1-0, zenity")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/debian/postinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/debian/prerm"
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/debian/postrm"
    )
    set(CPACK_RPM_PACKAGE_REQUIRES "systemd, acl, webkit2gtk4.1, zenity")
    set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-or-later")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Engineering")
    set(CPACK_RPM_PACKAGE_URL
      "https://github.com/iteathen/uci-arena-manager")
    set(CPACK_RPM_PACKAGE_DESCRIPTION
      "Local-first native control plane for UCI engine matches and services.")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
  endif()
  include(CPack)
endif()
