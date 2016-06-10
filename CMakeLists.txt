cmake_minimum_required(VERSION 2.6)

project(jerboa)

include_directories("${PROJECT_SOURCE_DIR}")
add_executable(test object.c parser.c language.c test.c gc.c vm/builder.c vm/call.c vm/dump.c vm/runtime.c vm/optimize.c)

target_compile_options(test PRIVATE "-g" "-Wall" "-Werror" "-pedantic" "-std=c99")
# target_link_libraries (test "libjemalloc.so")
install(TARGETS test RUNTIME DESTINATION bin)
