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

# BUILD_RPATH lets the loader find libbare-kit.so (a link-time dependency) at
# runtime without LD_LIBRARY_PATH. The linked native addons (sodium-native,
# udx-native, ...) are different: the runtime dlopens them by soname from inside
# libbare-kit.so, so the executable's rpath does not cover them - run with
# LD_LIBRARY_PATH=app/addons/<arch>/lib instead (see README).
set_target_properties(
  bare_linux
  PROPERTIES
    BUILD_RPATH ${CMAKE_SOURCE_DIR}/app/lib
)
