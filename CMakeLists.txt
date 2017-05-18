cmake_minimum_required(VERSION 2.6)

project(jerboa)

set( ENABLE_JIT false CACHE BOOL "Use LibJIT to flatten VM instructions." )

find_package(PkgConfig)

if (WIN32)
  set(CMAKE_EXE_LINKER_FLAGS "-static -mwindows")
  set(ICUUC "sicuuc")
  set(EXTRA_LIBS "stdc++" "z" "sicudt" "ws2_32" "shlwapi" "iconv" "readline" "ncurses")
else()
  set(ICUUC "icuuc")
  set(EXTRA_LIBS "dl")
endif()

set(FLAGS "-g" "-O0" "-Wall" "-Wno-maybe-uninitialized" "-Werror" "-pedantic" "-std=c11" "-D_POSIX_C_SOURCE=200809L" "-D_GNU_SOURCE")
pkg_check_modules (LIBICU REQUIRED icu-uc)
pkg_check_modules (LIBFFI REQUIRED libffi)
pkg_check_modules (LIBXML REQUIRED libxml-2.0)

find_path(LIBFFI_INCLUDE_DIR
  NAMES "ffi.h"
  PATHS "${LIBFFI_INCLUDE_DIRS}"
)

find_path(LIBXML_INCLUDE_DIR
  NAMES "libxml/parser.h"
  PATHS "${LIBXML_INCLUDE_DIRS}"
)

file(GLOB VM_SRC "src/vm/*.c")

if (ENABLE_JIT)
   set(FLAGS "${FLAGS}" "-DENABLE_JIT")
else()
  foreach(FILE ${VM_SRC})
    if("${FILE}" MATCHES "src/vm/myjit.c" OR "${FILE}" MATCHES "src/vm/myjit-lib.c")
      list(REMOVE_ITEM VM_SRC "${FILE}")
    endif()
  endforeach()
endif()

set(BASE_SRC "src/hash.c" "src/object.c" "src/parser.c" "src/print.c" "src/language.c"
             "src/gc.c" "src/util.c" "src/trie.c" "src/win32_compat.c" "src/static_keys.c")

include_directories("${PROJECT_SOURCE_DIR}/src" "${LIBFFI_INCLUDE_DIR}" "${LIBXML_INCLUDE_DIR}")
add_executable(jerboa src/jerboa.c ${BASE_SRC} ${VM_SRC})
add_executable(repl src/repl.c ${BASE_SRC} ${VM_SRC})

target_compile_options(jerboa PRIVATE ${FLAGS})
target_compile_options(repl PRIVATE ${FLAGS})
target_link_libraries (jerboa ffi ${ICUUC} xml2 m ${EXTRA_LIBS})
target_link_libraries (repl ffi ${ICUUC} xml2 m ${EXTRA_LIBS})
install(TARGETS jerboa RUNTIME DESTINATION bin)
install(TARGETS repl RUNTIME DESTINATION bin)
install(DIRECTORY c DESTINATION share/jerboa)
install(FILES game.jb geom.jb minheap.jb sched.jb sound.jb swig_xml_to_c.jb xmltools.jb
  DESTINATION share/jerboa)

enable_testing()
add_subdirectory( tests )

# CPack
include (InstallRequiredSystemLibraries)
set (CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set (CPACK_PACKAGE_VERSION_MAJOR "0")
set (CPACK_PACKAGE_VERSION_MINOR "1")
include (CPack)
