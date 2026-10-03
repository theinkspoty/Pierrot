// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Math3D para a Mesa 3D (estilo After Effects Classic 2010–2018).
// Header-only: Vec3 + Mat4 (coluna-major, OpenGL-style) + projeção/lookAt.
// Fase 0 do plano Mesa 3D — sem dependência de GPU.

#pragma once

#include <cmath>

namespace math3d {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        const double len = length();
        if (len < 1e-12) return {0, 0, 0};
        return (*this) * (1.0 / len);
    }
};

// Matriz 4×4 column-major: m[col * 4 + row] (padrão OpenGL / GLSL).
struct Mat4 {
    double m[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    static Mat4 identity() { return Mat4(); }

    static Mat4 translate(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }

    // Rotação em graus ao redor do eixo (0=X, 1=Y, 2=Z), sentido horário
    // para olhar de +Z (compatível com a convenção da Mesa).
    static Mat4 rotateAxis(int axis, double degrees) {
        const double rad = degrees * 3.14159265358979323846 / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        Mat4 r;
        if (axis == 0) { // X
            r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
        } else if (axis == 1) { // Y
            r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
        } else { // Z
            r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        }
        return r;
    }

    static Mat4 rotateXYZ(double rx, double ry, double rz) {
        // Aplica Z depois Y depois X (ordem extrínseca da Mesa/AE).
        return rotateAxis(2, rz) * rotateAxis(1, ry) * rotateAxis(0, rx);
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k)
                    sum += m[k * 4 + row] * o.m[col * 4 + k];
                r.m[col * 4 + row] = sum;
            }
        }
        return r;
    }

    Vec3 transformPoint(const Vec3& v) const {
        const double x = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12];
        const double y = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13];
        const double z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14];
        const double w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
        if (std::fabs(w) < 1e-12) return {x, y, z};
        return {x / w, y / w, z / w};
    }

    // Projeção perspectiva (fov em graus, aspect = w/h, near/far).
    static Mat4 perspective(double fovDegrees, double aspect,
                            double nearP = 0.1, double farP = 10000.0) {
        const double f = 1.0 / std::tan(fovDegrees * 3.14159265358979323846
                                        / 180.0 / 2.0);
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = 0.0;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farP + nearP) / (nearP - farP);
        r.m[11] = -1.0;
        r.m[14] = (2.0 * farP * nearP) / (nearP - farP);
        return r;
    }

    // LookAt: câmera em `eye`, olhando para `center`, com `up`.
    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        const Vec3 f = (center - eye).normalized();
        const Vec3 s = f.cross(up).normalized();
        const Vec3 u = s.cross(f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -s.dot(eye);
        r.m[13] = -u.dot(eye);
        r.m[14] = f.dot(eye);
        return r;
    }
};

} // namespace math3d
