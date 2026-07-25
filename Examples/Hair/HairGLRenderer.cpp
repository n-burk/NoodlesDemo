// Copyright (c) 2026 NoodlesDemo contributors.
// SPDX-License-Identifier: MIT

#include "HairGLRenderer.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
#define NOODLES_DEMO_HAIR_GLES 1
#define GLES_SILENCE_DEPRECATION 1
#include <OpenGLES/ES3/gl.h>
#else
#define NOODLES_DEMO_HAIR_GLES 0
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#endif
#else
#define NOODLES_DEMO_HAIR_GLES 0
#include <GL/gl3.h>
#endif

#include <cmath>
#include <vector>

namespace noodles::demo::hair {
namespace {

const char* kVersionHeader =
#if NOODLES_DEMO_HAIR_GLES
    "#version 300 es\nprecision highp float;\n";
#else
    "#version 330 core\n";
#endif

const char* kMeshVertexSource = R"(
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProjection;
out vec3 vNormal;
out vec2 vUV;
void main() {
  vNormal = aNormal;
  vUV = aUV;
  gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)";

const char* kMeshFragmentSource = R"(
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uLightDirection;
uniform vec3 uBaseColor;
uniform float uAmbient;
uniform sampler2D uClumpMap;
uniform float uClumpMapStrength;
out vec4 fragColor;
void main() {
  vec3 normal = normalize(vNormal);
  float lambert = max(dot(normal, normalize(uLightDirection)), 0.0);
  // A little rim term keeps the silhouette readable against the dark
  // background without needing a second light.
  float rim = pow(1.0 - abs(normal.z), 2.0) * 0.12;
  vec3 color = uBaseColor * (uAmbient + (1.0 - uAmbient) * lambert) + rim;

  if (uClumpMapStrength > 0.0) {
    // RGB = the Voronoi region's pastel (or the boundary ink on a cell edge),
    // A = painted clump weight. Erased regions desaturate toward the
    // background so the map shows both the layout and where it has been
    // painted out.
    vec4 clump = texture(uClumpMap, vUV);
    vec3 cell = mix(vec3(0.13, 0.14, 0.18), clump.rgb,
                    0.28 + 0.72 * clump.a);
    // Keep a trace of the surface shading so the dome still reads as 3D.
    cell *= 0.70 + 0.55 * lambert;
    color = mix(color, cell, uClumpMapStrength);
  }
  fragColor = vec4(color, 1.0);
}
)";

// Ribbons are turned to face the viewer here rather than on the CPU, which is
// what lets camera motion leave every vertex buffer untouched.
const char* kRibbonVertexSource = R"(
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aTangent;
layout(location = 2) in float aSide;
layout(location = 3) in float aT;
layout(location = 4) in float aShade;
layout(location = 5) in float aWidth;
uniform mat4 uViewProjection;
uniform vec3 uCameraPosition;
out float vT;
out float vShade;
out vec3 vTangent;
out vec3 vWorld;
void main() {
  vec3 view = normalize(uCameraPosition - aPosition);
  vec3 lateral = cross(aTangent, view);
  float len = length(lateral);
  lateral = len > 1e-6 ? lateral / len
                       : normalize(cross(aTangent, vec3(0.0, 0.0, 1.0)));
  vec3 world = aPosition + lateral * (aWidth * aSide);
  vT = aT;
  vShade = aShade;
  vTangent = aTangent;
  vWorld = world;
  gl_Position = uViewProjection * vec4(world, 1.0);
}
)";

// Kajiya-Kay: a strand has no surface normal, so shading is derived from the
// tangent instead.
const char* kRibbonFragmentSource = R"(
in float vT;
in float vShade;
in vec3 vTangent;
in vec3 vWorld;
uniform vec3 uLightDirection;
uniform vec3 uCameraPosition;
uniform vec3 uRootColor;
uniform vec3 uTipColor;
uniform float uShine;
uniform float uAmbient;
out vec4 fragColor;
void main() {
  vec3 tangent = normalize(vTangent);
  vec3 light = normalize(uLightDirection);
  vec3 view = normalize(uCameraPosition - vWorld);
  float tl = dot(tangent, light);
  float tv = dot(tangent, view);
  float sinTL = sqrt(max(1.0 - tl * tl, 0.0));
  float sinTV = sqrt(max(1.0 - tv * tv, 0.0));
  float specular = pow(max(sinTL * sinTV - tl * tv, 0.0), 28.0);
  vec3 base = mix(uRootColor, uTipColor, vT) * (0.62 + 0.72 * vShade);
  vec3 color = base * (uAmbient + (1.0 - uAmbient) * sinTL) +
               vec3(1.0, 0.97, 0.9) * specular * uShine;
  fragColor = vec4(color, 1.0);
}
)";

const char* kOverlayVertexSource = R"(
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 2) in float aSize;
uniform mat4 uViewProjection;
uniform float uPointScale;
out vec3 vColor;
void main() {
  vColor = aColor;
  gl_PointSize = aSize * uPointScale;
  gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)";

const char* kOverlayFragmentSource = R"(
in vec3 vColor;
out vec4 fragColor;
void main() {
  fragColor = vec4(vColor, 1.0);
}
)";

GLuint CompileShader(GLenum type, const std::string& source,
                     std::string* error) {
  const GLuint shader = glCreateShader(type);
  const std::string full = std::string(kVersionHeader) + source;
  const char* text = full.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_TRUE) return shader;

  GLint length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  std::string log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
  glGetShaderInfoLog(shader, length, nullptr, log.data());
  if (error) *error = log;
  glDeleteShader(shader);
  return 0;
}

GLuint LinkProgram(const std::string& vertexSource,
                   const std::string& fragmentSource, std::string* error) {
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource, error);
  if (!vertex) return 0;
  const GLuint fragment =
      CompileShader(GL_FRAGMENT_SHADER, fragmentSource, error);
  if (!fragment) {
    glDeleteShader(vertex);
    return 0;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint status = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_TRUE) return program;
  GLint length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  std::string log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
  glGetProgramInfoLog(program, length, nullptr, log.data());
  if (error) *error = log;
  glDeleteProgram(program);
  return 0;
}

void SetMatrix(GLuint program, const char* name, const Mat4& matrix) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, matrix.m);
}

void SetVec3(GLuint program, const char* name, const Vec3& value) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform3f(location, value.x, value.y, value.z);
}

void SetFloat(GLuint program, const char* name, float value) {
  const GLint location = glGetUniformLocation(program, name);
  if (location >= 0) glUniform1f(location, value);
}

// Hue-to-RGB for the Output node's material tint, kept in the demo rather than
// asking the user to author three channels.
Vec3 HueColor(float hue, float saturation, float value) {
  const float h = (hue - std::floor(hue)) * 6.0f;
  const float c = value * saturation;
  const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
  const float m = value - c;
  Vec3 rgb;
  if (h < 1.0f) rgb = Vec3{c, x, 0.0f};
  else if (h < 2.0f) rgb = Vec3{x, c, 0.0f};
  else if (h < 3.0f) rgb = Vec3{0.0f, c, x};
  else if (h < 4.0f) rgb = Vec3{0.0f, x, c};
  else if (h < 5.0f) rgb = Vec3{x, 0.0f, c};
  else rgb = Vec3{c, 0.0f, x};
  return Vec3{rgb.x + m, rgb.y + m, rgb.z + m};
}

}  // namespace

HairGLRenderer::HairGLRenderer() = default;

HairGLRenderer::~HairGLRenderer() = default;

bool HairGLRenderer::initialize() {
  if (ready_) return true;
  if (!buildPrograms()) return false;

  glGenVertexArrays(1, &meshVao_);
  glGenTextures(1, &clumpMapTexture_);
  glGenBuffers(1, &meshVbo_);
  glGenBuffers(1, &meshIbo_);
  glGenBuffers(1, &meshWireIbo_);
  glGenVertexArrays(1, &ribbonVao_);
  glGenBuffers(1, &ribbonVbo_);
  glGenBuffers(1, &ribbonIbo_);
  glGenVertexArrays(1, &lineVao_);
  glGenBuffers(1, &lineVbo_);
  glGenVertexArrays(1, &pointVao_);
  glGenBuffers(1, &pointVbo_);

  const GLsizei meshStride = kMeshVertexFloats * sizeof(float);
  glBindVertexArray(meshVao_);
  glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, meshStride, nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, meshStride,
                        reinterpret_cast<const void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, meshStride,
                        reinterpret_cast<const void*>(6 * sizeof(float)));

  const GLsizei ribbonStride = kRibbonVertexFloats * sizeof(float);
  glBindVertexArray(ribbonVao_);
  glBindBuffer(GL_ARRAY_BUFFER, ribbonVbo_);
  for (int index = 0; index < 6; ++index) glEnableVertexAttribArray(index);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ribbonStride, nullptr);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ribbonStride,
                        reinterpret_cast<const void*>(3 * sizeof(float)));
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, ribbonStride,
                        reinterpret_cast<const void*>(6 * sizeof(float)));
  glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, ribbonStride,
                        reinterpret_cast<const void*>(7 * sizeof(float)));
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, ribbonStride,
                        reinterpret_cast<const void*>(8 * sizeof(float)));
  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, ribbonStride,
                        reinterpret_cast<const void*>(9 * sizeof(float)));

  const GLsizei lineStride = kLineVertexFloats * sizeof(float);
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, lineStride, nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, lineStride,
                        reinterpret_cast<const void*>(3 * sizeof(float)));
  // Attribute 2 (point size) stays disabled here; lines never read
  // gl_PointSize, and the generic vertex attribute supplies a constant.

  const GLsizei pointStride = kPointVertexFloats * sizeof(float);
  glBindVertexArray(pointVao_);
  glBindBuffer(GL_ARRAY_BUFFER, pointVbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, pointStride, nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, pointStride,
                        reinterpret_cast<const void*>(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, pointStride,
                        reinterpret_cast<const void*>(6 * sizeof(float)));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  ready_ = true;
  return true;
}

bool HairGLRenderer::buildPrograms() {
  std::string error;
  meshProgram_ = LinkProgram(kMeshVertexSource, kMeshFragmentSource, &error);
  if (!meshProgram_) {
    lastError_ = "scalp shader: " + error;
    return false;
  }
  ribbonProgram_ =
      LinkProgram(kRibbonVertexSource, kRibbonFragmentSource, &error);
  if (!ribbonProgram_) {
    lastError_ = "hair shader: " + error;
    return false;
  }
  overlayProgram_ =
      LinkProgram(kOverlayVertexSource, kOverlayFragmentSource, &error);
  if (!overlayProgram_) {
    lastError_ = "overlay shader: " + error;
    return false;
  }
  return true;
}

void HairGLRenderer::releaseBuffers() {
  const unsigned vaos[4] = {meshVao_, ribbonVao_, lineVao_, pointVao_};
  for (const unsigned vao : vaos) {
    if (vao) glDeleteVertexArrays(1, &vao);
  }
  const unsigned buffers[7] = {meshVbo_,   meshIbo_,   meshWireIbo_, ribbonVbo_,
                               ribbonIbo_, lineVbo_,   pointVbo_};
  for (const unsigned buffer : buffers) {
    if (buffer) glDeleteBuffers(1, &buffer);
  }
  if (clumpMapTexture_) glDeleteTextures(1, &clumpMapTexture_);
  clumpMapTexture_ = 0;
  hasClumpMap_ = false;
  meshVao_ = ribbonVao_ = lineVao_ = pointVao_ = 0;
  meshVbo_ = meshIbo_ = meshWireIbo_ = 0;
  ribbonVbo_ = ribbonIbo_ = lineVbo_ = pointVbo_ = 0;
  lineCapacityBytes_ = pointCapacityBytes_ = 0;
  hasUpload_ = false;
}

void HairGLRenderer::shutdown() {
  if (!ready_) return;
  releaseBuffers();
  if (meshProgram_) glDeleteProgram(meshProgram_);
  if (ribbonProgram_) glDeleteProgram(ribbonProgram_);
  if (overlayProgram_) glDeleteProgram(overlayProgram_);
  meshProgram_ = ribbonProgram_ = overlayProgram_ = 0;
  meshIndexCount_ = meshWireIndexCount_ = ribbonIndexCount_ = 0;
  lineVertexCount_ = pointVertexCount_ = 0;
  ready_ = false;
}

void HairGLRenderer::uploadClumpMap(const ClumpMapImage& image) {
  hasClumpMap_ = !image.empty();
  if (!hasClumpMap_) return;
  glBindTexture(GL_TEXTURE_2D, clumpMapTexture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  // u is an azimuth and wraps; v is a polar angle and clamps.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void HairGLRenderer::uploadSolid(const HairSolidGeometry& geometry) {
  glBindVertexArray(meshVao_);
  glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.meshVertices.size() *
                                       sizeof(float)),
               geometry.meshVertices.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIbo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.meshIndices.size() *
                                       sizeof(std::uint32_t)),
               geometry.meshIndices.data(), GL_STATIC_DRAW);
  meshIndexCount_ = geometry.meshIndices.size();

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshWireIbo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.meshWireIndices.size() *
                                       sizeof(std::uint32_t)),
               geometry.meshWireIndices.data(), GL_STATIC_DRAW);
  meshWireIndexCount_ = geometry.meshWireIndices.size();

  glBindVertexArray(ribbonVao_);
  glBindBuffer(GL_ARRAY_BUFFER, ribbonVbo_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.ribbonVertices.size() *
                                       sizeof(float)),
               geometry.ribbonVertices.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ribbonIbo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(geometry.ribbonIndices.size() *
                                       sizeof(std::uint32_t)),
               geometry.ribbonIndices.data(), GL_STATIC_DRAW);
  ribbonIndexCount_ = geometry.ribbonIndices.size();

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void HairGLRenderer::uploadOverlay(const HairOverlayGeometry& geometry) {
  const std::size_t lineBytes = geometry.lineVertices.size() * sizeof(float);
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  // Overlay geometry changes every frame but its size barely moves, so the
  // buffer is orphaned only when it actually has to grow.
  if (lineBytes > lineCapacityBytes_) {
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lineBytes),
                 geometry.lineVertices.data(), GL_DYNAMIC_DRAW);
    lineCapacityBytes_ = lineBytes;
  } else if (lineBytes > 0) {
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(lineBytes),
                    geometry.lineVertices.data());
  }
  lineVertexCount_ = geometry.lineVertices.size() / kLineVertexFloats;

  const std::size_t pointBytes = geometry.pointVertices.size() * sizeof(float);
  glBindVertexArray(pointVao_);
  glBindBuffer(GL_ARRAY_BUFFER, pointVbo_);
  if (pointBytes > pointCapacityBytes_) {
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pointBytes),
                 geometry.pointVertices.data(), GL_DYNAMIC_DRAW);
    pointCapacityBytes_ = pointBytes;
  } else if (pointBytes > 0) {
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(pointBytes),
                    geometry.pointVertices.data());
  }
  pointVertexCount_ = geometry.pointVertices.size() / kPointVertexFloats;

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void HairGLRenderer::render(HairScene& scene, int widthPx, int heightPx,
                            float contentScale) {
  if (!ready_ || widthPx <= 0 || heightPx <= 0) return;

  const float scale = contentScale > 0.0f ? contentScale : 1.0f;
  scene.setViewport(static_cast<float>(widthPx) / scale,
                    static_cast<float>(heightPx) / scale);
  const HairViewState state = scene.viewState();
  if (!state.eval) return;
  const HairEvalResult& eval = *state.eval;

  // Hair is re-uploaded only when the evaluation generation moved. An orbit
  // bumps only the camera generation, so this is skipped entirely.
  if (!hasUpload_ || uploadedEvaluation_ != scene.generations().evaluation) {
    uploadSolid(BuildSolidGeometry(eval, eval.generation.width));
    uploadClumpMap(eval.clumpMapImage);
    uploadedEvaluation_ = scene.generations().evaluation;
    hasUpload_ = true;
  }
  uploadOverlay(BuildOverlayGeometry(state));

  const Mat4 viewProjection = CameraViewProjection(state.camera);
  const Vec3 cameraPosition = CameraPosition(state.camera);
  const Vec3 lightDirection = Normalized(Vec3{0.42f, 0.86f, 0.52f});

  glViewport(0, 0, widthPx, heightPx);
  glClearColor(0.055f, 0.065f, 0.085f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
#if !NOODLES_DEMO_HAIR_GLES
  glEnable(GL_PROGRAM_POINT_SIZE);
#endif

  // ── scalp ──
  if (meshIndexCount_ > 0) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glUseProgram(meshProgram_);
    SetMatrix(meshProgram_, "uViewProjection", viewProjection);
    SetVec3(meshProgram_, "uLightDirection", lightDirection);
    SetVec3(meshProgram_, "uBaseColor", Vec3{0.30f, 0.26f, 0.25f});
    SetFloat(meshProgram_, "uAmbient", 0.28f);
    const bool showMap = eval.display.showClumpMap && hasClumpMap_;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, showMap ? clumpMapTexture_ : 0);
    const GLint mapLocation = glGetUniformLocation(meshProgram_, "uClumpMap");
    if (mapLocation >= 0) glUniform1i(mapLocation, 0);
    SetFloat(meshProgram_, "uClumpMapStrength", showMap ? 0.85f : 0.0f);
    glBindVertexArray(meshVao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIbo_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(meshIndexCount_),
                   GL_UNSIGNED_INT, nullptr);
    glDisable(GL_CULL_FACE);

    if (meshWireIndexCount_ > 0) {
      SetVec3(meshProgram_, "uBaseColor", Vec3{0.55f, 0.60f, 0.70f});
      SetFloat(meshProgram_, "uAmbient", 1.0f);
      SetFloat(meshProgram_, "uClumpMapStrength", 0.0f);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshWireIbo_);
      glDrawElements(GL_LINES, static_cast<GLsizei>(meshWireIndexCount_),
                     GL_UNSIGNED_INT, nullptr);
    }
  }

  // ── hair ──
  lastTriangleCount_ = 0;
  if (ribbonIndexCount_ > 0) {
    const Vec3 tipColor = HueColor(eval.display.tint, 0.55f, 0.86f);
    const Vec3 rootColor = HueColor(eval.display.tint, 0.72f, 0.34f);
    glUseProgram(ribbonProgram_);
    SetMatrix(ribbonProgram_, "uViewProjection", viewProjection);
    SetVec3(ribbonProgram_, "uCameraPosition", cameraPosition);
    SetVec3(ribbonProgram_, "uLightDirection", lightDirection);
    SetVec3(ribbonProgram_, "uRootColor", rootColor);
    SetVec3(ribbonProgram_, "uTipColor", tipColor);
    SetFloat(ribbonProgram_, "uShine", eval.display.shine);
    SetFloat(ribbonProgram_, "uAmbient", eval.display.ambient);
    glBindVertexArray(ribbonVao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ribbonIbo_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(ribbonIndexCount_),
                   GL_UNSIGNED_INT, nullptr);
    lastTriangleCount_ = ribbonIndexCount_ / 3;
  }

  // ── overlay: grid, guides, points, handles, gizmos ──
  glUseProgram(overlayProgram_);
  SetMatrix(overlayProgram_, "uViewProjection", viewProjection);
  SetFloat(overlayProgram_, "uPointScale", scale);
  if (lineVertexCount_ > 0) {
    glBindVertexArray(lineVao_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineVertexCount_));
  }
  if (pointVertexCount_ > 0) {
    glBindVertexArray(pointVao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(pointVertexCount_));
  }

  // Leave the state GraphEditor expects to set up for itself, and undo the
  // things it never touches (cull face, program point size) so its composite
  // pass is not affected by this one.
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
#if !NOODLES_DEMO_HAIR_GLES
  glDisable(GL_PROGRAM_POINT_SIZE);
#endif
}

}  // namespace noodles::demo::hair
