#!/bin/bash
set -e
SOURCES=$(echo src/vm/*.c src/{gc,hash,language,object,parser,util,print}.c src/jerboa.c)
cat $SOURCES > all.c
SOURCES=all.c
FFI_FLAGS=$(pkg-config libffi --cflags --libs)
XML_FLAGS=$(pkg-config libxml-2.0 --cflags --libs)
# OPTFLAGS="-O3 -ffast-math -march=native -flto -fwhole-program -fno-omit-frame-pointer"
# OPTFLAGS="-O3 -ffast-math -march=native -flto -fwhole-program"
OPTFLAGS="-O2 -ffast-math -march=native -fno-omit-frame-pointer"
gcc -ljemalloc -DNDEBUG -D_GNU_SOURCE -g $OPTFLAGS $@ -std=c11 -Wall -pedantic -Isrc $FFI_FLAGS $XML_FLAGS $SOURCES -o build/jerboa -ldl
# clang -ljemalloc -DNDEBUG -D_GNU_SOURCE -g -Ofast $@ -std=c11 -Isrc -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -pedantic $FFI_FLAGS $XML_FLAGS $SOURCES -o build/jerboa -ldl
