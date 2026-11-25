cmake_minimum_required(VERSION 3.20)

project(c_demo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_path(XXHASH_INCLUDE_DIR NAMES xxhash.h REQUIRED)
find_library(XXHASH_LIBRARY NAMES xxhash REQUIRED)
find_path(SODIUM_INCLUDE_DIR NAMES sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium REQUIRED)
find_path(LZO_INCLUDE_DIR NAMES lzo/lzo1x.h REQUIRED)
find_library(LZO_LIBRARY NAMES lzo2 REQUIRED)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)

add_library(btrfs_checksum STATIC src/checksum.c)
target_include_directories(btrfs_checksum PUBLIC ${CMAKE_SOURCE_DIR}/hdr
    PRIVATE ${XXHASH_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR})
target_link_libraries(btrfs_checksum PUBLIC Threads::Threads
    ${XXHASH_LIBRARY} ${SODIUM_LIBRARY})
target_compile_options(btrfs_checksum PRIVATE -Wall -Wextra -Wpedantic)

add_library(btrfs_compression STATIC src/compression.c)
target_include_directories(btrfs_compression PUBLIC ${CMAKE_SOURCE_DIR}/hdr
    PRIVATE ${LZO_INCLUDE_DIR} ${ZSTD_INCLUDE_DIR})
target_link_libraries(btrfs_compression PUBLIC ZLIB::ZLIB
    ${LZO_LIBRARY} ${ZSTD_LIBRARY})
target_compile_options(btrfs_compression PRIVATE -Wall -Wextra -Wpedantic)

add_executable(c_demo
    src/main.c
    src/candidates.c
    src/cli.c
    src/report.c
    src/init.c
    src/layout.c
    src/csum_index.c
    src/chunk_tree.c
    src/root_tree.c
    src/file_tree.c
    src/RAID/base.c
    src/RAID/raid10.c
    src/RAID/single.c
    src/RAID/dup.c
    src/RAID/raid0.c
    src/RAID/raid1.c
)

target_include_directories(c_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/hdr
)

target_link_libraries(c_demo PRIVATE Threads::Threads btrfs_checksum btrfs_compression)

target_compile_options(c_demo PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
