
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
    test/test.c
)

target_link_libraries(archaic PRIVATE fmt::fmt)
target_compile_options(archaic PRIVATE -Wall -Wpedantic -Wextra)
