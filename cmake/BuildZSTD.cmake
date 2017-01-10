macro(zstd_build)
    add_library(zstd STATIC
        third_party/zstd/lib/common/zstd_common.c
        third_party/zstd/lib/common/entropy_common.c
        third_party/zstd/lib/common/xxhash.c
        third_party/zstd/lib/common/fse_decompress.c
        third_party/zstd/lib/decompress/zstd_decompress.c
        third_party/zstd/lib/decompress/huf_decompress.c
        third_party/zstd/lib/compress/zstd_compress.c
        third_party/zstd/lib/compress/huf_compress.c
        third_party/zstd/lib/compress/fse_compress.c
)
    set(ZSTD_LIBRARIES zstd)
    set(ZSTD_INCLUDE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/common)
    include_directories(${ZSTD_INCLUDE_DIRS})
    find_package_message(ZSTD "Using bundled ZSTD"
        "${ZSTD_LIBRARIES}:${ZSTD_INCLUDE_DIRS}")
    add_dependencies(build_bundled_libs zstd)
endmacro(zstd_build)
