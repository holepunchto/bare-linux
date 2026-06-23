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
# also provides the uv_* symbols the host uses. BUILD_RPATH lets the loader find
# it at runtime without LD_LIBRARY_PATH.
target_link_libraries(
  bare_linux
  PRIVATE
    ${CMAKE_SOURCE_DIR}/app/lib/libbare-kit.so
)

set_target_properties(
  bare_linux
  PROPERTIES
    BUILD_RPATH ${CMAKE_SOURCE_DIR}/app/lib
)
