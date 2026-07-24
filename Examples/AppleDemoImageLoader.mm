#import "AppleDemoImageLoader.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <limits>

namespace noodles::demo::examples {
namespace {

void SetError(std::string *destination, const char *message) {
  if (destination) *destination = message;
}

void UnpremultiplyRgba(std::vector<std::uint8_t> *pixels) {
  for (std::size_t offset = 0; offset + 3 < pixels->size(); offset += 4) {
    const unsigned int alpha = (*pixels)[offset + 3];
    if (alpha == 0) {
      (*pixels)[offset] = 0;
      (*pixels)[offset + 1] = 0;
      (*pixels)[offset + 2] = 0;
      continue;
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
      const unsigned int premultiplied = (*pixels)[offset + channel];
      (*pixels)[offset + channel] = static_cast<std::uint8_t>(
          std::min(255U, (premultiplied * 255U + alpha / 2U) / alpha));
    }
  }
}

}  // namespace

DemoRgbaImage DecodeDemoImageAtURL(NSURL *url, std::string *errorMessage) {
  DemoRgbaImage result;
  if (errorMessage) errorMessage->clear();
  if (!url) {
    SetError(errorMessage, "No image URL was provided");
    return result;
  }

  NSDictionary *sourceOptions = @{(__bridge NSString *)kCGImageSourceShouldCache : @NO};
  CGImageSourceRef source =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)sourceOptions);
  if (!source) {
    SetError(errorMessage, "The selected file is not a readable image");
    return result;
  }

  NSDictionary *thumbnailOptions = @{
    (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
    (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform : @YES,
    (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize : @2048,
    (__bridge NSString *)kCGImageSourceShouldCacheImmediately : @YES,
  };
  CGImageRef image =
      CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)thumbnailOptions);
  CFRelease(source);
  if (!image) {
    SetError(errorMessage, "The selected image could not be decoded");
    return result;
  }

  const std::size_t width = CGImageGetWidth(image);
  const std::size_t height = CGImageGetHeight(image);
  if (width == 0 || height == 0 ||
      width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      width > std::numeric_limits<std::size_t>::max() / height ||
      width * height > std::numeric_limits<std::size_t>::max() / 4) {
    CGImageRelease(image);
    SetError(errorMessage, "The selected image has invalid dimensions");
    return result;
  }

  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.pixels.resize(width * height * 4);

  CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  if (!colorSpace) colorSpace = CGColorSpaceCreateDeviceRGB();
  const CGBitmapInfo bitmapInfo = kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast;
  CGContextRef context = CGBitmapContextCreate(result.pixels.data(), width, height, 8, width * 4,
                                               colorSpace, bitmapInfo);
  CGColorSpaceRelease(colorSpace);
  if (!context) {
    CGImageRelease(image);
    result = {};
    SetError(errorMessage, "The selected image could not be converted to RGBA");
    return result;
  }

  // ImageIO applies EXIF orientation while creating the thumbnail. Drawing it
  // directly leaves row zero at the visual top in this bitmap-backed context.
  CGContextDrawImage(
      context, CGRectMake(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
      image);
  CGContextRelease(context);
  CGImageRelease(image);
  // Core Graphics requires a premultiplied-alpha bitmap context. The portable
  // demo image contract is straight RGBA8, so normalize once on this background
  // decode queue before the evaluator samples the pixels.
  UnpremultiplyRgba(&result.pixels);
  return result;
}

}  // namespace noodles::demo::examples
