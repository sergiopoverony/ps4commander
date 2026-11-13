#pragma once
// Проигрыватель видео (MP4/M4V/MOV и звук M4A/AAC) на системной библиотеке libSceAvPlayer.
// Библиотека подгружается во время работы (sceKernelLoadStartModule + sceKernelDlsym), структуры описаны
// вручную по документированной раскладке. Файл читается потоком из службы по HTTP Range, поэтому
// размер видео не ограничен памятью приложения. Кадры (NV12) переводятся в RGB для экрана.
#include <string>
#include "media.h"

bool video_open(const std::string& path, int userId);   // запускает сессию в фоне
void video_close();
int  video_state();                 // 0 - нет, 1 - запускается, 2 - играет, 3 - закончилось, 4 - ошибка
std::string video_error();
std::string video_note();           // кодек видеодорожки, которую плеер консоли не открыл (пусто - всё в порядке)
bool video_poll_frame();            // вызывать из главного потока: true - есть новый кадр
const Picture& video_picture();     // последний кадр (может быть пустым - звук без видео)
bool video_has_video();
void video_pause_toggle();
bool video_paused();
void video_seek(int deltaMs);       // перемотка относительно текущего места
double video_seconds();
double video_total();
