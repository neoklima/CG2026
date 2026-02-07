#pragma once
#include <vector>
#include <string>
#include "geometry.h"

class Model {
public:
    Model(const char* filename);

    int nfaces() const { return (int)faces_.size(); }
    int nverts() const { return (int)verts_.size(); }
    int nuv()    const { return (int)uv_.size(); }

    std::vector<int> face(int idx) const { return faces_[idx]; }

    Vec3f vert(int i) const;
    Vec2f uv(int i) const;

private:
    std::vector<Vec3f> verts_;
    std::vector<Vec2f> uv_;

   
    std::vector<std::vector<int>> faces_;
};
