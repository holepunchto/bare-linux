add_executable(bare_linux main.c)

set_target_properties(bare_linux PROPERTIES C_STANDARD 11)

# `.` for the vendored forward-declaration header (bare-kit.h); bare-headers
# supplies the real <uv.h>.
resolve_node_module(bare-headers bare_headers WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

target_include_directories(
  bare_linux
  PRIVATE
    .
    "${bare_headers}/include"
)

target_link_libraries(
  bare_linux
  PRIVATE
    "${CMAKE_BINARY_DIR}/app/lib/libbare-kit.so"
)

# Resolve libbare-kit.so and the dlopened native addons from ./lib next to the
# binary. The runtime dlopens the addons from inside libbare-kit.so, and dlopen
# does not consult DT_RUNPATH - so force the old DT_RPATH tag, which it does.
set_target_properties(
  bare_linux
  PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH ON
)

target_link_options(bare_linux PRIVATE -Wl,--disable-new-dtags)

target_compile_definitions(bare_linux PRIVATE BUNDLE_PATH="${CMAKE_BINARY_DIR}/app/app.bundle")

add_dependencies(bare_linux app_bundle)

# Smoke test: boot the host and confirm it emits an IPC frame. The node has no
# self-exit, so bound it with a timeout; the frame is the pass signal.
add_test(
  NAME boot
  COMMAND sh -c "timeout 15 $<TARGET_FILE:bare_linux> | grep -q 'ipc frame'"
)
