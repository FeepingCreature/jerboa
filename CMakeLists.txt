cmake_minimum_required(VERSION 2.6)

project(jerboa)

find_package(PkgConfig)
pkg_check_modules (LIBFFI REQUIRED libffi)
pkg_check_modules (LIBXML REQUIRED libxml-2.0)

file(GLOB VM_SRC "src/vm/*.c")
set(BASE_SRC "src/object.c" "src/parser.c" "src/language.c" "src/gc.c" "src/util.c")

include_directories("${PROJECT_SOURCE_DIR}/src" "${LIBFFI_INCLUDE_DIRS}" "${LIBXML_INCLUDE_DIRS}")
add_executable(test src/test.c ${BASE_SRC} ${VM_SRC})
add_executable(repl src/repl.c ${BASE_SRC} ${VM_SRC})

target_compile_options(repl PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
target_compile_options(test PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
target_link_libraries (repl dl ffi xml2)
target_link_libraries (test dl ffi xml2)
install(TARGETS test RUNTIME DESTINATION bin)
install(TARGETS repl RUNTIME DESTINATION bin)
