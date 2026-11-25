cmake_minimum_required(VERSION 3.20)
project(AIBrowserWorkbench VERSION 0.1.13 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT MSVC_TOOLSET_VERSION STREQUAL "142")
    message(FATAL_ERROR "Use Visual Studio 2019, v142 and Windows x64.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 is supported.")
endif()
if(NOT CMAKE_SYSTEM_VERSION STREQUAL "10.0.19041.0")
    message(FATAL_ERROR "The initial build requires SDK 10.0.19041.0.")
endif()
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_library(awb_options INTERFACE)
target_compile_definitions(awb_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(awb_options INTERFACE /W4 /WX /permissive- /Zc:__cplusplus /EHsc /utf-8)
target_link_options(awb_options INTERFACE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)

add_library(awb_sqlite STATIC third_party/sqlite/sqlite3.c)
target_compile_definitions(awb_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
target_compile_options(awb_sqlite PRIVATE /W0 /utf-8)

add_library(awb_core STATIC src/platform/windows/platform.cpp src/browser/browser.cpp src/browser/diagnostics.cpp src/browser/extraction.cpp src/browser/picker.cpp src/browser/discovery.cpp src/core/catalog.cpp
    src/node/node_agent.cpp src/admin/backend_protocol.cpp src/admin/official_payments.cpp src/admin/oidc_identity.cpp src/admin/node_scheduler.cpp src/admin/control_plane.cpp src/admin/http.cpp src/admin/client.cpp src/admin/commercial_client.cpp src/core/service.cpp src/core/local_api.cpp src/core/workflow.cpp src/core/flow_document.cpp src/core/credentials.cpp src/core/model_provider.cpp src/core/site_package.cpp
    src/recorder/recording.cpp)
target_include_directories(awb_core PUBLIC include)
target_include_directories(awb_core SYSTEM PUBLIC third_party)
target_link_libraries(awb_core PUBLIC awb_options awb_sqlite winhttp bcrypt ncrypt crypt32 advapi32 shell32 ole32 ws2_32)

add_executable(workbench-node src/node/main.cpp)
target_link_libraries(workbench-node PRIVATE awb_core)

add_executable(workbench-admin src/admin/main.cpp)
target_link_libraries(workbench-admin PRIVATE awb_core)

add_executable(workbench-cli src/cli/main.cpp)
target_link_libraries(workbench-cli PRIVATE awb_core)
add_executable(workbench-mcp src/mcp/main.cpp)
target_link_libraries(workbench-mcp PRIVATE awb_core)
add_executable(workbench WIN32 src/ui/main.cpp src/ui/account_window.cpp src/ui/flow/flow_editor.cpp src/ui/automation_windows.cpp src/ui/library_window.cpp)
target_link_libraries(workbench PRIVATE awb_core comctl32 comdlg32 d2d1 dwrite)

enable_testing()
add_executable(awb-core-tests tests/core_tests.cpp tests/flow_document_tests.cpp)
target_link_libraries(awb-core-tests PRIVATE awb_core)
add_test(NAME core COMMAND awb-core-tests)
add_executable(awb-storage-tests tests/storage_tests.cpp)
target_link_libraries(awb-storage-tests PRIVATE awb_core)
add_test(NAME storage COMMAND awb-storage-tests --report "${CMAKE_BINARY_DIR}/$<CONFIG>/storage-report.json")
add_test(NAME win32-shell COMMAND workbench --smoke-test)
set_tests_properties(core storage win32-shell PROPERTIES TIMEOUT 45)
