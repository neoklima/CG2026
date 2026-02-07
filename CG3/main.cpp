#include <iostream>
#include <limits>
#include "tgaimage.h"
#include "model.h"
#include "geometry.h"
#include "openfile.h"

const int width  = 800;
const int height = 800;

// растеризация треугольника по барицентрическим координатам
void triangle(Vec3f* pts, Vec2f* uv, TGAImage& image, TGAImage& texture, float* zbuffer) {
    Vec2i bboxmin(width-1, height-1);
    Vec2i bboxmax(0,0);

    for(int i=0;i<3;i++){
        bboxmin.x = std::max(0, std::min(bboxmin.x, (int)pts[i].x));
        bboxmin.y = std::max(0, std::min(bboxmin.y, (int)pts[i].y));
        bboxmax.x = std::min(width-1, std::max(bboxmax.x, (int)pts[i].x));
        bboxmax.y = std::min(height-1, std::max(bboxmax.y, (int)pts[i].y));
    }

    for(int x=bboxmin.x;x<=bboxmax.x;x++){
        for(int y=bboxmin.y;y<=bboxmax.y;y++){
            Vec3f bc = barycentric(pts, Vec3f((float)x,(float)y,0));
            if(bc.x<0||bc.y<0||bc.z<0) continue;

            float z = pts[0].z*bc.x + pts[1].z*bc.y + pts[2].z*bc.z;
            int idx = x + y*width;

            if(zbuffer[idx] < z){
                zbuffer[idx] = z;

                Vec2f uvp(
                    uv[0].x*bc.x + uv[1].x*bc.y + uv[2].x*bc.z,
                    uv[0].y*bc.x + uv[1].y*bc.y + uv[2].y*bc.z
                );

                int tx = (int)(uvp.x * (texture.get_width()-1));
                int ty = (int)((1.f-uvp.y) * (texture.get_height()-1));

                image.set(x,y, texture.get(tx,ty));
            }
        }
    }
}

int main() {
    // текстура
    TGAImage texture;
    if(!texture.read_tga_file("resources/african_head_diffuse.tga")) {
        std::cout << "Can't read texture (need uncompressed TGA)\n";
        return 1;
    }

    // модель
    Model model("resources/african_head.obj");
    if(model.nfaces()==0 || model.nverts()==0) {
        std::cout << "Model load failed\n";
        return 1;
    }

    TGAImage image(width,height,TGAImage::RGB);

    // Z-buffer
    float* zbuffer = new float[width*height];
    for(int i=0;i<width*height;i++)
        zbuffer[i] = -std::numeric_limits<float>::max();

    // проходим по всем треугольникам модели
    for(int i=0;i<model.nfaces();i++){
        auto f = model.face(i);

        Vec3f pts[3];
        Vec2f uv[3];

        for(int j=0;j<3;j++){
            int vidx = f[j*2];
            int tidx = f[j*2+1];

            Vec3f v = model.vert(vidx);
            pts[j] = Vec3f(
                (v.x+1.f)*width/2.f,
                (v.y+1.f)*height/2.f,
                v.z
            );
            uv[j] = model.uv(tidx);
        }

        triangle(pts, uv, image, texture, zbuffer);
    }

    image.flip_vertically();
    image.write_tga_file("output.tga");
    openImage("output.tga");

    delete[] zbuffer;
    return 0;
}
