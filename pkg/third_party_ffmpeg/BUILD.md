# FFmpeg и dav1d для PS4 Commander

FFmpeg n7.1.1 (LGPL-2.1+, без GPL/nonfree): libavutil, libavcodec, libavformat, libswscale, libswresample - все встроенные декодеры, демультиплексоры и парсеры (кодирования и сети нет).
dav1d 1.5.1 (BSD-2-Clause, декодер AV1, с x86-ассемблером) - `libdav1d.a`, подключён в libavcodec.
Собрано кросс-компиляцией clang (`--target=x86_64-pc-freebsd12-elf`) против заголовков musl из OpenOrbis; скрипт: `build_ffmpeg.sh` (порядок: сначала dav1d через meson с кросс-файлом, затем FFmpeg).
Исходники: https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz , https://code.videolan.org/videolan/dav1d (mirror: https://github.com/videolan/dav1d). По LGPL библиотеки подключены статически; пользователь может пересобрать их и перелинковать приложение.
