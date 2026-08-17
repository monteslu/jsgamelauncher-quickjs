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
  # CMAKE_MSVC_RUNTIME_LIBRARY is honoured only under policy CMP0091; without
  # NEW it is silently ignored and the flag below does nothing at all.
  cmake_policy(SET CMP0091 NEW)

  # Link the C runtime statically. Otherwise the binary imports MSVCP140.dll and
  # VCRUNTIME140*.dll, which are the Visual C++ redistributable — present on the
  # build runner but NOT guaranteed on a clean Windows install, so a downloaded
  # archive would fail to start with a missing-DLL dialog. This is the same
  # "works on the build machine" trap that shipped a broken macOS 0.1.0.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
  # SDL_net gives cross-platform TCP without a Winsock/BSD split of our own.
  # Optional: a build without it simply has no WebSocket, which the runtime
  # reports honestly rather than pretending to connect.
  # The module is spelled SDL2_net on Debian/Ubuntu and sdl2_net elsewhere;
  # checking one spelling silently disables WebSocket on the other.
  pkg_check_modules(SDL2_NET SDL2_net)
  if(NOT SDL2_NET_FOUND)
    pkg_check_modules(SDL2_NET sdl2_net)
  endif()
  # mbedTLS for https:// and wss://. Optional in the same way SDL_net is: without
  # it the build still works, TLS URLs throw by name, and nothing pretends to be
  # encrypted.
  pkg_check_modules(MBEDTLS mbedtls)
  if(MBEDTLS_FOUND)
    # mbedtls.pc advertises only -lmbedtls, but the library is split in three and
    # the linker needs all of them: without this the build fails at link with
    # "libmbedx509.so: DSO missing from command line".
    find_library(MBEDX509_LIB mbedx509)
    find_library(MBEDCRYPTO_LIB mbedcrypto)
    if(MBEDX509_LIB AND MBEDCRYPTO_LIB)
      list(APPEND MBEDTLS_LIBRARIES ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
    endif()
  endif()
  if(NOT MBEDTLS_FOUND)
    # mbedTLS 3.x ships no pkg-config in some distros; fall back to finding the
    # three libraries it splits into.
    find_library(MBEDTLS_LIB mbedtls)
    find_library(MBEDX509_LIB mbedx509)
    find_library(MBEDCRYPTO_LIB mbedcrypto)
    find_path(MBEDTLS_INC mbedtls/ssl.h)
    if(MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB AND MBEDTLS_INC)
      set(MBEDTLS_FOUND TRUE)
      set(MBEDTLS_INCLUDE_DIRS ${MBEDTLS_INC})
      set(MBEDTLS_LIBRARIES ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
    endif()
  endif()
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

  # SDL_net, discovered the same two ways for the same reason as SDL2 itself.
  find_package(SDL2_net CONFIG QUIET)
  if(SDL2_net_FOUND)
    set(SDL2_NET_FOUND TRUE)
    if(TARGET SDL2_net::SDL2_net)
      set(SDL2_NET_LIBRARIES SDL2_net::SDL2_net)
    elseif(TARGET SDL2_net::SDL2_net-static)
      set(SDL2_NET_LIBRARIES SDL2_net::SDL2_net-static)
    endif()
  elseif(PkgConfig_FOUND)
    pkg_check_modules(SDL2_NET sdl2_net)
  endif()

  # mbedTLS, same two-way discovery. vcpkg ships a CMake config; Homebrew ships
  # pkg-config.
  find_package(MbedTLS CONFIG QUIET)
  if(MbedTLS_FOUND)
    set(MBEDTLS_FOUND TRUE)
    set(MBEDTLS_LIBRARIES MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  elseif(PkgConfig_FOUND)
    pkg_check_modules(MBEDTLS mbedtls)
    if(NOT MBEDTLS_FOUND)
      find_library(MBEDTLS_LIB mbedtls)
      find_library(MBEDX509_LIB mbedx509)
      find_library(MBEDCRYPTO_LIB mbedcrypto)
      find_path(MBEDTLS_INC mbedtls/ssl.h)
      if(MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB AND MBEDTLS_INC)
        set(MBEDTLS_FOUND TRUE)
        set(MBEDTLS_INCLUDE_DIRS ${MBEDTLS_INC})
        set(MBEDTLS_LIBRARIES ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
      endif()
    endif()
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
  src/bind_gamepad.c
  src/bind_websocket.c
  src/net_tls.c
  src/bind_http.c
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
if(MBEDTLS_FOUND)
  target_compile_definitions(jsglq PRIVATE JSGLQ_HAVE_TLS=1)
  target_include_directories(jsglq PRIVATE ${MBEDTLS_INCLUDE_DIRS})
  target_link_libraries(jsglq PRIVATE ${MBEDTLS_LIBRARIES})
  target_link_directories(jsglq PRIVATE ${MBEDTLS_LIBRARY_DIRS})
else()
  message(WARNING
    "mbedTLS not found; https:// and wss:// will be unavailable.\n"
    "  Linux:   apt install libmbedtls-dev\n"
    "  macOS:   brew install mbedtls\n"
    "  Windows: vcpkg install mbedtls")
endif()

if(SDL2_NET_FOUND)
  target_compile_definitions(jsglq PRIVATE JSGLQ_HAVE_WEBSOCKET=1)
  target_include_directories(jsglq PRIVATE ${SDL2_NET_INCLUDE_DIRS})
  target_link_libraries(jsglq PRIVATE ${SDL2_NET_LIBRARIES})
  target_link_directories(jsglq PRIVATE ${SDL2_NET_LIBRARY_DIRS})
else()
  message(WARNING
    "SDL2_net not found; WebSocket will be unavailable in this build.\n"
    "  Linux:   apt install libsdl2-net-dev\n"
    "  macOS:   brew install sdl2_net\n"
    "  Windows: vcpkg install sdl2-net")
endif()

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
  #
  # BUILD_WITH_INSTALL_RPATH stops CMake appending every link directory as an
  # rpath. Those absolute paths (Homebrew's prefix, and the CI runner's own
  # working directory) end up baked into the shipped binary, which both leaks
  # the build machine's layout and makes the binary depend on it.
  set_target_properties(jsglq PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
    BUILD_WITH_INSTALL_RPATH TRUE
    MACOSX_RPATH TRUE)
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
  # vcpkg path that changes with the triplet. Only shared builds have a runtime
  # DLL to copy; a static SDL2 needs nothing here.
  if(TARGET SDL2::SDL2)
    get_target_property(_sdl2_type SDL2::SDL2 TYPE)
    if(_sdl2_type STREQUAL "SHARED_LIBRARY")
      add_custom_command(TARGET jsglq POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> ${CMAKE_SOURCE_DIR}/build)
    endif()
  endif()
endif()

if(APPLE)
  # Same idea as the Windows block: put ANGLE beside the binary at build time so
  # build/ holds everything the binary needs. Packaging then copies from build/,
  # which is the directory the binary was actually tested in. SDL2 is handled
  # separately by scripts/bundle-macos-libs.mjs, which must also rewrite its
  # install name (Homebrew bakes in an absolute path).
  set(ANGLE_LIB_DIR ${NATIVE_GLES_DIR}/../angle/lib)
  file(GLOB ANGLE_DYLIBS ${ANGLE_LIB_DIR}/*.dylib)
  foreach(dylib ${ANGLE_DYLIBS})
    add_custom_command(TARGET jsglq POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${dylib} ${CMAKE_SOURCE_DIR}/build)
  endforeach()
endif()
