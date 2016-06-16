cmake_minimum_required(VERSION 2.6)

project(jerboa)

find_package(PkgConfig)
pkg_check_modules (LIBFFI REQUIRED libffi)

include_directories("${PROJECT_SOURCE_DIR}" "${LIBFFI_INCLUDE_DIRS}")
add_executable(test test.c object.c parser.c language.c gc.c util.c vm/ffi.c vm/builder.c vm/call.c vm/dump.c vm/runtime.c vm/optimize.c)
add_executable(repl repl.c object.c parser.c language.c gc.c util.c vm/ffi.c vm/builder.c vm/call.c vm/dump.c vm/runtime.c vm/optimize.c)

target_compile_options(repl PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
target_compile_options(test PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
target_link_libraries (repl dl ffi)
target_link_libraries (test dl ffi)
install(TARGETS test RUNTIME DESTINATION bin)
install(TARGETS repl RUNTIME DESTINATION bin)
