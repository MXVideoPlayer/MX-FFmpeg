# Automatically generated by configure - do not modify!
shared=no
build_suffix=
prefix=/usr/local
libdir=${prefix}/lib
incdir=${prefix}/include
rpath=
source_path=.
LIBPREF=lib
LIBSUF=.a
extralibs_avutil="-pthread -lm"
extralibs_avcodec="-pthread -lm -lz -lmxutil -lmxutil -lmxutil -lmxutil"
extralibs_avformat="-lm -lmxutil -lz -lmxutil -lmxutil"
extralibs_avdevice="-lm"
extralibs_avfilter="-pthread -lm"
extralibs_avresample="-lm"
extralibs_postproc="-lm"
extralibs_swscale="-lm"
extralibs_swresample="-lm"
avdevice_deps="avformat avcodec swresample avutil"
avfilter_deps="swscale avutil"
swscale_deps="avutil"
postproc_deps="avutil"
avformat_deps="avcodec swresample avutil"
avcodec_deps="swresample avutil"
swresample_deps="avutil"
avresample_deps="avutil"
avutil_deps=""
