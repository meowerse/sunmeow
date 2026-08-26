#
# Loads the boost library giving the priority to the system package first, with a fallback to FetchContent.
#
include_guard(GLOBAL)

# MEOW-TOUCH(cmake-deps): 1.89.0 -> 1.92.0. This is not currency for its own sake. The
# `find_package(... EXACT)` below means a system Boost that is not this exact version is
# REJECTED, and the FetchContent fallback then compiles Boost from source on every clean
# configure. Arch ships 1.92.0, so on the reference machine (and in CI, whose container is
# Arch) upstream's 1.89.0 pin guaranteed a from-source build every time. Matching the system
# version skips it entirely. Components used are filesystem/log/program_options/system/locale
# -- all stable across these minors, and notably NOT boost::process, which is where Boost's
# API churn lives.
set(BOOST_VERSION "1.92.0")
set(BOOST_COMPONENTS
        filesystem
        log
        program_options
        system
)
# system is not used by Sunshine, but by Simple-Web-Server, added here for convenience

if(NOT WIN32)
    list(APPEND BOOST_COMPONENTS locale)
endif()

# algorithm, preprocessor, scope, and uuid are not used by Sunshine, but by libdisplaydevice, added here for convenience
if(WIN32)
    list(APPEND BOOST_COMPONENTS
            algorithm
            preprocessor
            scope
            uuid
    )
endif()

if(BOOST_USE_STATIC)
    set(Boost_USE_STATIC_LIBS ON)  # cmake-lint: disable=C0103
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    cmake_policy(SET CMP0167 NEW)  # Get BoostConfig.cmake from upstream
endif()
find_package(Boost CONFIG ${BOOST_VERSION} EXACT COMPONENTS ${BOOST_COMPONENTS})
if(NOT Boost_FOUND)
    message(STATUS "Boost v${BOOST_VERSION} package not found in the system. Falling back to FetchContent.")
    include(FetchContent)

    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        cmake_policy(SET CMP0135 NEW)  # Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24
    endif()
    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.0")
        cmake_policy(SET CMP0174 NEW)  # Handle empty variables
    endif()

    # more components required for compiling boost targets
    list(APPEND BOOST_COMPONENTS
            asio
            crc
            format
            process
            property_tree)

    set(BOOST_ENABLE_CMAKE ON)

    # Limit boost to the required libraries only
    set(BOOST_INCLUDE_LIBRARIES ${BOOST_COMPONENTS})
    set(BOOST_URL "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}-cmake.tar.xz")  # cmake-lint: disable=C0301
    set(BOOST_HASH "SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d")

    if(CMAKE_VERSION VERSION_LESS "3.24.0")
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
        )
    elseif(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.25.0")
        # add SYSTEM to FetchContent_Declare, this fails on debian bookworm
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
                SYSTEM  # requires CMake 3.25+
                OVERRIDE_FIND_PACKAGE  # requires CMake 3.24+, but we have a macro to handle it for other versions
        )
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
                OVERRIDE_FIND_PACKAGE  # requires CMake 3.24+, but we have a macro to handle it for other versions
        )
    endif()

    FetchContent_MakeAvailable(Boost)
    set(FETCH_CONTENT_BOOST_USED TRUE)

    set(Boost_FOUND TRUE)  # cmake-lint: disable=C0103
    set(Boost_INCLUDE_DIRS  # cmake-lint: disable=C0103
            "$<BUILD_INTERFACE:${Boost_SOURCE_DIR}/libs/headers/include>")

    if(WIN32)
        # Windows build is failing to create .h file in this directory
        file(MAKE_DIRECTORY ${Boost_BINARY_DIR}/libs/log/src/windows)
    endif()

    set(Boost_LIBRARIES "")  # cmake-lint: disable=C0103
    foreach(component ${BOOST_COMPONENTS})
        list(APPEND Boost_LIBRARIES "Boost::${component}")
    endforeach()
endif()

message(STATUS "Boost include dirs: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
