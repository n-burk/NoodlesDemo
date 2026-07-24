#include "DemoImageProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace noodles::demo::examples {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Hard ceiling on node executions per pixel. Ordinary graphs stay linear in
// node count; this only stops pathological authored fan-out (stacked diamond
// Mix chains) from exploding, deterministically yielding the no-signal result.
constexpr int kEvalBudget = 4096;

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
  return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount,
          a.b + (b.b - a.b) * amount};
}

Rgb Scale(const Rgb& color, double scale) {
  return {color.r * scale, color.g * scale, color.b * scale};
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

  const double ridge = 0.54 + 0.085 * std::sin((u * 1.35 + 0.10) * kPi * 2.0) +
                       0.040 * std::sin((u * 4.15 + 0.26) * kPi * 2.0);
  const double distant = SmoothStep(ridge - 0.008, ridge + 0.010, v);
  color = Mix(color, {0.20, 0.23, 0.31}, distant * 0.88);

  const double nearRidge = 0.69 +
                           0.055 * std::sin((u * 2.4 + 0.45) * kPi * 2.0) +
                           0.018 * std::sin((u * 9.0 + 0.12) * kPi * 2.0);
  const double foreground = SmoothStep(nearRidge - 0.006, nearRidge + 0.008, v);
  const Rgb ground = Mix({0.11, 0.19, 0.18}, {0.025, 0.07, 0.075},
                         Clamp((v - 0.65) / 0.35, 0.0, 1.0));
  color = Mix(color, ground, foreground);

  // A faint path gives the grade/mask operations detail to reveal.
  const double pathCenter = 0.45 + (v - 0.72) * 0.28;
  const double pathWidth = 0.006 + std::max(0.0, v - 0.72) * 0.14;
  const double path = (foreground > 0.5)
                          ? 1.0 - SmoothStep(pathWidth, pathWidth + 0.012,
                                             std::abs(u - pathCenter))
                          : 0.0;
  color = Mix(color, {0.43, 0.32, 0.22}, path * 0.72);

  const double vignetteX = (u - 0.5) * 2.0;
  const double vignetteY = (v - 0.5) * 2.0;
  const double vignette =
      1.0 -
      0.19 * Clamp(vignetteX * vignetteX + vignetteY * vignetteY, 0.0, 1.0);
  return Scale(color, vignette);
}

double ProceduralNoise(double u, double v, double frequency) {
  const double f = std::max(0.05, frequency);
  const double broad = std::sin((u * 2.17 + v * 1.31 + 0.17) * kPi * 2.0 * f);
  const double fine = std::sin((u * 5.13 - v * 4.71 + 0.63) * kPi * 2.0 * f);
  const double cross = std::sin((u * 11.7 + v * 8.9 + broad * 0.23) * kPi * f);
  return broad * 0.52 + fine * 0.30 + cross * 0.18;
}

Rgb NoSignal(double u, double v) {
  const int column = static_cast<int>(std::floor(u * 16.0));
  const int row = static_cast<int>(std::floor(v * 10.0));
  return ((column + row) & 1) == 0 ? Rgb{0.018, 0.024, 0.034}
                                   : Rgb{0.035, 0.043, 0.057};
}

struct ImageSample {
  Rgb color;
  bool valid = false;
};

struct MaskSample {
  double value = 0.0;
  bool valid = false;
};

const GraphProperty* FindNodeProperty(const GraphNode& node,
                                      const std::string& propertyName) {
  for (const GraphProperty& property : node.properties) {
    if (property.name == propertyName) return &property;
  }
  return nullptr;
}

double Number(const GraphNode& node, const std::string& propertyName,
              double fallback) {
  const GraphProperty* property = FindNodeProperty(node, propertyName);
  if (!property || !property->hasValue ||
      !std::isfinite(property->numericValue)) {
    return fallback;
  }
  return property->numericValue;
}

bool Toggle(const GraphNode& node, const std::string& propertyName,
            bool fallback) {
  return Number(node, propertyName, fallback ? 1.0 : 0.0) >= 0.5;
}

// Every schema type the demos author has an exec function below; a node's
// operation is chosen by its type (and, for the reused Generator/Processor
// types, by its property signature), never by its id, so nodes added at
// runtime execute exactly like the fixture's.
enum class OpKind : std::uint8_t {
  Invalid = 0,
  Source,     // Generator with a "path" asset: picked image or landscape
  Noise,      // Generator with "noise:*" controls
  Signal,     // Generator with plain frequency/amplitude controls
  Grade,      // Processor: out = mix(in, in * gain, mix)
  Composite,  // Processor with background/foreground and a mask relationship
  Invert,     // out = mix(in, 1 - in, mix)
  Pixelate,   // samples its input on a quantized uv grid
  Wave,       // samples its input through a sine uv distortion
  MixOp,      // out = mix(input, blend, mix)
  Blur,        // 3×3 box average of its input at ±radius
  Threshold,   // luma bright-pass: keeps highlights, crushes the rest
  Tint,        // per-channel gain (red/green/blue)
  ChromaShift, // chromatic aberration: R/B sampled at radial offsets
  Scanlines,   // CRT-style horizontal line attenuation
  Vignette,    // radial edge darkening
  Kaleido,     // polar mirror fold into rotated wedges
  Swirl,       // rotational uv distortion around the center
  Posterize,   // per-channel quantization to N levels
  Halftone,    // comic-print color dots on a cell grid
  Mask,        // Shape: ellipse mask sampled through a relationship
  Display,     // Output: render root with exposure/visible
};

OpKind KindOfNode(const GraphNode& node) {
  const std::string& type = node.schemaTypeName;
  if (type == "Output") return OpKind::Display;
  if (type == "Shape") return OpKind::Mask;
  if (type == "Invert") return OpKind::Invert;
  if (type == "Pixelate") return OpKind::Pixelate;
  if (type == "Wave") return OpKind::Wave;
  if (type == "Mix") return OpKind::MixOp;
  if (type == "Blur") return OpKind::Blur;
  if (type == "Threshold") return OpKind::Threshold;
  if (type == "Tint") return OpKind::Tint;
  if (type == "Chroma") return OpKind::ChromaShift;
  if (type == "Scanlines") return OpKind::Scanlines;
  if (type == "Vignette") return OpKind::Vignette;
  if (type == "Kaleido") return OpKind::Kaleido;
  if (type == "Swirl") return OpKind::Swirl;
  if (type == "Posterize") return OpKind::Posterize;
  if (type == "Halftone") return OpKind::Halftone;
  if (type == "Generator") {
    if (FindNodeProperty(node, "path")) return OpKind::Source;
    if (FindNodeProperty(node, "noise:frequency")) return OpKind::Noise;
    if (FindNodeProperty(node, "frequency")) return OpKind::Signal;
    return OpKind::Invalid;
  }
  if (type == "Processor") {
    if (FindNodeProperty(node, "background") &&
        FindNodeProperty(node, "foreground")) {
      return OpKind::Composite;
    }
    return OpKind::Grade;
  }
  return OpKind::Invalid;
}

struct EvalNode {
  OpKind kind = OpKind::Invalid;
  int inputA = -1;     // input / background / surface
  int inputB = -1;     // blend / foreground
  int maskIndex = -1;  // composite mask relationship target
  double p0 = 0.0;     // kind-specific: bias/frequency/size/amplitude/...
  double p1 = 0.0;
  double p2 = 0.0;
  double gain = 1.0;
  double mixAmount = 1.0;  // mix / opacity
  bool flag = true;        // enabled / invert / visible
};

class GraphEvaluator {
 public:
  GraphEvaluator(const GraphSnapshot& snapshot,
                 const DemoRgbaImage* sourceImage, int outputWidth,
                 int outputHeight)
      : snapshot_(snapshot) {
    std::unordered_map<std::string, int> indexOf;
    nodes_.reserve(snapshot.nodes.size());
    for (const GraphNode& node : snapshot.nodes) {
      indexOf.emplace(node.id, static_cast<int>(nodes_.size()));
      nodes_.push_back(BuildNode(node));
      if (nodes_.back().kind == OpKind::Display && root_ < 0) {
        root_ = static_cast<int>(nodes_.size()) - 1;
      }
    }
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
      ResolveInputs(snapshot.nodes[i], nodes_[i], indexOf);
    }
    active_.assign(nodes_.size(), 0);

    if (ValidSourceImage(sourceImage)) {
      sourceImage_ = sourceImage;
      const double sourceAspect = static_cast<double>(sourceImage_->width) /
                                  static_cast<double>(sourceImage_->height);
      const double outputAspect =
          static_cast<double>(outputWidth) / static_cast<double>(outputHeight);
      if (sourceAspect > outputAspect) {
        sourceUScale_ = outputAspect / sourceAspect;
      } else if (sourceAspect < outputAspect) {
        sourceVScale_ = sourceAspect / outputAspect;
      }
    }
  }

  Rgb DisplayPixel(double u, double v) const {
    const EvalNode* display = root_ >= 0 ? &nodes_[root_] : nullptr;
    if (display && !display->flag) return {0.012, 0.016, 0.024};
    budget_ = kEvalBudget;
    std::fill(active_.begin(), active_.end(), std::uint8_t{0});
    const ImageSample surface =
        EvalImage(display ? display->inputA : -1, u, v);
    const double exposure = display ? display->p0 : 1.0;
    return Scale(surface.valid ? surface.color : NoSignal(u, v), exposure);
  }

 private:
  static EvalNode BuildNode(const GraphNode& node) {
    EvalNode result;
    result.kind = KindOfNode(node);
    switch (result.kind) {
      case OpKind::Source:
        result.gain = Clamp(Number(node, "brightness", 1.0), 0.0, 4.0);
        result.p0 = Clamp(Number(node, "bias", 0.0), -1.0, 1.0);
        result.flag = Toggle(node, "enabled", true);
        break;
      case OpKind::Noise:
        result.p0 = Clamp(Number(node, "noise:frequency", 2.5), 0.05, 24.0);
        result.p1 = Clamp(Number(node, "noise:amplitude", 0.75), 0.0, 2.0);
        result.flag = Toggle(node, "enabled", true);
        break;
      case OpKind::Signal:
        result.p0 = Clamp(Number(node, "frequency", 2.0), 0.05, 32.0);
        result.p1 = Clamp(Number(node, "amplitude", 0.8), 0.0, 2.0);
        break;
      case OpKind::Grade:
        result.gain = Clamp(Number(node, "gain", 1.0), 0.0, 4.0);
        result.mixAmount = Clamp(Number(node, "mix", 1.0), 0.0, 1.0);
        break;
      case OpKind::Composite:
        result.mixAmount = Clamp(Number(node, "opacity", 1.0), 0.0, 1.0);
        break;
      case OpKind::Invert:
        result.mixAmount = Clamp(Number(node, "mix", 1.0), 0.0, 1.0);
        break;
      case OpKind::Pixelate:
        result.p0 = Clamp(Number(node, "size", 0.04), 0.002, 0.5);
        break;
      case OpKind::Wave:
        result.p0 = Clamp(Number(node, "amplitude", 0.03), 0.0, 0.5);
        result.p1 = Clamp(Number(node, "frequency", 6.0), 0.05, 40.0);
        break;
      case OpKind::MixOp:
        result.mixAmount = Clamp(Number(node, "mix", 0.5), 0.0, 1.0);
        break;
      case OpKind::Blur:
        result.p0 = Clamp(Number(node, "radius", 0.02), 0.0, 0.2);
        break;
      case OpKind::Threshold:
        result.p0 = Clamp(Number(node, "threshold", 0.7), 0.0, 1.0);
        result.p1 = Clamp(Number(node, "softness", 0.1), 0.001, 0.5);
        break;
      case OpKind::Tint:
        result.p0 = Clamp(Number(node, "red", 1.0), 0.0, 4.0);
        result.p1 = Clamp(Number(node, "green", 1.0), 0.0, 4.0);
        result.p2 = Clamp(Number(node, "blue", 1.0), 0.0, 4.0);
        break;
      case OpKind::ChromaShift:
        result.p0 = Clamp(Number(node, "shift", 0.015), 0.0, 0.1);
        break;
      case OpKind::Scanlines:
        result.p0 = Clamp(Number(node, "lines", 180.0), 10.0, 600.0);
        result.p1 = Clamp(Number(node, "strength", 0.35), 0.0, 1.0);
        break;
      case OpKind::Vignette:
        result.p0 = Clamp(Number(node, "strength", 0.5), 0.0, 1.0);
        result.p1 = Clamp(Number(node, "radius", 0.35), 0.0, 1.0);
        break;
      case OpKind::Kaleido:
        result.p0 = Clamp(std::floor(Number(node, "segments", 6.0)), 1.0, 32.0);
        result.p1 = Clamp(Number(node, "rotation", 0.0), -16.0, 16.0);
        break;
      case OpKind::Swirl:
        result.p0 = Clamp(Number(node, "twist", 1.2), -8.0, 8.0);
        result.p1 = Clamp(Number(node, "radius", 0.7), 0.05, 1.5);
        break;
      case OpKind::Posterize:
        result.p0 = Clamp(std::floor(Number(node, "levels", 5.0)), 2.0, 32.0);
        break;
      case OpKind::Halftone:
        result.p0 = Clamp(Number(node, "cells", 60.0), 8.0, 200.0);
        result.p1 = Clamp(Number(node, "scale", 1.0), 0.2, 2.0);
        break;
      case OpKind::Mask:
        result.p0 = Clamp(Number(node, "radius", 0.65), 0.04, 1.4);
        result.p1 = Clamp(Number(node, "feather", 0.12), 0.002, 0.75);
        result.flag = Toggle(node, "invert", false);
        break;
      case OpKind::Display:
        result.p0 =
            std::pow(2.0, Clamp(Number(node, "exposure", 0.0), -6.0, 6.0));
        result.flag = Toggle(node, "visible", true);
        break;
      case OpKind::Invalid:
        break;
    }
    return result;
  }

  void ResolveInputs(const GraphNode& node, EvalNode& eval,
                     const std::unordered_map<std::string, int>& indexOf) {
    switch (eval.kind) {
      case OpKind::Grade:
      case OpKind::Invert:
      case OpKind::Pixelate:
      case OpKind::Wave:
      case OpKind::Blur:
      case OpKind::Threshold:
      case OpKind::Tint:
      case OpKind::ChromaShift:
      case OpKind::Scanlines:
      case OpKind::Vignette:
      case OpKind::Kaleido:
      case OpKind::Swirl:
      case OpKind::Posterize:
      case OpKind::Halftone:
        eval.inputA = ResolveInput(node.id, "input", indexOf);
        break;
      case OpKind::MixOp:
        eval.inputA = ResolveInput(node.id, "input", indexOf);
        eval.inputB = ResolveInput(node.id, "blend", indexOf);
        break;
      case OpKind::Composite:
        eval.inputA = ResolveInput(node.id, "background", indexOf);
        eval.inputB = ResolveInput(node.id, "foreground", indexOf);
        eval.maskIndex = ResolveRelationship(node, "mask", indexOf);
        break;
      case OpKind::Display:
        eval.inputA = ResolveInput(node.id, "surface", indexOf);
        break;
      case OpKind::Source:
      case OpKind::Noise:
      case OpKind::Signal:
      case OpKind::Mask:
      case OpKind::Invalid:
        break;
    }
  }

  // The most recently authored edge into (inputNodeId, inputPort) wins, so a
  // newly drawn noodle takes effect immediately, even before the older noodle
  // is explicitly lifted. The winning edge must point at a typed image output.
  int ResolveInput(const std::string& inputNodeId, const std::string& inputPort,
                   const std::unordered_map<std::string, int>& indexOf) const {
    for (auto edge = snapshot_.edges.rbegin(); edge != snapshot_.edges.rend();
         ++edge) {
      if (edge->isRelationship || edge->sourceNodeId != inputNodeId ||
          edge->sourcePort != inputPort) {
        continue;
      }
      if (edge->targetPort.empty()) return -1;
      auto nodeIt = indexOf.find(edge->targetNodeId);
      if (nodeIt == indexOf.end()) return -1;
      const GraphProperty* property = FindNodeProperty(
          snapshot_.nodes[static_cast<std::size_t>(nodeIt->second)],
          edge->targetPort);
      if (!property ||
          property->direction != GraphPropertyDirection::Output ||
          property->type != "image") {
        return -1;
      }
      return nodeIt->second;
    }
    return -1;
  }

  int ResolveRelationship(
      const GraphNode& node, const std::string& relationshipName,
      const std::unordered_map<std::string, int>& indexOf) const {
    const GraphProperty* property =
        FindNodeProperty(node, relationshipName);
    if (!property || property->kind != GraphPropertyKind::Relationship) {
      return -1;
    }
    for (auto edge = snapshot_.edges.rbegin(); edge != snapshot_.edges.rend();
         ++edge) {
      if (edge->isRelationship && edge->sourceNodeId == node.id &&
          edge->sourcePort == relationshipName && edge->targetPort.empty()) {
        auto nodeIt = indexOf.find(edge->targetNodeId);
        return nodeIt == indexOf.end() ? -1 : nodeIt->second;
      }
    }
    return -1;
  }

  static bool ValidSourceImage(const DemoRgbaImage* image) {
    if (!image || image->width <= 0 || image->height <= 0) return false;
    const std::size_t width = static_cast<std::size_t>(image->width);
    const std::size_t height = static_cast<std::size_t>(image->height);
    if (width > std::numeric_limits<std::size_t>::max() / height) return false;
    const std::size_t pixels = width * height;
    return pixels <= std::numeric_limits<std::size_t>::max() / 4 &&
           image->pixels.size() == pixels * 4;
  }

  Rgb ReadSourcePixel(int x, int y) const {
    const std::size_t offset =
        (static_cast<std::size_t>(y) *
             static_cast<std::size_t>(sourceImage_->width) +
         static_cast<std::size_t>(x)) *
        4;
    constexpr double kToUnit = 1.0 / 255.0;
    return {sourceImage_->pixels[offset] * kToUnit,
            sourceImage_->pixels[offset + 1] * kToUnit,
            sourceImage_->pixels[offset + 2] * kToUnit};
  }

  Rgb SourcePixel(double u, double v) const {
    if (!sourceImage_) return Landscape(u, v);
    const double sourceU = Clamp(0.5 + (u - 0.5) * sourceUScale_, 0.0, 1.0);
    const double sourceV = Clamp(0.5 + (v - 0.5) * sourceVScale_, 0.0, 1.0);
    const double px = sourceU * static_cast<double>(sourceImage_->width - 1);
    const double py = sourceV * static_cast<double>(sourceImage_->height - 1);
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const int x1 = std::min(x0 + 1, sourceImage_->width - 1);
    const int y1 = std::min(y0 + 1, sourceImage_->height - 1);
    const Rgb top = Mix(ReadSourcePixel(x0, y0), ReadSourcePixel(x1, y0),
                        px - static_cast<double>(x0));
    const Rgb bottom = Mix(ReadSourcePixel(x0, y1), ReadSourcePixel(x1, y1),
                           px - static_cast<double>(x0));
    return Mix(top, bottom, py - static_cast<double>(y0));
  }

  ImageSample EvalImage(int index, double u, double v) const {
    if (index < 0 || budget_ <= 0) return {};
    --budget_;
    if (active_[static_cast<std::size_t>(index)]) return {};  // feedback loop
    const EvalNode& n = nodes_[static_cast<std::size_t>(index)];

    ImageSample result;
    active_[static_cast<std::size_t>(index)] = 1;
    switch (n.kind) {
      case OpKind::Source: {
        if (!n.flag) {
          result = {{0.0, 0.0, 0.0}, true};
          break;
        }
        const Rgb source = Scale(SourcePixel(u, v), n.gain);
        result = {{source.r + n.p0, source.g + n.p0, source.b + n.p0}, true};
        break;
      }
      case OpKind::Noise: {
        if (!n.flag) {
          result = {{0.0, 0.0, 0.0}, true};
          break;
        }
        const double noise =
            Clamp(0.5 + ProceduralNoise(u, v, n.p0) * 0.5 * n.p1, 0.0, 1.0);
        result = {{noise, noise * 0.88 + 0.04, noise * 0.72 + 0.10}, true};
        break;
      }
      case OpKind::Signal: {
        const double wobble =
            ProceduralNoise(u * 0.83 + 0.11, v * 1.07 - 0.05, n.p0) * 0.5 *
            n.p1;
        const double band =
            0.5 + 0.5 * std::sin((u * n.p0 + v * 0.35 + wobble) * kPi * 2.0);
        result = {{0.08 + band * 0.22, 0.10 + band * 0.55,
                   0.16 + band * 0.74},
                  true};
        break;
      }
      case OpKind::Grade: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          result = {Mix(input.color, Scale(input.color, n.gain), n.mixAmount),
                    true};
        }
        break;
      }
      case OpKind::Invert: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          const Rgb inverted{1.0 - input.color.r, 1.0 - input.color.g,
                             1.0 - input.color.b};
          result = {Mix(input.color, inverted, n.mixAmount), true};
        }
        break;
      }
      case OpKind::Pixelate: {
        const double size = n.p0;
        const double pu =
            Clamp((std::floor(u / size) + 0.5) * size, 0.0, 1.0);
        const double pv =
            Clamp((std::floor(v / size) + 0.5) * size, 0.0, 1.0);
        result = EvalImage(n.inputA, pu, pv);
        break;
      }
      case OpKind::Wave: {
        const double wu =
            Clamp(u + n.p0 * std::sin(v * n.p1 * kPi * 2.0), 0.0, 1.0);
        const double wv =
            Clamp(v + n.p0 * std::sin(u * n.p1 * kPi * 2.0), 0.0, 1.0);
        result = EvalImage(n.inputA, wu, wv);
        break;
      }
      case OpKind::Blur: {
        const double radius = n.p0;
        Rgb sum{};
        int taps = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const ImageSample tap =
                EvalImage(n.inputA, Clamp(u + dx * radius, 0.0, 1.0),
                          Clamp(v + dy * radius, 0.0, 1.0));
            if (!tap.valid) continue;
            sum.r += tap.color.r;
            sum.g += tap.color.g;
            sum.b += tap.color.b;
            ++taps;
          }
        }
        if (taps > 0) result = {Scale(sum, 1.0 / taps), true};
        break;
      }
      case OpKind::Threshold: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          const double luma = input.color.r * 0.299 + input.color.g * 0.587 +
                              input.color.b * 0.114;
          const double keep = SmoothStep(n.p0 - n.p1, n.p0 + n.p1, luma);
          result = {Scale(input.color, keep), true};
        }
        break;
      }
      case OpKind::Tint: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          result = {{input.color.r * n.p0, input.color.g * n.p1,
                     input.color.b * n.p2},
                    true};
        }
        break;
      }
      case OpKind::ChromaShift: {
        // Lens-style color fringing: R and B are sampled at opposite radial
        // offsets from the center while G stays put.
        const double dx = (u - 0.5) * n.p0;
        const double dy = (v - 0.5) * n.p0;
        const ImageSample red = EvalImage(n.inputA, Clamp(u + dx, 0.0, 1.0),
                                          Clamp(v + dy, 0.0, 1.0));
        const ImageSample green = EvalImage(n.inputA, u, v);
        const ImageSample blue = EvalImage(n.inputA, Clamp(u - dx, 0.0, 1.0),
                                           Clamp(v - dy, 0.0, 1.0));
        if (green.valid) {
          result = {{red.valid ? red.color.r : green.color.r, green.color.g,
                     blue.valid ? blue.color.b : green.color.b},
                    true};
        }
        break;
      }
      case OpKind::Scanlines: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          const double line =
              0.5 + 0.5 * std::sin(v * n.p0 * kPi * 2.0);
          result = {Scale(input.color, 1.0 - n.p1 * line), true};
        }
        break;
      }
      case OpKind::Vignette: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          const double dx = u - 0.5;
          const double dy = v - 0.5;
          // Normalized so 1.0 lands on the frame corner.
          const double distance =
              std::sqrt(dx * dx + dy * dy) * (1.0 / 0.70710678);
          const double fall = SmoothStep(n.p1, 1.0, distance);
          result = {Scale(input.color, 1.0 - n.p0 * fall), true};
        }
        break;
      }
      case OpKind::Kaleido: {
        const double cx = u - 0.5;
        const double cy = v - 0.5;
        const double radius = std::sqrt(cx * cx + cy * cy);
        const double wedge = kPi * 2.0 / n.p0;
        double angle = std::atan2(cy, cx) + n.p1;
        angle = std::fmod(angle, wedge);
        if (angle < 0.0) angle += wedge;
        if (angle > wedge * 0.5) angle = wedge - angle;  // mirror the wedge
        result = EvalImage(n.inputA,
                           Clamp(0.5 + radius * std::cos(angle), 0.0, 1.0),
                           Clamp(0.5 + radius * std::sin(angle), 0.0, 1.0));
        break;
      }
      case OpKind::Swirl: {
        const double cx = u - 0.5;
        const double cy = v - 0.5;
        const double radius = std::sqrt(cx * cx + cy * cy);
        if (radius < n.p1) {
          const double falloff = 1.0 - radius / n.p1;
          const double angle = n.p0 * falloff * falloff;
          const double ca = std::cos(angle);
          const double sa = std::sin(angle);
          result = EvalImage(n.inputA,
                             Clamp(0.5 + cx * ca - cy * sa, 0.0, 1.0),
                             Clamp(0.5 + cx * sa + cy * ca, 0.0, 1.0));
        } else {
          result = EvalImage(n.inputA, u, v);
        }
        break;
      }
      case OpKind::Posterize: {
        const ImageSample input = EvalImage(n.inputA, u, v);
        if (input.valid) {
          const double steps = n.p0 - 1.0;
          const auto quantize = [steps](double value) {
            return std::round(Clamp(value, 0.0, 1.0) * steps) / steps;
          };
          result = {{quantize(input.color.r), quantize(input.color.g),
                     quantize(input.color.b)},
                    true};
        }
        break;
      }
      case OpKind::Halftone: {
        // Comic-print color dots: each grid cell samples its center once and
        // draws a dot whose size follows the cell's darkness.
        const double cells = n.p0;
        const double cellU = (std::floor(u * cells) + 0.5) / cells;
        const double cellV = (std::floor(v * cells) + 0.5) / cells;
        const ImageSample input =
            EvalImage(n.inputA, Clamp(cellU, 0.0, 1.0), Clamp(cellV, 0.0, 1.0));
        if (input.valid) {
          const double luma = Clamp(input.color.r * 0.299 +
                                        input.color.g * 0.587 +
                                        input.color.b * 0.114,
                                    0.0, 1.0);
          const double dotRadius = 0.5 * n.p1 * (1.0 - luma);
          const double du = u * cells - std::floor(u * cells) - 0.5;
          const double dv = v * cells - std::floor(v * cells) - 0.5;
          const double distance = std::sqrt(du * du + dv * dv);
          const double ink =
              1.0 - SmoothStep(dotRadius - 0.06, dotRadius + 0.06, distance);
          const Rgb paper{0.96, 0.94, 0.9};
          result = {Mix(paper, Scale(input.color, 0.9), ink), true};
        }
        break;
      }
      case OpKind::MixOp: {
        const ImageSample a = EvalImage(n.inputA, u, v);
        const ImageSample b = EvalImage(n.inputB, u, v);
        if (a.valid && !b.valid) {
          result = a;
        } else if (!a.valid && b.valid) {
          result = b;
        } else if (a.valid && b.valid) {
          result = {Mix(a.color, b.color, n.mixAmount), true};
        }
        break;
      }
      case OpKind::Composite: {
        const ImageSample background = EvalImage(n.inputA, u, v);
        const ImageSample foreground = EvalImage(n.inputB, u, v);
        if (background.valid && !foreground.valid) {
          result = background;
          break;
        }
        if (!background.valid && foreground.valid) {
          result = foreground;
          break;
        }
        if (background.valid && foreground.valid) {
          const MaskSample mask = EvalMask(n.maskIndex, u, v);
          const double maskValue = mask.valid ? mask.value : 1.0;
          result = {Mix(background.color, foreground.color,
                        Clamp(maskValue * n.mixAmount, 0.0, 1.0)),
                    true};
        }
        break;
      }
      case OpKind::Mask:
      case OpKind::Display:
      case OpKind::Invalid:
        break;
    }
    active_[static_cast<std::size_t>(index)] = 0;
    return result;
  }

  MaskSample EvalMask(int index, double u, double v) const {
    if (index < 0) return {};
    const EvalNode& n = nodes_[static_cast<std::size_t>(index)];
    if (n.kind != OpKind::Mask) return {};
    const double dx = (u - 0.5) / n.p0;
    const double dy = (v - 0.5) / (n.p0 * 0.68);
    const double ellipseDistance = std::sqrt(dx * dx + dy * dy);
    double value =
        1.0 - SmoothStep(1.0 - n.p1, 1.0 + n.p1, ellipseDistance);
    if (n.flag) value = 1.0 - value;
    return {value, true};
  }

  const GraphSnapshot& snapshot_;
  std::vector<EvalNode> nodes_;
  int root_ = -1;
  mutable std::vector<std::uint8_t> active_;
  mutable int budget_ = kEvalBudget;
  const DemoRgbaImage* sourceImage_ = nullptr;
  double sourceUScale_ = 1.0;
  double sourceVScale_ = 1.0;
};

std::uint8_t ToByte(double value) {
  const double clamped = Clamp(value, 0.0, 1.0);
  return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
}

}  // namespace

DemoRgbaImage RenderDemoImage(const GraphSnapshot& snapshot, int width,
                              int height, const DemoRgbaImage* sourceImage) {
  DemoRgbaImage image;
  if (width <= 0 || height <= 0) return image;
  const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) return image;

  image.width = width;
  image.height = height;
  image.pixels.resize(pixelCount * 4);

  const GraphEvaluator evaluator(snapshot, sourceImage, width, height);

  for (int y = 0; y < height; ++y) {
    const double v =
        (static_cast<double>(y) + 0.5) / static_cast<double>(height);
    for (int x = 0; x < width; ++x) {
      const double u =
          (static_cast<double>(x) + 0.5) / static_cast<double>(width);
      const Rgb output = evaluator.DisplayPixel(u, v);

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

}  // namespace noodles::demo::examples
