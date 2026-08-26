# Scope the commit-dependent version definitions to the sources that actually read them.
#
# THE PROBLEM
#
# `cmake/prep/build_version.cmake` appends seven definitions to the global
# `SUNSHINE_DEFINITIONS` list, and `cmake/targets/common.cmake` applies that whole list to the
# `sunshine` target with `target_compile_definitions(... PUBLIC ...)`; `tests/CMakeLists.txt`
# does the same for the test target. Two of the seven change with every commit:
#
#   PROJECT_VERSION         gets a `-<short-sha>` suffix on every branch whose name is not
#                           `master`. In CI the checkout is detached, so `rev-parse
#                           --abbrev-ref HEAD` reports `HEAD` and the suffix is ALWAYS applied.
#   PROJECT_VERSION_COMMIT  is `$ENV{COMMIT}` -- empty in this repo's CI, the raw SHA in
#                           LizardByte's release workflows.
#
# ccache hashes the compiler command line. A definition on every command line therefore means
# every object is a cache miss on every new commit. Measured on run 32850943788, whose only
# changed file was `.github/workflows/ci.yml` -- no C++ at all:
#
#     Hits:              0 / 349 ( 0.00%)
#     Misses:          349 / 349 (100.0%)
#
# A re-run of an IDENTICAL sha hit 349/349 and built in 16s; any new commit paid the full 836s.
# That is the "it hits only on full match" behaviour: the cache was working exactly as designed,
# but the key included the commit identity for every translation unit.
#
# WHAT THIS DOES
#
# `reserve()` removes those two definitions from the copy of `SUNSHINE_DEFINITIONS` that the
# targets consume, and `apply()` hands them back to the only two sources that read them. Upstream's
# list literal in `cmake/prep/build_version.cmake` is NOT edited -- the values are produced there
# exactly as before and are re-emitted here verbatim, so the version string every consumer
# observes is byte-for-byte identical. This is a cache-key change, not a behaviour change.
#
# WHY THE OTHER FIVE STAY GLOBAL
#
# Scoping the minimum is the point; each definition moved off the global list is another place a
# future source can fail to compile. `PROJECT_FQDN`, `PROJECT_NAME` and
# `PROJECT_VERSION_MAJOR/MINOR/PATCH` contain no commit identity -- MAJOR/MINOR/PATCH are parsed
# by regex from the leading `<n>.<n>.<n>` of `PROJECT_VERSION`, ahead of the sha suffix, so they
# are stable across commits and cost nothing in cache misses. `PROJECT_NAME` in particular is
# read by `tests/unit/test_system_tray.cpp`; leaving it global means the test target is
# untouched by this change.
#
# CONSUMERS
#
#   git grep -nw PROJECT_VERSION        -- src/ tests/
#   git grep -nw PROJECT_VERSION_COMMIT -- src/ tests/
#
# return `src/main.cpp`, `src/confighttp.cpp` and `src/platform/windows/windows.rc`. The .rc does
# NOT read this list: `cmake/compile_definitions/windows.cmake` builds a dedicated
# `sunshine_rc_object` OBJECT library and gives it its own `COMPILE_DEFINITIONS` string
# containing `PROJECT_VERSION=${PROJECT_VERSION}`. The Windows resource build is therefore
# unaffected by anything in this file.
#
# FAILURE MODE IS LOUD, NOT SILENT
#
# Source-file properties in CMake are directory-scoped. If `apply()` were called with the wrong
# directory list, the definitions would simply not reach the compiler -- and because both
# consumers use `PROJECT_VERSION` as a bare identifier in an expression, the preprocessor leaves
# it undeclared and the build fails to compile. There is no path here that produces a working
# binary carrying a wrong or empty version string.

# Remove the commit-dependent definitions from SUNSHINE_DEFINITIONS and stash them in
# SUNSHINE_VERSION_DEFINITIONS for `meow_version_definitions_apply()`.
#
# Must be called BEFORE `target_compile_definitions(sunshine ...)` and BEFORE
# `add_subdirectory(tests)`, so that both targets see the reduced list.
#
# A macro, not a function: it edits SUNSHINE_DEFINITIONS in the caller's scope, and a function
# would need PARENT_SCOPE gymnastics for no benefit.
macro(meow_version_definitions_reserve)
    set(SUNSHINE_VERSION_DEFINITIONS
            PROJECT_VERSION="${PROJECT_VERSION}"
            PROJECT_VERSION_COMMIT="${GITHUB_COMMIT}")

    # REMOVE_ITEM matches whole elements, and these are the same strings build_version.cmake
    # appended, so they match exactly. If upstream ever stops appending them the call is a
    # harmless no-op and `apply()` still defines them on the two consumers.
    list(REMOVE_ITEM SUNSHINE_DEFINITIONS ${SUNSHINE_VERSION_DEFINITIONS})
endmacro()

# Re-apply the commit-dependent definitions to their two consumers.
#
# `test_dir` is upstream's `TEST_DIR` -- `${CMAKE_SOURCE_DIR}/tests` when BUILD_TESTS is on and
# an empty string otherwise. Passing it in the DIRECTORY list mirrors what upstream already does
# for `src/upnp.cpp` and `third-party/ViGEmClient/src/ViGEmClient.cpp` a few lines below the call
# site, including the empty-string case.
#
# Must be called AFTER `add_subdirectory(tests)`, which is why upstream's own per-source
# properties live in the same region ("custom compile flags, must be after adding tests").
#
# `main.cpp` is removed from the test target's sources by `tests/CMakeLists.txt`, so in that
# directory scope this only lands on `confighttp.cpp`. Naming both files for both scopes keeps
# the two identical and costs nothing.
#
# COMPILE_DEFINITIONS REPLACES rather than appends. Neither file carries any other per-source
# definitions today -- upstream sets only COMPILE_FLAGS, and only on `upnp.cpp` and `nvhttp.cpp`
# -- so nothing is being overwritten. A future per-source definition on either file must be
# merged into this call rather than added as a second `set_source_files_properties`.
macro(meow_version_definitions_apply test_dir)
    set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/src/main.cpp"
            "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
            DIRECTORY "${CMAKE_SOURCE_DIR}" "${test_dir}"
            PROPERTIES COMPILE_DEFINITIONS "${SUNSHINE_VERSION_DEFINITIONS}")
endmacro()
