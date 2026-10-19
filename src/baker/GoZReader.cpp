#include "GoZReader.h"
#include <windows.h>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace Baker {

// Official GoZ SDK Tags from GoZ_Binary.h
enum GoZTag {
    GoZ_TAG_END_OF_FILE = 0,
    GoZ_TAG_MESH = 1,
    GoZ_TAG_MATERIAL = 2,
    GoZ_TAG_FLAGS = 5001,
    GoZ_TAG_SUBDIV = 5002,
    GoZ_TAG_POINT_LIST = 10001,
    GoZ_TAG_FACE4_LIST_FORMAT_1 = 20001,
    GoZ_TAG_FACE4_LIST_FORMAT_2 = 20002,
    GoZ_TAG_FACE3_LIST = 20003,
    GoZ_TAG_UV4_LIST = 25001,
    GoZ_TAG_UV3_LIST = 25002,
    GoZ_TAG_MASK8_LIST = 30001,
    GoZ_TAG_MASK16_LIST = 30002,
    GoZ_TAG_MRGB_LIST = 35001,
    GoZ_TAG_GROUPS_LIST = 40001,
    GoZ_TAG_CREASE_LIST = 40002
};

std::unordered_map<std::string, GoZReader::CachedSubTool> GoZReader::s_subToolCache;

bool GoZReader::ReadGoZFile(
    const std::string& filePath,
    HighPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();
    outMesh.name = filePath;

    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outError = "Cannot open GoZ file: " + filePath;
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 32) {
        CloseHandle(hFile);
        outError = "Invalid or empty GoZ file: " + filePath;
        return false;
    }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        outError = "Failed to create memory mapping for GoZ file: " + filePath;
        return false;
    }

    const uint8_t* pData = static_cast<const uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!pData) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        outError = "Failed to map view of GoZ file: " + filePath;
        return false;
    }

    // 1. Verify 32-byte GoZ Header
    if (memcmp(pData, "GoZb", 4) != 0) {
        UnmapViewOfFile(pData);
        CloseHandle(hMap);
        CloseHandle(hFile);
        outError = "File is not a valid GoZ binary file (missing 'GoZb' magic signature): " + filePath;
        return false;
    }

    size_t offset = 32;
    size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);

    std::vector<uint32_t> rawPolyGroups;
    bool hasFaces = false;

    // 2. Parse GoZ Data Blocks
    while (offset + 16 <= totalBytes) {
        uint32_t tag = *reinterpret_cast<const uint32_t*>(pData + offset);
        uint32_t size = *reinterpret_cast<const uint32_t*>(pData + offset + 4);
        uint32_t count = *reinterpret_cast<const uint32_t*>(pData + offset + 8);
        // float modifier = *reinterpret_cast<const float*>(pData + offset + 12);

        if (size < 16 || offset + size > totalBytes) {
            break; // Corrupted block size or reached EOF
        }

        const uint8_t* blockData = pData + offset + 16;
        size_t dataSize = size - 16;

        if (tag == GoZ_TAG_END_OF_FILE) {
            break;
        }

        switch (tag) {
            case GoZ_TAG_POINT_LIST: {
                if (dataSize >= count * sizeof(Vec3)) {
                    outMesh.positions.resize(count);
                    memcpy(outMesh.positions.data(), blockData, count * sizeof(Vec3));
                }
                break;
            }

            case GoZ_TAG_FACE4_LIST_FORMAT_1: {
                // Each face is 4 int32_t values. Triangles have 4th index = -1.
                if (dataSize >= count * 16) {
                    hasFaces = true;
                    outMesh.isQuadMesh = true;
                    outMesh.quadIndices.reserve(count * 4);
                    outMesh.indices.reserve(count * 6); // Triangulated for rendering / fallback

                    const int32_t* faces = reinterpret_cast<const int32_t*>(blockData);
                    for (uint32_t i = 0; i < count; ++i) {
                        int32_t i0 = faces[i * 4 + 0];
                        int32_t i1 = faces[i * 4 + 1];
                        int32_t i2 = faces[i * 4 + 2];
                        int32_t i3 = faces[i * 4 + 3];

                        if (i3 == -1 || i3 == i2) {
                            // Triangle
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i0));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i1));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i2));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i2)); // Degenerate quad in Embree

                            outMesh.indices.push_back(static_cast<uint32_t>(i0));
                            outMesh.indices.push_back(static_cast<uint32_t>(i1));
                            outMesh.indices.push_back(static_cast<uint32_t>(i2));
                        } else {
                            // True Quad
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i0));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i1));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i2));
                            outMesh.quadIndices.push_back(static_cast<uint32_t>(i3));

                            // Split quad into 2 triangles for rasterizer & triangle rays
                            outMesh.indices.push_back(static_cast<uint32_t>(i0));
                            outMesh.indices.push_back(static_cast<uint32_t>(i1));
                            outMesh.indices.push_back(static_cast<uint32_t>(i2));

                            outMesh.indices.push_back(static_cast<uint32_t>(i0));
                            outMesh.indices.push_back(static_cast<uint32_t>(i2));
                            outMesh.indices.push_back(static_cast<uint32_t>(i3));
                        }
                    }
                }
                break;
            }

            case GoZ_TAG_FACE3_LIST: {
                if (dataSize >= count * 12) {
                    hasFaces = true;
                    outMesh.indices.resize(count * 3);
                    memcpy(outMesh.indices.data(), blockData, count * 3 * sizeof(uint32_t));
                }
                break;
            }

            case GoZ_TAG_MRGB_LIST: {
                // 32-bit MRGB per vertex (Material 8-bit, R 8-bit, G 8-bit, B 8-bit)
                if (dataSize >= count * sizeof(uint32_t)) {
                    outMesh.colors.resize(count);
                    const uint32_t* mrgb = reinterpret_cast<const uint32_t*>(blockData);
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t val = mrgb[i];
                        float r = ((val >> 16) & 0xFF) / 255.0f;
                        float g = ((val >> 8) & 0xFF) / 255.0f;
                        float b = (val & 0xFF) / 255.0f;
                        outMesh.colors[i] = Vec3(r, g, b);
                    }
                }
                break;
            }

            case GoZ_TAG_GROUPS_LIST: {
                // 16-bit uint group ID per polygon
                if (dataSize >= count * sizeof(uint16_t)) {
                    rawPolyGroups.resize(count);
                    const uint16_t* grps = reinterpret_cast<const uint16_t*>(blockData);
                    for (uint32_t i = 0; i < count; ++i) {
                        rawPolyGroups[i] = static_cast<uint32_t>(grps[i]);
                    }
                }
                break;
            }

            default:
                break; // Skip unsupported or optional tags seamlessly
        }

        offset += ((size + 3) & ~3);
    }

    UnmapViewOfFile(pData);
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (outMesh.positions.empty() || !hasFaces) {
        outError = "GoZ file contains no valid points or faces: " + filePath;
        return false;
    }

    // 3. Map PolyGroups to triangle faces if available
    if (!rawPolyGroups.empty()) {
        outMesh.polygroupPerFace.reserve(outMesh.indices.size() / 3);
        size_t triIdx = 0;
        for (size_t polyIdx = 0; polyIdx < rawPolyGroups.size() && triIdx < outMesh.indices.size() / 3; ++polyIdx) {
            uint32_t grp = rawPolyGroups[polyIdx];
            outMesh.polygroupPerFace.push_back(grp);
            triIdx++;
            // If the quad was split into 2 triangles, replicate group ID for 2nd triangle
            if (outMesh.isQuadMesh && triIdx < outMesh.indices.size() / 3) {
                outMesh.polygroupPerFace.push_back(grp);
                triIdx++;
            }
        }
    }

    // 4. Compute Smooth Vertex Normals
    outMesh.normals.assign(outMesh.positions.size(), Vec3(0, 0, 0));
    size_t numTris = outMesh.indices.size() / 3;
    for (size_t t = 0; t < numTris; ++t) {
        uint32_t i0 = outMesh.indices[t * 3 + 0];
        uint32_t i1 = outMesh.indices[t * 3 + 1];
        uint32_t i2 = outMesh.indices[t * 3 + 2];
        if (i0 < outMesh.positions.size() && i1 < outMesh.positions.size() && i2 < outMesh.positions.size()) {
            Vec3 e1 = outMesh.positions[i1] - outMesh.positions[i0];
            Vec3 e2 = outMesh.positions[i2] - outMesh.positions[i0];
            Vec3 fn = e1.Cross(e2);
            outMesh.normals[i0] = outMesh.normals[i0] + fn;
            outMesh.normals[i1] = outMesh.normals[i1] + fn;
            outMesh.normals[i2] = outMesh.normals[i2] + fn;
        }
    }

    for (auto& n : outMesh.normals) {
        n = n.Normalized();
    }

    return true;
}

std::future<bool> GoZReader::ReadGoZFileAsync(
    const std::string& filePath,
    std::shared_ptr<HighPolyMesh> outMesh,
    std::shared_ptr<std::string> outError
) {
    return std::async(std::launch::async, [filePath, outMesh, outError]() {
        return ReadGoZFile(filePath, *outMesh, *outError);
    });
}

bool GoZReader::GetOrLoadSubTool(
    const std::string& filePath,
    HighPolyMesh& outMesh,
    bool forceReload,
    std::string& outError
) {
    std::error_code ec;
    auto lastWrite = std::filesystem::last_write_time(filePath, ec);
    uint64_t currentTimestamp = ec ? 0 : static_cast<uint64_t>(lastWrite.time_since_epoch().count());
    uint64_t currentSize = ec ? 0 : std::filesystem::file_size(filePath, ec);

    auto it = s_subToolCache.find(filePath);
    if (!forceReload && it != s_subToolCache.end()) {
        if (it->second.fileTimestamp == currentTimestamp && it->second.fileSize == currentSize) {
            // Unchanged cache hit: zero reload cost!
            outMesh = it->second.mesh;
            return true;
        }
    }

    // Changed or new SubTool: read and update cache
    if (!ReadGoZFile(filePath, outMesh, outError)) {
        return false;
    }

    CachedSubTool cached;
    cached.subToolId = std::filesystem::path(filePath).stem().string();
    cached.filePath = filePath;
    cached.fileTimestamp = currentTimestamp;
    cached.fileSize = currentSize;
    cached.mesh = outMesh;
    s_subToolCache[filePath] = std::move(cached);

    return true;
}

void GoZReader::ClearCache() {
    s_subToolCache.clear();
}

size_t GoZReader::GetCachedSubToolCount() {
    return s_subToolCache.size();
}

} // namespace Baker
