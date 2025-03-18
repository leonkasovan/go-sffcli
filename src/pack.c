#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#ifdef __WIN32__
#define SEP '\\'
#else
#define SEP '/'
#endif

/* meta info formats */
enum { JSON, XML, TXT, SEXPR, CHDR, TNG };
const char* exts[] = { ".json", ".xml", ".txt", ".se", ".h", ".txt" };

/* command line arguments */
int width = 0, height = 0, unpack = 0, crop = 0, inpcrop = 1, fmt = JSON, tofile = 0;
char* comment = NULL;

/* internal variables */
typedef struct {
    char* name;
    int X, Y, W, H;
    uint8_t* data;
} meta_t;
char full[8192];
meta_t* files = NULL;
struct stbrp_rect* rects = NULL;
int num = 0, maxw = 0, maxh = 0;
int64_t prod = 0;

/**
 * Recursively add image files to list
 */
void find(char* name, int lvl) {
    DIR* dir;
    struct dirent* ent;
    int i, l, x, y, w, h;
    uint8_t* p;
    char* com, * s, * d, * e;

    if (!name || !*name || lvl > 8191 - 256) return;
    if ((dir = opendir(name)) != NULL) {
        if (name != full) { strcpy(full, name); lvl = strlen(full); }
        if (lvl > 0 && full[lvl - 1] != SEP) full[lvl++] = SEP;
        full[lvl] = 0;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            strcpy(full + lvl, ent->d_name);
            find(full, lvl + strlen(ent->d_name));
        }
        closedir(dir);
    } else {
        if ((p = image_load(name, &w, &h)) && w > 0 && h > 0) {
            i = num++;
            files = (meta_t*) realloc(files, num * sizeof(meta_t));
            rects = (struct stbrp_rect*) realloc(rects, num * sizeof(struct stbrp_rect));
            if (!files || !rects) { fprintf(stderr, "Not enough memory\r\n"); exit(1); }
            memset(&files[i], 0, sizeof(meta_t));
            l = strlen(name);
            files[i].name = (char*) malloc(l + 1);
            if (!files[i].name) { fprintf(stderr, "Not enough memory\r\n"); exit(1); }
            e = strrchr(name, '.'); if (!e) e = name + l;
            for (s = name, d = files[i].name; s < e && *s; s++)
                *d++ = *s == '/' || *s == '\\' || *s == ':' || *s == '\"' || *s == ' ' || *s == '\t' ||
                *s == '\n' || *s == '\r' ? '_' : *s;
            *d = 0;
            files[i].data = p;
            files[i].W = w; files[i].H = h;
            if (w > maxw) maxw = w;
            if (h > maxh) maxh = h;
            prod += (int64_t) w * (int64_t) h;
            /* crop input sprite to content */
            if (inpcrop) {
                for (y = 0; y < files[i].H && h > 0; y++) {
                    for (x = 0; x < files[i].W && !p[(y * files[i].W + x) * 4 + 3]; x++);
                    if (x < files[i].W) break;
                    files[i].Y++; h--;
                }
                for (y = files[i].H - 1; y >= files[i].Y && h > 0; y--) {
                    for (x = 0; x < files[i].W && !p[(y * files[i].W + x) * 4 + 3]; x++);
                    if (x < files[i].W) break;
                    h--;
                }
                for (x = 0; x < files[i].W && h > 0 && w > 0; x++) {
                    for (y = 0; y < h && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
                    if (y < h) break;
                    files[i].X++; w--;
                }
                for (x = files[i].W - 1; x >= files[i].X && h > 0 && w > 0; x--) {
                    for (y = 0; y < h && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
                    if (y < h) break;
                    w--;
                }
                if (w < 1 || h < 1) w = h = files[i].X = files[i].Y = 0;
            }
            memset(&rects[i], 0, sizeof(struct stbrp_rect));
            rects[i].id = i; rects[i].w = w; rects[i].h = h;
        } else
            if (p) free(p);
        com = stbi_comment(); if (com) free(com);
    }
}