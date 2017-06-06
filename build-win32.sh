#!/bin/bash
set -e
SOURCES=$(echo src/vm/*.c src/{gc,hash,language,object,parser,util,print,trie,win32_compat}.c src/jerboa.c rdparse/src/*.c)
FLAGS="-DNDEBUG -std=c11 -D_GNU_SOURCE -g -Wall -Irdparse/include -Isrc -o build/jerboa.exe -lm -lxml2 -static -static-libstdc++ -static-libgcc"
FFI_FLAGS=$(i686-w64-mingw32-pkg-config libffi --cflags --libs)
XML_FLAGS=$(i686-w64-mingw32-pkg-config libxml-2.0 --cflags --libs)
LIBS="$FFI_FLAGS $XML_FLAGS -lreadline -lncurses -liconv -lws2_32 -lz -lsicuuc -lshlwapi"
i686-w64-mingw32-gcc $FLAGS -O3 -ffast-math -march=native -flto -fno-omit-frame-pointer -pedantic $SOURCES $LIBS
i686-w64-mingw32-strip build/jerboa.exe
# upx --best build/jerboa.exe
