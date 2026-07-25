// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT
#pragma once

// Platform-neutral C++17 vector/matrix/ray math for the hair grooming demo.
// Header-only, no Apple, OpenGL, or OpenUSD types, so every routine here is
// directly unit testable on any host.

#include <cmath>
#include <cstdint>

namespace noodles::demo::hair {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline Vec2 MakeVec2(float x, float y) { return Vec2{x, y}; }
inline Vec3 MakeVec3(float x, float y, float z) { return Vec3{x, y, z}; }

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator-(const Vec3& a) { return Vec3{-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, float s) {
  return Vec3{a.x * s, a.y * s, a.z * s};
}
inline Vec3 operator*(float s, const Vec3& a) { return a * s; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) {
  a.x += b.x;
  a.y += b.y;
  a.z += b.z;
  return a;
}
inline Vec3& operator-=(Vec3& a, const Vec3& b) {
  a.x -= b.x;
  a.y -= b.y;
  a.z -= b.z;
  return a;
}
inline Vec3& operator*=(Vec3& a, float s) {
  a.x *= s;
  a.y *= s;
  a.z *= s;
  return a;
}

inline Vec2 operator-(const Vec2& a, const Vec2& b) {
  return Vec2{a.x - b.x, a.y - b.y};
}
inline Vec2 operator+(const Vec2& a, const Vec2& b) {
  return Vec2{a.x + b.x, a.y + b.y};
}
inline Vec2 operator*(const Vec2& a, float s) { return Vec2{a.x * s, a.y * s}; }

inline float Dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x};
}
inline float LengthSquared(const Vec3& a) { return Dot(a, a); }
inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline float Distance(const Vec3& a, const Vec3& b) { return Length(a - b); }
inline float LengthSquared(const Vec2& a) { return a.x * a.x + a.y * a.y; }
inline float Length(const Vec2& a) { return std::sqrt(LengthSquared(a)); }

// Returns the zero vector for a degenerate input rather than NaN, so callers
// can normalize tangents at curve ends without special-casing every site.
inline Vec3 Normalized(const Vec3& a) {
  const float lengthSquared = Dot(a, a);
  if (lengthSquared <= 1e-20f) return Vec3{0.0f, 0.0f, 0.0f};
  return a * (1.0f / std::sqrt(lengthSquared));
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
  return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
              a.z + (b.z - a.z) * t};
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float Clamp(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}
inline int ClampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}
inline float Saturate(float value) { return Clamp(value, 0.0f, 1.0f); }

// Any unit vector perpendicular to `axis`, chosen from the world axis that is
// least parallel to it so the result never degenerates.
inline Vec3 AnyPerpendicular(const Vec3& axis) {
  const Vec3 unit = Normalized(axis);
  if (LengthSquared(unit) <= 0.0f) return Vec3{1.0f, 0.0f, 0.0f};
  const Vec3 reference = std::fabs(unit.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                                  : Vec3{1.0f, 0.0f, 0.0f};
  return Normalized(Cross(unit, reference));
}

// Column-major 4x4, matching the layout OpenGL expects for glUniformMatrix4fv
// with transpose = GL_FALSE. m[column * 4 + row].
struct Mat4 {
  float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 result;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[column * 4 + k];
      result.m[column * 4 + row] = sum;
    }
  }
  return result;
}

// Transforms a point (w = 1) and returns the clip-space result.
inline void TransformPoint(const Mat4& matrix, const Vec3& point, float* outX,
                           float* outY, float* outZ, float* outW) {
  *outX = matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z +
          matrix.m[12];
  *outY = matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z +
          matrix.m[13];
  *outZ = matrix.m[2] * point.x + matrix.m[6] * point.y +
          matrix.m[10] * point.z + matrix.m[14];
  *outW = matrix.m[3] * point.x + matrix.m[7] * point.y +
          matrix.m[11] * point.z + matrix.m[15];
}

inline Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
  const Vec3 forward = Normalized(target - eye);
  Vec3 right = Normalized(Cross(forward, up));
  if (LengthSquared(right) <= 0.0f) right = AnyPerpendicular(forward);
  const Vec3 trueUp = Cross(right, forward);

  Mat4 view;
  view.m[0] = right.x;
  view.m[4] = right.y;
  view.m[8] = right.z;
  view.m[1] = trueUp.x;
  view.m[5] = trueUp.y;
  view.m[9] = trueUp.z;
  view.m[2] = -forward.x;
  view.m[6] = -forward.y;
  view.m[10] = -forward.z;
  view.m[12] = -Dot(right, eye);
  view.m[13] = -Dot(trueUp, eye);
  view.m[14] = Dot(forward, eye);
  view.m[3] = view.m[7] = view.m[11] = 0.0f;
  view.m[15] = 1.0f;
  return view;
}

inline Mat4 Perspective(float fovYRadians, float aspect, float nearZ,
                        float farZ) {
  Mat4 projection;
  const float safeAspect = aspect > 1e-6f ? aspect : 1.0f;
  const float f = 1.0f / std::tan(Clamp(fovYRadians, 0.02f, 3.0f) * 0.5f);
  for (int i = 0; i < 16; ++i) projection.m[i] = 0.0f;
  projection.m[0] = f / safeAspect;
  projection.m[5] = f;
  projection.m[10] = (farZ + nearZ) / (nearZ - farZ);
  projection.m[11] = -1.0f;
  projection.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
  return projection;
}

struct Ray {
  Vec3 origin;
  Vec3 direction;  // expected to be unit length
};

inline Vec3 PointOnRay(const Ray& ray, float t) {
  return ray.origin + ray.direction * t;
}

// Orbit camera. The viewport is expressed in view points (the same top-left
// origin, Y-down coordinate space GraphEditor uses for pointer input) so tools
// can convert pointer positions to rays without knowing the backing scale.
struct Camera {
  Vec3 target{0.0f, 0.92f, 0.0f};
  float distance = 3.6f;
  float yaw = 0.62f;
  float pitch = 0.18f;
  float fovY = 0.80f;
  float nearZ = 0.02f;
  float farZ = 120.0f;
  float viewportWidth = 1.0f;
  float viewportHeight = 1.0f;
};

inline Vec3 CameraPosition(const Camera& camera) {
  const float cosPitch = std::cos(camera.pitch);
  return Vec3{camera.target.x +
                  camera.distance * cosPitch * std::sin(camera.yaw),
              camera.target.y + camera.distance * std::sin(camera.pitch),
              camera.target.z +
                  camera.distance * cosPitch * std::cos(camera.yaw)};
}

inline Vec3 CameraForward(const Camera& camera) {
  return Normalized(camera.target - CameraPosition(camera));
}

inline Vec3 CameraRight(const Camera& camera) {
  const Vec3 forward = CameraForward(camera);
  Vec3 right = Normalized(Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
  if (LengthSquared(right) <= 0.0f) right = AnyPerpendicular(forward);
  return right;
}

inline Vec3 CameraUp(const Camera& camera) {
  return Cross(CameraRight(camera), CameraForward(camera));
}

inline float CameraAspect(const Camera& camera) {
  return camera.viewportHeight > 0.0f
             ? camera.viewportWidth / camera.viewportHeight
             : 1.0f;
}

inline Mat4 CameraView(const Camera& camera) {
  return LookAt(CameraPosition(camera), camera.target, Vec3{0.0f, 1.0f, 0.0f});
}

inline Mat4 CameraProjection(const Camera& camera) {
  return Perspective(camera.fovY, CameraAspect(camera), camera.nearZ,
                     camera.farZ);
}

inline Mat4 CameraViewProjection(const Camera& camera) {
  return Multiply(CameraProjection(camera), CameraView(camera));
}

// Pointer position in view points (top-left origin, Y down) to a world ray.
inline Ray CameraRayThroughPoint(const Camera& camera, float viewX,
                                 float viewY) {
  const float width = camera.viewportWidth > 0.0f ? camera.viewportWidth : 1.0f;
  const float height =
      camera.viewportHeight > 0.0f ? camera.viewportHeight : 1.0f;
  const float ndcX = (2.0f * viewX / width) - 1.0f;
  const float ndcY = 1.0f - (2.0f * viewY / height);
  const float tanHalf = std::tan(Clamp(camera.fovY, 0.02f, 3.0f) * 0.5f);

  const Vec3 eye = CameraPosition(camera);
  const Vec3 forward = CameraForward(camera);
  const Vec3 right = CameraRight(camera);
  const Vec3 up = Cross(right, forward);
  const Vec3 direction =
      Normalized(forward + right * (ndcX * tanHalf * CameraAspect(camera)) +
                 up * (ndcY * tanHalf));
  return Ray{eye, direction};
}

// World point to view points. Returns false for points at or behind the eye
// plane, which callers treat as "not pickable this frame".
inline bool ProjectToView(const Camera& camera, const Vec3& world,
                          Vec2* outViewPoint) {
  const Mat4 viewProjection = CameraViewProjection(camera);
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 0.0f;
  TransformPoint(viewProjection, world, &x, &y, &z, &w);
  if (w <= 1e-6f) return false;
  const float ndcX = x / w;
  const float ndcY = y / w;
  if (outViewPoint) {
    outViewPoint->x = (ndcX * 0.5f + 0.5f) * camera.viewportWidth;
    outViewPoint->y = (0.5f - ndcY * 0.5f) * camera.viewportHeight;
  }
  return true;
}

// Ray/plane intersection. `planeNormal` need not be unit length. Returns false
// for a ray parallel to the plane or a hit behind the ray origin.
inline bool IntersectRayPlane(const Ray& ray, const Vec3& planePoint,
                              const Vec3& planeNormal, float* outT) {
  const float denominator = Dot(ray.direction, planeNormal);
  if (std::fabs(denominator) <= 1e-8f) return false;
  const float t = Dot(planePoint - ray.origin, planeNormal) / denominator;
  if (t < 0.0f) return false;
  if (outT) *outT = t;
  return true;
}

// Ray/axis-aligned-ellipsoid intersection in world space. Returns the nearest
// non-negative hit.
inline bool IntersectRayEllipsoid(const Ray& ray, const Vec3& center,
                                  const Vec3& radii, float* outT) {
  const Vec3 safeRadii{radii.x > 1e-6f ? radii.x : 1e-6f,
                       radii.y > 1e-6f ? radii.y : 1e-6f,
                       radii.z > 1e-6f ? radii.z : 1e-6f};
  const Vec3 origin{(ray.origin.x - center.x) / safeRadii.x,
                    (ray.origin.y - center.y) / safeRadii.y,
                    (ray.origin.z - center.z) / safeRadii.z};
  const Vec3 direction{ray.direction.x / safeRadii.x,
                       ray.direction.y / safeRadii.y,
                       ray.direction.z / safeRadii.z};
  const float a = Dot(direction, direction);
  if (a <= 1e-20f) return false;
  const float b = 2.0f * Dot(origin, direction);
  const float c = Dot(origin, origin) - 1.0f;
  const float discriminant = b * b - 4.0f * a * c;
  if (discriminant < 0.0f) return false;
  const float root = std::sqrt(discriminant);
  const float t0 = (-b - root) / (2.0f * a);
  const float t1 = (-b + root) / (2.0f * a);
  const float t = t0 >= 0.0f ? t0 : t1;
  if (t < 0.0f) return false;
  if (outT) *outT = t;
  return true;
}

// Squared distance from `point` to the infinite line through the ray, plus the
// ray parameter of the closest approach. Used for screen-independent picking
// fallbacks and for brush falloff along a stroke.
inline float DistanceSquaredPointRay(const Ray& ray, const Vec3& point,
                                     float* outT) {
  const Vec3 toPoint = point - ray.origin;
  const float t = Dot(toPoint, ray.direction);
  if (outT) *outT = t;
  const Vec3 closest = ray.origin + ray.direction * t;
  return LengthSquared(point - closest);
}

// Deterministic, platform-independent hash-based random source. std::mt19937
// is portable but the standard distributions are not, so the demo carries its
// own generator to keep generated hair identical across macOS and iPadOS.
inline std::uint32_t HashUInt32(std::uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

inline std::uint32_t HashCombine(std::uint32_t seed, std::uint32_t value) {
  return HashUInt32(seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2)));
}

// Uniform float in [0, 1).
inline float RandomUnitFloat(std::uint32_t seed) {
  return static_cast<float>(HashUInt32(seed) >> 8) * (1.0f / 16777216.0f);
}

// Uniform float in [-1, 1).
inline float RandomSignedFloat(std::uint32_t seed) {
  return RandomUnitFloat(seed) * 2.0f - 1.0f;
}

inline Vec3 RandomSignedVec3(std::uint32_t seed) {
  return Vec3{RandomSignedFloat(HashCombine(seed, 11u)),
              RandomSignedFloat(HashCombine(seed, 23u)),
              RandomSignedFloat(HashCombine(seed, 37u))};
}

}  // namespace noodles::demo::hair
