if(UNIX)
    if(${SUNSHINE_CONFIGURE_HOMEBREW})
        configure_file(packaging/sunshine.rb sunshine.rb @ONLY)
    endif()
endif()

if(APPLE)
    if(${SUNSHINE_CONFIGURE_PORTFILE})
        configure_file(packaging/macos/Portfile Portfile @ONLY)
    endif()
elseif(UNIX)
    # configure the .desktop file
    set(SUNSHINE_DESKTOP_ICON "${PROJECT_FQDN}")
    if(${SUNSHINE_BUILD_APPIMAGE})
        configure_file(packaging/linux/AppImage/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
    elseif(${SUNSHINE_BUILD_FLATPAK})
        configure_file(packaging/linux/flatpak/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
    else()
        configure_file(packaging/linux/${PROJECT_FQDN}.desktop ${PROJECT_FQDN}.desktop @ONLY)
        configure_file(packaging/linux/${PROJECT_FQDN}.terminal.desktop ${PROJECT_FQDN}.terminal.desktop @ONLY)
    endif()

    # configure metadata file
    configure_file(packaging/linux/${PROJECT_FQDN}.metainfo.xml ${PROJECT_FQDN}.metainfo.xml @ONLY)

    # configure service
    configure_file(packaging/linux/app-${PROJECT_FQDN}.service.in app-${PROJECT_FQDN}.service @ONLY)

    # MEOW-TOUCH(rebrand): fail the configure rather than ship a unit that starts the distro
    # package's binary. That bug shipped once and was invisible from the outside -- the unit
    # was valid, the service came up, and the only symptom was that our config directory
    # stayed empty. A generated artifact is the only place the substituted result can be
    # checked, so it is checked here instead of trusted.
    file(READ "${CMAKE_CURRENT_BINARY_DIR}/app-${PROJECT_FQDN}.service" _meow_unit)
    if(NOT _meow_unit MATCHES "ExecStart=[^\n]*${SUNMEOW_BINARY_NAME}")
        message(FATAL_ERROR
                "Generated systemd unit does not start ${SUNMEOW_BINARY_NAME}. "
                "SUNSHINE_EXECUTABLE_PATH is '${SUNSHINE_EXECUTABLE_PATH}'.")
    endif()
    # Anchored to a line start: the file documents the old `Alias=sunshine.service` in a
    # comment on purpose, and an unanchored match cannot tell that apart from a live directive.
    #
    # `sunshine` must also be the directive's terminal token -- followed by a space (the flatpak
    # `--command=sunshine <fqdn>` form), a dot (`sunshine.service`), or the end of the line.
    # Matching it anywhere would fail the configure on a perfectly correct unit whose install
    # prefix merely contains the word, e.g. `ExecStart=/usr/share/sunshine/bin/sunmeow` -- which
    # cmake/packaging/unix.cmake can still produce when CMAKE_INSTALL_PREFIX is empty.
    if(_meow_unit MATCHES "\n(ExecStart|ExecStop|Alias)=[^\n]*sunshine[ .\n]")
        message(FATAL_ERROR
                "Generated systemd unit still names 'sunshine' in a directive. "
                "That is the distro package's binary and unit name, not ours.")
    endif()
    unset(_meow_unit)

    # configure kwin desktop permission file
    if (${SUNSHINE_ENABLE_KWIN})
        configure_file(packaging/linux/${PROJECT_FQDN}.kwin.desktop.in ${PROJECT_FQDN}.kwin.desktop @ONLY)
    endif()

    # configure the arch linux pkgbuild
    if(${SUNSHINE_CONFIGURE_PKGBUILD})
        configure_file(packaging/linux/Arch/PKGBUILD PKGBUILD @ONLY)
        configure_file(packaging/linux/Arch/sunshine.install sunshine.install @ONLY)
    endif()

    # configure the flatpak manifest
    if(${SUNSHINE_CONFIGURE_FLATPAK_MAN})
        configure_file(packaging/linux/flatpak/${PROJECT_FQDN}.yml ${PROJECT_FQDN}.yml @ONLY)
        file(COPY packaging/linux/flatpak/deps/ DESTINATION ${CMAKE_BINARY_DIR})
        file(COPY packaging/linux/flatpak/modules DESTINATION ${CMAKE_BINARY_DIR})
        file(COPY generated-sources.json DESTINATION ${CMAKE_BINARY_DIR})
        file(COPY package-lock.json DESTINATION ${CMAKE_BINARY_DIR})
    endif()
endif()

# return if configure only is set
if(${SUNSHINE_CONFIGURE_ONLY})
    # message
    message(STATUS "SUNSHINE_CONFIGURE_ONLY: ON, exiting...")
    set(END_BUILD ON)
else()
    set(END_BUILD OFF)
endif()
