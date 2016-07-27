#!/bin/sh
set -e
mkdir -p c
./swigify.sh -p SDL_ /usr/include/SDL/SDL.h libSDL.so c/sdl.jb $(pkg-config sdl --cflags --libs)
./swigify.sh -p gl -p GL_ /usr/include/GL/gl.h libGL.so c/gl.jb
./swigify.sh -p glu c/glu.h libGLU.so c/glu.jb -I. -I/usr/include
./swigify.sh -p glfw -p GLFW_ /usr/include/GLFW/glfw3.h libglfw.so c/glfw3.jb
./swigify.sh -p FT_ c/freetype.h libfreetype.so c/freetype.jb $(pkg-config freetype2 --cflags --libs) -I.
./swigify.sh -p hb_ -p HB_ /usr/include/harfbuzz/hb.h libharfbuzz.so c/harfbuzz.jb $(pkg-config harfbuzz --cflags --libs)
./swigify.sh -p hb_ -p HB_ c/harfbuzz_ft.h libharfbuzz.so c/harfbuzz_ft.jb $(pkg-config harfbuzz --cflags --libs) $(pkg-config freetype2 --cflags --libs) -I.
./swigify.sh -p cairo_ -p CAIRO_ /usr/include/cairo/cairo.h libcairo.so c/cairo.jb
./swigify.sh -p pango_ -p PANGO_ /usr/include/pango*/pango/pangocairo.h libpangocairo-1.0.so c/pangocairo.jb $(pkg-config pango --cflags --libs) $(pkg-config cairo --cflags --libs)
./swigify.sh -p glfwq -p GLFWQ_ -p GLFWQ -x gl -x GL -x PFNGL /usr/include/glfwq.h libglfwq.so c/glfwq.jb
