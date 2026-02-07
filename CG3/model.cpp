#include "model.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>   // sscanf
#include <iostream>

// парсер OBJ:
// v  x y z
// vt u v
Model::Model(const char* filename) {
    std::ifstream in(filename);
    if(!in) {
        std::cerr << "Cannot open OBJ: " << filename << "\n";
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        if (tag == "v") {
            Vec3f v;
            iss >> v.x >> v.y >> v.z;
            verts_.push_back(v);
        }
        else if (tag == "vt") {
            Vec2f t;
            iss >> t.x >> t.y;
            uv_.push_back(t);
        }
        else if (tag == "f") {
            std::vector<int> v_idx;
            std::vector<int> vt_idx;

            std::string token;
            while (iss >> token) {
                int v=0, vt=0, vn=0;

                // v/vt/vn
                if (sscanf(token.c_str(), "%d/%d/%d", &v, &vt, &vn) == 3) {
                    v_idx.push_back(v-1);
                    vt_idx.push_back(vt-1);
                }
                // v//vn
                else if (sscanf(token.c_str(), "%d//%d", &v, &vn) == 2) {
                    v_idx.push_back(v-1);
                    vt_idx.push_back(0);
                }
                // v/vt
                else if (sscanf(token.c_str(), "%d/%d", &v, &vt) == 2) {
                    v_idx.push_back(v-1);
                    vt_idx.push_back(vt-1);
                }
                // v
                else if (sscanf(token.c_str(), "%d", &v) == 1) {
                    v_idx.push_back(v-1);
                    vt_idx.push_back(0);
                }
            }

            if ((int)v_idx.size() < 3) continue;

            // Триангуляция
            for (int i=1; i+1 < (int)v_idx.size(); i++) {
                std::vector<int> f;
                f.push_back(v_idx[0]);   f.push_back(vt_idx[0]);
                f.push_back(v_idx[i]);   f.push_back(vt_idx[i]);
                f.push_back(v_idx[i+1]); f.push_back(vt_idx[i+1]);
                faces_.push_back(f);
            }
        }
    }

    std::cout << "Loaded OBJ: " << filename << "\n";
    std::cout << "verts=" << verts_.size() << " uv=" << uv_.size() << " faces=" << faces_.size() << "\n";
}

Vec3f Model::vert(int i) const {
    if (i < 0 || i >= (int)verts_.size()) return Vec3f(0,0,0);
    return verts_[i];
}

Vec2f Model::uv(int i) const {
    if (uv_.empty()) return Vec2f(0,0);
    if (i < 0 || i >= (int)uv_.size()) return Vec2f(0,0);
    return uv_[i];
}
