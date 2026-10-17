#include "MeshLoader.h"
#include "../baker/MikkTSpaceBridge.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <algorithm>
#include <set>
#include <iostream>
#include <fstream>
#include <sstream>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace Baker {

bool MeshLoader::LoadLowPolyOBJ(
    const std::string& filePath,
    LowPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

    tinyobj::ObjReaderConfig readerConfig;
    readerConfig.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(filePath, readerConfig)) {
        if (!reader.Error().empty()) {
            outError = reader.Error();
        } else {
            outError = "Failed to parse OBJ file: " + filePath;
        }
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    if (shapes.empty()) {
        outError = "OBJ file contains no geometry shapes";
        return false;
    }

    outMesh.name = filePath;
    std::set<int> udimSet;

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) {
                indexOffset += fv;
                continue;
            }

            LowPolyTriangle tri;
            float triU = 0.0f;
            float triV = 0.0f;

            for (size_t v = 0; v < 3; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                Vertex vert;

                if (idx.vertex_index >= 0) {
                    vert.position.x = attrib.vertices[3 * idx.vertex_index + 0];
                    vert.position.y = attrib.vertices[3 * idx.vertex_index + 1];
                    vert.position.z = attrib.vertices[3 * idx.vertex_index + 2];
                }

                if (idx.normal_index >= 0 && (size_t)(3 * idx.normal_index + 2) < attrib.normals.size()) {
                    vert.normal.x = attrib.normals[3 * idx.normal_index + 0];
                    vert.normal.y = attrib.normals[3 * idx.normal_index + 1];
                    vert.normal.z = attrib.normals[3 * idx.normal_index + 2];
                }

                if (idx.texcoord_index >= 0 && (size_t)(2 * idx.texcoord_index + 1) < attrib.texcoords.size()) {
                    vert.uv.u = attrib.texcoords[2 * idx.texcoord_index + 0];
                    vert.uv.v = attrib.texcoords[2 * idx.texcoord_index + 1];
                }

                triU += vert.uv.u;
                triV += vert.uv.v;

                uint32_t newIdx = static_cast<uint32_t>(outMesh.vertices.size());
                outMesh.vertices.push_back(vert);
                outMesh.indices.push_back(newIdx);
                tri.indices[v] = newIdx;
            }

            Vertex& v0 = outMesh.vertices[tri.indices[0]];
            Vertex& v1 = outMesh.vertices[tri.indices[1]];
            Vertex& v2 = outMesh.vertices[tri.indices[2]];
            Vec3 edge1 = v1.position - v0.position;
            Vec3 edge2 = v2.position - v0.position;
            Vec3 faceNormal = edge1.Cross(edge2).Normalized();

            if (v0.normal.Length() < 1e-4f) v0.normal = faceNormal;
            if (v1.normal.Length() < 1e-4f) v1.normal = faceNormal;
            if (v2.normal.Length() < 1e-4f) v2.normal = faceNormal;

            triU /= 3.0f;
            triV /= 3.0f;
            int tileId = CalculateUDIMTile(triU, triV);
            tri.udimTile = tileId;
            udimSet.insert(tileId);

            outMesh.triangles.push_back(tri);
            indexOffset += 3;
        }
    }

    outMesh.detectedUDIMs.assign(udimSet.begin(), udimSet.end());
    if (outMesh.detectedUDIMs.empty()) {
        outMesh.detectedUDIMs.push_back(1001);
    }

    MikkTSpaceBridge::CalculateTangents(outMesh);
    return true;
}

bool MeshLoader::LoadCageOBJ(
    const std::string& filePath,
    LowPolyMesh& lowPoly,
    std::string& outError
) {
    if (lowPoly.vertices.empty()) {
        outError = "Low-poly mesh must be loaded before loading an explicit cage";
        return false;
    }

    tinyobj::ObjReaderConfig readerConfig;
    readerConfig.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(filePath, readerConfig)) {
        outError = "Failed to parse cage OBJ: " + filePath;
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    std::vector<Vec3> cageVerts;
    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) {
                indexOffset += fv;
                continue;
            }

            for (size_t v = 0; v < 3; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                Vec3 p(0, 0, 0);
                if (idx.vertex_index >= 0) {
                    p.x = attrib.vertices[3 * idx.vertex_index + 0];
                    p.y = attrib.vertices[3 * idx.vertex_index + 1];
                    p.z = attrib.vertices[3 * idx.vertex_index + 2];
                }
                cageVerts.push_back(p);
            }
            indexOffset += 3;
        }
    }

    if (cageVerts.size() != lowPoly.vertices.size()) {
        outError = "Cage vertex count (" + std::to_string(cageVerts.size()) +
            ") does not match Low-Poly mesh vertex count (" +
            std::to_string(lowPoly.vertices.size()) + "). Topology must be identical.";
        return false;
    }

    lowPoly.cagePositions = std::move(cageVerts);
    return true;
}

bool MeshLoader::LoadHighPolyOBJ(
    const std::string& filePath,
    HighPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

    tinyobj::ObjReaderConfig readerConfig;
    readerConfig.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(filePath, readerConfig)) {
        if (!reader.Error().empty()) {
            outError = reader.Error();
        } else {
            outError = "Failed to parse high-poly OBJ file: " + filePath;
        }
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    if (shapes.empty()) {
        outError = "High-poly OBJ contains no geometry shapes";
        return false;
    }

    outMesh.name = filePath;

    size_t numVerts = attrib.vertices.size() / 3;
    outMesh.positions.resize(numVerts);
    for (size_t i = 0; i < numVerts; ++i) {
        outMesh.positions[i] = Vec3(
            attrib.vertices[3 * i + 0],
            attrib.vertices[3 * i + 1],
            attrib.vertices[3 * i + 2]
        );
    }

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv == 3) {
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 0].vertex_index));
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 1].vertex_index));
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 2].vertex_index));
            }
            indexOffset += fv;
        }
    }

    outMesh.normals.resize(numVerts, Vec3(0, 0, 0));
    for (size_t i = 0; i < outMesh.indices.size(); i += 3) {
        uint32_t i0 = outMesh.indices[i + 0];
        uint32_t i1 = outMesh.indices[i + 1];
        uint32_t i2 = outMesh.indices[i + 2];

        Vec3 e1 = outMesh.positions[i1] - outMesh.positions[i0];
        Vec3 e2 = outMesh.positions[i2] - outMesh.positions[i0];
        Vec3 fn = e1.Cross(e2);

        outMesh.normals[i0] = outMesh.normals[i0] + fn;
        outMesh.normals[i1] = outMesh.normals[i1] + fn;
        outMesh.normals[i2] = outMesh.normals[i2] + fn;
    }

    for (size_t i = 0; i < numVerts; ++i) {
        outMesh.normals[i] = outMesh.normals[i].Normalized();
    }

    return true;
}

// Binary layout:
// Magic (8 bytes): "ZUDM_BIN"
// Version (uint32_t): 1
// numVerts (uint64_t)
// numTris (uint64_t)
// Positions: Vec3[numVerts]
// Normals:   Vec3[numVerts]
// Indices:   uint32_t[numTris * 3]
static const char* BIN_MAGIC = "ZUDM_BIN";

bool MeshLoader::SaveBinaryGeometry(
    const std::string& binPath,
    const HighPolyMesh& mesh,
    std::string& outError
) {
    std::ofstream out(binPath, std::ios::binary);
    if (!out.is_open()) {
        outError = "Failed to create binary output file: " + binPath;
        return false;
    }

    out.write(BIN_MAGIC, 8);
    uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    uint64_t numVerts = mesh.positions.size();
    uint64_t numTris = mesh.indices.size() / 3;

    out.write(reinterpret_cast<const char*>(&numVerts), sizeof(numVerts));
    out.write(reinterpret_cast<const char*>(&numTris), sizeof(numTris));

    if (numVerts > 0) {
        out.write(reinterpret_cast<const char*>(mesh.positions.data()), sizeof(Vec3) * numVerts);
        out.write(reinterpret_cast<const char*>(mesh.normals.data()), sizeof(Vec3) * numVerts);
    }
    if (numTris > 0) {
        out.write(reinterpret_cast<const char*>(mesh.indices.data()), sizeof(uint32_t) * numTris * 3);
    }

    out.close();
    return true;
}

bool MeshLoader::LoadBinaryGeometry(
    const std::string& binPath,
    HighPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

#ifdef _WIN32
    // Use Memory-Mapped File (MMF) for ultra-fast zero-overhead reading
    HANDLE hFile = CreateFileA(binPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outError = "Cannot open binary file: " + binPath;
        return false;
    }

    LARGE_INTEGER fileSize;
    GetFileSizeEx(hFile, &fileSize);

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        outError = "Failed to create file mapping for: " + binPath;
        return false;
    }

    const uint8_t* pData = static_cast<const uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!pData) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        outError = "Failed to map view of file: " + binPath;
        return false;
    }

    if (memcmp(pData, BIN_MAGIC, 8) != 0) {
        UnmapViewOfFile(pData);
        CloseHandle(hMap);
        CloseHandle(hFile);
        outError = "Invalid binary geometry format magic in: " + binPath;
        return false;
    }

    size_t offset = 8;
    uint32_t version = *reinterpret_cast<const uint32_t*>(pData + offset);
    offset += sizeof(uint32_t);

    uint64_t numVerts = *reinterpret_cast<const uint64_t*>(pData + offset);
    offset += sizeof(uint64_t);

    uint64_t numTris = *reinterpret_cast<const uint64_t*>(pData + offset);
    offset += sizeof(uint64_t);

    outMesh.name = binPath;
    outMesh.positions.resize(numVerts);
    outMesh.normals.resize(numVerts);
    outMesh.indices.resize(numTris * 3);

    memcpy(outMesh.positions.data(), pData + offset, sizeof(Vec3) * numVerts);
    offset += sizeof(Vec3) * numVerts;

    memcpy(outMesh.normals.data(), pData + offset, sizeof(Vec3) * numVerts);
    offset += sizeof(Vec3) * numVerts;

    memcpy(outMesh.indices.data(), pData + offset, sizeof(uint32_t) * numTris * 3);

    UnmapViewOfFile(pData);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return true;
#else
    std::ifstream in(binPath, std::ios::binary);
    if (!in.is_open()) {
        outError = "Cannot open binary file: " + binPath;
        return false;
    }

    char magic[8];
    in.read(magic, 8);
    if (memcmp(magic, BIN_MAGIC, 8) != 0) {
        outError = "Invalid binary geometry magic";
        return false;
    }

    uint32_t version;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));

    uint64_t numVerts, numTris;
    in.read(reinterpret_cast<char*>(&numVerts), sizeof(numVerts));
    in.read(reinterpret_cast<char*>(&numTris), sizeof(numTris));

    outMesh.positions.resize(numVerts);
    outMesh.normals.resize(numVerts);
    outMesh.indices.resize(numTris * 3);

    in.read(reinterpret_cast<char*>(outMesh.positions.data()), sizeof(Vec3) * numVerts);
    in.read(reinterpret_cast<char*>(outMesh.normals.data()), sizeof(Vec3) * numVerts);
    in.read(reinterpret_cast<char*>(outMesh.indices.data()), sizeof(uint32_t) * numTris * 3);

    return true;
#endif
}

bool MeshLoader::LoadSubToolManifest(
    const std::string& manifestPath,
    MultiSubToolManifest& outManifest,
    std::string& outError
) {
    std::ifstream in(manifestPath);
    if (!in.is_open()) {
        outError = "Cannot open manifest file: " + manifestPath;
        return false;
    }

    // Line-delimited simple format:
    // SUBTOOL <name> <filePath> <maskId> <visible:0|1>
    // GROUP <groupName> <groupMask> <udim1,udim2,...>
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        if (tag == "SUBTOOL") {
            std::string name, path;
            unsigned int maskId = 1;
            int visible = 1;
            iss >> name >> path >> maskId >> visible;

            HighPolyMesh st;
            st.name = name;
            st.maskId = maskId;
            st.isVisible = (visible != 0);

            std::string subErr;
            // Check if binary or OBJ
            if (path.rfind(".bin") != std::string::npos) {
                if (LoadBinaryGeometry(path, st, subErr)) {
                    outManifest.subtools.push_back(std::move(st));
                }
            } else {
                if (LoadHighPolyOBJ(path, st, subErr)) {
                    outManifest.subtools.push_back(std::move(st));
                }
            }
        } else if (tag == "GROUP") {
            std::string groupName;
            unsigned int groupMask = 0xFFFFFFFF;
            std::string udimsStr;
            iss >> groupName >> groupMask >> udimsStr;

            BakeGroup grp;
            grp.name = groupName;
            grp.groupMask = groupMask;

            std::stringstream ss(udimsStr);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) {
                    grp.targetUDIMs.push_back(std::stoi(item));
                }
            }
            outManifest.groups.push_back(grp);
        }
    }

    return true;
}

} // namespace Baker
