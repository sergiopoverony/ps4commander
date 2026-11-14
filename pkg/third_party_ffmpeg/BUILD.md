# FFmpeg для PS4 Commander

FFmpeg n7.1.1 (LGPL-2.1+, без GPL/nonfree), статические библиотеки libavutil, libavcodec, libavformat, libswscale, libswresample.
Собраны кросс-компиляцией clang (`--target=x86_64-pc-freebsd12-elf`) против заголовков musl из OpenOrbis; с x86-ассемблером (nasm), без сети, без потоков ОС кроме pthread.
Исходники FFmpeg: https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz . Согласно LGPL библиотеки подключены статически вместе с исходниками объектов по запросу; пользователь может пересобрать их и перелинковать приложение.
