#include "tgaimage.h"
#include <fstream>
#include <cstring>
#include <algorithm>

#pragma pack(push,1)
struct TGAHeader {
    uint8_t  idlength = 0;
    uint8_t  colormaptype = 0;
    uint8_t  datatypecode = 2; 
    uint16_t colormaporigin = 0;
    uint16_t colormaplength = 0;
    uint8_t  colormapdepth = 0;
    uint16_t x_origin = 0;
    uint16_t y_origin = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  bitsperpixel = 24;
    uint8_t  imagedescriptor = 0x20; 
};
#pragma pack(pop)

TGAImage::TGAImage(int w, int h, int bpp) : width(w), height(h), bytespp(bpp) {
    data.assign((size_t)width*height*bytespp, 0);
}

bool TGAImage::read_tga_file(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) return false;

    TGAHeader header{};
    in.read((char*)&header, sizeof(header));
    if (!in) return false;

    if (header.datatypecode != 2) return false; 

    width  = header.width;
    height = header.height;
    bytespp = header.bitsperpixel / 8;
    if (bytespp != 3 && bytespp != 4) return false;

    if (header.idlength) in.seekg(header.idlength, std::ios::cur);

    data.resize((size_t)width*height*bytespp);
    in.read((char*)data.data(), (std::streamsize)data.size());
    if (!in) return false;

    bool origin_bottom = (header.imagedescriptor & 0x20) == 0;
    if (origin_bottom) flip_vertically();

    return true;
}

bool TGAImage::write_tga_file(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;

    TGAHeader header{};
    header.datatypecode = 2;
    header.width  = (uint16_t)width;
    header.height = (uint16_t)height;
    header.bitsperpixel = (uint8_t)(bytespp*8);
    header.imagedescriptor = 0x20;

    out.write((char*)&header, sizeof(header));
    out.write((char*)data.data(), (std::streamsize)data.size());
    return (bool)out;
}

void TGAImage::set(int x, int y, const TGAColor& c) {
    if (x<0 || y<0 || x>=width || y>=height) return;
    size_t idx = ((size_t)y*width + x) * bytespp;
    data[idx + 0] = c.bgra[0];
    data[idx + 1] = c.bgra[1];
    data[idx + 2] = c.bgra[2];
    if (bytespp == 4) data[idx + 3] = c.bgra[3];
}

TGAColor TGAImage::get(int x, int y) const {
    TGAColor c;
    if (x<0 || y<0 || x>=width || y>=height) return c;
    size_t idx = ((size_t)y*width + x) * bytespp;
    c.bytespp = (uint8_t)bytespp;
    c.bgra[0] = data[idx+0];
    c.bgra[1] = data[idx+1];
    c.bgra[2] = data[idx+2];
    c.bgra[3] = (bytespp==4) ? data[idx+3] : 255;
    return c;
}

void TGAImage::flip_vertically() {
    if (width<=0 || height<=0) return;
    size_t bytes_per_line = (size_t)width * bytespp;
    std::vector<uint8_t> line(bytes_per_line);

    for (int y=0; y<height/2; y++) {
        uint8_t* top = data.data() + (size_t)y * bytes_per_line;
        uint8_t* bot = data.data() + (size_t)(height-1-y) * bytes_per_line;

        std::memcpy(line.data(), top, bytes_per_line);
        std::memcpy(top, bot, bytes_per_line);
        std::memcpy(bot, line.data(), bytes_per_line);
    }
}
