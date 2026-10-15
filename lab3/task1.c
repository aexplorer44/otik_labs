// gcc -Wall -Wextra -O2 -o task1 task1.c

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// параметры формата:
static const unsigned char SIGNATURE[6] = { 'L', 'A', 'B', '3', '0', '1' };
static const uint16_t      FORMAT_VERSION = 0;   /* для ЛЗ-1 — 0 */ 

// запись заголовка в архив (little-endian для чисел)
static int write_header(FILE *out, uint64_t original_len) {
    if (fwrite(SIGNATURE, 1, 6, out) != 6) return -1;

    // uint16 little-endian
    unsigned char v[2] = {
        (unsigned char)(FORMAT_VERSION & 0xFF),
        (unsigned char)((FORMAT_VERSION >> 8) & 0xFF)
    };
    if (fwrite(v, 1, 2, out) != 2) return -1;

    // uint64 little-endian
    unsigned char n[8];
    for (int i = 0; i < 8; ++i)
        n[i] = (unsigned char)((original_len >> (8 * i)) & 0xFF);
    if (fwrite(n, 1, 8, out) != 8) return -1;

    return 0;
}

// кодер
static int encode(const char *in_path, const char *archive_path) {
    FILE *in = fopen(in_path, "rb"); /* чтение бинарно */
    if (!in) { perror("fopen input"); return 1; }

    // узнаем длину входного файла: переход на последнюю позицию, rewind - возврат и сброс флага ошибки
    if (fseek(in, 0, SEEK_END) != 0) { perror("fseek"); fclose(in); return 1; }
    long sz = ftell(in);
    if (sz < 0) { perror("ftell"); fclose(in); return 1; }
    rewind(in);

    FILE *out = fopen(archive_path, "wb"); /* запись бинарно */
    if (!out) { perror("fopen archive"); fclose(in); return 1; }

    if (write_header(out, (uint64_t)sz) != 0) {
        fprintf(stderr, "Ошибка записи заголовка\n");
        fclose(in); fclose(out); return 1;
    }

    // побайтовое копирование
    unsigned char buf[65536];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), in)) > 0) /* по одному байту до конца буфера */
    {
        if (fwrite(buf, 1, rd, out) != rd) {
            fprintf(stderr, "Ошибка записи данных\n");
            fclose(in); fclose(out); return 1;
        }
    }
    if (ferror(in)) { perror("fread"); fclose(in); fclose(out); return 1; }

    fclose(in);
    fclose(out);
    return 0;
}

// декодер
static int decode(const char *archive_path, const char *out_path) {
    FILE *in = fopen(archive_path, "rb");
    if (!in) { perror("fopen archive"); return 1; }

    // 1) сигнатура
    unsigned char sig[6];
    if (fread(sig, 1, 6, in) != 6) {
        fprintf(stderr, "архив слишком короткий (нет сигнатуры)\n");
        fclose(in); return 1;
    }
    if (memcmp(sig, SIGNATURE, 6) != 0) /* побайтовое сравнение */
    {
        fprintf(stderr, "неверная сигнатура архива\n");
        fclose(in); return 1;
    }
    // пока мы не убедились, что это наш архив, не открываем выходной файл

    // 2) версия
    unsigned char v[2];
    if (fread(v, 1, 2, in) != 2) {
        fprintf(stderr, "архив повреждён (нет версии)\n");
        fclose(in); return 1;
    }
    uint16_t version = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
    if (version != FORMAT_VERSION) {
        fprintf(stderr, "неподдерживаемая версия формата: %u\n", version);
        fclose(in); return 1;
    }

    // 3) длина
    unsigned char n[8];
    if (fread(n, 1, 8, in) != 8) {
        fprintf(stderr, "архив повреждён (нет длины)\n");
        fclose(in); return 1;
    }
    uint64_t original_len = 0;
    for (int i = 0; i < 8; ++i)
        original_len |= ((uint64_t)n[i]) << (8 * i);

    // теперь открываем входной файл
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen output"); fclose(in); return 1; }

    // 4) данные
    unsigned char buf[65536];
    uint64_t left = original_len;
    while (left > 0) {
        size_t want = (left < sizeof(buf)) ? (size_t)left : sizeof(buf); 
        // сколько читаем за раз: либо остаток left, либо целый буфер
        size_t rd = fread(buf, 1, want, in);
        if (rd == 0) {
            fprintf(stderr, "неожиданный конец архива\n");
            fclose(in); fclose(out); return 1;
        }
        if (fwrite(buf, 1, rd, out) != rd) {
            fprintf(stderr, "ошибка записи выходного файла\n");
            fclose(in); fclose(out); return 1;
        }
        left -= rd;
    }

    fclose(in);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) /* 4: имя программы, команда, исходный файл, файл выхода*/
    {
        fprintf(stderr,
            "как использовать:\n"
            "  %s encode <входной_файл> <архив>\n"
            "  %s decode <архив> <выходной_файл>\n",
            argv[0], argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode") == 0)
        return encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0)
        return decode(argv[2], argv[3]);

    fprintf(stderr, "неизвестный режим: %s\n", argv[1]);
    return 2;
}