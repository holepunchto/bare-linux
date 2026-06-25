add_executable(bare_linux main.c)

set_target_properties(bare_linux PROPERTIES C_STANDARD 11)

# `.` for the vendored forward-declaration header (bare-kit.h); bare-headers
# supplies the real <uv.h> (and <bare.h> etc. in later phases).
target_include_directories(
  bare_linux
  PRIVATE
    .
    ${CMAKE_SOURCE_DIR}/node_modules/bare-headers/include
)

# The prebuilt libbare-kit.so (fetched into app/lib) bundles bare + libuv, so it
# also provides the uv_* symbols the host uses.
target_link_libraries(
  bare_linux
  PRIVATE
    ${CMAKE_SOURCE_DIR}/app/lib/libbare-kit.so
)

# Let the loader find libbare-kit.so (a link-time dependency) and the linked
# native addons (sodium-native, udx-native, ...) at runtime without
# LD_LIBRARY_PATH. The addons are dlopened by soname from inside libbare-kit.so,
# and dlopen only searches DT_RPATH (not the modern DT_RUNPATH), so we force the
# old tag with --disable-new-dtags.
set_target_properties(
  bare_linux
  PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/app/lib;${CMAKE_SOURCE_DIR}/app/addons/lib"
)

target_link_options(bare_linux PRIVATE -Wl,--disable-new-dtags)
