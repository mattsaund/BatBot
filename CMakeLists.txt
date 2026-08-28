cmake_minimum_required(VERSION 3.24)
project(batbot
    VERSION     0.1.0
    DESCRIPTION "A local-first TUI AI delegator: BatBot routes prompts to JIT-loaded subject experts"
    LANGUAGES   C CXX)

# ---------------------------------------------------------------------------
# Standards & defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Keep binaries in one predictable place: build/bin/batbot
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# BatBot options
# ---------------------------------------------------------------------------
# GPU backends are OFF by default because they need extra toolchains:
#   BATBOT_CUDA   -> needs the CUDA toolkit (nvcc)
#   BATBOT_VULKAN -> needs the Vulkan SDK + glslc (shader compiler)
# See README "GPU acceleration" for the exact packages.
option(BATBOT_CUDA     "Build llama.cpp with the CUDA backend"   OFF)
option(BATBOT_VULKAN   "Build llama.cpp with the Vulkan backend" OFF)
option(BATBOT_NATIVE   "Tune the build for this exact CPU"       ON)
option(BATBOT_WARNINGS "Enable a strict warning set for BatBot's own sources" ON)
option(BATBOT_BUILD_TESTS "Build the unit tests" ON)
option(BATBOT_BUILD_TOOLS "Build the developer tools (routebench)" ON)

include(cmake/BatBotDependencies.cmake)

# ---------------------------------------------------------------------------
# batbot_core -- everything that is not the terminal UI.
# Kept as its own library so the engine can be driven headlessly (tests, a
# future `batbot --repl`, or a daemon) without dragging FTXUI along.
# ---------------------------------------------------------------------------
add_library(batbot_core STATIC
    src/core/subject.cpp
    src/core/text.cpp
    src/core/config.cpp
    src/core/trust.cpp
    src/core/uninstall.cpp
    src/core/paths.cpp
    src/core/model_catalog.cpp
    src/core/model_host.cpp
    src/core/router.cpp
    src/core/state.cpp
    src/core/engine.cpp
)
add_library(batbot::core ALIAS batbot_core)

target_include_directories(batbot_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(batbot_core
    PUBLIC  llama nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads)

target_compile_definitions(batbot_core PUBLIC
    BATBOT_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# batbot -- the TUI binary
# ---------------------------------------------------------------------------
add_executable(batbot
    src/main.cpp
    src/ui/bat_sprite.cpp
    src/ui/roundtable.cpp
    src/ui/settings.cpp
    src/ui/app.cpp
)

target_link_libraries(batbot PRIVATE
    batbot::core
    ftxui::screen
    ftxui::dom
    ftxui::component)

# ---------------------------------------------------------------------------
# Warnings -- applied only to our own targets, never to the vendored deps.
# ---------------------------------------------------------------------------
if(BATBOT_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(_batbot_warnings
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wcast-align
        -Wunused -Woverloaded-virtual -Wdouble-promotion)
    target_compile_options(batbot_core PRIVATE ${_batbot_warnings})
    target_compile_options(batbot      PRIVATE ${_batbot_warnings})
endif()

# ---------------------------------------------------------------------------
# Tools
#
# batbot-routebench measures how well a candidate delegator picks subjects,
# which is the thing that decides whether BatBot works well. Built by default
# because it is small and it is how you evaluate a new router model.
# ---------------------------------------------------------------------------
if(BATBOT_BUILD_TOOLS)
    add_executable(batbot-routebench tools/routebench.cpp)
    target_link_libraries(batbot-routebench PRIVATE batbot::core)
    if(BATBOT_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(batbot-routebench PRIVATE ${_batbot_warnings})
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These cover the parts that need no model on disk, so `ctest` runs in under a
# second on a clean checkout and is safe to gate CI on.
# ---------------------------------------------------------------------------
if(BATBOT_BUILD_TESTS)
    enable_testing()
    add_executable(batbot_tests tests/test_core.cpp)
    target_link_libraries(batbot_tests PRIVATE batbot::core)
    target_include_directories(batbot_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(BATBOT_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(batbot_tests PRIVATE ${_batbot_warnings})
    endif()
    add_test(NAME core COMMAND batbot_tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS batbot RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BATBOT_BUILD_TOOLS)
    install(TARGETS batbot-routebench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  BatBot ${PROJECT_VERSION}")
message(STATUS "  ------------------------------------------")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  CUDA backend  : ${BATBOT_CUDA}")
message(STATUS "  Vulkan backend: ${BATBOT_VULKAN}")
message(STATUS "  native tuning : ${BATBOT_NATIVE}")
message(STATUS "  tests         : ${BATBOT_BUILD_TESTS}")
if(NOT BATBOT_CUDA AND NOT BATBOT_VULKAN)
    message(STATUS "  -> CPU-only build. See README 'GPU acceleration' to enable a GPU backend.")
endif()
message(STATUS "")
