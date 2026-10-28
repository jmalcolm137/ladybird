# The Ladybird engine host used by the LibGUI frontend. It is a normal Ladybird
# UI process without a native window: a LibGUI process drives it over a socket.
add_executable(LadybirdEngine
    EngineApplication.cpp
    EngineServer.cpp
    main.cpp
)

add_dependencies(LadybirdEngine ladybird_build_resource_files ${ladybird_helper_processes})
target_link_libraries(LadybirdEngine PRIVATE
    AK
    LibCore
    LibGfx
    LibIPC
    LibJS
    LibMain
    LibRequests
    LibURL
    LibWakeLock
    LibWeb
    LibWebView
)

target_link_libraries(LadybirdEngine PRIVATE OpenSSL::Crypto OpenSSL::SSL)

target_include_directories(LadybirdEngine PRIVATE ${LADYBIRD_SOURCE_DIR})

# Helpers live in libexec, and helper path discovery starts from the executable's
# directory. Keeping the host there lets it find WebContent without an installed
# prefix. The LibGUI chrome looks for it in the same place.
set_target_properties(LadybirdEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_LIBEXECDIR}"
)

install(TARGETS LadybirdEngine
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
    COMPONENT ladybird_Runtime
)
