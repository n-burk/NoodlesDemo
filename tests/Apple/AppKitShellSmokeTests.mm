#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <NoodlesDemo/AppKit/NoodlesDemoGraphView.h>

#include "../../Examples/AppleDemoImageLoader.h"

#include <noodles/demo/GraphDocument.h>
#include <noodles/demo/GraphEditor.h>
#include <noodles/demo/InMemoryGraphDocument.h>

#include <cstdio>
#include <memory>

namespace na = noodles::demo;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                   #condition);                                                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

int main() {
  @autoreleasepool {
    // Pin the native loader's row orientation and straight-alpha contract. A
    // CGImage data provider stores these pixels in top-to-bottom row order.
    const std::uint8_t encodedPixels[] = {
        255, 0, 0, 255, 0, 128, 0, 128,
        0, 0, 255, 255, 0, 0, 0, 0,
    };
    NSData *encodedData = [NSData dataWithBytes:encodedPixels
                                         length:sizeof(encodedPixels)];
    CGDataProviderRef encodedProvider =
        CGDataProviderCreateWithCFData((__bridge CFDataRef)encodedData);
    CGColorSpaceRef encodedColorSpace = CGColorSpaceCreateDeviceRGB();
    CGImageRef encodedImage = CGImageCreate(
        2, 2, 8, 32, 8, encodedColorSpace,
        kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast,
        encodedProvider, nullptr, false, kCGRenderingIntentDefault);
    CHECK(encodedImage != nullptr);
    NSURL *imageURL = [NSURL fileURLWithPath:[NSTemporaryDirectory()
        stringByAppendingPathComponent:[[NSUUID UUID].UUIDString
                                           stringByAppendingPathExtension:@"png"]]];
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)imageURL, CFSTR("public.png"), 1, nullptr);
    CHECK(destination != nullptr);
    CGImageDestinationAddImage(destination, encodedImage, nullptr);
    CHECK(CGImageDestinationFinalize(destination));
    CFRelease(destination);
    CGImageRelease(encodedImage);
    CGColorSpaceRelease(encodedColorSpace);
    CGDataProviderRelease(encodedProvider);

    std::string decodeError;
    const auto decoded =
        noodles::demo::examples::DecodeDemoImageAtURL(imageURL, &decodeError);
    CHECK(decodeError.empty());
    CHECK(decoded.width == 2);
    CHECK(decoded.height == 2);
    CHECK(decoded.pixels.size() == 16);
    CHECK(decoded.pixels[0] > 250 && decoded.pixels[1] < 5 &&
          decoded.pixels[2] < 5);
    CHECK(decoded.pixels[4] < 5 && decoded.pixels[5] > 250 &&
          decoded.pixels[6] < 5 && decoded.pixels[7] >= 127 &&
          decoded.pixels[7] <= 129);
    CHECK(decoded.pixels[8] < 5 && decoded.pixels[9] < 5 &&
          decoded.pixels[10] > 250);
    [[NSFileManager defaultManager] removeItemAtURL:imageURL error:nil];

    na::GraphNode node;
    node.id = "/Smoke/Node";
    node.name = "Smoke Node";
    node.schemaTypeName = "Test";
    node.hasPosition = true;
    node.posX = 40.0;
    node.posY = 50.0;
    na::GraphProperty value;
    value.name = "value";
    value.type = "float";
    value.hasValue = true;
    value.isScrubable = true;
    value.numericValue = 0.5;
    value.displayValue = "0.5";
    node.properties.push_back(value);
    na::GraphProperty assetPath;
    assetPath.name = "asset:path";
    assetPath.type = "asset";
    assetPath.hasValue = true;
    assetPath.isScrubable = false;
    assetPath.displayValue = "/tmp/source.png";
    node.properties.push_back(assetPath);

    na::GraphSnapshot snapshot;
    snapshot.nodes.push_back(node);
    auto document =
        std::make_shared<na::InMemoryGraphDocument>(std::move(snapshot));
    auto editor = std::make_shared<na::GraphEditor>();
    editor->setDocument(document);

    NoodlesDemoGraphView *view =
        [[NoodlesDemoGraphView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)
                                              editor:editor
                                          assetsPath:@"/tmp/noodles-assets"];
    CHECK(view != nil);
    CHECK([view graphEditor] == editor);
    CHECK([view.assetsPath isEqualToString:@"/tmp/noodles-assets"]);
    CHECK(editor->nodeCount() == 1);
    CHECK([[view selectedNodeId] isEqualToString:@""]);

    __block NSString *activatedNodeId = nil;
    __block NSString *activatedAttributeName = nil;
    view.onAttributeActivated = ^(NSString *nodeId,
                                  NSString *attributeName) {
      activatedNodeId = [nodeId copy];
      activatedAttributeName = [attributeName copy];
    };

    const auto pins = editor->nodePins("/Smoke/Node");
    const na::GraphPinInfo *assetPin = nullptr;
    for (const na::GraphPinInfo &pin : pins) {
      if (pin.name == "asset:path" && !pin.isOutput) {
        assetPin = &pin;
        break;
      }
    }
    CHECK(assetPin != nullptr);
    CHECK(assetPin->hasValue);
    CHECK(!assetPin->isScrubable);
    double nodeX = 0.0;
    double nodeWidth = 0.0;
    CHECK(editor->nodePosition("/Smoke/Node", &nodeX, nullptr));
    CHECK(editor->nodeSize("/Smoke/Node", &nodeWidth, nullptr));
    const NSPoint assetRowCenter =
        NSMakePoint(nodeX + nodeWidth * 0.5, assetPin->centerY);
    [view pointerDown:assetRowCenter];
    [view pointerUp:assetRowCenter];
    CHECK([activatedNodeId isEqualToString:@"/Smoke/Node"]);
    CHECK([activatedAttributeName isEqualToString:@"asset:path"]);

    [view setOverlayOpacity:0.5f];
    [view setClearColorRed:0.0f green:0.0f blue:0.0f alpha:1.0f];
    [view setValueScrubEnabled:YES];
    [view setDisplayFrame:12.0];
    [view reloadGraph];
    (void)[view frameAllWithPadding:24.0];
    CHECK(editor->nodeCount() == 1);
    CHECK([[view selectedNodeId] isEqualToString:@"/Smoke/Node"]);
  }
  std::puts("NoodlesDemo AppKit shell smoke ok");
  return 0;
}
