cmake_minimum_required(VERSION 3.24)
project(crucible
    VERSION     0.1.0
    DESCRIPTION "A local-first TUI AI delegator: Crucible routes prompts to JIT-loaded subject experts"
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

# Keep binaries in one predictable place: build/bin/crucible
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Crucible options
# ---------------------------------------------------------------------------
# CRUCIBLE_BACKEND_DL is the normal way to build. ggml gets its dlopen-based
# backend loader, the CPU backend is emitted as a loadable module next to the
# binary, and CUDA/Vulkan are built later and on demand by the settings screen.
# Turning it OFF gives the old monolithic build: one static binary with at most
# one backend frozen in, selected by CRUCIBLE_CUDA / CRUCIBLE_VULKAN below. That
# mode exists for anyone who wants a single self-contained executable and is
# willing to decide the backend at compile time. It is no longer the fallback
# for filesystems without symlinks -- the loadable build works there now, see
# cmake/CrucibleUnversion.cmake.
option(CRUCIBLE_BACKEND_DL "Build with loadable GPU runtimes (recommended)" ON)

# Only consulted when CRUCIBLE_BACKEND_DL is OFF:
#   CRUCIBLE_CUDA   -> needs the CUDA toolkit (nvcc)
#   CRUCIBLE_VULKAN -> needs the Vulkan SDK + glslc + SPIR-V headers
option(CRUCIBLE_CUDA     "Monolithic build only: compile in the CUDA backend"   OFF)
option(CRUCIBLE_VULKAN   "Monolithic build only: compile in the Vulkan backend" OFF)
option(CRUCIBLE_NATIVE   "Tune the build for this exact CPU"       ON)
option(CRUCIBLE_WARNINGS "Enable a strict warning set for Crucible's own sources" ON)
option(CRUCIBLE_BUILD_TESTS "Build the unit tests" ON)
option(CRUCIBLE_BUILD_TOOLS "Build the developer tools (routebench)" ON)

# Needed before the dependencies are configured: ggml decides where to install
# its backend modules while it is being added, so CRUCIBLE_LIBDIR has to exist by
# then.
include(GNUInstallDirs)
set(CRUCIBLE_LIBDIR ${CMAKE_INSTALL_LIBDIR}/crucible)

include(cmake/CrucibleDependencies.cmake)

# ---------------------------------------------------------------------------
# crucible_core -- everything that is not the terminal UI.
# Kept as its own library so the engine can be driven headlessly (tests, a
# future `crucible --repl`, or a daemon) without dragging FTXUI along.
# ---------------------------------------------------------------------------
add_library(crucible_core STATIC
    # util: small standalone helpers with no Crucible dependencies
    src/util/text.cpp
    src/util/format.cpp
    src/util/markdown.cpp
    src/util/resources.cpp
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

    # cook: the long-running improvement loop and the record it keeps
    src/cook/journal.cpp

    # routing: the expert roster, the two routers, and the command list
    src/routing/expert.cpp
    src/routing/router.cpp
    src/routing/benchmark.cpp
    src/routing/completion.cpp

    # llm: everything that touches llama.cpp
    src/llm/model_catalog.cpp
    src/llm/model_shape.cpp
    src/llm/response_filter.cpp
    src/tools/web_search.cpp
    src/tools/workshop.cpp
    src/llm/sampling.cpp
    src/llm/loaded_model.cpp
    src/llm/model_host.cpp

    # engine: the delegation loop and the state the UI reads
    src/engine/state.cpp
    src/engine/route_policy.cpp
    src/engine/engine.cpp
    src/engine/engine_cook.cpp

    # app: process-level actions that are not the TUI
    src/app/cli.cpp
    src/app/trust_gate.cpp
    src/app/uninstall.cpp
)
add_library(crucible::core ALIAS crucible_core)

target_include_directories(crucible_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(crucible_core
    PUBLIC  llama nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads)

target_compile_definitions(crucible_core PUBLIC
    CRUCIBLE_VERSION="${PROJECT_VERSION}"
    # The runtime builder compiles GPU backends from this exact llama.cpp tag.
    # A backend built from a different one would load and then crash on the
    # first tensor, so the tag the binary was built against travels with it.
    CRUCIBLE_LLAMA_TAG="${CRUCIBLE_LLAMA_TAG}")

# ---------------------------------------------------------------------------
# crucible -- the TUI binary
# ---------------------------------------------------------------------------
add_executable(crucible
    src/main.cpp
    src/ui/app.cpp
    src/ui/commands.cpp
    src/ui/transcript.cpp
    src/ui/widgets/crucible_sprite.cpp
    src/ui/widgets/roundtable.cpp
    src/ui/widgets/markdown_view.cpp
    src/ui/widgets/scroll.cpp
    src/ui/widgets/resource_meter.cpp
    src/ui/widgets/expert_form.cpp
    src/ui/settings/settings_view.cpp
    src/ui/settings/line_editor.cpp
    src/ui/settings/model_picker.cpp
    src/ui/settings/directory_browser.cpp
    src/ui/settings/runtime_view.cpp
    src/ui/settings/gpu_order_view.cpp
    src/ui/settings/model_manager_view.cpp
    src/ui/session_picker.cpp
)

target_link_libraries(crucible PRIVATE
    crucible::core
    ftxui::screen
    ftxui::dom
    ftxui::component)

# ---------------------------------------------------------------------------
# Warnings -- applied only to our own targets, never to the vendored deps.
# ---------------------------------------------------------------------------
if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(_crucible_warnings
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wcast-align
        -Wunused -Woverloaded-virtual -Wdouble-promotion)
    target_compile_options(crucible_core PRIVATE ${_crucible_warnings})
    target_compile_options(crucible      PRIVATE ${_crucible_warnings})
endif()

# ---------------------------------------------------------------------------
# Tools
#
# crucible-routebench measures how well a candidate delegator picks subjects,
# which is the thing that decides whether Crucible works well. Built by default
# because it is small and it is how you evaluate a new router model.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BUILD_TOOLS)
    add_executable(crucible-routebench tools/routebench.cpp)
    target_link_libraries(crucible-routebench PRIVATE crucible::core)
    if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(crucible-routebench PRIVATE ${_crucible_warnings})
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These cover the parts that need no model on disk, so `ctest` runs in under a
# second on a clean checkout and is safe to gate CI on.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BUILD_TESTS)
    enable_testing()
    add_executable(crucible_tests tests/test_core.cpp)
    target_link_libraries(crucible_tests PRIVATE crucible::core)
    target_include_directories(crucible_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(crucible_tests PRIVATE ${_crucible_warnings})
    endif()
    add_test(NAME core COMMAND crucible_tests)
endif()

# ---------------------------------------------------------------------------
# Install
#
# A loadable-runtime build is no longer a single file: the binary sits in
# bin/ and llama.cpp's shared libraries in lib/crucible/. RPATH is what keeps
# `crucible` runnable from anywhere on PATH without LD_LIBRARY_PATH -- $ORIGIN
# is the directory of the binary itself, so bin/../lib/crucible resolves whether
# the prefix is ~/.local or /usr/local.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BACKEND_DL)
    set_target_properties(crucible PROPERTIES
        BUILD_RPATH   "$ORIGIN/../${CRUCIBLE_LIBDIR}"
        INSTALL_RPATH "$ORIGIN/../${CRUCIBLE_LIBDIR}")
endif()

# Everything Crucible installs goes in one component, and the install is run as
# `cmake --install build --component crucible`.
#
# That is not tidiness for its own sake. llama.cpp and ggml carry their own
# unconditional install rules -- libllama.so and libggml*.so straight into
# <prefix>/lib, plus ggml-config.cmake and ggml.pc -- written for people who
# are installing llama.cpp as a library. Crucible is not: it wants those shared
# objects in a private directory it can also delete again. Installing only
# this component leaves those rules unexecuted, so a system-wide install
# cannot drop a libllama.so into /usr/local/lib that shadows someone else's,
# and `crucible --uninstall` really does remove everything it put down.
set(CRUCIBLE_INSTALL_COMPONENT crucible)

install(TARGETS crucible
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
if(CRUCIBLE_BUILD_TOOLS)
    install(TARGETS crucible-routebench
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
endif()

if(CRUCIBLE_BACKEND_DL)
    # llama.cpp's core libraries, into Crucible's own lib directory.
    foreach(_lib llama ggml ggml-base)
        if(TARGET ${_lib})
            # These targets carry a PUBLIC_HEADER list for people who install
            # llama.cpp as a library. Crucible only wants the .so, so clear it
            # rather than scatter llama.cpp's headers through the prefix.
            set_target_properties(${_lib} PROPERTIES PUBLIC_HEADER "")
            install(TARGETS ${_lib}
                    LIBRARY DESTINATION ${CRUCIBLE_LIBDIR}
                    COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
        endif()
    endforeach()

    # No backend modules are installed, because none are built: see the
    # GGML_CPU comment in cmake/CrucibleDependencies.cmake. A fresh install has
    # an empty runtimes directory and the settings screen fills it.
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  Crucible ${PROJECT_VERSION}")
message(STATUS "  ------------------------------------------")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  native tuning : ${CRUCIBLE_NATIVE}")
message(STATUS "  tests         : ${CRUCIBLE_BUILD_TESTS}")
if(CRUCIBLE_BACKEND_DL)
    message(STATUS "  runtimes      : loadable, none built in -- add them from settings")
else()
    message(STATUS "  runtimes      : compiled in -- CUDA=${CRUCIBLE_CUDA} Vulkan=${CRUCIBLE_VULKAN}")
    if(NOT CRUCIBLE_CUDA AND NOT CRUCIBLE_VULKAN)
        message(STATUS "  -> CPU-only build with no way to add a GPU backend later.")
    endif()
endif()
message(STATUS "")
