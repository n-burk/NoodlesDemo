#include "DemoImageProcessor.h"

#include <algorithm>
#include <array>
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
  return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount,
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
  const GraphProperty* property = FindProperty(snapshot, nodeId, propertyName);
  if (!property || !property->hasValue ||
      !std::isfinite(property->numericValue)) {
    return fallback;
  }
  return property->numericValue;
}

bool Toggle(const GraphSnapshot& snapshot, const std::string& nodeId,
            const std::string& propertyName, bool fallback) {
  return Number(snapshot, nodeId, propertyName, fallback ? 1.0 : 0.0) >= 0.5;
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

const GraphEdge* FindIncomingEdge(const GraphSnapshot& snapshot,
                                  const std::string& inputNodeId,
                                  const std::string& inputPort) {
  // InMemoryGraphDocument permits multiple authored sources for one input.
  // Prefer the most recently authored edge so a newly drawn noodle takes
  // effect immediately, even before the older noodle is explicitly lifted.
  for (auto edge = snapshot.edges.rbegin(); edge != snapshot.edges.rend();
       ++edge) {
    if (!edge->isRelationship && edge->sourceNodeId == inputNodeId &&
        edge->sourcePort == inputPort) {
      return &*edge;
    }
  }
  return nullptr;
}

bool IsTypedOutput(const GraphSnapshot& snapshot, const GraphEdge& edge,
                   const std::string& type) {
  if (edge.targetPort.empty()) return false;
  const GraphProperty* property =
      FindProperty(snapshot, edge.targetNodeId, edge.targetPort);
  return property && property->direction == GraphPropertyDirection::Output &&
         property->type == type;
}

enum class DemoNode : std::uint8_t {
  Invalid = 0,
  SourceImage,
  Noise,
  Grade,
  Composite,
  Mask,
};

DemoNode DemoNodeFromId(const std::string& nodeId) {
  if (nodeId == "/Demo/SourceImage") return DemoNode::SourceImage;
  if (nodeId == "/Demo/Noise") return DemoNode::Noise;
  if (nodeId == "/Demo/Grade") return DemoNode::Grade;
  if (nodeId == "/Demo/Composite") return DemoNode::Composite;
  if (nodeId == "/Demo/Mask") return DemoNode::Mask;
  return DemoNode::Invalid;
}

constexpr std::uint32_t NodeBit(DemoNode node) {
  return 1U << static_cast<std::uint32_t>(node);
}

class GraphEvaluator {
 public:
  GraphEvaluator(const GraphSnapshot& snapshot,
                 const DemoRgbaImage* sourceImage, int outputWidth,
                 int outputHeight) {
    displayInput_ = ResolveInput(snapshot, "/Demo/Display", "surface", "image");
    gradeInput_ = ResolveInput(snapshot, "/Demo/Grade", "input", "image");
    compositeBackground_ =
        ResolveInput(snapshot, "/Demo/Composite", "background", "image");
    compositeForeground_ =
        ResolveInput(snapshot, "/Demo/Composite", "foreground", "image");
    compositeMask_ =
        ResolveRelationship(snapshot, "/Demo/Composite", "mask");

    sourceBrightness_ = Clamp(
        Number(snapshot, "/Demo/SourceImage", "brightness", 1.0), 0.0, 4.0);
    sourceBias_ =
        Clamp(Number(snapshot, "/Demo/SourceImage", "bias", 0.0), -1.0, 1.0);
    sourceEnabled_ = Toggle(snapshot, "/Demo/SourceImage", "enabled", true);
    noiseFrequency_ = Clamp(
        Number(snapshot, "/Demo/Noise", "noise:frequency", 2.5), 0.05, 24.0);
    noiseAmplitude_ =
        Clamp(Number(snapshot, "/Demo/Noise", "noise:amplitude", 0.75), 0.0,
              2.0);
    noiseEnabled_ = Toggle(snapshot, "/Demo/Noise", "enabled", true);
    gradeGain_ = Clamp(Number(snapshot, "/Demo/Grade", "gain", 1.0), 0.0, 4.0);
    gradeMix_ = Clamp(Number(snapshot, "/Demo/Grade", "mix", 1.0), 0.0, 1.0);
    compositeOpacity_ =
        Clamp(Number(snapshot, "/Demo/Composite", "opacity", 1.0), 0.0, 1.0);
    maskRadius_ =
        Clamp(Number(snapshot, "/Demo/Mask", "radius", 0.65), 0.04, 1.4);
    maskFeather_ =
        Clamp(Number(snapshot, "/Demo/Mask", "feather", 0.12), 0.002, 0.75);
    maskInvert_ = Toggle(snapshot, "/Demo/Mask", "invert", false);
    displayExposureScale_ = std::pow(
        2.0,
        Clamp(Number(snapshot, "/Demo/Display", "exposure", 0.0), -6.0, 6.0));
    displayVisible_ = Toggle(snapshot, "/Demo/Display", "visible", true);

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
    if (!displayVisible_) return {0.012, 0.016, 0.024};
    PixelCache cache;
    const ImageSample surface = ImageNode(displayInput_, u, v, 0U, cache);
    return Scale(surface.valid ? surface.color : NoSignal(u, v),
                 displayExposureScale_);
  }

 private:
  struct PixelCache {
    std::array<ImageSample, 6> images{};
    std::array<MaskSample, 6> masks{};
    std::uint32_t computedImages = 0U;
    std::uint32_t computedMasks = 0U;
  };

  static DemoNode ResolveInput(const GraphSnapshot& snapshot,
                               const std::string& inputNodeId,
                               const std::string& inputPort,
                               const std::string& expectedType) {
    const GraphEdge* edge = FindIncomingEdge(snapshot, inputNodeId, inputPort);
    if (!edge || !IsTypedOutput(snapshot, *edge, expectedType)) {
      return DemoNode::Invalid;
    }
    return DemoNodeFromId(edge->targetNodeId);
  }

  static DemoNode ResolveRelationship(const GraphSnapshot& snapshot,
                                      const std::string& sourceNodeId,
                                      const std::string& relationshipName) {
    const GraphProperty* property =
        FindProperty(snapshot, sourceNodeId, relationshipName);
    if (!property || property->kind != GraphPropertyKind::Relationship) {
      return DemoNode::Invalid;
    }
    for (auto edge = snapshot.edges.rbegin(); edge != snapshot.edges.rend();
         ++edge) {
      if (edge->isRelationship && edge->sourceNodeId == sourceNodeId &&
          edge->sourcePort == relationshipName && edge->targetPort.empty()) {
        return DemoNodeFromId(edge->targetNodeId);
      }
    }
    return DemoNode::Invalid;
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

  ImageSample ImageNode(DemoNode node, double u, double v,
                        std::uint32_t activeNodes, PixelCache& cache) const {
    if (node == DemoNode::Invalid || node == DemoNode::Mask) return {};
    const std::uint32_t bit = NodeBit(node);
    const std::size_t index = static_cast<std::size_t>(node);
    if ((cache.computedImages & bit) != 0U) return cache.images[index];
    if ((activeNodes & bit) != 0U) return {};
    activeNodes |= bit;

    ImageSample result;
    switch (node) {
      case DemoNode::SourceImage: {
        if (!sourceEnabled_) {
          result = {{0.0, 0.0, 0.0}, true};
          break;
        }
        const Rgb source = Scale(SourcePixel(u, v), sourceBrightness_);
        result = {{source.r + sourceBias_, source.g + sourceBias_,
                   source.b + sourceBias_},
                  true};
        break;
      }
      case DemoNode::Noise: {
        if (!noiseEnabled_) {
          result = {{0.0, 0.0, 0.0}, true};
          break;
        }
        const double noise =
            Clamp(0.5 + ProceduralNoise(u, v, noiseFrequency_) * 0.5 *
                            noiseAmplitude_,
                  0.0, 1.0);
        result = {{noise, noise * 0.88 + 0.04, noise * 0.72 + 0.10}, true};
        break;
      }
      case DemoNode::Grade: {
        const ImageSample input =
            ImageNode(gradeInput_, u, v, activeNodes, cache);
        if (input.valid) {
          result = {Mix(input.color, Scale(input.color, gradeGain_), gradeMix_),
                    true};
        }
        break;
      }
      case DemoNode::Composite: {
        const ImageSample background =
            ImageNode(compositeBackground_, u, v, activeNodes, cache);
        const ImageSample foreground =
            ImageNode(compositeForeground_, u, v, activeNodes, cache);
        if (background.valid && !foreground.valid) {
          result = background;
          break;
        }
        if (!background.valid && foreground.valid) {
          result = foreground;
          break;
        }
        if (background.valid && foreground.valid) {
          const MaskSample mask =
              MaskNode(compositeMask_, u, v, activeNodes, cache);
          const double maskValue = mask.valid ? mask.value : 1.0;
          result = {
              Mix(background.color, foreground.color,
                  Clamp(maskValue * compositeOpacity_, 0.0, 1.0)),
              true};
        }
        break;
      }
      case DemoNode::Invalid:
      case DemoNode::Mask:
        break;
    }

    cache.images[index] = result;
    cache.computedImages |= bit;
    return result;
  }

  MaskSample MaskNode(DemoNode node, double u, double v,
                      std::uint32_t activeNodes, PixelCache& cache) const {
    if (node != DemoNode::Mask) return {};
    const std::uint32_t bit = NodeBit(node);
    const std::size_t index = static_cast<std::size_t>(node);
    if ((cache.computedMasks & bit) != 0U) return cache.masks[index];
    if ((activeNodes & bit) != 0U) return {};

    const double dx = (u - 0.5) / maskRadius_;
    const double dy = (v - 0.5) / (maskRadius_ * 0.68);
    const double ellipseDistance = std::sqrt(dx * dx + dy * dy);
    double value = 1.0 - SmoothStep(1.0 - maskFeather_, 1.0 + maskFeather_,
                                    ellipseDistance);
    if (maskInvert_) value = 1.0 - value;
    const MaskSample result{value, true};
    cache.masks[index] = result;
    cache.computedMasks |= bit;
    return result;
  }

  DemoNode displayInput_ = DemoNode::Invalid;
  DemoNode gradeInput_ = DemoNode::Invalid;
  DemoNode compositeBackground_ = DemoNode::Invalid;
  DemoNode compositeForeground_ = DemoNode::Invalid;
  DemoNode compositeMask_ = DemoNode::Invalid;
  double sourceBrightness_ = 1.0;
  double sourceBias_ = 0.0;
  bool sourceEnabled_ = true;
  double noiseFrequency_ = 2.5;
  double noiseAmplitude_ = 0.75;
  bool noiseEnabled_ = true;
  double gradeGain_ = 1.0;
  double gradeMix_ = 1.0;
  double compositeOpacity_ = 1.0;
  double maskRadius_ = 0.65;
  double maskFeather_ = 0.12;
  bool maskInvert_ = false;
  double displayExposureScale_ = 1.0;
  bool displayVisible_ = true;
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

}  // namespace noodles::apple::examples
