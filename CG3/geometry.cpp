#include "geometry.h"

// Векторное произведение
Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}

// Скалярное произведение
float dot(const Vec3f& a, const Vec3f& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

// Барицентрические координаты
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
