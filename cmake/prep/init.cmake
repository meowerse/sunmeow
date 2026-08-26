if (WIN32)
elseif (APPLE)
    if (NOT SUNSHINE_BUILD_HOMEBREW)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
        set(CMAKE_INSTALL_RPATH "")
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
elseif (UNIX)
    include(GNUInstallDirs)

    # MEOW-TOUCH(rebrand): default to the binary we actually install, by absolute path.
    #
    # Upstream's default was the bare name `sunshine`, which systemd resolves against its own
    # search path -- and on a machine that also has the distro Sunshine package, that path hits
    # /usr/bin/sunshine first. The unit therefore launched the DISTRO package's binary,
    # silently, because that binary starts and serves just fine.
    #
    # Spelling the fixed name `sunmeow` here would fix today's symptom while keeping the
    # mechanism: resolution would still depend on search order, so a stale binary under a
    # different prefix would silently win. An absolute path cannot be captured that way, and
    # every packaging override already passes one.
    if(NOT DEFINED SUNSHINE_EXECUTABLE_PATH)
        set(SUNSHINE_EXECUTABLE_PATH "${CMAKE_INSTALL_FULL_BINDIR}/${SUNMEOW_BINARY_NAME}")
    endif()

    if(SUNSHINE_BUILD_FLATPAK)
        # MEOW-TOUCH(rebrand): --command names the binary inside the sandbox, which OUTPUT_NAME
        # renamed. `sunshine` no longer exists there, so this unit could never have started.
        set(SUNSHINE_SERVICE_START_COMMAND "ExecStart=flatpak run --command=${SUNMEOW_BINARY_NAME} ${PROJECT_FQDN}")
        set(SUNSHINE_SERVICE_STOP_COMMAND "ExecStop=flatpak kill ${PROJECT_FQDN}")
    else()
        set(SUNSHINE_SERVICE_START_COMMAND "ExecStart=${SUNSHINE_EXECUTABLE_PATH}")
        set(SUNSHINE_SERVICE_STOP_COMMAND "")
    endif()
endif()
