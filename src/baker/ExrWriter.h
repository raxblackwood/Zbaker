#pragma once

#include <string>
#include <vector>

namespace Baker {

struct ExrExportOptions {
    int width = 2048;
    int height = 2048;
    int channels = 1; // 1 for displacement (R), 3 for normal (RGB), 4 for RGBA
    bool isHalfFloat = false; // false = 32-bit float, true = 16-bit half float
};

class ExrWriter {
public:
    // Saves a single-channel 32-bit float displacement map
    static bool SaveFloatImage(
        const std::string& filePath,
        const float* data,
        int width,
        int height,
        std::string& outError
    );

    // Saves an RGB float image (e.g. normal map or vector displacement)
    static bool SaveRGBFloatImage(
        const std::string& filePath,
        const float* rData,
        const float* gData,
        const float* bData,
        int width,
        int height,
        bool asHalfFloat,
        std::string& outError
    );

    // Saves an RGB image as an 8-bit PNG (e.g. 8-bit normal map or preview)
    static bool SaveRGBPNG(
        const std::string& filePath,
        const float* rData,
        const float* gData,
        const float* bData,
        int width,
        int height,
        std::string& outError
    );

    // Saves a single-channel float image as an 8-bit grayscale PNG (e.g. Ambient Occlusion)
    static bool SaveGrayscalePNG(
        const std::string& filePath,
        const float* grayData,
        int width,
        int height,
        std::string& outError
    );
};

} // namespace Baker

