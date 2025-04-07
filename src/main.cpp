/*
RELEASE BUILD:
g++ -O3 -DNDEBUG -o sffcli.exe src/main.cpp -lpng

DEBUG BUILD:
g++ -fsanitize=address -static-libasan -g -o sffcli.exe src/main.cpp -lpng
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <map>
#include <array>
#include <vector>
#include "png.h"

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
} Sprite;

typedef struct {
    SffHeader header;
    Sprite** sprites;
    PaletteList palList;
    char filename[256];
    // std::vector<std::array<png_color, 256>> palettes;
    std::vector<png_color*> palettes;
} Sff;

Sprite* newSprite() {
    Sprite* sprite = (Sprite*) malloc(sizeof(Sprite));
    memset(sprite, 0, sizeof(Sprite));
    sprite->palidx = -1;
    return sprite;
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
        fprintf(stderr, "Failed to open file for writing\n");
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
    snprintf(pngFilename, sizeof(pngFilename), "kfm %d %d.png", s->Group, s->Number);

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
        save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
        free(px);
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
        save_as_png(pngFilename, s->Size[0], s->Size[1], px, png_palette);
        free(px);
    }
    // printf("[DEBUG] src/main.cpp:%d ps=%d paletteSame=%d palidx=%d palLen=%d palSize=%d srcLen=%ld\n", __LINE__, ps, paletteSame, s->palidx, palettes->size(), palSize, srcLen);
    // printf("%u\n", palHash);

    return 0;
}

void save_png(Sprite* s, FILE* file, uint32_t data_size, Sff* sff){
    char pngFilename[256];
    snprintf(pngFilename, sizeof(pngFilename), "kfm %d %d.png", s->Group, s->Number);
    
    // Create a PNG file
    FILE* pngFile = fopen(pngFilename, "wb");
    if (!pngFile) {
        fprintf(stderr, "Error creating PNG file %s\n", pngFilename);
        return;
    }    
    char *buffer = (char *)malloc(data_size-4);
    if (!buffer) {
        // Handle memory allocation failure
        fprintf(stderr, "Error allocating memory for PNG data\n");
        fclose(pngFile);
        return;
    }

    size_t read = fread(buffer, 1, data_size-4, file);
    if (read > 0) {
        fwrite(buffer, 1, read, pngFile);
    }
    free(buffer);
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
        snprintf(pngFilename, sizeof(pngFilename), "kfm %d %d.png", s->Group, s->Number);

        switch (format) {
        case 2:
            // printf("Decoding sprite with RLE8\n");
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
                free(px);
            } else {
                fprintf(stderr, "Error decoding RLE8 sprite data\n");
                return -1;
            }
            break;
        case 3:
            // printf("Decoding sprite with RLE5\n");
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
                free(px);
            } else {
                fprintf(stderr, "Error decoding RLE5 sprite data\n");
                return -1;
            }
            break;
        case 4:
            // printf("Decoding sprite with LZ55 palidx=%d\n", s->palidx);
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
                free(px);
            } else {
                fprintf(stderr, "Error decoding LZ5 sprite data\n");
                return -1;
            }
            break;
        case 10:
            printf("Decoding sprite with PNG10: ");
            save_png(s, file, datasize, sff);
            break;
        case 11:
            printf("Decoding sprite with PNG11: ");
            save_png(s, file, datasize, sff);
            break;
        case 12:
            printf("Decoding sprite with PNG12: ");
            save_png(s, file, datasize, sff);
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
    strncpy(sff->filename, "kfmZ.sff", sizeof(sff->filename) - 1);

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
            printf("Palette %d: Group %d, Number %d, ColNumber %d\n", i, gn[0], gn[1], gn[2]);

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
                printf("Palette %d(%d,%d) is not unique, using palette %d\nIncomplete code", i, gn[0], gn[1], uniquePals[key]);
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
                // printf("Info: Sprite[%d] use prev Sprite[%d]\n", i, indexOfPrevious);
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
            prev = sff->sprites[i];
        }
        if (sff->header.Ver0 == 1) {
            shofs = xofs;
        } else {
            shofs += 28;
        }
    }
    fclose(file);

    // clean up sprite
    for (int i = 0; i < sff->header.NumberOfSprites; i++) {
        free(sff->sprites[i]);
    }
    free(sff->sprites);

    // clean up sff->palettes
    for (png_color* palette : sff->palettes) {
        delete[] palette;
    }
    sff->palettes.clear();

    return 0;
}

int main(int argc, char* argv[]) {
    Sff sff;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return -1;
    }
    extractSff(&sff, argv[1]);
    return 0;
}
