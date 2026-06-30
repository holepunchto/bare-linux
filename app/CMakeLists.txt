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

# sync_hrpc/sync_schema are OBJECT libraries; objects reached only transitively
# through another OBJECT library aren't pulled into the executable, so link both
# directly. Their rpc/compact/utf deps come via rpc_static, whose static-archive
# interface does propagate.
target_link_libraries(
  bare_linux
  PRIVATE
    "${CMAKE_BINARY_DIR}/app/lib/libbare-kit.so"
    sync_hrpc
    sync_schema
    rpc_static
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

# Smoke test: pipe `off` and confirm the stdin command drives a set-state whose
# reply decodes, plus the startup `info` event. Timeout-bounded since the node
# never self-exits.
add_test(
  NAME boot
  COMMAND sh -c "out=$(printf 'off\n' | timeout 15 $<TARGET_FILE:bare_linux>); echo \"$out\" | grep -q 'set-state reply: switch is off' && echo \"$out\" | grep -q 'info: key '"
)
