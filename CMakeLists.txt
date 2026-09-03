cmake_minimum_required(VERSION 3.24)
# `CMAKE_CUDA_ARCHITECTURES` requires 3.18
# `set_source_files_properties` requires 3.18
# `cmake_path(CONVERT ... TO_NATIVE_PATH_LIST ...)` requires 3.20
# `third-party/libvirtualhid` requires 3.24
# todo - set this conditionally

# MEOW-TOUCH(rebrand): the product name a user actually sees -- window titles, the tray, the
# web UI, the startup log line, and the Windows resource block. Upstream's `Sunshine` is
# indistinguishable from the distro package a user may already be running, which is exactly the
# confusion this fork has to avoid. Internal identifiers are deliberately NOT renamed: the CMake
# target stays `sunshine`, the C++ namespaces stay, and `src/` keeps its layout -- same reasoning
# as moonmeow keeping the `com.limelight` namespace while changing its applicationId.
project(Sunmeow VERSION 0.0.0
        DESCRIPTION "Self-hosted desktop stream host for Moonmeow"
        HOMEPAGE_URL "https://github.com/meowerse/sunmeow")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

set(PROJECT_LICENSE "GPL-3.0-only")

# MEOW-TOUCH(rebrand): drives the systemd unit name, the .desktop/.metainfo filenames, the
# installed icon name and the XDG portal application id. Sharing upstream's FQDN meant sharing
# the distro package's systemd unit name byte-for-byte -- installing this fork would have
# collided with it, and portal notifications could not tell them apart.
set(PROJECT_FQDN "meow.alxnko.sunmeow")

# MEOW-TOUCH(rebrand): the one place the installed executable is named. Everything that
# builds it, installs it, or merely *refers* to it reads this. The rebrand originally spelled
# `sunmeow` in one place and left `sunshine` in the six that only name it, which is how the
# generated unit came to launch the distro package's binary -- silently, because that binary
# runs. One variable makes that class of mistake unrepresentable rather than merely fixed.
set(SUNMEOW_BINARY_NAME "sunmeow")

set(PROJECT_BRIEF_DESCRIPTION "GameStream host for Moonmeow")  # must be <= 35 characters

set(PROJECT_LONG_DESCRIPTION "Offering low latency, cloud gaming server capabilities with support for AMD, Intel, \
and Nvidia GPUs for hardware encoding. Software encoding is also available. You can connect to Sunmeow from any \
Moonmeow client on a variety of devices. A web UI is provided to allow configuration, and client pairing, from \
your favorite web browser. Pair from the local server or any mobile device.")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(FREEBSD ON)
endif()

# set the module path, used for includes
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# CPM
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpm/CPM.cmake)
CPMUsePackageLock(package-lock.cmake)

# export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set version info for this build
include(${CMAKE_MODULE_PATH}/prep/build_version.cmake)

# cmake build flags
include(${CMAKE_MODULE_PATH}/prep/options.cmake)
include(${CMAKE_MODULE_PATH}/prep/static_qt.cmake)

# initial prep
include(${CMAKE_MODULE_PATH}/prep/init.cmake)

# configure special package files, such as sunshine.desktop, Flatpak manifest, Portfile , etc.
include(${CMAKE_MODULE_PATH}/prep/special_package_configuration.cmake)

# Exit early if END_BUILD is ON, i.e. when only generating package manifests
if(${END_BUILD})
    return()
endif()

# project constants
include(${CMAKE_MODULE_PATH}/prep/constants.cmake)

# load macros
include(${CMAKE_MODULE_PATH}/macros/common.cmake)

# load dependencies
include(${CMAKE_MODULE_PATH}/dependencies/common.cmake)

# setup compile definitions
include(${CMAKE_MODULE_PATH}/compile_definitions/common.cmake)

# target definitions
include(${CMAKE_MODULE_PATH}/targets/common.cmake)

# packaging
include(${CMAKE_MODULE_PATH}/packaging/common.cmake)
