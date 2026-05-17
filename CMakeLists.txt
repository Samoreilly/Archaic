project(archaic)
cmake_minimum_required(VERSION 4.3)
set(CMAKE_C_STANDARD 23)

find_package(fmt)

add_executable(
    archaic
    main.c
    src/trie.c
    src/trie-storage.c
    src/lru.c
    src/io/fileloader.c
    src/scanner.c
    test/test.c
    test/perf.c
    ipc/server.c
    ipc/client.c
    src/threadpool.c
    src/metrics.c
)

target_link_libraries(archaic PRIVATE fmt::fmt m pthread)
target_compile_options(archaic PRIVATE -Wall -Wpedantic -Wextra)

add_executable(
    archaic-cli
    ipc/test_client.c
    ipc/client.c
)

target_include_directories(archaic-cli PRIVATE ipc)
target_link_libraries(archaic-cli PRIVATE m pthread)
target_compile_options(archaic-cli PRIVATE -Wall -Wpedantic -Wextra)
