#pragma once
#include <cmath>
#include <algorithm>

struct Vec2i {
    int x, y;
    Vec2i():x(0),y(0){}
    Vec2i(int X,int Y):x(X),y(Y){}
    int& operator[](int i){ return i==0?x:y; }
    const int& operator[](int i) const { return i==0?x:y; }
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
    float norm() const { return std::sqrt(x*x+y*y+z*z); }
    Vec3f normalize() const {
        float n = norm();
        if(n == 0) return *this;
        return Vec3f(x/n,y/n,z/n);
    }
};

Vec3f cross(const Vec3f& a, const Vec3f& b);
float dot(const Vec3f& a, const Vec3f& b);

// барицентрические координаты точки P относительно треугольника pts (по экранным x,y)
Vec3f barycentric(Vec3f* pts, Vec3f P);
