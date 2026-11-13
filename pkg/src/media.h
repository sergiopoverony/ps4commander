#pragma once
// Просмотр картинок и проигрывание музыки в PKG-приложении.
// Картинки: PNG/JPG/BMP/GIF (stb_image). Музыка: WAV (PCM), OGG Vorbis (stb_vorbis), MP3 (minimp3).
// Файл целиком лежит в памяти (скачан из службы), звук декодируется и выводится в отдельном потоке.
#include <string>
#include <vector>
#include <stdint.h>

// Готовая к показу картинка (вписана в maxW x maxH), пиксели 0x00RRGGBB.
struct Picture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};
bool picture_decode(const std::string& data, int maxW, int maxH, Picture& out);

// Звук.
bool audio_start(std::string& data, const std::string& ext, int userId);   // запускает в фоне; false - не удалось стартовать
void audio_stop();                 // останавливает и ждёт завершения потока
void audio_pause_toggle();
bool audio_paused();
int  audio_state();                // 0 - нет, 1 - запускается, 2 - играет, 3 - закончилось, 4 - ошибка
double audio_seconds();            // сколько проиграно
double audio_total();              // длительность, 0 - неизвестна
