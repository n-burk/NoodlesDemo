#pragma once

#import <Foundation/Foundation.h>

#include "DemoImageProcessor.h"

#include <string>

namespace noodles::demo::examples {

// Decode the first frame of an Apple-supported image into bounded,
// top-to-bottom straight-alpha RGBA8 pixels. File access/security coordination
// remains the calling controller's responsibility.
DemoRgbaImage DecodeDemoImageAtURL(NSURL *url, std::string *errorMessage);

}  // namespace noodles::demo::examples
