#include "geometry.h"
#include <cstring>

Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}

float dot(const Vec3f& a, const Vec3f& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

Vec3f normalize(const Vec3f& v) {
    float n = std::sqrt(dot(v,v));
    if (n < 1e-8f) return v;
    return Vec3f(v.x/n, v.y/n, v.z/n);
}


Vec3f barycentric(Vec3f* pts, Vec3f P) {
    Vec3f u = cross(
        Vec3f(pts[2].x - pts[0].x, pts[1].x - pts[0].x, pts[0].x - P.x),
        Vec3f(pts[2].y - pts[0].y, pts[1].y - pts[0].y, pts[0].y - P.y)
    );
    if (std::abs(u.z) < 1e-6f) return Vec3f(-1,1,1);
    return Vec3f(
        1.f - (u.x + u.y)/u.z,
        u.y/u.z,
        u.x/u.z
    );
}


Mat4::Mat4() { std::memset(m, 0, sizeof(m)); }

Mat4 Mat4::identity() {
    Mat4 r;
    r.m[0][0]=r.m[1][1]=r.m[2][2]=r.m[3][3]=1.f;
    return r;
}

Mat4 Mat4::operator*(const Mat4& r) const {
    Mat4 o;
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            float s=0;
            for(int k=0;k<4;k++) s += m[i][k]*r.m[k][j];
            o.m[i][j]=s;
        }
    }
    return o;
}

Vec4f Mat4::operator*(const Vec4f& v) const {
    Vec4f o;
    o.x = m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3]*v.w;
    o.y = m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3]*v.w;
    o.z = m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]*v.w;
    o.w = m[3][0]*v.x + m[3][1]*v.y + m[3][2]*v.z + m[3][3]*v.w;
    return o;
}


Mat4 Mat4::lookAt(const Vec3f& eye, const Vec3f& center, const Vec3f& up) {
    Vec3f f = normalize(center - eye);
    Vec3f s = normalize(cross(f, up));
    Vec3f u = cross(s, f);

    Mat4 r = Mat4::identity();
    r.m[0][0]= s.x; r.m[0][1]= s.y; r.m[0][2]= s.z;
    r.m[1][0]= u.x; r.m[1][1]= u.y; r.m[1][2]= u.z;
    r.m[2][0]=-f.x; r.m[2][1]=-f.y; r.m[2][2]=-f.z;

    r.m[0][3] = -dot(s, eye);
    r.m[1][3] = -dot(u, eye);
    r.m[2][3] =  dot(f, eye);

    return r;
}


Mat4 Mat4::perspective(float fov_deg, float aspect, float znear, float zfar) {
    float fov = fov_deg * 3.14159265f / 180.f;
    float t = std::tan(fov/2.f);

    Mat4 r;
    r.m[0][0] = 1.f/(aspect*t);
    r.m[1][1] = 1.f/t;
    r.m[2][2] = -(zfar+znear)/(zfar-znear);
    r.m[2][3] = -(2.f*zfar*znear)/(zfar-znear);
    r.m[3][2] = -1.f;
    return r;
}

