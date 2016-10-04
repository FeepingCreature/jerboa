cmake_minimum_required(VERSION 2.6)

project(jerboa)

set(FLAGS "-g" "-O0" "-Wall" "-Werror" "-pedantic" "-std=c11" "-D_POSIX_C_SOURCE=200809L" "-D_GNU_SOURCE")

find_package(PkgConfig)
pkg_check_modules (LIBICU REQUIRED icu-uc)
pkg_check_modules (LIBFFI REQUIRED libffi)
pkg_check_modules (LIBXML REQUIRED libxml-2.0)

file(GLOB VM_SRC "src/vm/*.c")
set(BASE_SRC "src/hash.c" "src/object.c" "src/parser.c" "src/print.c" "src/language.c" "src/gc.c" "src/util.c" "src/trie.c")

include_directories("${PROJECT_SOURCE_DIR}/src" "${LIBFFI_INCLUDE_DIRS}" "${LIBXML_INCLUDE_DIRS}")
add_executable(jerboa src/jerboa.c ${BASE_SRC} ${VM_SRC})
add_executable(repl src/repl.c ${BASE_SRC} ${VM_SRC})

target_compile_options(jerboa PRIVATE ${FLAGS})
target_compile_options(repl PRIVATE ${FLAGS})
target_link_libraries (jerboa dl ffi icuuc xml2 m)
target_link_libraries (repl dl ffi icuuc xml2 m)
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
