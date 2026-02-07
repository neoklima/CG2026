#pragma once
#include <cmath>
#include <algorithm>

struct Vec2i {
    int x, y;
    Vec2i():x(0),y(0){}
    Vec2i(int X,int Y):x(X),y(Y){}
};

struct Vec2f {
    float x,y;
    Vec2f():x(0),y(0){}
    Vec2f(float X,float Y):x(X),y(Y){}
};

struct Vec3f {
    float x,y,z;
    Vec3f():x(0),y(0),z(0){}
    Vec3f(float X,float Y,float Z):x(X),y(Y),z(Z){}
    Vec3f operator+(const Vec3f& v) const { return Vec3f(x+v.x,y+v.y,z+v.z); }
    Vec3f operator-(const Vec3f& v) const { return Vec3f(x-v.x,y-v.y,z-v.z); }
    Vec3f operator*(float f) const { return Vec3f(x*f,y*f,z*f); }
};

struct Vec4f {
    float x,y,z,w;
    Vec4f():x(0),y(0),z(0),w(1){}
    Vec4f(float X,float Y,float Z,float W):x(X),y(Y),z(Z),w(W){}
};

Vec3f cross(const Vec3f& a, const Vec3f& b);
float dot(const Vec3f& a, const Vec3f& b);
Vec3f normalize(const Vec3f& v);


Vec3f barycentric(Vec3f* pts, Vec3f P);


struct Mat4 {
    float m[4][4];

    Mat4();
    static Mat4 identity();
    static Mat4 lookAt(const Vec3f& eye, const Vec3f& center, const Vec3f& up);
    static Mat4 perspective(float fov_deg, float aspect, float znear, float zfar);

    Mat4 operator*(const Mat4& r) const;
    Vec4f operator*(const Vec4f& v) const;
};

struct Camera {
    Vec3f eye, target, up;
    float fov_deg, aspect, znear, zfar;

    Camera(const Vec3f& e, const Vec3f& t, const Vec3f& u,
           float fov, float asp, float zn, float zf)
        : eye(e), target(t), up(u), fov_deg(fov), aspect(asp), znear(zn), zfar(zf) {}

    Mat4 view() const { return Mat4::lookAt(eye, target, up); }
    Mat4 proj() const { return Mat4::perspective(fov_deg, aspect, znear, zfar); }
};
