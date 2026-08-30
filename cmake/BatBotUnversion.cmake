# ---------------------------------------------------------------------------
# Symlink-free shared libraries.
#
# A shared library normally lands on disk under a versioned name --
# libggml-base.so.0.9.4 -- with the plain libggml-base.so beside it as a
# symlink. That is the right layout for a library other software links against
# and resolves by SONAME. It is the wrong layout for BatBot: llama.cpp's shared
# objects live in a private directory that nothing but BatBot ever loads from,
# no other program resolves them, and there is only ever one version of each.
#
# The symlinks are not free, though. exFAT and NTFS cannot hold one, and a
# checkout on an external drive shared with a Windows install is an ordinary
# place to keep a project -- so the build simply failed there, with a linker
# error a thousand lines into the log. Clearing VERSION and SOVERSION makes
# each library a single plain file, which builds anywhere: Linux, macOS,
# Windows, and any filesystem any of them can mount.
#
# Nothing downstream cared about the version. RPATH finds the libraries by
# directory, ggml opens backend modules by exact file name, and the installer
# copies whatever the build produced.
# ---------------------------------------------------------------------------

# Clear VERSION/SOVERSION on every shared library and loadable module defined
# in `dir` or any directory beneath it.
#
# Recursive rather than a flat list of target names: ggml's CPU backend is a
# dozen targets under GGML_CPU_ALL_VARIANTS (ggml-cpu-haswell, ggml-cpu-zen4,
# ...) whose names depend on which feature levels the compiler supports, and
# any list written here would go stale the next time llama.cpp is bumped.
function(batbot_unversion_directory dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
            # Setting a property with no value unsets it, which is what makes
            # CMake fall back to the plain, unsuffixed file name.
            set_property(TARGET ${_target} PROPERTY VERSION)
            set_property(TARGET ${_target} PROPERTY SOVERSION)
            # The Mach-O equivalents, so macOS dylibs come out the same way.
            set_property(TARGET ${_target} PROPERTY MACHO_CURRENT_VERSION)
            set_property(TARGET ${_target} PROPERTY MACHO_COMPATIBILITY_VERSION)
        endif()
    endforeach()

    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        batbot_unversion_directory("${_subdir}")
    endforeach()
endfunction()
