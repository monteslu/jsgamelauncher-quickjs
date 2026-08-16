cmake_minimum_required(VERSION 3.16)
project(jsgamelauncher_quickjs C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

# Reproducible builds: without this, assert()/__FILE__ bake the builder's home
# directory into the shipped binary (found in wasmcart-jsgame) and the output
# differs machine to machine for no reason.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

# GLES comes from the system on Linux and from ANGLE on macOS/Windows, which is
# exactly how native-gles resolves it — the same ANGLE binaries, downloaded by that
# project's own script, so the two cannot drift apart.
set(ANGLE_DIR ${CMAKE_SOURCE_DIR}/../native-gles/angle)

if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  pkg_check_modules(EGL REQUIRED egl)
  pkg_check_modules(GLES REQUIRED glesv2)
else()
  find_package(SDL2 REQUIRED)
  if(NOT EXISTS ${ANGLE_DIR})
    message(FATAL_ERROR
      "ANGLE not found at ${ANGLE_DIR}.\n"
      "macOS and Windows have no system GLES. Fetch it with:\n"
      "  cd ../native-gles && npm install --ignore-scripts && npm run download-angle")
  endif()
  set(EGL_INCLUDE_DIRS ${ANGLE_DIR}/include)
  set(GLES_INCLUDE_DIRS ${ANGLE_DIR}/include)
  if(APPLE)
    set(EGL_LIBRARIES ${ANGLE_DIR}/lib/libEGL.dylib)
    set(GLES_LIBRARIES ${ANGLE_DIR}/lib/libGLESv2.dylib)
  else()
    set(EGL_LIBRARIES ${ANGLE_DIR}/lib/libEGL.lib)
    set(GLES_LIBRARIES ${ANGLE_DIR}/lib/libGLESv2.lib)
  endif()
  set(SDL2_LIBRARIES SDL2::SDL2)
endif()

set(QUICKJS_DIR ${CMAKE_SOURCE_DIR}/deps/quickjs)
set(NATIVE_GLES_DIR ${CMAKE_SOURCE_DIR}/../native-gles/src)

if(NOT EXISTS ${QUICKJS_DIR}/quickjs.c)
  message(FATAL_ERROR "quickjs not found at ${QUICKJS_DIR}. Run scripts/fetch-deps.sh")
endif()
if(NOT EXISTS ${NATIVE_GLES_DIR}/gl_bindings.cpp)
  message(FATAL_ERROR "native-gles not found at ${NATIVE_GLES_DIR}")
endif()

# ---------------------------------------------------------------------- quickjs
# Built from source rather than linked as a library: we deliberately do NOT compile
# quickjs-libc.c. No os module, no std module, no fs, no process — the sandbox is
# structural, not a set of deletions a game can undo.
# Source list mirrors upstream's own `qjs_sources` (deps/quickjs/CMakeLists.txt).
# Note what is NOT here: cutils.c is header-only in 0.16.x and xsum.c does not
# exist — guessing at this list is a documented way to lose an afternoon.
add_library(quickjs STATIC
  ${QUICKJS_DIR}/quickjs.c
  ${QUICKJS_DIR}/libregexp.c
  ${QUICKJS_DIR}/libunicode.c
  ${QUICKJS_DIR}/dtoa.c
)
target_include_directories(quickjs PUBLIC ${QUICKJS_DIR})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
target_compile_options(quickjs PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable
                                       -Wno-sign-compare -Wno-array-bounds)

# ------------------------------------------------------------------ native-gles
# egl_context.cpp contains ZERO Napi and compiles verbatim.
# gl_bindings.cpp compiles verbatim too, against src/napi_shim.h via src/napi.h,
# which takes include priority over node-addon-api's header.
add_library(nativegles STATIC
  ${NATIVE_GLES_DIR}/egl_context.cpp
  ${NATIVE_GLES_DIR}/gl_bindings.cpp
)
target_include_directories(nativegles PUBLIC
  ${CMAKE_SOURCE_DIR}/src        # our napi.h must precede any real one
  ${QUICKJS_DIR}
  ${NATIVE_GLES_DIR}
  ${EGL_INCLUDE_DIRS}
  ${GLES_INCLUDE_DIRS}
)
target_compile_options(nativegles PRIVATE -Wno-unused-parameter)

# ----------------------------------------------------------------- webaudio-node
# The C++ DSP engine (16 node types + mp3/wav/flac/vorbis/aac/opus decoders) is
# compiled NATIVELY from upstream sources with ZERO edits. src/compat/ intercepts
# <emscripten.h> and <wasm_simd128.h>; see src/emscripten_compat.h for why that is
# the entire gap between "runs in V8's wasm engine" and "runs as native code".
set(WEBAUDIO_DIR ${CMAKE_SOURCE_DIR}/../webaudio-node/src)
if(EXISTS ${WEBAUDIO_DIR}/wasm/audio_graph_simple.cpp)
  # All 15 node implementations plus the graph. Globbed deliberately: upstream adds
  # node types over time and a hand-listed set would silently miss them, surfacing
  # as an undefined reference at link rather than anything actionable.
  file(GLOB WEBAUDIO_NODE_SOURCES ${WEBAUDIO_DIR}/wasm/nodes/*.cpp)
  file(GLOB WEBAUDIO_UTIL_SOURCES ${WEBAUDIO_DIR}/wasm/utils/*.cpp)
  add_library(webaudio STATIC
    ${WEBAUDIO_DIR}/wasm/audio_graph_simple.cpp
    ${CMAKE_SOURCE_DIR}/src/audio_decoders_native.cpp   # wrapper: opus stubbed
    ${WEBAUDIO_DIR}/wasm/media_stream_source.cpp
    ${WEBAUDIO_NODE_SOURCES}
    ${WEBAUDIO_UTIL_SOURCES}
  )
  target_include_directories(webaudio PUBLIC
    ${CMAKE_SOURCE_DIR}/src/compat     # must precede system headers
    ${WEBAUDIO_DIR}/wasm
    ${WEBAUDIO_DIR}
    # opus ships its headers in three places and audio_decoders.cpp includes them
    # by bare name; missing these is the documented "opus_multistream.h not found".
  )
  target_compile_options(webaudio PRIVATE
    -Wno-unused-variable -Wno-unused-function -Wno-sign-compare)
  set(HAVE_WEBAUDIO ON)
else()
  message(WARNING "webaudio-node not found at ${WEBAUDIO_DIR}; audio disabled")
  set(HAVE_WEBAUDIO OFF)
endif()

# ------------------------------------------------- generated GL binding table
# src/gl_table.inc is DERIVED from native-gles's own module.cpp, so it is generated
# rather than committed: a checked-in copy would silently drift from upstream and
# surface as a missing GL function at runtime, in whichever game happened to call it.
find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NOT NODE_EXECUTABLE)
  message(FATAL_ERROR "node is required to generate the GL binding table")
endif()

add_custom_command(
  OUTPUT ${CMAKE_SOURCE_DIR}/src/gl_table.inc
  COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-gl-table.mjs
  DEPENDS ${NATIVE_GLES_DIR}/module.cpp
          ${NATIVE_GLES_DIR}/gl_bindings.h
          ${CMAKE_SOURCE_DIR}/scripts/gen-gl-table.mjs
  COMMENT "Generating GL binding table from native-gles"
  VERBATIM
)
add_custom_target(gl_table DEPENDS ${CMAKE_SOURCE_DIR}/src/gl_table.inc)

# ------------------------------------------------------------------------- host
add_executable(jsglq
  src/main.c
  src/engine.c
  src/platform.c
  src/bind_core.c
  src/bind_io.c
  src/bind_canvas2d.c
  src/events.c
  src/bind_image.c
  src/bind_audio.c
  src/bind_worker.c
  src/bundle.c
  src/sprite2d.c
  src/bind_gl.cpp
  src/window.cpp
)
target_include_directories(jsglq PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${QUICKJS_DIR}
  ${NATIVE_GLES_DIR}
  ${SDL2_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLES_INCLUDE_DIRS}
)
if(HAVE_WEBAUDIO)
  target_compile_definitions(jsglq PRIVATE JSGLQ_HAVE_AUDIO=1)
endif()

target_link_libraries(jsglq PRIVATE
  nativegles quickjs
  $<$<BOOL:${HAVE_WEBAUDIO}>:webaudio>
  ${SDL2_LIBRARIES} ${EGL_LIBRARIES} ${GLES_LIBRARIES}
  m dl pthread
)
target_link_directories(jsglq PRIVATE ${SDL2_LIBRARY_DIRS})

add_dependencies(jsglq gl_table)

if(APPLE)
  # ANGLE ships beside the executable, so look there first at load time.
  set_target_properties(jsglq PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path")
endif()

set_target_properties(jsglq PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
