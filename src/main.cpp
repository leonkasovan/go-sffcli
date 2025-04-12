/*
RELEASE BUILD:
g++ -O3 -DNDEBUG -o sffcli.exe src/main.cpp -lpng -lz

DEBUG BUILD:
g++ -DDEBUG -fsanitize=address -static-libasan -g -o sffcli_dbg.exe src/main.cpp -lpng -lz
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <zlib.h>
#include <map>
#include <array>
#include <vector>
#include <filesystem>
#include "png.h"
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#ifdef _WIN32
    #include <direct.h>
    #include <sys/stat.h>
    #define MKDIR(dir) _mkdir(dir)
    #define STAT_STRUCT struct _stat
    #define STAT_FUNC _stat
    #define S_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#else
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #define MKDIR(dir) mkdir(dir, 0755)
    #define STAT_STRUCT struct stat
    #define STAT_FUNC stat
#endif

#define MAX_PAL_NO 256

// Constants for the hash function
#define PRIME 0x9E3779B1

typedef struct {
    uint8_t Ver3, Ver2, Ver1, Ver0;
    uint32_t FirstSpriteHeaderOffset;
    uint32_t FirstPaletteHeaderOffset;
    uint32_t NumberOfSprites;
    uint32_t NumberOfPalettes;
} SffHeader;

typedef struct {
    uint32_t palettes[MAX_PAL_NO][256];
    int paletteMap[MAX_PAL_NO];
    int numPalettes;
} PaletteList;

typedef struct {
    uint32_t* Pal;
    int16_t Group;
    int16_t Number;
    uint16_t Size[2];
    int16_t Offset[2];
    int palidx;
    int rle;
    uint8_t coldepth;
    uint8_t *data;
    size_t atlas_x, atlas_y;
} Sprite;

typedef struct {
    SffHeader header;
    Sprite** sprites;
    PaletteList palList;
    char filename[256];
    // std::vector<std::array<png_color, 256>> palettes;
    std::vector<png_color*> palettes;
} Sff;

typedef struct {
    uint16_t width, height;
    struct stbrp_rect* rects;
    Sff* sff;
} Atlas;

Sprite* newSprite() {
    Sprite* sprite = (Sprite*) malloc(sizeof(Sprite));
    memset(sprite, 0, sizeof(Sprite));
    sprite->palidx = -1;
    return sprite;
}

int createDirectory(const char *name) {
    STAT_STRUCT st;

    // Check if path exists
    if (STAT_FUNC(name, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            // Directory already exists
            return 0;
        } else {
            // Exists but is not a directory
            fprintf(stderr, "Path exists but is not a directory: %s\n", name);
            return -2;
        }
    }

    // Try to create directory
    if (MKDIR(name) != 0) {
        fprintf(stderr, "Failed to create directory '%s'\n", name);
        return -1;
    }

    return 0; // Success
}

// Extracts basename without extension from a given path
// The result is written into 'out', which must be at least 'out_size' bytes
/*
Usage:
   const char* paths[] = {
        "./bird.png",
        ".\\bird.png",
        "C:\\tmp\\bird.png",
        "/usr/tmp/bird.png"
    };

    char buffer[256];

    for (int i = 0; i < 4; ++i) {
        get_basename_no_ext(paths[i], buffer, sizeof(buffer));
        printf("Base name: %s\n", buffer);
    }

Result:
Base name: bird
Base name: bird
Base name: bird
Base name: bird
*/
void get_basename_no_ext(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;

    // Find the last path separator
    const char* last_slash = strrchr(path, '/');
    const char* last_backslash = strrchr(path, '\\');
    const char* filename = path;

    if (last_slash || last_backslash) {
        filename = (last_slash > last_backslash) ? last_slash + 1 : last_backslash + 1;
    }

    // Find the last dot (extension)
    const char* last_dot = strrchr(filename, '.');
    size_t len = last_dot ? (size_t)(last_dot - filename) : strlen(filename);

    // Ensure we don't overflow the buffer
    if (len >= out_size) {
        len = out_size - 1;
    }

    strncpy(out, filename, len);
    out[len] = '\0';
}

#define PNG_SIG_BYTES 8

// Helper to write 4-byte big-endian integer
void write_be32(FILE *f, uint32_t val) {
    fputc((val >> 24) & 0xFF, f);
    fputc((val >> 16) & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

// Helper to compute CRC (uses zlib)
uint32_t crc(const uint8_t *type_and_data, size_t len) {
    return crc32(0, type_and_data, len);
}

// Write a PNG chunk
void write_chunk(FILE *f, const char *type, const uint8_t *data, size_t length) {
    write_be32(f, (uint32_t)length);
    fwrite(type, 1, 4, f);
    if (length > 0) fwrite(data, 1, length, f);
    uint8_t *crc_buf = (uint8_t *)malloc(4 + length);
    memcpy(crc_buf, type, 4);
    if (length > 0) memcpy(crc_buf + 4, data, length);
    uint32_t c = crc(crc_buf, 4 + length);
    write_be32(f, c);
    free(crc_buf);
}

// Check PNG signature
int check_png_signature(FILE *in) {
    uint8_t sig[PNG_SIG_BYTES];
    fread(sig, 1, PNG_SIG_BYTES, in);
    const uint8_t expected_sig[PNG_SIG_BYTES] = {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    return memcmp(sig, expected_sig, PNG_SIG_BYTES) == 0;
}

// Fast hash function for a 256-element array of uint32_t
uint32_t fast_hash(const uint32_t* data, size_t len) {
    uint32_t h = len * PRIME;
    for (size_t i = 0; i < len; ++i) {
        h = ((h + data[i]) << 13) | ((h + data[i]) >> (32 - 13)); // Rotate bits
        h *= PRIME;
    }
    return h;
}

void savePalette(uint32_t* palette, const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Error creating file %s\n", filename);
        return;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t r = (palette[i] >> 0) & 0xFF;
        uint8_t g = (palette[i] >> 8) & 0xFF;
        uint8_t b = (palette[i] >> 16) & 0xFF;
        fwrite(&r, 1, 1, file);
        fwrite(&g, 1, 1, file);
        fwrite(&b, 1, 1, file);
    }
    fclose(file);
}

int readSffHeader(Sff* sff, FILE* file, uint32_t* lofs, uint32_t* tofs) {

    // Validate header by comparing 12 first bytes with "ElecbyteSpr\x0"
    char headerCheck[12];
    fread(headerCheck, 12, 1, file);
    if (memcmp(headerCheck, "ElecbyteSpr\0", 12) != 0) {
        fprintf(stderr, "Invalid SFF file [%s]\n", headerCheck);
        return -1;
    }

    // Read versions in the header
    if (fread(&sff->header.Ver3, 1, 1, file) != 1) {
        fprintf(stderr, "Error reading version\n");
        return -1;
    }
    if (fread(&sff->header.Ver2, 1, 1, file) != 1) {
        fprintf(stderr, "Error reading version\n");
        return -1;
    }
    if (fread(&sff->header.Ver1, 1, 1, file) != 1) {
        fprintf(stderr, "Error reading version\n");
        return -1;
    }
    if (fread(&sff->header.Ver0, 1, 1, file) != 1) {
        fprintf(stderr, "Error reading version\n");
        return -1;
    }
    uint32_t dummy;
    if (fread(&dummy, sizeof(uint32_t), 1, file) != 1) {
        fprintf(stderr, "Error reading dummy\n");
        return -1;
    }

    if (sff->header.Ver0 == 2) {
        for (int i = 0; i < 4; i++) {
            if (fread(&dummy, sizeof(uint32_t), 1, file) != 1) {
                fprintf(stderr, "Error reading dummy\n");
                return -1;
            }
        }
        // read FirstSpriteHeaderOffset
        if (fread(&sff->header.FirstSpriteHeaderOffset, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading FirstSpriteHeaderOffset\n");
            return -1;
        }
        // read NumberOfSprites
        if (fread(&sff->header.NumberOfSprites, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading NumberOfSprites\n");
            return -1;
        }
        // read FirstPaletteHeaderOffset
        if (fread(&sff->header.FirstPaletteHeaderOffset, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading FirstPaletteHeaderOffset\n");
            return -1;
        }
        // read NumberOfPalettes
        if (fread(&sff->header.NumberOfPalettes, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading NumberOfPalettes\n");
            return -1;
        }
        // read lofs
        if (fread(lofs, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading lofs\n");
            return -1;
        }
        if (fread(&dummy, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading dummy\n");
            return -1;
        }
        // read tofs
        if (fread(tofs, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading tofs\n");
            return -1;
        }
    } else if (sff->header.Ver0 == 1) {
        // read NumberOfSprites
        if (fread(&sff->header.NumberOfSprites, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading NumberOfSprites\n");
            return -1;
        }
        // read FirstSpriteHeaderOffset
        if (fread(&sff->header.FirstSpriteHeaderOffset, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "Error reading FirstSpriteHeaderOffset\n");
            return -1;
        }
        sff->header.FirstPaletteHeaderOffset = 0;
        sff->header.NumberOfPalettes = 0;
        *lofs = 0;
        *tofs = 0;
    } else {
        fprintf(stderr, "Unsupported SFF version: %d\n", sff->header.Ver0);
        return -1;
    }

    // Print header information
    printf("Version: %d.%d.%d.%d\n", sff->header.Ver0, sff->header.Ver1, sff->header.Ver2, sff->header.Ver3);
    printf("First Sprite Header Offset: %u\n", sff->header.FirstSpriteHeaderOffset);
    printf("First Palette Header Offset: %u\n", sff->header.FirstPaletteHeaderOffset);
    printf("Number of Sprites: %u\n", sff->header.NumberOfSprites);
    printf("Number of Palettes: %u\n", sff->header.NumberOfPalettes);

    return 0;
}

int readSpriteHeaderV1(Sprite* sprite, FILE* file, uint32_t* ofs, uint32_t* size, uint16_t* link) {
    // Read ofs
    if (fread(ofs, sizeof(uint32_t), 1, file) != 1) {
        fprintf(stderr, "Error reading ofs\n");
        return -1;
    }
    // Read size
    if (fread(size, sizeof(uint32_t), 1, file) != 1) {
        fprintf(stderr, "Error reading size\n");
        return -1;
    }
    if (fread(&sprite->Offset[0], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite offset\n");
        return -1;
    }
    if (fread(&sprite->Offset[1], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite offset\n");
        return -1;
    }
    // Read sprite header
    if (fread(&sprite->Group, sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite group\n");
        return -1;
    }
    if (fread(&sprite->Number, sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite number\n");
        return -1;
    }
    // Read the link to the next sprite header
    if (fread(link, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite link\n");
        return -1;
    }
    // Print sprite header information
    // printf("Sprite v1 Group, Number: %d, %d\n", sprite->Group, sprite->Number);
    return 0;
}

int readSpriteHeaderV2(Sprite* sprite, FILE* file, uint32_t* ofs, uint32_t* size, uint32_t lofs, uint32_t tofs, uint16_t* link) {
    // Read sprite header
    if (fread(&sprite->Group, sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite group\n");
        return -1;
    }
    if (fread(&sprite->Number, sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite number\n");
        return -1;
    }
    if (fread(&sprite->Size[0], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite size\n");
        return -1;
    }
    if (fread(&sprite->Size[1], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite size\n");
        return -1;
    }
    if (fread(&sprite->Offset[0], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite offset\n");
        return -1;
    }
    if (fread(&sprite->Offset[1], sizeof(int16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite offset\n");
        return -1;
    }
    // Read the link to the next sprite header
    if (fread(link, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite link\n");
        return -1;
    }
    char format;
    if (fread(&format, sizeof(char), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite format\n");
        return -1;
    }
    sprite->rle = -format;
    // Read color depth
    if (fread(&sprite->coldepth, sizeof(uint8_t), 1, file) != 1) {
        fprintf(stderr, "Error reading color depth\n");
        return -1;
    }
    // Read ofs
    if (fread(ofs, sizeof(uint32_t), 1, file) != 1) {
        fprintf(stderr, "Error reading ofs\n");
        return -1;
    }
    // Read size
    if (fread(size, sizeof(uint32_t), 1, file) != 1) {
        fprintf(stderr, "Error reading size\n");
        return -1;
    }
    uint16_t tmp;
    // Read tmp
    if (fread(&tmp, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading tmp\n");
        return -1;
    }
    sprite->palidx = tmp;
    // Read tmp
    if (fread(&tmp, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading tmp\n");
        return -1;
    }
    if ((tmp & 1) == 0) {
        *ofs += lofs;
    } else {
        *ofs += tofs;
    }

    // Print sprite header information
    // printf("Sprite v2 (%d,%d) ofs=%d size=%d\n", sprite->Group, sprite->Number, *ofs, *size);
    // printf("Sprite Size: %d x %d\n", sprite->Size[0], sprite->Size[1]);
    // printf("Sprite Offset: %d x %d\n", sprite->Offset[0], sprite->Offset[1]);
    // printf("Sprite Link: %d\n", *link);
    // printf("Sprite Format: %d\n", format);

    return 0;
}

uint8_t* TestDecode(Sprite* s, uint8_t* srcPx, size_t srcLen) {
    if (srcLen == 0) {
        fprintf(stderr, "Warning LZ5 data length is zero\n");
        return NULL;
    }

    int dstLen = s->Size[0] * s->Size[1];
    uint8_t* dstPx = (uint8_t*) malloc(dstLen);
    if (!dstPx) {
        fprintf(stderr, "Error allocating memory for LZ5 decoded data\n");
        return NULL;
    }

    for (int y = 0; y < s->Size[1]; y++) {
        uint8_t col = 24 + rand() % 5;
        for (int x = 0; x < s->Size[0]; x++) {
            int i = y * s->Size[0] + x;
            dstPx[i] = col;
        }
    }
    return dstPx;
}

uint8_t* Lz5Decode(Sprite* s, uint8_t* srcPx, size_t srcLen) {
    if (srcLen == 0) {
        fprintf(stderr, "Warning LZ5 data length is zero\n");
        return NULL;
    }

    int dstLen = s->Size[0] * s->Size[1];
    uint8_t* dstPx = (uint8_t*) malloc(dstLen);
    if (!dstPx) {
        fprintf(stderr, "Error allocating memory for LZ5 decoded data\n");
        return NULL;
    }

    // Decode the LZ5 data
    long i = 0, j = 0, n = 0;
    uint8_t ct = srcPx[i], cts = 0, rb = 0, rbc = 0;
    if (i < srcLen - 1) {
        i++;
    }

    while (j < dstLen) {
        int d = (int) srcPx[i];
        if (i < srcLen - 1) {
            i++;
        }

        if (ct & (1 << cts)) {
            if ((d & 0x3f) == 0) {
                d = (d << 2 | (int) srcPx[i]) + 1;
                if (i < srcLen - 1) {
                    i++;
                }
                n = (int) srcPx[i] + 2;
                if (i < srcLen - 1) {
                    i++;
                }
            } else {
                rb |= (uint8_t) ((d & 0xc0) >> rbc);
                rbc += 2;
                n = (int) (d & 0x3f);
                if (rbc < 8) {
                    d = (int) srcPx[i] + 1;
                    if (i < srcLen - 1) {
                        i++;
                    }
                } else {
                    d = (int) rb + 1;
                    rb = rbc = 0;
                }
            }
            for (;;){
                if (j < dstLen) {
					dstPx[j] = dstPx[j-d];
					j++;
				}
				n--;
				if (n < 0) {
					break;
				}
            }
        } else {
            if ((d & 0xe0) == 0) {
                n = (int) srcPx[i] + 8;
                if (i < srcLen - 1) {
                    i++;
                }
            } else {
                n = d >> 5;
                d &= 0x1f;
            }
            while (n-- > 0 && j < dstLen) {
                dstPx[j] = (uint8_t) d;
                j++;
            }
        }
        cts++;
        if (cts >= 8) {
            ct = srcPx[i];
            cts = 0;
            if (i < srcLen - 1) {
                i++;
            }
        }
    }

    return dstPx;
}

uint8_t* Rle8Decode(Sprite* s, uint8_t* srcPx, int srcLen) {
    if (srcLen == 0) {
        fprintf(stderr, "Warning RLE8 data length is zero\n");
        return NULL;
    }

    size_t dstLen = s->Size[0] * s->Size[1];
    uint8_t* dstPx = (uint8_t*) malloc(dstLen);
    if (!dstPx) {
        fprintf(stderr, "Error allocating memory for RLE decoded data\n");
        return NULL;
    }
    long i = 0, j = 0;
    // Decode the RLE data
    while (j < dstLen) {
        long n = 1;
        uint8_t d = srcPx[i];
        if (i < (srcLen - 1)) {
            i++;
        }
        if ((d & 0xc0) == 0x40) {
            n = d & 0x3f;
            d = srcPx[i];
            if (i < (srcLen - 1)) {
                i++;
            }
        }
        for (; n > 0; n--) {
            if (j < dstLen) {
                dstPx[j] = d;
                j++;
            }
        }
    }
    return dstPx;
}

uint8_t* Rle5Decode(Sprite* s, uint8_t* srcPx, size_t srcLen) {
    if (srcLen == 0) {
        fprintf(stderr, "Warning RLE5 data length is zero\n");
        return NULL;
    }

    int dstLen = s->Size[0] * s->Size[1];
    uint8_t* dstPx = (uint8_t*) malloc(dstLen);
    if (!dstPx) {
        fprintf(stderr, "Error allocating memory for RLE decoded data\n");
        return NULL;
    }

    size_t i = 0, j = 0;
    while (j < dstLen) {
        int rl = (int) srcPx[i];
        if (i < srcLen - 1) {
            i++;
        }
        int dl = (int) (srcPx[i] & 0x7f);
        uint8_t c = 0;
        if (srcPx[i] >> 7 != 0) {
            if (i < srcLen - 1) {
                i++;
            }
            c = srcPx[i];
        }
        if (i < srcLen - 1) {
            i++;
        }
        while (1) {
            if (j < dstLen) {
                dstPx[j] = c;
                j++;
            }
            rl--;
            if (rl < 0) {
                dl--;
                if (dl < 0) {
                    break;
                }
                c = srcPx[i] & 0x1f;
                rl = (int) (srcPx[i] >> 5);
                if (i < srcLen - 1) {
                    i++;
                }
            }
        }
    }

    return dstPx;
}

uint8_t* RlePcxDecode(Sprite* s, uint8_t* srcPx, size_t srcLen) {
    if (srcLen == 0) {
        fprintf(stderr, "Warning PCX data length is zero\n");
        return NULL;
    }

    int dstLen = s->Size[0] * s->Size[1];
    // printf("Allocating memory for PCX decoded data dstLen=%ld srcLen=%ld %dx%d\n", dstLen, srcLen, s->Size[0], s->Size[1]);
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
    uint8_t* dstPx = (uint8_t*) malloc(dstLen);
    // uint8_t* dstPx;
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
    // dstPx = (uint8_t*) malloc(dstLen * sizeof(uint8_t));
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
    if (!dstPx) {
        fprintf(stderr, "Error allocating memory for PCX decoded data dstLen=%ld srcLen=%ld %dx%d\n", dstLen, srcLen, s->Size[0], s->Size[1]);
        return NULL;
    }
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);

    size_t i = 0, j = 0, k = 0, w = s->Size[0];
    while (j < dstLen) {
        int n = 1, d = srcPx[i];
        if (i < (srcLen - 1)) {
            i++;
        }
        if (d >= 0xc0) {
            n = d & 0x3f;
            d = srcPx[i];
            if (i < (srcLen - 1)) {
                i++;
            }
        }
        for (; n > 0; n--) {
            if ((k < w) && (j < dstLen)) {
                dstPx[j] = d;
                j++;
            }
            k++;
            if (k == s->rle) {
                k = 0;
                n = 1;
            }
        }
    }
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
    s->rle = 0;
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
    return dstPx;
}

void save_as_png(const char* filename, int img_width, int img_height, png_byte* img_data, png_color* palette) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open file '%s' for writing\n", filename);
        return;
    }
    // printf("%s\n", filename);

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fprintf(stderr, "Failed to create PNG write struct\n");
        fclose(fp);
        return;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        fprintf(stderr, "Failed to create PNG info struct\n");
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return;
    }

    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "Failed during PNG creation\n");
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return;
    }

    png_init_io(png, fp);

    png_set_IHDR(
        png,
        info,
        img_width, img_height,
        8,
        PNG_COLOR_TYPE_PALETTE,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    png_set_PLTE(png, info, palette, 256);

    png_byte trans[256];
    memset(trans, 255, 256);
    trans[0] = 0; // Set palette index 0 to be transparent
    png_set_tRNS(png, info, trans, 256, NULL);

    png_write_info(png, info);

    png_bytep rows[img_height];
    for (int y = 0; y < img_height; y++) {
        rows[y] = img_data + y * img_width;
    }

    png_write_image(png, rows);
    png_write_end(png, NULL);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
    // puts(filename);
}

int readPcxHeader(Sprite* s, FILE* file, uint64_t offset) {
    fseek(file, offset, SEEK_SET);
    uint16_t dummy;
    if (fread(&dummy, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading uint16_t dummy\n");
        return -1;
    }
    uint8_t encoding, bpp;
    if (fread(&encoding, sizeof(uint8_t), 1, file) != 1) {
        fprintf(stderr, "Error reading uint8_t encoding\n");
        return -1;
    }
    if (fread(&bpp, sizeof(uint8_t), 1, file) != 1) {
        fprintf(stderr, "Error reading uint8_t bpp\n");
        return -1;
    }
    if (bpp != 8) {
        fprintf(stderr, "Invalid PCX color depth: expected 8-bit, got %d", bpp);
        return -1;
    }
    uint16_t rect[4];
    if (fread(rect, sizeof(uint16_t), 4, file) != 4) {
        fprintf(stderr, "Error reading rectangle\n");
        return -1;
    }
    fseek(file, offset + 66, SEEK_SET);
    uint16_t bpl;
    if (fread(&bpl, sizeof(uint16_t), 1, file) != 1) {
        fprintf(stderr, "Error reading bpl\n");
        return -1;
    }
    s->Size[0] = rect[2] - rect[0] + 1;
    s->Size[1] = rect[3] - rect[1] + 1;
    if (encoding == 1) {
        s->rle = bpl;
    } else {
        s->rle = 0;
    }
    return 0;
}

int readSpriteDataV1(Sprite* s, FILE* file, Sff* sff, uint64_t offset, uint32_t datasize, uint32_t nextSubheader, Sprite* prev, std::vector<png_color*>* palettes, bool c00) {
    if (nextSubheader > offset) {
        // Ignore datasize except last
        datasize = nextSubheader - offset;
    }

    uint8_t ps;
    if (fread(&ps, sizeof(uint8_t), 1, file) != 1) {
        fprintf(stderr, "Error reading sprite ps data\n");
        return -1;
    }
    bool paletteSame = ps != 0 && prev != NULL;
    if (readPcxHeader(s, file, offset) != 0) {
        fprintf(stderr, "Error reading sprite PCX header\n");
        return -1;
    }

    fseek(file, offset + 128, SEEK_SET);
    uint32_t palHash = 0;
    uint32_t palSize;
    if (c00 || paletteSame) {
        palSize = 0;
    } else {
        palSize = 768;
    }
    if (datasize < 128 + palSize) {
        datasize = 128 + palSize;
    }
    // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);

    char pngFilename[256];
    char basename[256];
    get_basename_no_ext(sff->filename, basename, sizeof(basename));
    if (createDirectory(basename) != 0) {
        return -1;
    }
#ifdef _WIN32
    snprintf(pngFilename, sizeof(pngFilename), "%s\\%s %d %d.png", basename, basename, s->Group, s->Number);
#else
    snprintf(pngFilename, sizeof(pngFilename), "%s/%s %d %d.png", basename, basename, s->Group, s->Number);
#endif
    size_t srcLen = datasize - (128 + palSize);
    uint8_t* srcPx = (uint8_t*) malloc(srcLen);
    if (!srcPx) {
        fprintf(stderr, "Error allocating memory for sprite data\n");
        return -1;
    }
    if (fread(srcPx, srcLen, 1, file) != 1) {
        fprintf(stderr, "Error reading sprite PCX data pixel\n");
        return -1;
    }

    s->data = NULL;
    // printf("PCX: ps=%d ", ps);
    if (paletteSame) {
        png_color* png_palette;
        // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
        if (prev != NULL) {
            s->palidx = prev->palidx;
            // printf("Info: Same palette (%d,%d) with (%d,%d) = %d\n", s->Group, s->Number, prev->Group, prev->Number, prev->palidx);
        }
        if (s->palidx < 0) {
            png_color* palette = new png_color[256];
            palettes->push_back(palette);
            s->palidx = palettes->size() - 1;
            png_palette = palette;
            printf("Warning: incompleted code for handling palette in main.cpp line %d\n", __LINE__);
        } else {
            // fprintf(stderr, "Warning: palette index %d\npal_len=%d\n", s->palidx, palettes->size());
            png_palette = palettes->at(s->palidx);
            // for (int i = 0; i < 256; i++) {
            //     png_palette[i].red = pal[i].red;
            //     png_palette[i].green = pal[i].green;
            //     png_palette[i].blue = pal[i].blue;
                // png_palette[i].red = 100;
                // png_palette[i].green = 200;
                // png_palette[i].blue = 0;
            // }
        }
        // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
        uint8_t* px = RlePcxDecode(s, srcPx, srcLen);
        free(srcPx);
        if (!px) {
            fprintf(stderr, "Error decoding PCX sprite data\n");
            return -1;
        }
        // palHash = fast_hash(pal, 256);
        // printf("old_pal=%d ", s->palidx);
        // save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
        s->data = px;
        // free(px);
    } else {
        png_color* png_palette = new png_color[256];
        if (c00) {
            fseek(file, offset + datasize - 768, 0);
        }
        uint8_t rgb[3];

        for (int i = 0;i < 256;i++) {
            if (fread(rgb, sizeof(uint8_t), 3, file) != 3) {
                fprintf(stderr, "Error reading palette rgb data\n");
                return -1;
            }
            uint8_t alpha = 255;
            if (i == 0) {
                alpha = 0;
            }
            png_palette[i].red = rgb[0];
            png_palette[i].green = rgb[1];
            png_palette[i].blue = rgb[2];
        }
        palettes->push_back(png_palette);
        s->palidx = palettes->size() - 1;
        // savePalette(pal, fmt.Sprintf("%v %v %v.act", "char_pal", s.Group, s.Number))
        // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
        uint8_t* px = RlePcxDecode(s, srcPx, srcLen);
        free(srcPx);
        if (!px) {
            fprintf(stderr, "Error decoding PCX sprite data\n");
            return -1;
        }
        // printf("[DEBUG] src/main.cpp:%d\n", __LINE__);
        // palHash = fast_hash(pal, 256);
        // printf("new_pal=%d ", s->palidx);
        // save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
        s->data = px;
        // free(px);
    }
    // printf("[DEBUG] src/main.cpp:%d ps=%d paletteSame=%d palidx=%d palLen=%d palSize=%d srcLen=%ld\n", __LINE__, ps, paletteSame, s->palidx, palettes->size(), palSize, srcLen);
    // printf("%u\n", palHash);
    return 0;
}

int copy_png_with_palette(FILE *in, FILE *out, uint32_t palette[256]) {
    if (!check_png_signature(in)) {
        fprintf(stderr, "Not a valid PNG file\n");
        return -1;
    }

    // Write PNG signature to output
    const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(png_sig, 1, 8, out);

    int found_IHDR = 0;
    int found_PLTE = 0;
    int wrote_PLTE = 0;
    int wrote_tRNS = 0;

    while (!feof(in)) {
        uint8_t len_bytes[4], type[4];
        if (fread(len_bytes, 1, 4, in) != 4) break;
        uint32_t length = (len_bytes[0] << 24) | (len_bytes[1] << 16) | (len_bytes[2] << 8) | len_bytes[3];

        if (fread(type, 1, 4, in) != 4) break;

        uint8_t *data = (uint8_t *)malloc(length);
        if (fread(data, 1, length, in) != length) {
            free(data);
            break;
        }

        uint8_t crc_buf[4];
        fread(crc_buf, 1, 4, in); // ignore CRC

        if (memcmp(type, "IHDR", 4) == 0) {
            found_IHDR = 1;

            if (length != 13) {
                fprintf(stderr, "Invalid IHDR length\n");
                free(data);
                return -1;
            }

            uint8_t bit_depth = data[8];
            uint8_t color_type = data[9];

            if (bit_depth != 8 || color_type != 3) {
                fprintf(stderr, "Only 8-bit indexed PNGs are supported\n");
                free(data);
                return -1;
            }

            // Write IHDR chunk as-is
            write_chunk(out, "IHDR", data, length);
        } else if (memcmp(type, "PLTE", 4) == 0) {
            found_PLTE = 1;

            // Replace PLTE chunk
            uint8_t new_plte[256 * 3];
            for (int i = 0; i < 256; i++) {
                new_plte[i * 3 + 0] = (palette[i] >> 0) & 0xFF;   // R
                new_plte[i * 3 + 1] = (palette[i] >> 8) & 0xFF;   // G
                new_plte[i * 3 + 2] = (palette[i] >> 16) & 0xFF;  // B
            }
            write_chunk(out, "PLTE", new_plte, 256 * 3);
            wrote_PLTE = 1;

            // Write tRNS chunk (only index 0 transparent)
            uint8_t trns[256];
            for (int i = 0; i < 256; i++) {
                trns[i] = (i == 0) ? 0 : 255;
            }
            write_chunk(out, "tRNS", trns, 256);
            wrote_tRNS = 1;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            // Skip original tRNS (we added our own)
            free(data);
            continue;
        } else {
            // Copy other chunks as-is
            write_chunk(out, (char *)type, data, length);
        }

        free(data);

        // Stop if we hit IEND
        if (memcmp(type, "IEND", 4) == 0)
            break;
    }

    if (!found_IHDR || !found_PLTE || !wrote_PLTE || !wrote_tRNS) {
        fprintf(stderr, "PNG missing IHDR or PLTE or failed writing replacements\n");
        return -1;
    }

    return 0;
}

int copy_png(FILE *in, FILE *out, uint32_t data_size){
    uint32_t png_len = data_size - 4;
    char *buffer = (char *)malloc(png_len);
    if (!buffer) {
        // Handle memory allocation failure
        fprintf(stderr, "Error allocating memory for PNG data\n");
        return -1;
    }

    size_t read = fread(buffer, png_len, 1, in);
    if (read != 1) {
        fprintf(stderr, "Error reading PNG data\n");
        free(buffer);
        return -1;
    }
    fwrite(buffer, png_len, read, out);
    free(buffer);
    return 0;
}

void save_png(Sprite* s, FILE* file, uint32_t data_size, Sff* sff, bool with_palette){
    char pngFilename[256];
    char basename[256];
    get_basename_no_ext(sff->filename, basename, sizeof(basename));
    if (createDirectory(basename) != 0) {
        return;
    }
#ifdef _WIN32
    snprintf(pngFilename, sizeof(pngFilename), "%s\\%s %d %d.png", basename, basename, s->Group, s->Number);
#else
    snprintf(pngFilename, sizeof(pngFilename), "%s/%s %d %d.png", basename, basename, s->Group, s->Number);
#endif
    
    
    // Create a PNG file
    FILE* pngFile = fopen(pngFilename, "wb");
    if (!pngFile) {
        fprintf(stderr, "Error creating PNG file %s\n", pngFilename);
        return;
    }
    // Copy the PNG data from the input file to the output file
    if (with_palette)
        copy_png_with_palette(file, pngFile, sff->palList.palettes[s->palidx]);
    else
        copy_png(file, pngFile, data_size);
    fclose(pngFile);
    printf("%s\n", pngFilename);
}

int readSpriteDataV2(Sprite* s, FILE* file, uint64_t offset, uint32_t datasize, Sff* sff) {
    uint8_t* px = NULL;
    if (s->rle > 0) return -1;

    if (s->rle == 0) {
        px = (uint8_t*) malloc(datasize);
        if (!px) {
            fprintf(stderr, "Error allocating memory for sprite data\n");
            return -1;
        }
        // Read sprite data
        fseek(file, offset, SEEK_SET);
        if (fread(px, datasize, 1, file) != 1) {
            fprintf(stderr, "Error reading V2 uncompress sprite data\n");
            free(px);
            return -1;
        }
    } else {
        size_t srcLen;
        uint8_t* srcPx = NULL;
        fseek(file, offset + 4, SEEK_SET);
        int format = -s->rle;
        int rc;

        if (2 <= format && format <= 4) {
            if (datasize < 4) {
                datasize = 4;
            }
            srcLen = datasize - 4;
            srcPx = (uint8_t*) malloc(srcLen);
            if (!srcPx) {
                fprintf(stderr, "Error allocating memory for sprite data\n");
                return -1;
            }
            // printf("srcPx=%p srcLen=%ld\n", srcPx, srcLen);
            rc = fread(srcPx, srcLen, 1, file);
            if (rc != 1) {
                fprintf(stderr, "Error reading V2 RLE sprite data (len=%ld). RC=%d.\n", srcLen, rc);
                free(srcPx);
                return -1;
            }
        }

        char pngFilename[256];
        char basename[256];
        get_basename_no_ext(sff->filename, basename, sizeof(basename));
        if (createDirectory(basename) != 0) {
            return -1;
        }
#ifdef _WIN32
        snprintf(pngFilename, sizeof(pngFilename), "%s\\%s %d %d.png", basename, basename, s->Group, s->Number);
#else
        snprintf(pngFilename, sizeof(pngFilename), "%s/%s %d %d.png", basename, basename, s->Group, s->Number);
#endif

        s->data = NULL;
        switch (format) {
        case 2:
            // printf("Decoding sprite with RLE8\n");
            printf("RLE8: ");
            px = Rle8Decode(s, srcPx, srcLen);
            free(srcPx);
            if (px) {
                uint32_t* sff_palette = sff->palList.palettes[s->palidx];
                png_color png_palette[256];
                for (int i = 0; i < 256; i++) {
                    png_palette[i].red = (sff_palette[i] >> 0) & 0xFF;
                    png_palette[i].green = (sff_palette[i] >> 8) & 0xFF;
                    png_palette[i].blue = (sff_palette[i] >> 16) & 0xFF;
                }
                save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
                s->data = px;
                // free(px);
            } else {
                fprintf(stderr, "Error decoding RLE8 sprite data\n");
                return -1;
            }
            break;
        case 3:
            // printf("Decoding sprite with RLE5\n");
            printf("RLE5: ");
            px = Rle5Decode(s, srcPx, srcLen);
            free(srcPx);
            if (px) {
                uint32_t* sff_palette = sff->palList.palettes[s->palidx];
                png_color png_palette[256];
                for (int i = 0; i < 256; i++) {
                    png_palette[i].red = (sff_palette[i] >> 0) & 0xFF;
                    png_palette[i].green = (sff_palette[i] >> 8) & 0xFF;
                    png_palette[i].blue = (sff_palette[i] >> 16) & 0xFF;
                }
                save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
                s->data = px;
                // free(px);
            } else {
                fprintf(stderr, "Error decoding RLE5 sprite data\n");
                return -1;
            }
            break;
        case 4:
            // printf("Decoding sprite with LZ55 palidx=%d\n", s->palidx);
            // printf("LZ5: ");
            px = Lz5Decode(s, srcPx, srcLen);
            // px = TestDecode(s, srcPx, srcLen);
            free(srcPx);
            if (px) {
                uint32_t* sff_palette = sff->palList.palettes[s->palidx];
                png_color png_palette[256];
                for (int i = 0; i < 256; i++) {
                    png_palette[i].red = (sff_palette[i] >> 0) & 0xFF;
                    png_palette[i].green = (sff_palette[i] >> 8) & 0xFF;
                    png_palette[i].blue = (sff_palette[i] >> 16) & 0xFF;
                }
                save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
                s->data = px;
                // free(px);
            } else {
                fprintf(stderr, "Error decoding LZ5 sprite data\n");
                return -1;
            }
            break;
        case 10:
            printf("PNG10: ");
            save_png(s, file, datasize, sff, true);
            break;
        case 11:
            printf("PNG11: ");
            save_png(s, file, datasize, sff, true);
            break;
        case 12:
            printf("PNG12: ");
            save_png(s, file, datasize, sff, false);
            break;
        }
    }
    return 0;
}

void spriteCopy(Sprite* dst, const Sprite* src) {
    dst->Pal = src->Pal;
    dst->Group = src->Group;
    dst->Number = src->Number;
    dst->Size[0] = src->Size[0];
    dst->Size[1] = src->Size[1];
    dst->Offset[0] = src->Offset[0];
    dst->Offset[1] = src->Offset[1];
    // if (dst->palidx < 0) {
    dst->palidx = src->palidx;
    // }
    dst->rle = src->rle;
    dst->coldepth = src->coldepth;
    // dst->data = src->data;
}

// function to extract SFF
int extractSff(Sff* sff, const char* filename) {
    bool character = true;
    FILE* file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error opening file %s\n", filename);
        return -1;
    }

    // Copy filename to sff structure
    // strncpy(sff->filename, filename, sizeof(sff->filename) - 1);
    strncpy(sff->filename, filename, sizeof(sff->filename) - 1);

    // Read the header
    uint32_t lofs, tofs;
    if (readSffHeader(sff, file, &lofs, &tofs) != 0) {
        fclose(file);
        return -1;
    }

    if (sff->header.Ver0 != 1) {
        // Allocate memory for palettes
        // sff->palList.palettes = (uint32_t**) malloc(sff->header.NumberOfPalettes * sizeof(uint32_t*));
        std::map<std::array<int, 2>, int> uniquePals;
        sff->palList.numPalettes = 0;
        for (int i = 0; i < sff->header.NumberOfPalettes && i < MAX_PAL_NO; i++) {
            fseek(file, sff->header.FirstPaletteHeaderOffset + i * 16, SEEK_SET);
            // sff->palList.palettes[i] = (uint32_t*) malloc(256 * sizeof(uint32_t));
            // if (!sff->palList.palettes[i]) {
            //     fprintf(stderr, "Error allocating memory for palette %d\n", i);
            //     fclose(file);
            //     return -1;
            // }
            int16_t gn[3];
            if (fread(gn, sizeof(uint16_t), 3, file) != 3) {
                fprintf(stderr, "Error reading palette group\n");
                fclose(file);
                return -1;
            }
            // printf("Palette %d: Group %d, Number %d, ColNumber %d\n", i, gn[0], gn[1], gn[2]);

            uint16_t link;
            if (fread(&link, sizeof(uint16_t), 1, file) != 1) {
                fprintf(stderr, "Error reading palette link\n");
                fclose(file);
                return -1;
            }
            // printf("Palette link: %d\n", link);

            uint32_t ofs, siz;
            if (fread(&ofs, sizeof(uint32_t), 1, file) != 1) {
                fprintf(stderr, "Error reading palette offset\n");
                fclose(file);
                return -1;
            }
            if (fread(&siz, sizeof(uint32_t), 1, file) != 1) {
                fprintf(stderr, "Error reading palette size\n");
                fclose(file);
                return -1;
            }

            // Check if the palette is unique
            std::array<int, 2> key = { gn[0], gn[1] };
            if (uniquePals.find(key) == uniquePals.end()) {
                fseek(file, lofs + ofs, SEEK_SET);
                if (fread(sff->palList.palettes[i], sizeof(uint32_t), 256, file) != 256) {
                    fprintf(stderr, "Error reading palette data\n");
                    fclose(file);
                    return -1;
                }
                uniquePals[key] = i;
                sff->palList.paletteMap[i] = sff->palList.numPalettes;
                sff->palList.numPalettes++;
            } else {
                // If the palette is not unique, use the existing one
                // sff->palList.palettes[i] = sff->palList.palettes[uniquePals[key]];
                // sff->palList.paletteMap[i] = sff->palList.paletteMap[uniquePals[key]];
                printf("Palette %d(%d,%d) is not unique, using palette %d\nIncomplete code\n", i, gn[0], gn[1], uniquePals[key]);
            }
        }
    }

    // Allocate memory for sprites
    sff->sprites = (Sprite**) malloc(sff->header.NumberOfSprites * sizeof(Sprite*));
    Sprite* prev = NULL;
    long shofs = sff->header.FirstSpriteHeaderOffset;
    for (int i = 0; i < sff->header.NumberOfSprites; i++) {
        uint32_t xofs, size;
        uint16_t indexOfPrevious;
        sff->sprites[i] = newSprite();
        fseek(file, shofs, SEEK_SET);
        switch (sff->header.Ver0) {
        case 1:
            if (readSpriteHeaderV1(sff->sprites[i], file, &xofs, &size, &indexOfPrevious) != 0) {
                fclose(file);
                return -1;
            }
            break;
        case 2:
            if (readSpriteHeaderV2(sff->sprites[i], file, &xofs, &size, lofs, tofs, &indexOfPrevious) != 0) {
                fclose(file);
                return -1;
            }
            // printf("readSpriteHeaderV2(%d: %d,%d) xofs=%d size=%d lofs=%d tofs=%d indexOfPrevious=%d\n", i, sff->sprites[i]->Group, sff->sprites[i]->Number, xofs, size, lofs, tofs, indexOfPrevious);
            break;
        }
        if (size == 0) {
            if (indexOfPrevious < i) {
                Sprite* dst = sff->sprites[i];
                Sprite* src = sff->sprites[indexOfPrevious];
                spriteCopy(dst, src);
                printf("Info: Sprite[%d] use prev Sprite[%d]\n", i, indexOfPrevious);
            } else {
                printf("Warning: Sprite %d has no size\n", i);
                sff->sprites[i]->palidx = 0;
            }
        } else {
            switch (sff->header.Ver0) {
            case 1:
                if (sff->sprites[i]->Group == 0 && sff->sprites[i]->Number == 0) {
                    character = false;
                }
                // printf("Info: Sprite[%d] (%d,%d) offset=%d size=%d\n", i, sff->sprites[i]->Group, sff->sprites[i]->Number, shofs + 32, size);
                // printf("Sprite[%d] (%d,%d) ", i, sff->sprites[i]->Group, sff->sprites[i]->Number);
                if (readSpriteDataV1(sff->sprites[i], file, sff, shofs + 32, size, xofs, prev, &sff->palettes, character) != 0) {
                    fclose(file);
                    return -1;
                }
                
                break;
            case 2:
                if (readSpriteDataV2(sff->sprites[i], file, xofs, size, sff) != 0) {
                    fclose(file);
                    return -1;
                }
                break;
            }

            // if use previous sprite Group 9000 and Number 0 only (fix for SFF v1)
            if (sff->sprites[i]->Group == 9000) {
                if (sff->sprites[i]->Number == 0) {
                    prev = sff->sprites[i];
                }
            } else {
                prev = sff->sprites[i];
            }
        }
        // Prepare atlas data: dimension, offset, rects coords, etc.

        if (sff->header.Ver0 == 1) {
            shofs = xofs;
        } else {
            shofs += 28;
        }
    }
    fclose(file);
    return 0;
}

int initAtlas(Atlas *atlas, Sff *sff) {
    int64_t prod = 0;
    int crop = 1;
    int inpcrop = 1;
    size_t maxw = 0, maxh = 0;

    atlas->sff = sff;
    atlas->rects = (struct stbrp_rect*) malloc(sff->header.NumberOfSprites * sizeof(struct stbrp_rect));
    memset(atlas->rects, 0, sff->header.NumberOfSprites * sizeof(struct stbrp_rect));
    printf("\ninitAtlas\n");
    for (int i = 0; i < sff->header.NumberOfSprites; i++) {
        size_t x, y;
        int16_t sprite_width = sff->sprites[i]->Size[0];
        int16_t sprite_height = sff->sprites[i]->Size[1];
        uint8_t* p = sff->sprites[i]->data;

        if (!p) {
            printf("Warning: sprite %d has no data\n", i);
            continue;
        }

        sff->sprites[i]->atlas_x = 0;
        sff->sprites[i]->atlas_y = 0;
        if (sff->sprites[i]->Size[0] > maxw) {
            maxw = sff->sprites[i]->Size[0];
        }
        if (sff->sprites[i]->Size[1] > maxh) {
            maxh = sff->sprites[i]->Size[1];
        }
        prod += sff->sprites[i]->Size[0] * sff->sprites[i]->Size[1];
        /* crop input sprite to content */
        if(inpcrop) {
            for(y = 0; y < sff->sprites[i]->Size[1] && sprite_height > 0; y++) {
                // printf("i=%d x=%d y=%d spr_w=%d\n", i, x, y, sff->sprites[i]->Size[0]);
                for(x = 0; x < sff->sprites[i]->Size[0] && !p[(y * sff->sprites[i]->Size[0] + x)]; x++);
                if(x < sff->sprites[i]->Size[0]) break;
                sff->sprites[i]->atlas_y++; sprite_height--;
            }
            for(y = sff->sprites[i]->Size[1] - 1; y >= sff->sprites[i]->atlas_y && sprite_height > 0; y--) {
                for(x = 0; x < sff->sprites[i]->Size[0] && !p[(y * sff->sprites[i]->Size[0] + x)]; x++);
                if(x < sff->sprites[i]->Size[0]) break;
                sprite_height--;
            }
            for(x = 0; x < sff->sprites[i]->Size[0] && sprite_height > 0 && sprite_width > 0; x++) {
                for(y = 0; y < sprite_height && !p[((y + sff->sprites[i]->atlas_y) * sff->sprites[i]->Size[0] + x)]; y++);
                if(y < sprite_height) break;
                sff->sprites[i]->atlas_x++; sprite_width--;
            }
            for(x = sff->sprites[i]->Size[0] - 1; x >= sff->sprites[i]->atlas_x && sprite_height > 0 && sprite_width > 0; x--) {
                for(y = 0; y < sprite_height && !p[((y + sff->sprites[i]->atlas_y) * sff->sprites[i]->Size[0] + x)]; y++);
                if(y < sprite_height) break;
                sprite_width--;
            }
            if(sprite_width < 1 || sprite_height < 1) {
                sprite_width = sprite_height = sff->sprites[i]->atlas_x = sff->sprites[i]->atlas_y = 0;
            }
        }
        atlas->rects[i].id = i;
        atlas->rects[i].w = sprite_width;
        atlas->rects[i].h = sprite_height;

        printf("Sprite %d[%d,%d]: %dx%d -> %dx%d\n", i, sff->sprites[i]->Group, sff->sprites[i]->Number,sff->sprites[i]->Size[0], sff->sprites[i]->Size[1], sprite_width, sprite_height);
    }

    /* calculate an optimal atlas size */
    size_t i;
    for(i = 0; i * i < prod; i++);
    if(i < maxw) i = maxw;
    for(atlas->width = 1; atlas->width < i; atlas->width <<= 1);
    i = (prod + atlas->width - 1) / atlas->width;
    if(i < maxh) i = maxh;
    for(atlas->height = 1; atlas->height < i; atlas->height <<= 1);

    return 0;
}

int atlasSave(uint8_t *p, int w, int h, const char *fn, const char *meta) {
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
        texts[0].key = (png_charp)"Comment"; texts[0].text = (png_charp)meta;
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

int generateAtlas(Atlas *atlas) {
    int crop = 1, tofile = 1;
    uint8_t *o, *p, *src, *dst;
    char *meta, *s;
    stbrp_context ctx;
    stbrp_node *nodes;
    uint32_t i, j, num = atlas->sff->header.NumberOfSprites;

    nodes = (stbrp_node*)malloc((atlas->width + 1) * sizeof(stbrp_node));
    if(!nodes) { fprintf(stderr,"Not enough memory\n"); exit(1); }
    memset(nodes, 0, (atlas->width + 1) * sizeof(stbrp_node));
    stbrp_init_target(&ctx, atlas->width, atlas->height, nodes, atlas->width + 1);
    printf("Packing %u sprites into %u x %u atlas\n", num, atlas->width, atlas->height);
    if(!stbrp_pack_rects(&ctx, atlas->rects, num)) {
        atlas->height <<= 1;
        memset(nodes, 0, (atlas->width + 1) * sizeof(stbrp_node));
        for(i = 0; i < num; i++) atlas->rects[i].was_packed = atlas->rects[i].x = atlas->rects[i].y = 0;
        stbrp_init_target(&ctx, atlas->width, atlas->height, nodes, atlas->width + 1);
        if(stbrp_pack_rects(&ctx, atlas->rects, num)) goto ok;
        fprintf(stderr,"Error, sprites do not fit into %u x %u atlas.\n",atlas-> width, atlas->height);
        exit(2);
    }
ok:
    free(nodes);
    
    // Crop the atlas to the actual size of the sprites
    if(crop) atlas->width = atlas->height = 0;
    uint32_t l;
    for(i = l = 0; i < num; i++) {
        if(crop) {
            if(atlas->rects[i].x + atlas->rects[i].w > atlas->width) atlas->width = atlas->rects[i].x + atlas->rects[i].w;
            if(atlas->rects[i].y + atlas->rects[i].h > atlas->height) atlas->height = atlas->rects[i].y + atlas->rects[i].h;
        }
        l += 32 + 256;
    }

    // Check atlas size after cropping
    if(atlas->width <= 0 || atlas->height <= 0) {
        fprintf(stderr,"Error, nothing left after cropping atlas. Size= %u x %u\n",atlas-> width, atlas->height);
        return -2;
    }
    
    meta = s = (char*)malloc(l);
    if(!meta) { fprintf(stderr,"Not enough memory for meta data\n"); exit(1); }
    memset(meta, 0, l);
    o = (uint8_t*)malloc(atlas->width * atlas->height);
    if(!o) { fprintf(stderr,"Not enough memory for atlas output image data\n"); exit(1); }
    memset(o, 0, atlas->width * atlas->height);
    
    /* records */
    char filename[256];
    for(i = 0; i < num; i++) {
        snprintf(filename, sizeof(filename), "%d_%d.png", atlas->sff->sprites[i]->Group, atlas->sff->sprites[i]->Number);
        if(atlas->rects[i].w > 0 && atlas->rects[i].h > 0) {
            src = atlas->sff->sprites[i]->data + (atlas->sff->sprites[i]->atlas_y * atlas->sff->sprites[i]->Size[0] + atlas->sff->sprites[i]->atlas_x);
            dst = o + (atlas->width * atlas->rects[i].y + atlas->rects[i].x);
            for(j = 0; j < atlas->rects[i].h; j++, dst += atlas->width, src += atlas->sff->sprites[i]->Size[0])
                memcpy(dst, src, atlas->rects[i].w);
        }
        s += sprintf(s, "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\r\n",
            atlas->rects[i].x, atlas->rects[i].y, atlas->rects[i].w, atlas->rects[i].h,
            atlas->sff->sprites[i]->atlas_x, atlas->sff->sprites[i]->atlas_y, atlas->sff->sprites[i]->Size[0], atlas->sff->sprites[i]->Size[1],
            filename);
    }
    
    if (atlas->sff->header.Ver0 == 1) {
        save_as_png("sprite_atlas.png", atlas->width, atlas->height, o, atlas->sff->palettes[atlas->sff->sprites[0]->palidx]);
    } else {
        uint32_t* sff_palette = atlas->sff->palList.palettes[atlas->sff->sprites[0]->palidx];
        png_color png_palette[256];
        for (int i = 0; i < 256; i++) {
            png_palette[i].red = (sff_palette[i] >> 0) & 0xFF;
            png_palette[i].green = (sff_palette[i] >> 8) & 0xFF;
            png_palette[i].blue = (sff_palette[i] >> 16) & 0xFF;
        }
        save_as_png("sprite_atlas.png", atlas->width, atlas->height, o, png_palette);
    }
    free(o);
    /* save meta info to a separate file too */
    if(tofile) {
        FILE* f = fopen("sprite_atlas.txt", "wb+");
        if(f) {
            fwrite(meta, 1, s - meta, f);
            fclose(f);
        }
    }
    free(meta);
    return 0;
}

int deinitAtlas(Atlas *atlas) {
    free(atlas->rects);
    return 0;
}

void freeSff(Sff* sff) {
    // clean up sprite
    for (int i = 0; i < sff->header.NumberOfSprites; i++) {
        if (sff->sprites[i]->data) free(sff->sprites[i]->data);
        free(sff->sprites[i]);
    }
    free(sff->sprites);

    // clean up sff->palettes
    for (png_color* palette : sff->palettes) {
        delete[] palette;
    }
    sff->palettes.clear();
}

void printAtlas(Atlas* atlas) {
    printf("Atlas size: %d x %d\n", atlas->width, atlas->height);
    for (int i = 0; i < atlas->sff->header.NumberOfSprites; i++) {
        printf("Sprite %d: %dx%d -> %dx%d\n", i, atlas->sff->sprites[i]->Size[0], atlas->sff->sprites[i]->Size[1], atlas->rects[i].w, atlas->rects[i].h);
    }
}

void printSff(Sff* sff) {
    printf("SFF file: %s\n", sff->filename);
    printf("Number of sprites: %d\n", sff->header.NumberOfSprites);
    printf("Number of palettes: %d\n", sff->header.NumberOfPalettes);
    for (int i = 0; i < sff->header.NumberOfSprites; i++) {
        printf("Sprite %d: Group %d, Number %d, Size %dx%d\n", i, sff->sprites[i]->Group, sff->sprites[i]->Number, sff->sprites[i]->Size[0], sff->sprites[i]->Size[1]);
    }
}

int main(int argc, char* argv[]) {
    Atlas atlas;
    Sff sff;

    if (argc < 2) {
        // iterate current directory with sff file
        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            if (strcasecmp(entry.path().extension().string().c_str(), ".sff") == 0) {
                extractSff(&sff, entry.path().string().c_str());
            }
        }
    } else {
        // iterate all arguments
        for (int i = 1; i < argc; i++) {
            extractSff(&sff, argv[i]);
        }
    }

    initAtlas(&atlas, &sff);
    // printSff(&sff);
    // printAtlas(&atlas);
    generateAtlas(&atlas);
    freeSff(&sff);
    deinitAtlas(&atlas);
    
    return 0;
}
