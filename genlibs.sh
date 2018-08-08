#!/bin/sh
set -e
mkdir -p c
FLAGS="-x _fpstate -x sigcontext -x _libc -x __pthread -x mcontext_t"
./swigify.sh $FLAGS -p SDL_ /usr/include/SDL/SDL.h libSDL.so c/sdl.jb $(pkg-config sdl --cflags --libs)
./swigify.sh $FLAGS -p gl -p GL_ -p GL /usr/include/GL/gl.h libGL.so c/gl.jb
./swigify.sh $FLAGS -p glu c/glu.h libGLU.so c/glu.jb -I. -I/usr/include
./swigify.sh $FLAGS -p glfw -p GLFW_ /usr/include/GLFW/glfw3.h libglfw.so c/glfw3.jb
./swigify.sh $FLAGS -p FT_ c/freetype.h libfreetype.so c/freetype.jb $(pkg-config freetype2 --cflags --libs) -I.
./swigify.sh $FLAGS -p hb_ -p HB_ /usr/include/harfbuzz/hb.h libharfbuzz.so c/harfbuzz.jb $(pkg-config harfbuzz --cflags --libs)
./swigify.sh $FLAGS -p hb_ -p HB_ c/harfbuzz_ft.h libharfbuzz.so c/harfbuzz_ft.jb $(pkg-config harfbuzz --cflags --libs) $(pkg-config freetype2 --cflags --libs) -I.
./swigify.sh $FLAGS -p cairo_ -p CAIRO_ /usr/include/cairo/cairo.h libcairo.so c/cairo.jb
./swigify.sh $FLAGS -x G_ -i g_object_unref -x g_ -x cairo_ -p pango_ -p PANGO_ /usr/include/pango*/pango/pangocairo.h libpangocairo-1.0.so c/pangocairo.jb $(pkg-config pango --cflags --libs) $(pkg-config cairo --cflags --libs)
./swigify.sh $FLAGS -p glfwq -p GLFWQ_ -p GLFWQ -x gl -x GL -x PFNGL /usr/include/glfwq.h libglfwq.so c/glfwq.jb
./swigify.sh $FLAGS -p sf /usr/include/SFML/Audio.h libcsfml-audio.so c/sfml-audio.jb -cpperraswarn -I/usr/include/ -DsfInt64=int64_t
./swigify.sh $FLAGS -p sf /usr/include/SFML/System.h libcsfml-system.so c/sfml-system.jb -cpperraswarn -I/usr/include/ -DsfInt64=int64_t
./swigify.sh $FLAGS -p png_ -p PNG_ /usr/include/png.h libpng.so c/png.jb -cpperraswarn
./swigify.sh $FLAGS -p SOIL_ /usr/include/SOIL.h libSOIL.so c/soil.jb
./swigify.sh $FLAGS /usr/include/time.h librt.so c/time.jb
./swigify.sh $FLAGS -x __ /usr/include/stdio.h libc.so.6 c/stdio.jb
./swigify.sh $FLAGS -x __ /usr/include/stdlib.h libc.so.6 c/stdlib.jb
./swigify.sh $FLAGS -x __ /usr/include/string.h libc.so.6 c/string.jb
