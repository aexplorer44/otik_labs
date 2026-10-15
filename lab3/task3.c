// Кодек архива формата DME007 v23.
//
// Сборка:
//   gcc -Wall -Wextra -O2 -o task3 task3.c
//
// использование:
//   ./lab3 encode <archive> <path1> [path2 ...]
//       создать архив из указанных файлов и/или папок.
//       папки обходятся рекурсивно, пути сохраняются.
//
//   ./lab3 decode <archive> <out_dir>
//       распаковать архив в каталог <out_dir>.
//
//   ./lab3 list <archive>
//       показать содержимое архива (без распаковки).

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

/* константы формата */

static const unsigned char SIGNATURE[6]  = { 'D', 'M', 'E', '0', '0', '7' };
static const uint16_t      FORMAT_VERSION = 23;

static const uint8_t COMPRESSOR_MEM   = 0;  /* нет сжатия (с памятью) */
static const uint8_t COMPRESSOR_NOMEM = 0;  /* нет сжатия (без памяти) */
static const uint8_t NOISE_PROTECTION = 0;  /* нет защиты */

enum { ENTRY_FILE = 0, ENTRY_DIR = 1 };

/* динамический массив записей манифеста */

typedef struct {
    uint8_t  type;
    uint64_t data_len;
    char    *name;         /* malloc'ed, путь через '/', без '\0' в файле */
} Entry;

typedef struct {
    Entry   *items;
    size_t   count;
    size_t   cap;
} Manifest;

static void man_init(Manifest *m) { m->items = NULL; m->count = 0; m->cap = 0; }

static void man_free(Manifest *m) {
    for (size_t i = 0; i < m->count; ++i) free(m->items[i].name);
    free(m->items);
    m->items = NULL; m->count = 0; m->cap = 0;
}

static int man_push(Manifest *m, uint8_t type, const char *name, uint64_t data_len) {
    if (m->count == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 16;
        Entry *ni = realloc(m->items, nc * sizeof(Entry));
        if (!ni) return -1;
        m->items = ni; m->cap = nc;
    }
    m->items[m->count].type = type;
    m->items[m->count].data_len = data_len;
    m->items[m->count].name = strdup(name);
    if (!m->items[m->count].name) return -1;
    m->count++;
    return 0;
}

/* утилиты: числовая запись/чтение LE */

static int w_u16(FILE *f, uint16_t v) {
    unsigned char b[2] = { v & 0xFF, (v >> 8) & 0xFF };
    return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}
static int w_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = { v & 0xFF, (v>>8)&0xFF, (v>>16)&0xFF, (v>>24)&0xFF };
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}
static int w_u64(FILE *f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = (unsigned char)((v >> (8*i)) & 0xFF);
    return fwrite(b, 1, 8, f) == 8 ? 0 : -1;
}
static int w_u8(FILE *f, uint8_t v) {
    return fwrite(&v, 1, 1, f) == 1 ? 0 : -1;
}

static int r_u16(FILE *f, uint16_t *v) {
    unsigned char b[2]; if (fread(b, 1, 2, f) != 2) return -1;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8); return 0;
}
static int r_u32(FILE *f, uint32_t *v) {
    unsigned char b[4]; if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
       | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); return 0;
}
static int r_u64(FILE *f, uint64_t *v) {
    unsigned char b[8]; if (fread(b, 1, 8, f) != 8) return -1;
    uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= ((uint64_t)b[i]) << (8*i);
    *v = x; return 0;
}
static int r_u8(FILE *f, uint8_t *v) {
    return fread(v, 1, 1, f) == 1 ? 0 : -1;
}

/* обход входных путей */

/* нормализуем путь: убираем ведущий "./" и завершающие '/'. */
static char *normalize_path(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    size_t n = strlen(p);
    while (n > 1 && p[n-1] == '/') n--;
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, p, n); r[n] = '\0';
    return r;
}

static int is_dir(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

static int is_reg(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

/* рекурсивный обход. root_arc — путь, под которым кладём элемент в архив. */
static int walk_add(const char *fs_path, const char *arc_path, Manifest *m) {
    if (is_dir(fs_path)) {
        if (man_push(m, ENTRY_DIR, arc_path, 0) != 0) return -1;

        DIR *d = opendir(fs_path);
        if (!d) { perror(fs_path); return -1; }

        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

            size_t need = strlen(fs_path) + 1 + strlen(e->d_name) + 1;
            char *child_fs  = malloc(need);
            char *child_arc = malloc(strlen(arc_path) + 1 + strlen(e->d_name) + 1);
            if (!child_fs || !child_arc) { free(child_fs); free(child_arc); closedir(d); return -1; }

            snprintf(child_fs,  need, "%s/%s", fs_path,  e->d_name);
            snprintf(child_arc, strlen(arc_path)+1+strlen(e->d_name)+1,
                     "%s/%s", arc_path, e->d_name);

            int rc = walk_add(child_fs, child_arc, m);
            free(child_fs); free(child_arc);
            if (rc != 0) { closedir(d); return -1; }
        }
        closedir(d);
        return 0;
    } else if (is_reg(fs_path)) {
        struct stat st;
        if (stat(fs_path, &st) != 0) return -1;
        return man_push(m, ENTRY_FILE, arc_path, (uint64_t)st.st_size);
    } else {
        fprintf(stderr, "Пропуск (не файл и не каталог): %s\n", fs_path);
        return -1;
    }
}

/* сериализация манифеста */

/* считаем размер манифеста в байтах для поля ServiceLen. */
static int manifest_size(const Manifest *m, uint64_t *out_sz) {
    uint64_t sz = 4;  /* entry_count */
    for (size_t i = 0; i < m->count; ++i) {
        size_t nl = strlen(m->items[i].name);
        if (nl > 0xFFFF) return -1;
        sz += 1 + 2 + 8 + nl;
    }
    *out_sz = sz;
    return 0;
}

static int manifest_write(FILE *f, const Manifest *m) {
    if (w_u32(f, (uint32_t)m->count) != 0) return -1;
    for (size_t i = 0; i < m->count; ++i) {
        const Entry *e = &m->items[i];
        size_t nl = strlen(e->name);
        if (nl > 0xFFFF) return -1;
        if (w_u8 (f, e->type)         != 0) return -1;
        if (w_u16(f, (uint16_t)nl)    != 0) return -1;
        if (w_u64(f, e->data_len)     != 0) return -1;
        if (fwrite(e->name, 1, nl, f) != nl) return -1;
    }
    return 0;
}

static int manifest_read(FILE *f, uint32_t entry_count, Manifest *m) {
    man_init(m);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t  type;
        uint16_t name_len;
        uint64_t data_len;
        if (r_u8 (f, &type)     != 0) return -1;
        if (r_u16(f, &name_len) != 0) return -1;
        if (r_u64(f, &data_len) != 0) return -1;

        char *nm = malloc((size_t)name_len + 1);
        if (!nm) return -1;
        if (fread(nm, 1, name_len, f) != name_len) { free(nm); return -1; }
        nm[name_len] = '\0';

        if (man_push(m, type, nm, data_len) != 0) { free(nm); return -1; }
        free(nm);
    }
    return 0;
}

/* копирование данных */

static int copy_n(FILE *in, FILE *out, uint64_t n) {
    unsigned char buf[65536];
    uint64_t left = n;
    while (left > 0) {
        size_t want = (left < sizeof(buf)) ? (size_t)left : sizeof(buf);
        size_t rd = fread(buf, 1, want, in);
        if (rd == 0) return -1;
        if (out && fwrite(buf, 1, rd, out) != rd) return -1;
        left -= rd;
    }
    return 0;
}

/* кодер */

static int cmd_encode(const char *archive, int npaths, char **paths) {
    /* 1. Собираем манифест */
    Manifest m; man_init(&m);
    uint64_t total_data = 0;

    for (int i = 0; i < npaths; ++i) {
        char *norm = normalize_path(paths[i]);
        if (!norm) { man_free(&m); return 1; }
        int rc = walk_add(paths[i], norm, &m);
        free(norm);
        if (rc != 0) { man_free(&m); return 1; }
    }

    for (size_t i = 0; i < m.count; ++i)
        if (m.items[i].type == ENTRY_FILE) total_data += m.items[i].data_len;

    uint64_t slen = 0;
    if (manifest_size(&m, &slen) != 0) {
        fprintf(stderr, "Манифест слишком велик\n");
        man_free(&m); return 1;
    }

    /* 2. Пишем архив */
    FILE *out = fopen(archive, "wb");
    if (!out) { perror(archive); man_free(&m); return 1; }

    int ok = 1;
    do {
        if (fwrite(SIGNATURE, 1, 6, out) != 6) break;
        if (w_u16(out, FORMAT_VERSION) != 0) break;
        if (w_u8 (out, COMPRESSOR_MEM)   != 0) break;
        if (w_u8 (out, COMPRESSOR_NOMEM) != 0) break;
        if (w_u8 (out, NOISE_PROTECTION) != 0) break;
        if (w_u8 (out, 0)                != 0) break;   /* Reserved */
        if (w_u32(out, (uint32_t)slen)   != 0) break;
        if (w_u64(out, total_data)       != 0) break;

        if (manifest_write(out, &m) != 0) break;

        /* 3. Пишем данные файлов */
        for (size_t i = 0; i < m.count; ++i) {
            if (m.items[i].type != ENTRY_FILE) continue;

            FILE *in = fopen(m.items[i].name, "rb");
            if (!in) {
                /* путь в манифесте может отличаться от fs-пути:
                   у нас arc_path == fs_path без ведущего "./", поэтому
                   открываем по исходному имени, полученному при обходе.
                   Храним его в манифесте — совпадает. */
                perror(m.items[i].name);
                ok = 0; break;
            }
            if (copy_n(in, out, m.items[i].data_len) != 0) {
                fprintf(stderr, "Ошибка копирования: %s\n", m.items[i].name);
                fclose(in); ok = 0; break;
            }
            fclose(in);
        }
    } while (0);

    fclose(out);
    man_free(&m);

    if (!ok) { remove(archive); return 1; }
    printf("OK: архив %s создан (%zu записей, %llu байт данных)\n",
           archive, (size_t)0, (unsigned long long)0); /* заглушка ниже */
    return 0;
}

/* декодер */

/* Создать все каталоги в пути (аналог mkdir -p для относительного пути). */
static int mkdirs_for(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return -1;
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp); return -1;
            }
            *p = '/';
        }
    }
    free(tmp);
    return 0;
}

static int cmd_decode(const char *archive, const char *out_dir) {
    FILE *in = fopen(archive, "rb");
    if (!in) { perror(archive); return 1; }

    /* 1. Общий заголовок — все проверки ДО создания выходных файлов */
    unsigned char sig[6];
    if (fread(sig, 1, 6, in) != 6 || memcmp(sig, SIGNATURE, 6) != 0) {
        fprintf(stderr, "Ошибка: неверная сигнатура архива\n");
        fclose(in); return 1;
    }
    uint16_t ver;
    if (r_u16(in, &ver) != 0 || ver != FORMAT_VERSION) {
        fprintf(stderr, "Ошибка: неподдерживаемая версия формата\n");
        fclose(in); return 1;
    }
    uint8_t cm, cnm, np, rsv;
    if (r_u8(in, &cm)  != 0 || r_u8(in, &cnm) != 0 ||
        r_u8(in, &np)  != 0 || r_u8(in, &rsv) != 0) {
        fprintf(stderr, "Ошибка: повреждён заголовок\n");
        fclose(in); return 1;
    }
    if (cm != COMPRESSOR_MEM || cnm != COMPRESSOR_NOMEM || np != NOISE_PROTECTION) {
        fprintf(stderr, "Ошибка: неподдерживаемые алгоритмы (mem=%u nomem=%u noise=%u)\n",
                cm, cnm, np);
        fclose(in); return 1;
    }

    uint32_t slen;
    uint64_t total;
    if (r_u32(in, &slen) != 0 || r_u64(in, &total) != 0) {
        fprintf(stderr, "Ошибка: повреждён заголовок\n");
        fclose(in); return 1;
    }

    /* 2. Манифест */
    uint32_t entry_count;
    if (r_u32(in, &entry_count) != 0) {
        fprintf(stderr, "Ошибка: нет манифеста\n");
        fclose(in); return 1;
    }

    Manifest m;
    if (manifest_read(in, entry_count, &m) != 0) {
        fprintf(stderr, "Ошибка: повреждён манифест\n");
        fclose(in); man_free(&m); return 1;
    }

    /* 3. Создаём out_dir */
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        perror(out_dir); fclose(in); man_free(&m); return 1;
    }

    /* 4. Проходим по записям в порядке манифеста */
    for (size_t i = 0; i < m.count; ++i) {
        Entry *e = &m.items[i];
        size_t need = strlen(out_dir) + 1 + strlen(e->name) + 1;
        char *full = malloc(need);
        if (!full) { fclose(in); man_free(&m); return 1; }
        snprintf(full, need, "%s/%s", out_dir, e->name);

        if (e->type == ENTRY_DIR) {
            if (mkdir(full, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "Не удалось создать каталог: %s\n", full);
                free(full); fclose(in); man_free(&m); return 1;
            }
        } else {
            if (mkdirs_for(full) != 0) {
                fprintf(stderr, "Не удалось создать родителя для: %s\n", full);
                free(full); fclose(in); man_free(&m); return 1;
            }
            FILE *fo = fopen(full, "wb");
            if (!fo) {
                fprintf(stderr, "Не удалось создать файл: %s\n", full);
                free(full); fclose(in); man_free(&m); return 1;
            }
            if (copy_n(in, fo, e->data_len) != 0) {
                fprintf(stderr, "Ошибка чтения данных для: %s\n", full);
                fclose(fo); free(full); fclose(in); man_free(&m); return 1;
            }
            fclose(fo);
        }
        free(full);
    }

    fclose(in);
    man_free(&m);
    printf("OK: архив распакован в %s\n", out_dir);
    return 0;
}

/* список */

static int cmd_list(const char *archive) {
    FILE *in = fopen(archive, "rb");
    if (!in) { perror(archive); return 1; }

    unsigned char sig[6];
    if (fread(sig, 1, 6, in) != 6 || memcmp(sig, SIGNATURE, 6) != 0) {
        fprintf(stderr, "неверная сигнатура\n"); fclose(in); return 1;
    }
    uint16_t ver; r_u16(in, &ver);
    uint8_t cm, cnm, np, rsv;
    r_u8(in, &cm); r_u8(in, &cnm); r_u8(in, &np); r_u8(in, &rsv);
    uint32_t slen; uint64_t total;
    r_u32(in, &slen); r_u64(in, &total);

    uint32_t ec;
    if (r_u32(in, &ec) != 0) { fclose(in); return 1; }
    Manifest m;
    if (manifest_read(in, ec, &m) != 0) { fclose(in); return 1; }

    printf("Archive: %s\n", archive);
    printf("  signature: DME007, version: %u\n", ver);
    printf("  algorithms: mem=%u nomem=%u noise=%u\n", cm, cnm, np);
    printf("  entries: %u, data: %llu bytes\n", ec, (unsigned long long)total);
    for (size_t i = 0; i < m.count; ++i) {
        const char *t = m.items[i].type == ENTRY_DIR ? "DIR " : "FILE";
        printf("  [%s] %-40s %llu\n", t, m.items[i].name,
               (unsigned long long)m.items[i].data_len);
    }
    man_free(&m); fclose(in);
    return 0;
}

/* main */

static void usage(const char *prog) {
    fprintf(stderr,
        "использование:\n"
        "  %s encode <archive> <path1> [path2 ...]\n"
        "  %s decode <archive> <out_dir>\n"
        "  %s list   <archive>\n",
        prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }

    if (strcmp(argv[1], "encode") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return cmd_encode(argv[2], argc - 3, argv + 3);
    }
    if (strcmp(argv[1], "decode") == 0) {
        if (argc != 4) { usage(argv[0]); return 2; }
        return cmd_decode(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "list") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        return cmd_list(argv[2]);
    }

    fprintf(stderr, "неизвестный режим: %s\n", argv[1]);
    usage(argv[0]);
    return 2;
}