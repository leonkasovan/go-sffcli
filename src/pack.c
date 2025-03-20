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

void calculate_image(unsigned char *img_px, int img_width, int img_height, const char *img_tag){
    DIR *dir;
    struct dirent *ent;
    int i, l, x, y;
    uint8_t *p;
    char *com, *s, *d, *e;
	char name[64];
	
	sprintf(name, "%d_%d", img_height, img_height);
    i = num++;
	files = (meta_t*)realloc(files, num * sizeof(meta_t));
	rects = (struct stbrp_rect*)realloc(rects, num * sizeof(struct stbrp_rect));
	if(!files || !rects) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	memset(&files[i], 0, sizeof(meta_t));
	l = strlen(name);
	files[i].name = (char*)malloc(l + 1);
	if(!files[i].name) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	e = strrchr(name, '.'); if(!e) e = name + l;
	for(s = name, d = files[i].name; s < e && *s; s++)
		*d++ = *s == '/' || *s == '\\' || *s == ':' || *s == '\"' || *s == ' ' || *s == '\t' ||
			*s == '\n' || *s == '\r' ? '_' : *s;
	*d = 0;
	files[i].data = p;
	files[i].W = img_width; files[i].H = img_height;
	if(img_width > maxw) maxw = img_width;
	if(img_height > maxh) maxh = img_height;
	prod += (int64_t)img_width * (int64_t)img_height;
	/* crop input sprite to content */
	if(inpcrop) {
		for(y = 0; y < files[i].H && img_height > 0; y++) {
			for(x = 0; x < files[i].W && !p[(y * files[i].W + x) * 4 + 3]; x++);
			if(x < files[i].W) break;
			files[i].Y++; img_height--;
		}
		for(y = files[i].H - 1; y >= files[i].Y && img_height > 0; y--) {
			for(x = 0; x < files[i].W && !p[(y * files[i].W + x) * 4 + 3]; x++);
			if(x < files[i].W) break;
			img_height--;
		}
		for(x = 0; x < files[i].W && img_height > 0 && img_width > 0; x++) {
			for(y = 0; y < img_height && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
			if(y < img_height) break;
			files[i].X++; img_width--;
		}
		for(x = files[i].W - 1; x >= files[i].X && img_height > 0 && img_width > 0; x--) {
			for(y = 0; y < img_height && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
			if(y < img_height) break;
			img_width--;
		}
		if(img_width < 1 || img_height < 1) img_width = img_height = files[i].X = files[i].Y = 0;
	}
	memset(&rects[i], 0, sizeof(struct stbrp_rect));
	rects[i].id = i; rects[i].w = img_width; rects[i].h = img_height;
}

void calculate_image3(FILE *f, int img_width, int img_height){
}

void print_info(){
	printf("Total num=%d\nmaxw=%d maxh=%d\n", num, maxw, maxh);
}