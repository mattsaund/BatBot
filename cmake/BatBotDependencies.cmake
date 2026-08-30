# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Everything is fetched at configure time and pinned to an exact tag so a
# fresh clone always builds the same thing. To move a dependency forward,
# change the tag here and nothing else.
#
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to point at a local checkout if you want
# to develop against one without re-downloading, e.g.
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_LLAMA=/path/to/llama.cpp
# ---------------------------------------------------------------------------
include(FetchContent)

find_package(Threads REQUIRED)

set(BATBOT_LLAMA_TAG b10678     CACHE STRING "llama.cpp git tag to build against")
set(BATBOT_FTXUI_TAG v7.0.3     CACHE STRING "FTXUI git tag to build against")
set(BATBOT_JSON_TAG  v3.12.0    CACHE STRING "nlohmann/json git tag to build against")

# ---------------------------------------------------------------------------
# Symlink-free shared libraries
#
# See cmake/BatBotUnversion.cmake. In short: llama.cpp's shared objects would
# normally be written as libggml-base.so.0.9.4 plus a libggml-base.so symlink,
# and a build tree on exFAT or NTFS cannot hold the symlink. BatBot strips the
# versioning instead, so the build works on any filesystem on any platform.
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/BatBotUnversion.cmake)

# ---------------------------------------------------------------------------
# FTXUI and nlohmann/json are always static: they are BatBot's own code as far
# as deployment is concerned, and there is no reason to ship them as separate
# files. Only llama.cpp is built shared, and only when runtimes are loadable.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)

# --- nlohmann/json ---------------------------------------------------------
# Header-only; used for the config file, the trust store and session history.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${BATBOT_JSON_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

# --- FTXUI -----------------------------------------------------------------
set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(FTXUI_BUILD_DOCS     OFF CACHE INTERNAL "")
set(FTXUI_BUILD_TESTS    OFF CACHE INTERNAL "")
set(FTXUI_BUILD_MODULES  OFF CACHE INTERNAL "")
set(FTXUI_ENABLE_INSTALL OFF CACHE INTERNAL "")
set(FTXUI_QUIET          ON  CACHE INTERNAL "")

FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        ${BATBOT_FTXUI_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(nlohmann_json ftxui)

# --- llama.cpp -------------------------------------------------------------
# We link libllama directly and use the raw C API, so none of llama.cpp's own
# binaries or its `common` helper library are needed. Turning them off saves a
# large amount of build time and drops the libcurl dependency.
set(LLAMA_BUILD_TESTS    OFF CACHE INTERNAL "")
set(LLAMA_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(LLAMA_BUILD_TOOLS    OFF CACHE INTERNAL "")
set(LLAMA_BUILD_SERVER   OFF CACHE INTERNAL "")
set(LLAMA_BUILD_APP      OFF CACHE INTERNAL "")
set(LLAMA_BUILD_COMMON   OFF CACHE INTERNAL "")
set(LLAMA_CURL           OFF CACHE INTERNAL "")

set(GGML_BUILD_TESTS     OFF CACHE INTERNAL "")
set(GGML_BUILD_EXAMPLES  OFF CACHE INTERNAL "")
if(BATBOT_BACKEND_DL)
    # The whole point of this mode: ggml gains the ability to dlopen a backend
    # at startup, so CUDA and Vulkan become files in a directory the settings
    # screen manages rather than a decision frozen at compile time. ggml
    # refuses to build this way against static archives, so llama.cpp -- and
    # only llama.cpp -- is shared here.
    set(BUILD_SHARED_LIBS ON)
    set(GGML_BACKEND_DL ON  CACHE INTERNAL "")

    # No backend at all is compiled here -- not even the CPU one.
    #
    # BatBot installs with an empty runtimes directory and the settings screen
    # builds whichever backends you ask for, which is what makes the choice
    # reversible. Building the CPU modules here would produce a dozen shared
    # objects that the install rules would then have to leave behind, and it
    # roughly quadruples the time the installer spends compiling.
    #
    # GGML_NATIVE has to go with it: ggml rejects it outright in DL mode,
    # because a module chosen at run time cannot be compiled for whatever CPU
    # happened to build it. The runtime builder passes GGML_CPU_ALL_VARIANTS
    # instead, which emits one module per x86-64 feature level and lets ggml
    # score them at load.
    set(GGML_NATIVE           OFF CACHE INTERNAL "")
    set(GGML_CPU              OFF CACHE INTERNAL "")
    set(GGML_CPU_ALL_VARIANTS OFF CACHE INTERNAL "")

    # GGML_BACKEND_DIR is deliberately not set. It would bake an absolute
    # search path into the binary at build time -- wrong the moment the install
    # is relocated -- and its install rule lands outside BatBot's own install
    # component. The runtimes directory is passed to ggml at startup instead,
    # by RuntimeRegistry::load_all().

    set(GGML_CUDA       OFF CACHE INTERNAL "")
    set(GGML_VULKAN     OFF CACHE INTERNAL "")
else()
    # Monolithic fallback: one static binary with at most one GPU backend
    # compiled in. Nothing is loadable and the runtime manager reports itself
    # as unavailable, but it builds anywhere, including on exFAT.
    set(BUILD_SHARED_LIBS OFF)
    set(GGML_NATIVE     ${BATBOT_NATIVE} CACHE INTERNAL "")
    set(GGML_BACKEND_DL OFF           CACHE INTERNAL "")
    set(GGML_CPU        ON            CACHE INTERNAL "")
    set(GGML_CUDA       ${BATBOT_CUDA}   CACHE INTERNAL "")
    set(GGML_VULKAN     ${BATBOT_VULKAN} CACHE INTERNAL "")
endif()

FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${BATBOT_LLAMA_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(llama)

# llama, ggml, ggml-base and every backend module, stripped of their version
# suffixes. Safe in the monolithic build too, where all of them are static
# archives and there is nothing to strip.
batbot_unversion_directory("${llama_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Treat every dependency's headers as system headers, so BatBot can keep a
# strict warning set without drowning in diagnostics from llama.cpp and FTXUI.
# ---------------------------------------------------------------------------
foreach(_dep llama ggml ggml-base ggml-cpu nlohmann_json screen dom component)
    if(TARGET ${_dep})
        # ALIAS targets reject set_target_properties, so resolve through them.
        get_target_property(_aliased ${_dep} ALIASED_TARGET)
        if(_aliased)
            set(_real ${_aliased})
        else()
            set(_real ${_dep})
        endif()
        set_target_properties(${_real} PROPERTIES SYSTEM TRUE)
    endif()
endforeach()
