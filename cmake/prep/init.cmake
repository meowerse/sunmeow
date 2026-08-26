if (WIN32)
elseif (APPLE)
    if (NOT SUNSHINE_BUILD_HOMEBREW)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
        set(CMAKE_INSTALL_RPATH "")
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
elseif (UNIX)
    include(GNUInstallDirs)

    # MEOW-TOUCH(rebrand): default to the name we actually install. `OUTPUT_NAME sunmeow`
    # (cmake/targets/common.cmake) renames the binary, but this path -- which becomes the
    # unit's `ExecStart` below -- kept upstream's `sunshine`. The unit therefore launched the
    # DISTRO package's binary, silently, because that binary starts and serves just fine.
    if(NOT DEFINED SUNSHINE_EXECUTABLE_PATH)
        set(SUNSHINE_EXECUTABLE_PATH "sunmeow")
    endif()

    if(SUNSHINE_BUILD_FLATPAK)
        set(SUNSHINE_SERVICE_START_COMMAND "ExecStart=flatpak run --command=sunshine ${PROJECT_FQDN}")
        set(SUNSHINE_SERVICE_STOP_COMMAND "ExecStop=flatpak kill ${PROJECT_FQDN}")
    else()
        set(SUNSHINE_SERVICE_START_COMMAND "ExecStart=${SUNSHINE_EXECUTABLE_PATH}")
        set(SUNSHINE_SERVICE_STOP_COMMAND "")
    endif()
endif()
