cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-bare-bundle REQUIRED PATHS node_modules/cmake-bare-bundle)

project(bare_linux C)

enable_testing()

bare_arch(arch)

# The prebuilt bare-kit runtime (libbare-kit.so) for this arch. The release zip
# extracts to linux/<arch>/libbare-kit.so.
fetch_package("https://github.com/holepunchto/bare-kit/releases/download/v2.3.0/prebuilds.zip" SOURCE_DIR barekit)

# Link the worklet's native addons (sodium-native, udx-native, ...) for this
# host into app/lib next to where the binary lands, so an $ORIGIN rpath resolves
# them. There is no CMake module for bare-link yet, so drive it directly; it
# auto-detects the host arch and writes lib/*.so.
find_program(node NAMES node REQUIRED)

execute_process(
  COMMAND "${node}" node_modules/.bin/bare-link --out "${CMAKE_BINARY_DIR}/app" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE bare_link_result
)

if(NOT bare_link_result EQUAL 0)
  message(FATAL_ERROR "bare-link failed (${bare_link_result})")
endif()

# Drop libbare-kit.so into the same lib directory (after bare-link, which owns
# that output dir).
file(COPY "${barekit}/linux/${arch}/libbare-kit.so" DESTINATION "${CMAKE_BINARY_DIR}/app/lib")

# Pack the worklet bundle from bare-switch-core's backend.
add_bare_bundle(app_bundle
  ENTRY node_modules/bare-switch-core/backend.js
  OUT "${CMAKE_BINARY_DIR}/app/app.bundle"
  LINKED
)

# Typed RPC: librpc provides the `rpc` target and transitively fetches libcompact.
# Pinned to a short hash (native libraries aren't release-tagged).
fetch_package("github:holepunchto/librpc#38deb71")

resolve_node_module(bare-switch-core switch_core WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# schema before hrpc: hrpc's sync_schema target must exist first.
add_subdirectory("${switch_core}/spec/schema" "${CMAKE_BINARY_DIR}/spec/schema")
add_subdirectory("${switch_core}/spec/hrpc" "${CMAKE_BINARY_DIR}/spec/hrpc")

add_subdirectory(app)
