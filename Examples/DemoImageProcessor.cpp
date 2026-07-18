#include "DemoImageProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace noodles::apple::examples {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

double Clamp(double value, double low, double high) {
  if (!std::isfinite(value)) return low;
  return std::max(low, std::min(value, high));
}

double SmoothStep(double edge0, double edge1, double value) {
  if (edge0 == edge1) return value < edge0 ? 0.0 : 1.0;
  const double t = Clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

Rgb Mix(const Rgb& a, const Rgb& b, double amount) {
  return {a.r + (b.r - a.r) * amount,
          a.g + (b.g - a.g) * amount,
          a.b + (b.b - a.b) * amount};
}

Rgb Scale(const Rgb& color, double scale) {
  return {color.r * scale, color.g * scale, color.b * scale};
}

const GraphProperty* FindProperty(const GraphSnapshot& snapshot,
                                  const std::string& nodeId,
                                  const std::string& propertyName) {
  for (const GraphNode& node : snapshot.nodes) {
    if (node.id != nodeId) continue;
    for (const GraphProperty& property : node.properties) {
      if (property.name == propertyName) return &property;
    }
  }
  return nullptr;
}

double Number(const GraphSnapshot& snapshot, const std::string& nodeId,
              const std::string& propertyName, double fallback) {
  const GraphProperty* property =
      FindProperty(snapshot, nodeId, propertyName);
  if (!property || !property->hasValue ||
      !std::isfinite(property->numericValue)) {
    return fallback;
  }
  return property->numericValue;
}

bool Toggle(const GraphSnapshot& snapshot, const std::string& nodeId,
            const std::string& propertyName, bool fallback) {
  return Number(snapshot, nodeId, propertyName, fallback ? 1.0 : 0.0) >=
         0.5;
}

bool HasEdge(const GraphSnapshot& snapshot, const std::string& sourceNode,
             const std::string& sourcePort, const std::string& targetNode,
             const std::string& targetPort, bool relationship) {
  for (const GraphEdge& edge : snapshot.edges) {
    if (edge.sourceNodeId == sourceNode && edge.sourcePort == sourcePort &&
        edge.targetNodeId == targetNode && edge.targetPort == targetPort &&
        edge.isRelationship == relationship) {
      return true;
    }
  }
  return false;
}

// A procedural photo surrogate: sky, sun, distant mountains and foreground.
// It is intentionally inexpensive enough to rerender synchronously while a
// graph value is being scrubbed.
Rgb Landscape(double u, double v) {
  const Rgb skyTop{0.075, 0.18, 0.39};
  const Rgb skyHorizon{0.93, 0.55, 0.36};
  Rgb color = Mix(skyTop, skyHorizon, SmoothStep(0.02, 0.72, v));

  const double sunX = u - 0.72;
  const double sunY = v - 0.30;
  const double sunDistance = std::sqrt(sunX * sunX + sunY * sunY);
  const double glow = 1.0 - SmoothStep(0.025, 0.29, sunDistance);
  color = Mix(color, {1.0, 0.76, 0.38}, glow * 0.55);
  const double disc = 1.0 - SmoothStep(0.070, 0.078, sunDistance);
  color = Mix(color, {1.0, 0.91, 0.62}, disc * 0.95);

  const double ridge =
      0.54 + 0.085 * std::sin((u * 1.35 + 0.10) * kPi * 2.0) +
      0.040 * std::sin((u * 4.15 + 0.26) * kPi * 2.0);
  const double distant = SmoothStep(ridge - 0.008, ridge + 0.010, v);
  color = Mix(color, {0.20, 0.23, 0.31}, distant * 0.88);

  const double nearRidge =
      0.69 + 0.055 * std::sin((u * 2.4 + 0.45) * kPi * 2.0) +
      0.018 * std::sin((u * 9.0 + 0.12) * kPi * 2.0);
  const double foreground =
      SmoothStep(nearRidge - 0.006, nearRidge + 0.008, v);
  const Rgb ground = Mix({0.11, 0.19, 0.18}, {0.025, 0.07, 0.075},
                         Clamp((v - 0.65) / 0.35, 0.0, 1.0));
  color = Mix(color, ground, foreground);

  // A faint path gives the grade/mask operations detail to reveal.
  const double pathCenter = 0.45 + (v - 0.72) * 0.28;
  const double pathWidth = 0.006 + std::max(0.0, v - 0.72) * 0.14;
  const double path =
      (foreground > 0.5)
          ? 1.0 - SmoothStep(pathWidth, pathWidth + 0.012,
                             std::abs(u - pathCenter))
          : 0.0;
  color = Mix(color, {0.43, 0.32, 0.22}, path * 0.72);

  const double vignetteX = (u - 0.5) * 2.0;
  const double vignetteY = (v - 0.5) * 2.0;
  const double vignette =
      1.0 - 0.19 * Clamp(vignetteX * vignetteX + vignetteY * vignetteY,
                         0.0, 1.0);
  return Scale(color, vignette);
}

double ProceduralNoise(double u, double v, double frequency) {
  const double f = std::max(0.05, frequency);
  const double broad =
      std::sin((u * 2.17 + v * 1.31 + 0.17) * kPi * 2.0 * f);
  const double fine =
      std::sin((u * 5.13 - v * 4.71 + 0.63) * kPi * 2.0 * f);
  const double cross =
      std::sin((u * 11.7 + v * 8.9 + broad * 0.23) * kPi * f);
  return broad * 0.52 + fine * 0.30 + cross * 0.18;
}

std::uint8_t ToByte(double value) {
  const double clamped = Clamp(value, 0.0, 1.0);
  return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
}

}  // namespace

DemoRgbaImage RenderDemoImage(const GraphSnapshot& snapshot, int width,
                              int height) {
  DemoRgbaImage image;
  if (width <= 0 || height <= 0) return image;
  const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) return image;

  image.width = width;
  image.height = height;
  image.pixels.resize(pixelCount * 4);

  const bool noiseFeedsGrade =
      HasEdge(snapshot, "/Demo/Grade", "input", "/Demo/Noise",
              "output", false);
  const bool gradeFeedsDisplay =
      HasEdge(snapshot, "/Demo/Display", "surface", "/Demo/Grade", "output",
              false);
  const bool maskFeedsGrade =
      HasEdge(snapshot, "/Demo/Grade", "mask", "/Demo/Mask", "", true);

  const double frequency =
      Clamp(Number(snapshot, "/Demo/Noise", "noise:frequency", 2.5), 0.05,
            24.0);
  const double amplitude =
      Clamp(Number(snapshot, "/Demo/Noise", "noise:amplitude", 0.75), 0.0,
            3.0);
  const bool noiseEnabled =
      Toggle(snapshot, "/Demo/Noise", "enabled", true);
  const double gain =
      Clamp(Number(snapshot, "/Demo/Grade", "gain", 1.0), 0.0, 4.0);
  const double mix =
      Clamp(Number(snapshot, "/Demo/Grade", "mix", 1.0), 0.0, 1.0);
  const double radius =
      Clamp(Number(snapshot, "/Demo/Mask", "radius", 0.65), 0.04, 1.4);
  const double feather =
      Clamp(Number(snapshot, "/Demo/Mask", "feather", 0.12), 0.002, 0.75);
  const bool invert = Toggle(snapshot, "/Demo/Mask", "invert", false);
  const double exposure =
      Clamp(Number(snapshot, "/Demo/Display", "exposure", 0.0), -6.0, 6.0);
  const bool visible =
      Toggle(snapshot, "/Demo/Display", "visible", true);
  const double exposureScale = std::pow(2.0, exposure);

  for (int y = 0; y < height; ++y) {
    const double v = (static_cast<double>(y) + 0.5) /
                     static_cast<double>(height);
    for (int x = 0; x < width; ++x) {
      const double u = (static_cast<double>(x) + 0.5) /
                       static_cast<double>(width);
      const Rgb original = Landscape(u, v);
      Rgb stageInput = original;
      if (noiseFeedsGrade && noiseEnabled && amplitude > 0.0) {
        const double noise = ProceduralNoise(u, v, frequency);
        const double strength = amplitude * 0.17;
        stageInput = {stageInput.r + noise * strength,
                      stageInput.g + noise * strength * 0.82,
                      stageInput.b + noise * strength * 0.64};
      }

      Rgb output = original;
      if (gradeFeedsDisplay) {
        const Rgb graded = Scale(stageInput, gain);
        const Rgb mixedGrade = Mix(stageInput, graded, mix);
        double mask = 1.0;
        if (maskFeedsGrade) {
          const double dx = (u - 0.5) / radius;
          const double dy = (v - 0.5) / (radius * 0.68);
          const double ellipseDistance = std::sqrt(dx * dx + dy * dy);
          mask = 1.0 -
                 SmoothStep(1.0 - feather, 1.0 + feather, ellipseDistance);
          if (invert) mask = 1.0 - mask;
        }
        output = Mix(stageInput, mixedGrade, mask);
      }

      if (!visible) output = {0.012, 0.016, 0.024};
      output = Scale(output, exposureScale);

      const std::size_t offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4;
      image.pixels[offset + 0] = ToByte(output.r);
      image.pixels[offset + 1] = ToByte(output.g);
      image.pixels[offset + 2] = ToByte(output.b);
      image.pixels[offset + 3] = 255;
    }
  }
  return image;
}

std::uint64_t DemoImageChecksum(const DemoRgbaImage& image) {
  // FNV-1a includes dimensions so differently shaped images with the same byte
  // prefix cannot report the same contract checksum.
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mixByte = [&hash](std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (int shift = 0; shift < 32; shift += 8) {
    mixByte(static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(image.width) >> shift) & 0xffU));
    mixByte(static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(image.height) >> shift) & 0xffU));
  }
  for (const std::uint8_t value : image.pixels) mixByte(value);
  return hash;
}

}  // namespace noodles::apple::examples
