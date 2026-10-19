#include "ExrWriter.h"

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

#include <vector>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <miniz.h>

namespace Baker {

bool ExrWriter::SaveFloatImage(
    const std::string& filePath,
    const float* data,
    int width,
    int height,
    std::string& outError
) {
    if (!data || width <= 0 || height <= 0) {
        outError = "Invalid image dimensions or null data";
        return false;
    }

    EXRHeader header;
    InitEXRHeader(&header);

    EXRImage image;
    InitEXRImage(&image);

    image.num_channels = 1;

    std::vector<float> image_data(width * height);
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        std::memcpy(&image_data[y * width], &data[srcY * width], sizeof(float) * width);
    }

    float* image_ptr[1];
    image_ptr[0] = image_data.data();

    image.images = reinterpret_cast<unsigned char**>(image_ptr);
    image.width = width;
    image.height = height;

    header.num_channels = 1;
    header.channels = static_cast<EXRChannelInfo*>(malloc(sizeof(EXRChannelInfo) * header.num_channels));
    
    // Channel name "Y" or "R" for displacement
    snprintf(header.channels[0].name, 255, "R");

    header.pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));
    header.requested_pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));
    header.pixel_types[0] = TINYEXR_PIXELTYPE_FLOAT; // 32-bit float
    header.requested_pixel_types[0] = TINYEXR_PIXELTYPE_FLOAT;

    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    const char* err = nullptr;
    int ret = SaveEXRImageToFile(&image, &header, filePath.c_str(), &err);

    free(header.channels);
    free(header.pixel_types);
    free(header.requested_pixel_types);

    if (ret != TINYEXR_SUCCESS) {
        if (err) {
            outError = err;
            FreeEXRErrorMessage(err);
        } else {
            outError = "Unknown TinyEXR error occurred";
        }
        return false;
    }

    return true;
}

bool ExrWriter::SaveRGBFloatImage(
    const std::string& filePath,
    const float* rData,
    const float* gData,
    const float* bData,
    int width,
    int height,
    bool asHalfFloat,
    std::string& outError
) {
    if (!rData || !gData || !bData || width <= 0 || height <= 0) {
        outError = "Invalid image dimensions or null data";
        return false;
    }

    EXRHeader header;
    InitEXRHeader(&header);

    EXRImage image;
    InitEXRImage(&image);

    image.num_channels = 3;

    std::vector<float> b_data(width * height);
    std::vector<float> g_data(width * height);
    std::vector<float> r_data(width * height);
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        std::memcpy(&b_data[y * width], &bData[srcY * width], sizeof(float) * width);
        std::memcpy(&g_data[y * width], &gData[srcY * width], sizeof(float) * width);
        std::memcpy(&r_data[y * width], &rData[srcY * width], sizeof(float) * width);
    }

    // OpenEXR channels are typically ordered B, G, R
    const float* image_ptr[3];
    image_ptr[0] = b_data.data();
    image_ptr[1] = g_data.data();
    image_ptr[2] = r_data.data();

    image.images = reinterpret_cast<unsigned char**>(const_cast<float**>(image_ptr));
    image.width = width;
    image.height = height;

    header.num_channels = 3;
    header.channels = static_cast<EXRChannelInfo*>(malloc(sizeof(EXRChannelInfo) * header.num_channels));
    snprintf(header.channels[0].name, 255, "B");
    snprintf(header.channels[1].name, 255, "G");
    snprintf(header.channels[2].name, 255, "R");

    header.pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));
    header.requested_pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));

    int pixelType = asHalfFloat ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
    for (int i = 0; i < 3; ++i) {
        header.pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
        header.requested_pixel_types[i] = pixelType;
    }

    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    const char* err = nullptr;
    int ret = SaveEXRImageToFile(&image, &header, filePath.c_str(), &err);

    free(header.channels);
    free(header.pixel_types);
    free(header.requested_pixel_types);

    if (ret != TINYEXR_SUCCESS) {
        if (err) {
            outError = err;
            FreeEXRErrorMessage(err);
        } else {
            outError = "Unknown TinyEXR error occurred";
        }
        return false;
    }

    return true;
}

bool ExrWriter::SaveRGBPNG(
    const std::string& filePath,
    const float* rData,
    const float* gData,
    const float* bData,
    int width,
    int height,
    std::string& outError
) {
    if (!rData || !gData || !bData || width <= 0 || height <= 0) {
        outError = "Invalid image parameters for PNG write";
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        for (int x = 0; x < width; ++x) {
            int srcIdx = srcY * width + x;
            int dstIdx = (y * width + x) * 3;
            float r = std::clamp(rData[srcIdx], 0.0f, 1.0f);
            float g = std::clamp(gData[srcIdx], 0.0f, 1.0f);
            float b = std::clamp(bData[srcIdx], 0.0f, 1.0f);

            pixels[dstIdx + 0] = static_cast<uint8_t>(std::round(r * 255.0f));
            pixels[dstIdx + 1] = static_cast<uint8_t>(std::round(g * 255.0f));
            pixels[dstIdx + 2] = static_cast<uint8_t>(std::round(b * 255.0f));
        }
    }

    size_t pngLen = 0;
    void* pngData = tdefl_write_image_to_png_file_in_memory(pixels.data(), width, height, 3, &pngLen);
    if (!pngData) {
        outError = "Failed to compress PNG image";
        return false;
    }

    std::ofstream out(filePath, std::ios::binary);
    if (!out.is_open()) {
        mz_free(pngData);
        outError = "Failed to open destination file for PNG: " + filePath;
        return false;
    }

    out.write(static_cast<const char*>(pngData), pngLen);
    out.close();
    mz_free(pngData);
    return true;
}

bool ExrWriter::SaveGrayscalePNG(
    const std::string& filePath,
    const float* grayData,
    int width,
    int height,
    std::string& outError
) {
    if (!grayData || width <= 0 || height <= 0) {
        outError = "Invalid grayscale parameters for PNG write";
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        int srcY = height - 1 - y;
        for (int x = 0; x < width; ++x) {
            int srcIdx = srcY * width + x;
            int dstIdx = y * width + x;
            float g = std::clamp(grayData[srcIdx], 0.0f, 1.0f);
            pixels[dstIdx] = static_cast<uint8_t>(std::round(g * 255.0f));
        }
    }

    size_t pngLen = 0;
    void* pngData = tdefl_write_image_to_png_file_in_memory(pixels.data(), width, height, 1, &pngLen);
    if (!pngData) {
        outError = "Failed to compress grayscale PNG image";
        return false;
    }

    std::ofstream out(filePath, std::ios::binary);
    if (!out.is_open()) {
        mz_free(pngData);
        outError = "Failed to open destination file for PNG: " + filePath;
        return false;
    }

    out.write(static_cast<const char*>(pngData), pngLen);
    out.close();
    mz_free(pngData);
    return true;
}

} // namespace Baker

