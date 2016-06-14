cmake_minimum_required(VERSION 2.6)

project(jerboa)

find_package(PkgConfig)
pkg_check_modules (LIBFFI REQUIRED libffi)

include_directories("${PROJECT_SOURCE_DIR}" "${LIBFFI_INCLUDE_DIRS}")
add_executable(test object.c parser.c language.c test.c gc.c util.c vm/builder.c vm/call.c vm/dump.c vm/runtime.c vm/optimize.c)

target_compile_options(test PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
target_link_libraries (test dl ffi)
install(TARGETS test RUNTIME DESTINATION bin)
