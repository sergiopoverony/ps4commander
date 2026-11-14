#pragma once
// Универсальный проигрыватель на FFmpeg (libavformat/libavcodec/libswscale/libswresample): запасной путь для того,
// что системный плеер консоли не умеет (MKV/WebM/AVI/TS/FLV, фрагментированный MP4 с перемоткой, HEVC/VP9/MPEG-4 и т.д.).
// Чтение файла - через службу (Range-запросы), звук - через AudioOut, синхронизация по звуку.
#include <string>
#include "media.h"

struct AvEngine;

AvEngine* ave_start(const std::string& path, std::string& err);     // открывает файл и запускает потоки; nullptr - не получилось
void ave_stop(AvEngine* e);                                         // останавливает потоки и освобождает всё
int ave_state(AvEngine* e);                                         // 1 - загрузка, 2 - играет, 3 - закончилось, 4 - ошибка
bool ave_has_video(AvEngine* e);
std::string ave_info(AvEngine* e);                                  // описание потоков для лога/экрана
void ave_pause(AvEngine* e, bool paused);
void ave_seek(AvEngine* e, long long deltaMs);                      // относительно текущей позиции
long long ave_time_ms(AvEngine* e);
long long ave_total_ms(AvEngine* e);                                // 0 - неизвестно
bool ave_seekable(AvEngine* e);
bool ave_poll(AvEngine* e, Picture& pic);                           // true - забран новый кадр
bool ave_slow(AvEngine* e);                                      // декодер систематически не успевает
