#pragma once
#include <cstdint>
#include <vector>
#include <string>

struct TGAColor {
    uint8_t bgra[4] = {0,0,0,255};
    uint8_t bytespp = 4;

    TGAColor() = default;
    TGAColor(uint8_t R, uint8_t G, uint8_t B, uint8_t A=255) {
        bgra[0]=B; bgra[1]=G; bgra[2]=R; bgra[3]=A;
        bytespp = 4;
    }
};

class TGAImage {
public:
    enum Format { GRAYSCALE=1, RGB=3, RGBA=4 };

    TGAImage() = default;
    TGAImage(int w, int h, int bpp);

    bool read_tga_file(const std::string& filename);   
    bool write_tga_file(const std::string& filename) const;

    int get_width()  const { return width; }
    int get_height() const { return height; }
    int get_bytespp() const { return bytespp; }

    void set(int x, int y, const TGAColor& c);
    TGAColor get(int x, int y) const;

    void flip_vertically();

private:
    int width  = 0;
    int height = 0;
    int bytespp = 0; // 3 or 4
    std::vector<uint8_t> data;
};
