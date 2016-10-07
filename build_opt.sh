#!/bin/bash
set -e
SOURCES=$(echo src/vm/*.c src/{gc,hash,language,object,parser,util,print,trie,win32_compat}.c src/jerboa.c)
cat $SOURCES > all.c
SOURCES=all.c
FLAGS="-DNDEBUG -std=c11 -D_GNU_SOURCE -g -Wall -Isrc -o build/jerboa -ljemalloc -ldl -lm -licuuc"
FFI_FLAGS=$(pkg-config libffi --cflags --libs)
XML_FLAGS=$(pkg-config libxml-2.0 --cflags --libs)
FLAGS="${FLAGS} ${FFI_FLAGS} ${XML_FLAGS}"
OPTFLAGS="-O3 -ffast-math -march=native -flto -fwhole-program"
# OPTFLAGS="-O3 -ffast-math -march=native -flto -fwhole-program -fno-omit-frame-pointer"
# OPTFLAGS="-O2 -ffast-math -march=native -fno-omit-frame-pointer"
# gcc $FLAGS $OPTFLAGS $@ -pedantic $SOURCES
clang $FLAGS -Ofast $@ -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pedantic -fno-omit-frame-pointer $SOURCES
