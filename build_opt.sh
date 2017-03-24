#!/bin/bash
set -e
SOURCES=$(echo src/vm/*.c src/{gc,hash,language,object,parser,util,print,trie,win32_compat,static_keys}.c src/jerboa.c)
cat $SOURCES > all.c
SOURCES=all.c
FLAGS="-DNDEBUG -std=c11 -D_GNU_SOURCE -DENABLE_JIT -g -Wall -Isrc -o build/jerboa -ljemalloc -ldl -lm -licuuc"
FFI_FLAGS=$(pkg-config libffi --cflags --libs)
XML_FLAGS=$(pkg-config libxml-2.0 --cflags --libs)
FLAGS="${FLAGS} ${FFI_FLAGS} ${XML_FLAGS}"
CLANG_WARNINGS="-Wextra -pedantic -Wno-unused-parameter -Wno-missing-field-initializers"
# OPTFLAGS="-Ofast -march=native -flto -fno-omit-frame-pointer"
# avx switching is slow with llvm??
OPTFLAGS="-Ofast -march=native -mno-avx -flto"
# OPTFLAGS="-Ofast -march=native -flto -fno-omit-frame-pointer"
# OPTFLAGS="-O2 -ffast-math -march=native -fno-omit-frame-pointer"
# gcc $FLAGS $OPTFLAGS $@ -pedantic -fwhole-program $SOURCES
clang $FLAGS $OPTFLAGS $@ -fvisibility=hidden $CLANG_WARNINGS $SOURCES
# clang $FLAGS -Ofast $@ $CLANG_WARNINGS -fno-omit-frame-pointer $SOURCES
