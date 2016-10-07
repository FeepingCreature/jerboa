#!/bin/sh
set -e
echo "---- building interpreter ----"
make -C build
echo "---- building win32 executable ----"
./build-win32.sh
echo "---- generating 32-bit bindings ----"
export CFLAGS="-m32"
./genlibs.sh >/dev/null

echo "---- creating release tree ----"
RELEASE=jerboa-win32-$(git describe --always)
rm "$RELEASE".zip || true
mkdir "$RELEASE"
cp build/jerboa.exe "$RELEASE"
cp -L *.dll "$RELEASE"
cp -R c/ "$RELEASE"
cp -R std/ "$RELEASE"
cp {game,geom,minheap,sched,sound,swig_xml_to_c,xmltools}.jb "$RELEASE"
zip -r "$RELEASE".zip "$RELEASE"
rm -rf "$RELEASE"

echo "---- regenerating default bindings ----"
export CFLAGS=
./genlibs.sh >/dev/null
