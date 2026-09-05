cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBGME_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_gme.c
)

SET(LIBGME_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --whole-archive rather than a plain link: game-music-emu keeps one descriptor
# object per emulator (gme_nsf_type, gme_spc_type, ...) and reaches them
# through a table the linker does not follow into the archive. Without this the
# side module ends up importing those data symbols from the host, and fails to
# instantiate - the filter then never registers and the graph has nothing to
# decode with.
add_filter(libgme
        "${LIBGME_SRC}"
        ""
        []
        ""
        "${LIBGME_INC}"
        "-Wl,--whole-archive ${CMAKE_CURRENT_SOURCE_DIR}/lib/libgme.a -Wl,--no-whole-archive"
        "1")
