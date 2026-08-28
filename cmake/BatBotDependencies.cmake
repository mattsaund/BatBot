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

# Build everything, dependencies included, as static archives linked into one
# self-contained `batbot` binary. Two reasons this is not merely a preference:
#   1. `batbot` is meant to be copied onto PATH and run from any directory; a
#      binary that drags a set of .so files around does not survive that.
#   2. Shared-library versioning needs symlinks (libggml.so -> libggml.so.0.x),
#      and this repository may live on a filesystem that has none -- exFAT and
#      NTFS volumes both fail the link step with "Operation not permitted".
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

find_package(Threads REQUIRED)

set(BATBOT_LLAMA_TAG b10678     CACHE STRING "llama.cpp git tag to build against")
set(BATBOT_FTXUI_TAG v7.0.3     CACHE STRING "FTXUI git tag to build against")
set(BATBOT_JSON_TAG  v3.12.0    CACHE STRING "nlohmann/json git tag to build against")

# --- nlohmann/json ---------------------------------------------------------
# Header-only; used for the config file and the trust store.
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
set(GGML_NATIVE   ${BATBOT_NATIVE} CACHE INTERNAL "")
set(GGML_CUDA     ${BATBOT_CUDA}   CACHE INTERNAL "")
set(GGML_VULKAN   ${BATBOT_VULKAN} CACHE INTERNAL "")

FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${BATBOT_LLAMA_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(nlohmann_json ftxui llama)

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
