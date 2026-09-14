cmake_minimum_required(VERSION 3.22)
project(archaic)
set(CMAKE_C_STANDARD 23)

# cmake -S . -B build-asan -DARCHAIC_SANITIZE=address|undefined|thread
# then: cmake --build build-asan && ./build-asan/archaic-unit
set(ARCHAIC_SANITIZE "" CACHE STRING "Sanitizer to build with (address, undefined, thread)")
if(ARCHAIC_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
elseif(ARCHAIC_SANITIZE STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
elseif(ARCHAIC_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(NOT ARCHAIC_SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unknown ARCHAIC_SANITIZE value: ${ARCHAIC_SANITIZE}")
endif()

find_package(fmt REQUIRED)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SYSTEMD libsystemd)
endif()

add_executable(
    archaic
    main.c
    src/trie.c
    src/trie-storage.c
    src/lru.c
    src/io/fileloader.c
    src/scanner.c
    src/ignore-file.c
    src/hashset.c
    ipc/server.c
    ipc/client.c
    src/threadpool.c
    src/metrics.c
    src/config.c
    src/cache.c
    src/log.c
    src/recent-files.c
    src/watcher.c
    src/path-utils.c
)

target_link_libraries(archaic PRIVATE fmt::fmt m pthread)
target_compile_options(archaic PRIVATE -Wall -Wpedantic -Wextra
    $<$<CONFIG:Release>:-O3 -fno-math-errno>)

if(SYSTEMD_FOUND)
    target_compile_definitions(archaic PRIVATE HAVE_SYSTEMD)
    target_link_libraries(archaic PRIVATE ${SYSTEMD_LIBRARIES})
    target_include_directories(archaic PRIVATE ${SYSTEMD_INCLUDE_DIRS})
endif()

add_executable(
    archaic-cli
    ipc/test_client.c
    ipc/client.c
    src/config.c
)

target_include_directories(archaic-cli PRIVATE ipc ${CMAKE_SOURCE_DIR})
target_link_libraries(archaic-cli PRIVATE m pthread)
target_compile_options(archaic-cli PRIVATE -Wall -Wpedantic -Wextra)

add_executable(
    archaic-helper
    src/helper.c
    src/config.c
)

target_include_directories(archaic-helper PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(archaic-helper PRIVATE m pthread)
target_compile_options(archaic-helper PRIVATE -Wall -Wpedantic -Wextra)

add_executable(
    archaic-bench
    test/bench.c
    src/trie.c
)

target_include_directories(archaic-bench PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(archaic-bench PRIVATE m pthread)
target_compile_options(archaic-bench PRIVATE -Wall -Wpedantic -Wextra
    $<$<CONFIG:Release>:-O3 -fno-math-errno>)

add_executable(
    archaic-bench-e2e
    test/bench-e2e.c
    ipc/client.c
    src/config.c
)

target_include_directories(archaic-bench-e2e PRIVATE ipc ${CMAKE_SOURCE_DIR})
target_link_libraries(archaic-bench-e2e PRIVATE m pthread)
target_compile_options(archaic-bench-e2e PRIVATE -Wall -Wpedantic -Wextra
    $<$<CONFIG:Release>:-O3 -fno-math-errno>)

# ── Install rules ─────────────────────────────────────────────────────────────

# Install binaries
install(TARGETS archaic archaic-cli archaic-helper archaic-bench archaic-bench-e2e
    RUNTIME DESTINATION bin
)

# Install Fish plugin
install(FILES fish/archaic.fish
    DESTINATION share/fish/vendor_conf.d
)

# Install Bash completion
install(FILES bash/archaic.bash
    DESTINATION share/bash-completion/completions
    RENAME archaic
)

# Install example config
install(FILES config.example.toml
    DESTINATION etc/archaic
    RENAME config.toml
)

# Install systemd service
install(FILES archaic.service
    DESTINATION lib/systemd/system
)
install(FILES archaic-user.service
    DESTINATION lib/systemd/user
)

# Install man pages
install(FILES man/archaic.1 man/archaic-cli.1
    DESTINATION share/man/man1
)
install(FILES man/archaic.conf.5
    DESTINATION share/man/man5
)

# ── Unit test target ──────────────────────────────────────────────────────────
add_executable(
    archaic-unit
    test/unit_tests.c
    src/trie.c
    src/trie-storage.c
    src/lru.c
    src/io/fileloader.c
    src/scanner.c
    src/ignore-file.c
    src/hashset.c
    src/threadpool.c
    src/metrics.c
    src/config.c
    src/cache.c
    src/log.c
    src/recent-files.c
    src/watcher.c
    src/path-utils.c
    ipc/server.c
    ipc/client.c
)

target_include_directories(archaic-unit PRIVATE ipc ${CMAKE_SOURCE_DIR})
target_link_libraries(archaic-unit PRIVATE fmt::fmt m pthread)
target_compile_options(archaic-unit PRIVATE -Wall -Wpedantic -Wextra)

# ── Uninstall target ──────────────────────────────────────────────────────────
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake"
        IMMEDIATE @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake
    )
endif()
