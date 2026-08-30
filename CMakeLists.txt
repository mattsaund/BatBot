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
# BATBOT_BACKEND_DL is the normal way to build. ggml gets its dlopen-based
# backend loader, the CPU backend is emitted as a loadable module next to the
# binary, and CUDA/Vulkan are built later and on demand by the settings screen.
# Turning it OFF gives the old monolithic build: one static binary with at most
# one backend frozen in, selected by BATBOT_CUDA / BATBOT_VULKAN below. That
# mode exists for anyone who wants a single self-contained executable and is
# willing to decide the backend at compile time. It is no longer the fallback
# for filesystems without symlinks -- the loadable build works there now, see
# cmake/BatBotUnversion.cmake.
option(BATBOT_BACKEND_DL "Build with loadable GPU runtimes (recommended)" ON)

# Only consulted when BATBOT_BACKEND_DL is OFF:
#   BATBOT_CUDA   -> needs the CUDA toolkit (nvcc)
#   BATBOT_VULKAN -> needs the Vulkan SDK + glslc + SPIR-V headers
option(BATBOT_CUDA     "Monolithic build only: compile in the CUDA backend"   OFF)
option(BATBOT_VULKAN   "Monolithic build only: compile in the Vulkan backend" OFF)
option(BATBOT_NATIVE   "Tune the build for this exact CPU"       ON)
option(BATBOT_WARNINGS "Enable a strict warning set for BatBot's own sources" ON)
option(BATBOT_BUILD_TESTS "Build the unit tests" ON)
option(BATBOT_BUILD_TOOLS "Build the developer tools (routebench)" ON)

# Needed before the dependencies are configured: ggml decides where to install
# its backend modules while it is being added, so BATBOT_LIBDIR has to exist by
# then.
include(GNUInstallDirs)
set(BATBOT_LIBDIR ${CMAKE_INSTALL_LIBDIR}/batbot)

include(cmake/BatBotDependencies.cmake)

# ---------------------------------------------------------------------------
# batbot_core -- everything that is not the terminal UI.
# Kept as its own library so the engine can be driven headlessly (tests, a
# future `batbot --repl`, or a daemon) without dragging FTXUI along.
# ---------------------------------------------------------------------------
add_library(batbot_core STATIC
    # util: small standalone helpers with no BatBot dependencies
    src/util/text.cpp
    src/util/format.cpp
    src/util/subprocess.cpp

    # config: what is on disk -- settings, paths, folder trust
    src/config/paths.cpp
    src/config/config.cpp
    src/config/config_io.cpp
    src/config/gpu_policy.cpp
    src/config/trust.cpp

    # runtime: the loadable compute backends and the devices they expose
    src/runtime/backend.cpp
    src/runtime/registry.cpp
    src/runtime/builder.cpp
    src/runtime/devices.cpp

    # session: token accounting and per-project conversation history
    src/session/usage.cpp
    src/session/store.cpp

    # routing: the subject table, the two routers, and the command list
    src/routing/subject.cpp
    src/routing/router.cpp
    src/routing/completion.cpp

    # llm: everything that touches llama.cpp
    src/llm/model_catalog.cpp
    src/llm/model_shape.cpp
    src/llm/sampling.cpp
    src/llm/loaded_model.cpp
    src/llm/model_host.cpp

    # engine: the delegation loop and the state the UI reads
    src/engine/state.cpp
    src/engine/route_policy.cpp
    src/engine/engine.cpp

    # app: process-level actions that are not the TUI
    src/app/cli.cpp
    src/app/trust_gate.cpp
    src/app/uninstall.cpp
)
add_library(batbot::core ALIAS batbot_core)

target_include_directories(batbot_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(batbot_core
    PUBLIC  llama nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads)

target_compile_definitions(batbot_core PUBLIC
    BATBOT_VERSION="${PROJECT_VERSION}"
    # The runtime builder compiles GPU backends from this exact llama.cpp tag.
    # A backend built from a different one would load and then crash on the
    # first tensor, so the tag the binary was built against travels with it.
    BATBOT_LLAMA_TAG="${BATBOT_LLAMA_TAG}")

# ---------------------------------------------------------------------------
# batbot -- the TUI binary
# ---------------------------------------------------------------------------
add_executable(batbot
    src/main.cpp
    src/ui/app.cpp
    src/ui/commands.cpp
    src/ui/transcript.cpp
    src/ui/widgets/bat_sprite.cpp
    src/ui/widgets/roundtable.cpp
    src/ui/settings/settings_view.cpp
    src/ui/settings/line_editor.cpp
    src/ui/settings/model_picker.cpp
    src/ui/settings/directory_browser.cpp
    src/ui/settings/runtime_view.cpp
    src/ui/settings/gpu_order_view.cpp
    src/ui/settings/model_manager_view.cpp
    src/ui/session_picker.cpp
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
#
# A loadable-runtime build is no longer a single file: the binary sits in
# bin/ and llama.cpp's shared libraries in lib/batbot/. RPATH is what keeps
# `batbot` runnable from anywhere on PATH without LD_LIBRARY_PATH -- $ORIGIN
# is the directory of the binary itself, so bin/../lib/batbot resolves whether
# the prefix is ~/.local or /usr/local.
# ---------------------------------------------------------------------------
if(BATBOT_BACKEND_DL)
    set_target_properties(batbot PROPERTIES
        BUILD_RPATH   "$ORIGIN/../${BATBOT_LIBDIR}"
        INSTALL_RPATH "$ORIGIN/../${BATBOT_LIBDIR}")
endif()

# Everything BatBot installs goes in one component, and the install is run as
# `cmake --install build --component batbot`.
#
# That is not tidiness for its own sake. llama.cpp and ggml carry their own
# unconditional install rules -- libllama.so and libggml*.so straight into
# <prefix>/lib, plus ggml-config.cmake and ggml.pc -- written for people who
# are installing llama.cpp as a library. BatBot is not: it wants those shared
# objects in a private directory it can also delete again. Installing only
# this component leaves those rules unexecuted, so a system-wide install
# cannot drop a libllama.so into /usr/local/lib that shadows someone else's,
# and `batbot --uninstall` really does remove everything it put down.
set(BATBOT_INSTALL_COMPONENT batbot)

install(TARGETS batbot
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${BATBOT_INSTALL_COMPONENT})
if(BATBOT_BUILD_TOOLS)
    install(TARGETS batbot-routebench
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${BATBOT_INSTALL_COMPONENT})
endif()

if(BATBOT_BACKEND_DL)
    # llama.cpp's core libraries, into BatBot's own lib directory.
    foreach(_lib llama ggml ggml-base)
        if(TARGET ${_lib})
            # These targets carry a PUBLIC_HEADER list for people who install
            # llama.cpp as a library. BatBot only wants the .so, so clear it
            # rather than scatter llama.cpp's headers through the prefix.
            set_target_properties(${_lib} PROPERTIES PUBLIC_HEADER "")
            install(TARGETS ${_lib}
                    LIBRARY DESTINATION ${BATBOT_LIBDIR}
                    COMPONENT ${BATBOT_INSTALL_COMPONENT})
        endif()
    endforeach()

    # No backend modules are installed, because none are built: see the
    # GGML_CPU comment in cmake/BatBotDependencies.cmake. A fresh install has
    # an empty runtimes directory and the settings screen fills it.
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  BatBot ${PROJECT_VERSION}")
message(STATUS "  ------------------------------------------")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  native tuning : ${BATBOT_NATIVE}")
message(STATUS "  tests         : ${BATBOT_BUILD_TESTS}")
if(BATBOT_BACKEND_DL)
    message(STATUS "  runtimes      : loadable, none built in -- add them from settings")
else()
    message(STATUS "  runtimes      : compiled in -- CUDA=${BATBOT_CUDA} Vulkan=${BATBOT_VULKAN}")
    if(NOT BATBOT_CUDA AND NOT BATBOT_VULKAN)
        message(STATUS "  -> CPU-only build with no way to add a GPU backend later.")
    endif()
endif()
message(STATUS "")
