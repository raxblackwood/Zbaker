#include "ExrWriter.h"

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

#include <vector>
#include <cstring>

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
    std::memcpy(image_data.data(), data, sizeof(float) * width * height);

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

    // OpenEXR channels are typically ordered B, G, R
    const float* image_ptr[3];
    image_ptr[0] = bData;
    image_ptr[1] = gData;
    image_ptr[2] = rData;

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

} // namespace Baker
