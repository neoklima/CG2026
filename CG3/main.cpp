#include <iostream>
#include <vector>
#include <limits>
#include <algorithm>
#include <cmath>

#include "tgaimage.h"
#include "model.h"
#include "geometry.h"
#include "openfile.h"

const int width  = 800;
const int height = 800;

TGAColor blend_over(const TGAColor& dst, const TGAColor& src, float a) {
    int r = (int)(dst.bgra[2]*(1.f-a) + src.bgra[2]*a);
    int g = (int)(dst.bgra[1]*(1.f-a) + src.bgra[1]*a);
    int b = (int)(dst.bgra[0]*(1.f-a) + src.bgra[0]*a);
    r = std::clamp(r,0,255); g = std::clamp(g,0,255); b = std::clamp(b,0,255);
    return TGAColor((uint8_t)r,(uint8_t)g,(uint8_t)b,255);
}

// проекция вершины 
Vec3f project_to_screen(const Vec3f& v_world, const Mat4& V, const Mat4& P) {
    Vec4f vw(v_world.x, v_world.y, v_world.z, 1.f);

    Vec4f vview4 = V * vw;               
    float depth = -vview4.z;             

    Vec4f vclip = P * vview4;            
    if (std::abs(vclip.w) < 1e-8f) vclip.w = 1.f;

    float ndc_x = vclip.x / vclip.w;
    float ndc_y = vclip.y / vclip.w;

    float sx = (ndc_x + 1.f) * width  * 0.5f;
    float sy = (ndc_y + 1.f) * height * 0.5f;

    return Vec3f(sx, sy, depth);
}

// треугольник головы
void triangle_textured(Vec3f* pts, Vec2f* uv, TGAImage& img, TGAImage& tex, float* zbuf) {
    Vec2i bmin(width-1, height-1), bmax(0,0);
    for(int i=0;i<3;i++){
        bmin.x = std::max(0, std::min(bmin.x, (int)pts[i].x));
        bmin.y = std::max(0, std::min(bmin.y, (int)pts[i].y));
        bmax.x = std::min(width-1, std::max(bmax.x, (int)pts[i].x));
        bmax.y = std::min(height-1, std::max(bmax.y, (int)pts[i].y));
    }

    for(int x=bmin.x; x<=bmax.x; x++){
        for(int y=bmin.y; y<=bmax.y; y++){
            Vec3f bc = barycentric(pts, Vec3f((float)x,(float)y,0));
            if(bc.x<0 || bc.y<0 || bc.z<0) continue;

            float z = pts[0].z*bc.x + pts[1].z*bc.y + pts[2].z*bc.z;
            int idx = x + y*width;

            if (z > zbuf[idx]) {
                zbuf[idx] = z;

                Vec2f uvp(
                    uv[0].x*bc.x + uv[1].x*bc.y + uv[2].x*bc.z,
                    uv[0].y*bc.x + uv[1].y*bc.y + uv[2].y*bc.z
                );

                int tx = (int)(uvp.x * (tex.get_width()-1));
                int ty = (int)((1.f-uvp.y) * (tex.get_height()-1));
                img.set(x,y, tex.get(tx,ty));
            }
        }
    }
}


void triangle_alpha_ztest(Vec3f* pts, TGAImage& img, const TGAColor& col, float alpha, const float* zbuf) {
    Vec2i bmin(width-1, height-1), bmax(0,0);
    for(int i=0;i<3;i++){
        bmin.x = std::max(0, std::min(bmin.x, (int)pts[i].x));
        bmin.y = std::max(0, std::min(bmin.y, (int)pts[i].y));
        bmax.x = std::min(width-1, std::max(bmax.x, (int)pts[i].x));
        bmax.y = std::min(height-1, std::max(bmax.y, (int)pts[i].y));
    }

    for(int x=bmin.x; x<=bmax.x; x++){
        for(int y=bmin.y; y<=bmax.y; y++){
            Vec3f bc = barycentric(pts, Vec3f((float)x,(float)y,0));
            if(bc.x<0 || bc.y<0 || bc.z<0) continue;

            float z = pts[0].z*bc.x + pts[1].z*bc.y + pts[2].z*bc.z;
            int idx = x + y*width;

            
            if (z > zbuf[idx]) {
                TGAColor dst = img.get(x,y);
                img.set(x,y, blend_over(dst, col, alpha));
            }
        }
    }
}


struct Face {
    int a,b,c,d;
    float z;
    bool front;
    float alphaMul; 
};

static void build_cube_faces(std::vector<Face>& faces) {
    faces.clear();

    
    faces.push_back({0,1,2,3,0,false,1.0f}); 
    faces.push_back({4,5,6,7,0,false,1.0f});  
    faces.push_back({0,1,5,4,0,false,0.25f}); 
    faces.push_back({2,3,7,6,0,false,1.0f});  
    faces.push_back({1,2,6,5,0,false,1.0f});  
    faces.push_back({0,3,7,4,0,false,1.0f});  
}

static Vec3f face_normal_world(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    return normalize(cross(b-a, c-a));
}


void draw_cube_pass_ztest(TGAImage& img,
                          const std::vector<Vec3f>& Vs,   
                          std::vector<Face> faces,
                          bool wantFront,
                          const TGAColor& col,
                          float alpha,
                          const float* zbuf) {
    for (auto& f : faces) {
        f.z = (Vs[f.a].z + Vs[f.b].z + Vs[f.c].z + Vs[f.d].z) * 0.25f;
    }

    
    std::sort(faces.begin(), faces.end(), [](const Face& x, const Face& y){
        return x.z < y.z;
    });

    for (const auto& f : faces) {
        if (f.front != wantFront) continue;

        Vec3f p0 = Vs[f.a], p1 = Vs[f.b], p2 = Vs[f.c], p3 = Vs[f.d];
        Vec3f t1[3] = { p0, p1, p2 };
        Vec3f t2[3] = { p0, p2, p3 };

        float a = alpha * f.alphaMul; 

        triangle_alpha_ztest(t1, img, col, a, zbuf);
        triangle_alpha_ztest(t2, img, col, a, zbuf);
    }
}


void line(Vec2i p0, Vec2i p1, TGAImage& image, const TGAColor& color) {
    bool steep = false;
    if (std::abs(p0.x - p1.x) < std::abs(p0.y - p1.y)) {
        std::swap(p0.x, p0.y);
        std::swap(p1.x, p1.y);
        steep = true;
    }
    if (p0.x > p1.x) std::swap(p0, p1);

    int dx = p1.x - p0.x;
    int dy = std::abs(p1.y - p0.y);
    int err = dx / 2;
    int y = p0.y;
    int ystep = (p0.y < p1.y) ? 1 : -1;

    for (int x = p0.x; x <= p1.x; x++) {
        if (steep) image.set(y, x, color);
        else       image.set(x, y, color);

        err -= dy;
        if (err < 0) { y += ystep; err += dx; }
    }
}

void drawCubeEdges(const std::vector<Vec3f>& cubeS, TGAImage& image, const TGAColor& color) {
    static const int E[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };

    for (int i=0;i<12;i++) {
        int a = E[i][0];
        int b = E[i][1];
        Vec2i p0((int)cubeS[a].x, (int)cubeS[a].y);
        Vec2i p1((int)cubeS[b].x, (int)cubeS[b].y);
        line(p0, p1, image, color);
    }
}

int main() {
    // texture
    TGAImage texture;
    if (!texture.read_tga_file("resources/african_head_diffuse.tga")) {
        std::cout << "Can't read texture (need uncompressed TGA)\n";
        return 1;
    }

    // model
    Model model("resources/african_head.obj");
    if (model.nfaces()==0) {
        std::cout << "Model load failed\n";
        return 1;
    }

    float headScale = 1.0f;

    
    Vec3f bbmin( 1e9f, 1e9f, 1e9f);
    Vec3f bbmax(-1e9f,-1e9f,-1e9f);

    for (int i=0; i<model.nverts(); i++) {
        Vec3f v = model.vert(i);
        bbmin.x = std::min(bbmin.x, v.x);
        bbmin.y = std::min(bbmin.y, v.y);
        bbmin.z = std::min(bbmin.z, v.z);
        bbmax.x = std::max(bbmax.x, v.x);
        bbmax.y = std::max(bbmax.y, v.y);
        bbmax.z = std::max(bbmax.z, v.z);
    }
    Vec3f headCenter((bbmin.x+bbmax.x)*0.5f, (bbmin.y+bbmax.y)*0.5f, (bbmin.z+bbmax.z)*0.5f);

    
    Camera cam(
        Vec3f(2.8f, 1.8f, 3.8f), 
        headCenter,              
        Vec3f(0.f, 1.f, 0.f),    
        50.f,
        (float)width/(float)height,
        0.1f,
        100.f
    );

    Mat4 V = cam.view();
    Mat4 P = cam.proj();

    TGAImage image(width, height, TGAImage::RGB);

    
    float* zbuffer = new float[width*height];
    for(int i=0;i<width*height;i++) zbuffer[i] = -std::numeric_limits<float>::max();

    
    float cubeSize = 1.25f;
    Vec3f C = headCenter * headScale;

    std::vector<Vec3f> cubeW = {
        C + Vec3f(-cubeSize,-cubeSize,-cubeSize),
        C + Vec3f( cubeSize,-cubeSize,-cubeSize),
        C + Vec3f( cubeSize, cubeSize,-cubeSize),
        C + Vec3f(-cubeSize, cubeSize,-cubeSize),
        C + Vec3f(-cubeSize,-cubeSize, cubeSize),
        C + Vec3f( cubeSize,-cubeSize, cubeSize),
        C + Vec3f( cubeSize, cubeSize, cubeSize),
        C + Vec3f(-cubeSize, cubeSize, cubeSize)
    };

    
    std::vector<Vec3f> cubeS(8);
    for(int i=0;i<8;i++) cubeS[i] = project_to_screen(cubeW[i], V, P);

    
    std::vector<Face> faces;
    build_cube_faces(faces);
    for (auto& f : faces) {
        Vec3f a = cubeW[f.a], b = cubeW[f.b], c = cubeW[f.c];
        Vec3f n = face_normal_world(a,b,c);
        Vec3f center = (cubeW[f.a] + cubeW[f.b] + cubeW[f.c] + cubeW[f.d]) * 0.25f;
        Vec3f toCam = normalize(cam.eye - center);
        f.front = (dot(n, toCam) > 0.f);
    }

    
    TGAColor cubeBlue(50, 130, 255, 255);

    
    float alphaBack  = 0.10f;
    float alphaFront = 0.22f;

    
    draw_cube_pass_ztest(image, cubeS, faces, false, cubeBlue, alphaBack, zbuffer);

    
    for (int i=0; i<model.nfaces(); i++) {
        auto f = model.face(i);

        Vec3f pts[3];
        Vec2f uv[3];

        for (int j=0; j<3; j++) {
            int vidx = f[j*2];
            int tidx = f[j*2 + 1];

            Vec3f v = model.vert(vidx) * headScale;

            
            v = Vec3f(-v.x, v.y, -v.z);

            pts[j] = project_to_screen(v, V, P);
            uv[j]  = model.uv(tidx);
        }

        triangle_textured(pts, uv, image, texture, zbuffer);
    }

    
    draw_cube_pass_ztest(image, cubeS, faces, true, cubeBlue, alphaFront, zbuffer);

   
    TGAColor edgeBlue(15, 70, 190, 255);
    drawCubeEdges(cubeS, image, edgeBlue);

    image.flip_vertically();
    image.write_tga_file("output.tga");
    openImage("output.tga");

    delete[] zbuffer;
    return 0;
}
