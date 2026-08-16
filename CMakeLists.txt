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

if(MSVC)
  # MSVC deprecates the POSIX names (strdup, _putenv...) and defaults to treating
  # some C constructs more strictly than clang/gcc. These keep the shared sources
  # compiling without sprinkling _CRT_ ifdefs through code that is otherwise
  # perfectly portable.
  # _USE_MATH_DEFINES: the MSVC CRT hides M_PI and friends behind it. The audio
  # DSP sources use them freely, as every other toolchain defines them by default.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
                          _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/utf-8)
else()
  # Reproducible builds: without this, assert()/__FILE__ bake the builder's home
  # directory into the shipped binary and the output differs machine to machine.
  add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
endif()

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
  # Homebrew ships SDL2 with pkg-config but not always a CMake config package, and
  # vcpkg ships the config but no pkg-config. Try the config first and fall back,
  # rather than requiring one and failing on the platform that has the other.
  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL2 sdl2)
    endif()
  endif()
  if(NOT SDL2_FOUND)
    message(FATAL_ERROR
      "SDL2 not found.\n"
      "  macOS:   brew install sdl2\n"
      "  Windows: vcpkg install sdl2 (and pass the vcpkg toolchain file)")
  endif()

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
  # The config package exposes an imported target; pkg-config exposes raw flags.
  if(TARGET SDL2::SDL2)
    set(SDL2_LIBRARIES SDL2::SDL2)
  endif()
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
# Third-party sources: quiet the warnings we are not going to fix upstream.
# MSVC rejects unknown /W arguments outright (D8021) rather than ignoring them,
# so these are gcc/clang only.
if(NOT MSVC)
  target_compile_options(quickjs PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable
                                         -Wno-sign-compare -Wno-array-bounds)
else()
  target_compile_options(quickjs PRIVATE /wd4101 /wd4018 /wd4244 /wd4267)
endif()

# ------------------------------------------------------------------ native-gles
# egl_context.cpp contains ZERO Napi and compiles verbatim.
# gl_bindings.cpp compiles verbatim too, against src/napi_shim.h via src/napi.h,
# which takes include priority over node-addon-api's header.
set(NATIVE_GLES_SOURCES
  ${NATIVE_GLES_DIR}/egl_context.cpp
  ${NATIVE_GLES_DIR}/gl_bindings.cpp
)
if(APPLE)
  # egl_context.cpp calls gles_mac_* to build the CAMetalLayer ANGLE presents
  # into. Those live in an ObjC++ file that only makes sense on macOS, which is
  # why it is added per-platform rather than unconditionally.
  list(APPEND NATIVE_GLES_SOURCES ${NATIVE_GLES_DIR}/mac_native_window.mm)
endif()

add_library(nativegles STATIC ${NATIVE_GLES_SOURCES})

if(APPLE)
  # The ObjC++ file needs ARC off (native-gles manages its own retain/release)
  # and the frameworks ANGLE's Metal backend links against.
  set_source_files_properties(${NATIVE_GLES_DIR}/mac_native_window.mm
    PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
  target_link_libraries(nativegles PUBLIC
    "-framework Cocoa" "-framework QuartzCore" "-framework Foundation")
endif()
target_include_directories(nativegles PUBLIC
  ${CMAKE_SOURCE_DIR}/src        # our napi.h must precede any real one
  ${QUICKJS_DIR}
  ${NATIVE_GLES_DIR}
  ${EGL_INCLUDE_DIRS}
  ${GLES_INCLUDE_DIRS}
)
if(NOT MSVC)
  target_compile_options(nativegles PRIVATE -Wno-unused-parameter)
else()
  target_compile_options(nativegles PRIVATE /wd4100 /wd4244 /wd4267)
endif()

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
  if(NOT MSVC)
    target_compile_options(webaudio PRIVATE
      -Wno-unused-variable -Wno-unused-function -Wno-sign-compare)
  else()
    target_compile_options(webaudio PRIVATE /wd4101 /wd4505 /wd4018 /wd4244 /wd4267)
  endif()
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
)
if(WIN32)
  # winmm supplies timeBeginPeriod, which platform.c needs to get sleeps finer
  # than the default ~15ms scheduler tick. m/dl/pthread are POSIX and are folded
  # into the CRT here.
  target_link_libraries(jsglq PRIVATE winmm)
  # SDL_MAIN_HANDLED: on Windows SDL2 #defines main to SDL_main and expects to
  # supply the real entry point. We already have our own main() and a console
  # subsystem, so tell SDL to keep its hands off it.
  target_compile_definitions(jsglq PRIVATE SDL_MAIN_HANDLED=1)
else()
  target_link_libraries(jsglq PRIVATE m dl pthread)
endif()
target_link_directories(jsglq PRIVATE ${SDL2_LIBRARY_DIRS})

add_dependencies(jsglq gl_table)

if(APPLE)
  # ANGLE ships beside the executable, so look there first at load time.
  set_target_properties(jsglq PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path")
endif()

# The binary always lands in build/, on every generator. Multi-config generators
# (Visual Studio, Xcode) otherwise append the config name and produce
# build/Release/jsglq.exe, which every script that looks in build/ would miss;
# setting the per-config property explicitly pins the path everywhere.
set_target_properties(jsglq PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES} Release Debug RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} cfg_upper)
  set_target_properties(jsglq PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${CMAKE_SOURCE_DIR}/build)
endforeach()

if(WIN32)
  # Windows has no rpath: a DLL is found next to the executable or not at all.
  # vcpkg's SDL2 is a shared library and ANGLE is always shared, so without this
  # the binary builds and links and then fails to start with no diagnostic beyond
  # an exit code. Copy them beside the exe as part of the build so a developer
  # and CI both get something that actually runs.
  set(ANGLE_LIB_DIR ${NATIVE_GLES_DIR}/../angle/lib)
  file(GLOB ANGLE_DLLS ${ANGLE_LIB_DIR}/*.dll)
  foreach(dll ${ANGLE_DLLS})
    add_custom_command(TARGET jsglq POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${dll} ${CMAKE_SOURCE_DIR}/build)
  endforeach()
  # SDL2::SDL2 knows where its own DLL is; ask the target rather than guessing a
  # vcpkg path that changes with the triplet.
  if(TARGET SDL2::SDL2)
    add_custom_command(TARGET jsglq POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:SDL2::SDL2> ${CMAKE_SOURCE_DIR}/build)
  endif()
endif()
