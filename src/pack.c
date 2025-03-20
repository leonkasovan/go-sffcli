#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "libpng/png.h"

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
 * Load an image
 */
 unsigned char *image_load(char *fn, int *w, int *h)
 {
	 FILE *f;
	 stbi__context s;
	 stbi__result_info ri;
	 unsigned char *data = NULL;
	 int c = 0, nf = 1;
 
	 *w = *h = 0;
	 f = stbi__fopen(fn, "rb");
	 if(!f) return NULL;
	 stbi__start_file(&s, f);
	 if(stbi__gif_test(&s)) {
		 data = stbi__load_gif_main(&s, NULL, w, h, &nf, &c, 4);
		 if(data && *w > 0 && *h > 0 && nf > 1)
			 *h *= nf;
	 } else {
		 data = stbi__load_main(&s, w, h, &c, 4, &ri, 8);
	 }
	 fclose(f);
	 if(data && *w > 0 && *h > 0)
		 return data;
	 if(data) free(data);
	 return NULL;
 }
 
 /**
  * Write image to file
  */
 int image_save(uint8_t *p, int w, int h, char *fn, char *meta)
 {
	 FILE *f;
	 uint32_t *ptr = (uint32_t*)p, pal[256];
	 uint8_t *data, *pal2 = (uint8_t*)&pal;
	 png_color pngpal[256];
	 png_byte pngtrn[256];
	 png_structp png_ptr;
	 png_infop info_ptr;
	 png_bytep *rows;
	 png_text texts[1] = { 0 };
	 int i, j, nc = 0;
 
	 if(!p || !fn || !*fn || w < 1 || h < 1) return 0;
	 printf("Saving %s\r\n", fn);
	 f = fopen(fn, "wb+");
	 if(!f) { fprintf(stderr,"Unable to write %s\r\n", fn); exit(2); }
	 png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	 if(!png_ptr) { fclose(f); return 0; }
	 info_ptr = png_create_info_struct(png_ptr);
	 if(!info_ptr) { png_destroy_write_struct(&png_ptr, NULL); fclose(f); return 0; }
	 if(setjmp(png_jmpbuf(png_ptr))) { png_destroy_write_struct(&png_ptr, &info_ptr); fclose(f); return 0; }
	 png_init_io(png_ptr, f);
	 png_set_compression_level(png_ptr, 9);
	 png_set_compression_strategy(png_ptr, 0);
	 png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE, PNG_FILTER_VALUE_SUB);
	 rows = (png_bytep*)malloc(h * sizeof(png_bytep));
	 data = (uint8_t*)malloc(w * h);
	 if(!rows || !data) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	 /* lets see if we can save this as an indexed image */
	 for(i = 0; i < w * h; i++) {
		 for(j = 0; j < nc && pal[j] != ptr[i]; j++);
		 if(j >= nc) {
			 if(nc == 256) { nc = -1; break; }
			 pal[nc++] = ptr[i];
		 }
		 data[i] = j;
	 }
	 if(nc != -1) {
		 for(i = j = 0; i < nc; i++) {
			 pngpal[i].red = pal2[i * 4 + 0];
			 pngpal[i].green = pal2[i * 4 + 1];
			 pngpal[i].blue = pal2[i * 4 + 2];
			 pngtrn[i] = pal2[i * 4 + 3];
		 }
		 png_set_PLTE(png_ptr, info_ptr, pngpal, nc);
		 png_set_tRNS(png_ptr, info_ptr, pngtrn, nc, NULL);
		 for(i = 0; i < h; i++) rows[i] = data + i * w;
	 } else
		 for(i = 0; i < h; i++) rows[i] = p + i * w * 4;
	 png_set_IHDR(png_ptr, info_ptr, w, h, 8, nc == -1 ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_PALETTE,
		 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	 if(meta && *meta) {
		 texts[0].key = "Comment"; texts[0].text = meta;
		 png_set_text(png_ptr, info_ptr, texts, 1);
	 }
	 png_write_info(png_ptr, info_ptr);
	 png_write_image(png_ptr, rows);
	 png_write_end(png_ptr, info_ptr);
	 png_destroy_write_struct(&png_ptr, &info_ptr);
	 free(rows);
	 free(data);
	 fclose(f);
	 return 1;
 }

void calculate_image(unsigned char *img_px, int img_width, int img_height, const char *img_tag){
    DIR *dir;
    struct dirent *ent;
    int i, l, x, y;
    uint8_t *p;
    char *com, *d;
	const char *s, *e;

    i = num++;
	printf("\n\ni=%d img_width=%d img_height=%d img_tag=%s\n", i, img_width, img_height, img_tag);
	files = (meta_t*)realloc(files, num * sizeof(meta_t));
	rects = (struct stbrp_rect*)realloc(rects, num * sizeof(struct stbrp_rect));
	if(!files || !rects) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	memset(&files[i], 0, sizeof(meta_t));
	l = strlen(img_tag);
	files[i].name = (char*)malloc(l + 1);
	if(!files[i].name) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	e = strrchr(img_tag, '.'); if(!e) e = img_tag + l;
	for(s = img_tag, d = files[i].name; s < e && *s; s++)
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
		unsigned char px;
		for(y = 0; y < files[i].H && img_height > 0; y++) {
			printf("line=%d y=%d files[i].W=%d offset=%d\n", __LINE__, y, files[i].W, (y * files[i].W + x) * 4 + 3);
			px = p[(y * files[i].W + x) *4 + 3];
			for(x = 0; x < files[i].W && !px; x++){
				printf("line=%d x=%d off=%d\n", __LINE__, x, (y * files[i].W + x) *4 + 3);
				px = p[(y * files[i].W + x) *4 + 3];
			}
			printf("line=%d\n", __LINE__);
			if(x < files[i].W) break;
			printf("line=%d\n", __LINE__);
			files[i].Y++; img_height--;
			printf("line=%d\n", __LINE__);
		}
		printf("files[%d].Y=%d\n", i, files[i].Y);
		for(y = files[i].H - 1; y >= files[i].Y && img_height > 0; y--) {
			for(x = 0; x < files[i].W && !p[(y * files[i].W + x) * 4 + 3]; x++);
			if(x < files[i].W) break;
			img_height--;
		}
		printf("img_height=%d\n", img_height);
		for(x = 0; x < files[i].W && img_height > 0 && img_width > 0; x++) {
			for(y = 0; y < img_height && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
			if(y < img_height) break;
			files[i].X++; img_width--;
		}
		printf("files[%d].X=%d\n", i, files[i].X);
		for(x = files[i].W - 1; x >= files[i].X && img_height > 0 && img_width > 0; x--) {
			for(y = 0; y < img_height && !p[((y + files[i].Y) * files[i].W + x) * 4 + 3]; y++);
			if(y < img_height) break;
			img_width--;
		}
		printf("img_width=%d\n", img_width);
		if(img_width < 1 || img_height < 1) img_width = img_height = files[i].X = files[i].Y = 0;
	}
	printf("img_width=%d img_height=%d\n", img_width, img_height);
	memset(&rects[i], 0, sizeof(struct stbrp_rect));
	rects[i].id = i; rects[i].w = img_width; rects[i].h = img_height;
	printf("rects[%d].w=%d rects[%d].h=%d\n", i, rects[i].w, i, rects[i].h);
}

void calculate_image3(FILE *f, int img_width, int img_height){
}

void print_info(){
	stbrp_context ctx;
    stbrp_node *nodes;
    uint8_t *o, *p, *src, *dst;
    char *png = NULL, *meta = NULL, *s, *e, fn[8192], bs, be;
    int i, j, l = 0, x, y, w, h, X, Y, W, H;
    FILE *f;

	/* calculate an optimal atlas size if not given */
	l = 0;
	if(!width || !height) {
		l = crop = 1;
		for(i = 0; i * i < prod; i++);
		if(i < maxw) i = maxw;
		for(width = 1; width < i; width <<= 1);
		i = (prod + width - 1) / width;
		if(i < maxh) i = maxh;
		for(height = 1; height < i; height <<= 1);
	}
	nodes = (stbrp_node*)malloc((width + 1) * sizeof(stbrp_node));
	if(!nodes) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
	memset(nodes, 0, (width + 1) * sizeof(stbrp_node));
	stbrp_init_target(&ctx, width, height, nodes, width + 1);
	if(!stbrp_pack_rects(&ctx, rects, num)) {
		if(l) {
			height <<= 1;
			memset(nodes, 0, (width + 1) * sizeof(stbrp_node));
			for(i = 0; i < num; i++) rects[i].was_packed = rects[i].x = rects[i].y = 0;
			stbrp_init_target(&ctx, width, height, nodes, width + 1);
			if(stbrp_pack_rects(&ctx, rects, num)) goto ok;
		}
		fprintf(stderr,"Error, sprites do not fit into %u x %u atlas.\r\n", width, height);
		exit(2);
	}
ok:     
	free(nodes);
	if(crop) width = height = 0;
	for(i = l = 0; i < num; i++) {
		if(crop) {
			if(rects[i].x + rects[i].w > width) width = rects[i].x + rects[i].w;
			if(rects[i].y + rects[i].h > height) height = rects[i].y + rects[i].h;
		}
		l += strlen(files[i].name) + 256;
	}
	if(width > 0 && height > 0) {
		meta = s = (char*)malloc(l);
		if(!meta) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
		memset(meta, 0, l);
		o = (uint8_t*)malloc(width * height * 4);
		if(!o) { fprintf(stderr,"Not enough memory\r\n"); exit(1); }
		memset(o, 0, width * height * 4);
		/* header */
		switch(fmt) {
			case TXT: break;
			default: s += sprintf(s, "[\r\n"); break;
		}
		/* records */
		for(i = 0; i < num; i++) {
			if(rects[i].w > 0 && rects[i].h > 0) {
				src = files[i].data + (files[i].Y * files[i].W + files[i].X) * 4;
				dst = o + (width * rects[i].y + rects[i].x) * 4;
				for(j = 0; j < rects[i].h; j++, dst += width * 4, src += files[i].W * 4)
					memcpy(dst, src, rects[i].w * 4);
			}
			switch(fmt) {
				case TXT:
					s += sprintf(s, "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\r\n",
						rects[i].x, rects[i].y, rects[i].w, rects[i].h,
						files[rects[i].id].X, files[rects[i].id].Y, files[rects[i].id].W, files[rects[i].id].H,
						files[rects[i].id].name);
				break;
				default:
					s += sprintf(s, "%s{ \"x\": %u, \"y\": %u, \"w\": %u, \"h\": %u, "
						"\"X\": %u, \"Y\": %u, \"W\": %u, \"H\": %u, \"name\": \"%s\" }",
						i ? ",\r\n" : "", rects[i].x, rects[i].y, rects[i].w, rects[i].h,
						files[rects[i].id].X, files[rects[i].id].Y, files[rects[i].id].W, files[rects[i].id].H,
						files[rects[i].id].name);
				break;
			}
		}
		/* footer */
		switch(fmt) {
			case TXT: break;
			default: s += sprintf(s, "\r\n]"); break;
		}
		image_save(o, width, height, png, meta);
		free(o);
		/* save meta info to a separate file too */
		if(tofile) {
			e = strrchr(png, '.'); if(!e) e = png + strlen(png);
			memcpy(fn, png, e - png);
			strcpy(fn + (e - png), exts[fmt]);
			printf("Saving %s\r\n", fn);
			f = fopen(fn, "wb+");
			if(f) {
				fwrite(meta, 1, s - meta, f);
				fclose(f);
			}
		}
		free(meta);
	} else
		fprintf(stderr,"Error, nothing left after cropping to contents?\r\n");
	for(i = 0; i < num; i++) {
		if(files[i].name) free(files[i].name);
		if(files[i].data) free(files[i].data);
	}
	free(files);
	free(rects);
	printf("Total num=%d\nmaxw=%d maxh=%d\n", num, maxw, maxh);
}