#!/bin/sh
set -e
echo "---- building interpreter ----"
make -C build
echo "---- building executable ----"
./build_opt.sh

echo "---- creating release tree ----"
RELEASE=jerboa-$(git describe --always)
rm "$RELEASE".tar.xz || true
mkdir "$RELEASE"
# TODO share with build-win32-dist.sh
cp build/jerboa "$RELEASE"
cp -R c/ "$RELEASE"
cp -R std/ "$RELEASE"
cp {game,geom,minheap,sched,sound,swig_xml_to_c,xmltools}.jb "$RELEASE"
tar cfJ "$RELEASE".tar.xz "$RELEASE"
rm -rf "$RELEASE"
