#!/bin/bash
# Сборка FFmpeg для PS4 (OpenOrbis) на Linux-хосте: configure запускается обычным gcc, затем config.mak/config.h правятся под
# кросс-компиляцию clang против заголовков musl из OpenOrbis. Нужны: clang/llvm, nasm, исходники FFmpeg n7.1.1 (/tmp/ffm),
# заголовки musl (/tmp/openorbis_musl), каталог /tmp/muslinc с bits/alltypes.h, dav1d (кросс: /tmp/dav_inst, хост для configure: /tmp/dav_host_inst).
set -e
B=/tmp/ffnew
rm -rf $B && mkdir $B && cd $B
export PKG_CONFIG_PATH=/tmp/dav_host_inst/lib/pkgconfig
/tmp/ffm/configure --prefix=/tmp/ffinst --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-debug --disable-iconv --disable-zlib --disable-bzlib \
  --disable-lzma --disable-sdl2 --disable-xlib --disable-vaapi --disable-vdpau --disable-encoders --disable-muxers \
  --disable-hwaccels --disable-indevs --disable-outdevs --disable-protocols --enable-protocol=file \
  --enable-pic --enable-pthreads --enable-swscale --enable-swresample --enable-avformat --enable-avcodec --enable-avutil \
  --enable-libdav1d --disable-static --enable-static >/dev/null
# правки под кросс-сборку: то, что отличалось в уже проверенной сборке
OLD=/tmp/ffbuild
python3 - <<'P'
import re
new='/tmp/ffnew/'
old='/tmp/ffbuild/'
ref='/tmp/ffold/'
def lines(p): return open(p).read().split('\n')
# config.h: значения HAVE_* берём из рабочей сборки
rw={}
for a,b in zip(lines(ref+'config.h'),lines(old+'config.h')):
    if a!=b:
        m=re.match(r'#define (\w+) (\d+)',b)
        if m: rw[m.group(1)]=m.group(2)
s=open(new+'config.h').read()
for k,v in rw.items(): s=re.sub(r'#define %s \d+'%k,'#define %s %s'%(k,v),s)
open(new+'config.h','w').write(s)
m=open(new+'ffbuild/config.mak').read()
for key in ['CC','AS','AR','RANLIB','CPPFLAGS','CFLAGS']:
    nv=[l for l in lines(old+'ffbuild/config.mak') if l.startswith(key+'=')][-1]
    m=re.sub(r'^%s=.*$'%key,lambda _:nv,m,flags=re.M)
open(new+'ffbuild/config.mak','w').write(m)
P
sed -i 's|^CFLAGS=|CFLAGS=-I/tmp/dav_inst/include |' ffbuild/config.mak
make -j8 libavutil/libavutil.a libavcodec/libavcodec.a libavformat/libavformat.a libswscale/libswscale.a libswresample/libswresample.a 2>&1 | tail -5
