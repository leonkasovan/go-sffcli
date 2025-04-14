// Description: Merges two indexed PNG images with palettes, removing duplicates and remapping pixels.
// Compile: g++ -o merge_png.exe src/merge_png.cpp -lpng -fopenmp -std=c++11
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <functional>
#include <thread>
#include "png.h"

// Structure to hold RGB color
struct RGB {
    uint8_t r, g, b;

    // Define operator< for ordering (already present)
    bool operator<(const RGB& other) const {
        return std::tie(r, g, b) < std::tie(other.r, other.g, other.b);
    }

    // Define operator== for equality comparison
    bool operator==(const RGB& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
};

// Define a custom hash function for RGB
namespace std {
    template <>
    struct hash<RGB> {
        size_t operator()(const RGB& color) const {
            return (static_cast<size_t>(color.r) << 16) |
                   (static_cast<size_t>(color.g) << 8) |
                   static_cast<size_t>(color.b);
        }
    };
}

// Function to load PNG and extract pixel data and palette
bool load_png(const char* filename, std::vector<uint8_t>& pixels, std::vector<RGB>& palette, int& width, int& height) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename);
        return false;
    }

    uint8_t header[8];
    fread(header, 1, 8, fp);
    if (png_sig_cmp(header, 0, 8)) {
        fprintf(stderr, "Error: File %s is not a valid PNG\n", filename);
        fclose(fp);
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (color_type != PNG_COLOR_TYPE_PALETTE || bit_depth != 8) {
        fprintf(stderr, "Error: Only 8-bit indexed PNGs are supported\n");
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return false;
    }

    png_colorp png_palette;
    int num_palette;
    png_get_PLTE(png, info, &png_palette, &num_palette);

    palette.clear();
    for (int i = 0; i < num_palette; i++) {
        palette.push_back({png_palette[i].red, png_palette[i].green, png_palette[i].blue});
    }

    png_bytep* row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
    pixels.resize(width * height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = &pixels[y * width];
    }

    png_read_image(png, row_pointers);
    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return true;
}

// Function to save PNG with a new palette
bool save_png(const char* filename, const std::vector<uint8_t>& pixels, const std::vector<RGB>& palette, int width, int height) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open file %s for writing\n", filename);
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);

    png_set_IHDR(
        png, info, width, height, 8, PNG_COLOR_TYPE_PALETTE,
        PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );

    png_color png_palette[256];
    for (size_t i = 0; i < palette.size(); i++) {
        png_palette[i].red = palette[i].r;
        png_palette[i].green = palette[i].g;
        png_palette[i].blue = palette[i].b;
    }
    png_set_PLTE(png, info, png_palette, palette.size());

    png_byte trans[256];
    memset(trans, 255, 256);
    trans[0] = 0; // Set palette index 0 to be transparent
    png_set_tRNS(png, info, trans, palette.size(), NULL);

    png_write_info(png, info);

    png_bytep rows[height];
    for (int y = 0; y < height; y++) {
        rows[y] = (png_bytep) &pixels[y * width];
    }

    png_write_image(png, rows);
    png_write_end(png, NULL);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

// Helper function to calculate the Euclidean distance between two colors
double color_distance(const RGB& c1, const RGB& c2) {
    return std::sqrt(
        (c1.r - c2.r) * (c1.r - c2.r) +
        (c1.g - c2.g) * (c1.g - c2.g) +
        (c1.b - c2.b) * (c1.b - c2.b)
    );
}

// Function to quantize the palette to 256 colors
void quantize_palette(const std::vector<RGB>& input_palette, std::vector<RGB>& output_palette) {
    output_palette.clear();
    if (input_palette.size() <= 256) {
        output_palette = input_palette;
        return;
    }

    // Use a simple clustering approach (e.g., K-Means or Median Cut)
    // Here, we use a naive approach: pick the first 256 colors
    for (size_t i = 0; i < 256; i++) {
        output_palette.push_back(input_palette[i]);
    }
}

// Helper function to find the nearest color in the merged palette
uint8_t find_nearest_color(const RGB& color, const std::vector<RGB>& merged_palette) {
    double min_distance = std::numeric_limits<double>::max();
    uint8_t best_index = 0;
    for (size_t i = 0; i < merged_palette.size(); i++) {
        double distance = color_distance(color, merged_palette[i]);
        if (distance < min_distance) {
            min_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

// Optimized function to merge palettes and remap pixels
void merge_palettes_and_remap(
    const std::vector<RGB>& palette1, const std::vector<uint8_t>& pixels1, int width1, int height1,
    const std::vector<RGB>& palette2, const std::vector<uint8_t>& pixels2, int width2, int height2,
    std::vector<RGB>& merged_palette, std::vector<uint8_t>& remapped_pixels1, std::vector<uint8_t>& remapped_pixels2
) {
    // Combine palettes
    std::vector<RGB> combined_palette = palette1;
    combined_palette.insert(combined_palette.end(), palette2.begin(), palette2.end());

    // Remove duplicate colors
    std::unordered_map<RGB, uint8_t> color_to_index;
    std::vector<RGB> unique_palette;
    for (const auto& color : combined_palette) {
        if (color_to_index.find(color) == color_to_index.end()) {
            color_to_index[color] = unique_palette.size();
            unique_palette.push_back(color);
        }
    }

    // Quantize the palette to 256 colors
    quantize_palette(unique_palette, merged_palette);

    // Precompute nearest colors for all unique colors in the original palettes
    std::unordered_map<RGB, uint8_t> nearest_color_cache;
    for (const auto& color : unique_palette) {
        nearest_color_cache[color] = find_nearest_color(color, merged_palette);
    }

    // Function to remap pixels for a single image
    auto remap_pixels = [&](const std::vector<uint8_t>& pixels, const std::vector<RGB>& palette, int width, int height, std::vector<uint8_t>& remapped_pixels) {
        remapped_pixels.resize(width * height);
        #pragma omp parallel for // Use OpenMP for parallel processing
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                RGB color = palette[pixels[y * width + x]];
                remapped_pixels[y * width + x] = nearest_color_cache[color];
            }
        }
    };

    // Remap pixels for both images
    remap_pixels(pixels1, palette1, width1, height1, remapped_pixels1);
    remap_pixels(pixels2, palette2, width2, height2, remapped_pixels2);
}

int main(int argc, char* argv[]) {
    // Check if two filenames are provided
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <image1.png> <image2.png>\n", argv[0]);
        return 1;
    }

    const char* file1 = argv[1];
    const char* file2 = argv[2];

    // Load two PNGs
    std::vector<uint8_t> pixels1, pixels2;
    std::vector<RGB> palette1, palette2;
    int width1, height1, width2, height2;

    printf("Loading images...\n");
    if (!load_png(file1, pixels1, palette1, width1, height1)) return 1;
    if (!load_png(file2, pixels2, palette2, width2, height2)) return 1;

    // Merge palettes and remap pixels
    std::vector<RGB> merged_palette;
    std::vector<uint8_t> remapped_pixels1, remapped_pixels2;

    printf("Merging palettes and remapping pixels...\n");
    merge_palettes_and_remap(
        palette1, pixels1, width1, height1,
        palette2, pixels2, width2, height2,
        merged_palette, remapped_pixels1, remapped_pixels2
    );

    // Save the updated images with the new shared palette
    printf("Saving updated images...\n");
    if (!save_png(file1, remapped_pixels1, merged_palette, width1, height1)) return 1;
    if (!save_png(file2, remapped_pixels2, merged_palette, width2, height2)) return 1;

    printf("Updated images saved with remapped colors and shared palette.\n");
    return 0;
}