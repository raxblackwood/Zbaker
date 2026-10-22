#include "MeshLoader.h"
#include "../baker/MikkTSpaceBridge.h"
#include "../baker/GoZReader.h"

#define TINYOBJLOADER_STREAM_READER_MAX_BYTES (size_t(8) * size_t(1024) * size_t(1024) * size_t(1024))
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
#include <ufbx.h>

#include <algorithm>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cctype>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace Baker {

static std::string GetFileExtension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)std::tolower(c);
    return ext;
}

namespace {

struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct PosHasher {
    size_t operator()(const PosKey& k) const {
        size_t h = 14695981039346656037ULL;
        h ^= static_cast<size_t>(k.x); h *= 1099511628211ULL;
        h ^= static_cast<size_t>(k.y); h *= 1099511628211ULL;
        h ^= static_cast<size_t>(k.z); h *= 1099511628211ULL;
        return h;
    }
};

inline PosKey QuantizePos(const Vec3& p) {
    return PosKey{
        static_cast<int64_t>(std::floor(p.x * 100000.0f + 0.5f)),
        static_cast<int64_t>(std::floor(p.y * 100000.0f + 0.5f)),
        static_cast<int64_t>(std::floor(p.z * 100000.0f + 0.5f))
    };
}

} // namespace

bool MeshLoader::LoadLowPolyMesh(
    const std::string& filePath,
    LowPolyMesh& outMesh,
    std::string& outError
) {
    std::string ext = GetFileExtension(filePath);
    if (ext == ".fbx") {
        return LoadLowPolyFBX(filePath, outMesh, outError);
    }
    return LoadLowPolyOBJ(filePath, outMesh, outError);
}

bool MeshLoader::LoadCageMesh(
    const std::string& filePath,
    LowPolyMesh& lowPoly,
    std::string& outError
) {
    std::string ext = GetFileExtension(filePath);
    if (ext == ".fbx") {
        return LoadCageFBX(filePath, lowPoly, outError);
    }
    return LoadCageOBJ(filePath, lowPoly, outError);
}

bool MeshLoader::LoadHighPolyMesh(
    const std::string& filePath,
    HighPolyMesh& outMesh,
    std::string& outError
) {
    std::string ext = GetFileExtension(filePath);
    if (ext == ".fbx") {
        return LoadHighPolyFBX(filePath, outMesh, outError);
    } else if (ext == ".bin") {
        return LoadBinaryGeometry(filePath, outMesh, outError);
    } else if (ext == ".goz") {
        return GoZReader::GetOrLoadSubTool(filePath, outMesh, false, outError);
    }
    return LoadHighPolyOBJ(filePath, outMesh, outError);
}

bool MeshLoader::LoadLowPolyFBX(
    const std::string& filePath,
    LowPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(filePath.c_str(), &opts, &error);
    if (!scene) {
        outError = std::string("Failed to load FBX: ") + error.description.data;
        return false;
    }

    outMesh.name = filePath;
    std::set<int> udimSet;

    for (size_t n = 0; n < scene->nodes.count; ++n) {
        ufbx_node* node = scene->nodes.data[n];
        if (!node->mesh) continue;
        ufbx_mesh* mesh = node->mesh;
        ufbx_matrix normMat = ufbx_matrix_for_normals(&node->geometry_to_world);

        uint32_t nodeIdx = static_cast<uint32_t>(n);
        uint32_t nodeMask = (1u << (nodeIdx % 32));

        for (size_t f = 0; f < mesh->faces.count; ++f) {
            ufbx_face face = mesh->faces.data[f];
            if (face.num_indices < 3) continue;

            std::vector<uint32_t> faceIndices(face.num_indices);
            float triU = 0.0f;
            float triV = 0.0f;

            for (uint32_t v = 0; v < face.num_indices; ++v) {
                uint32_t meshIdx = face.index_begin + v;
                ufbx_vec3 localPos = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIdx);
                ufbx_vec3 worldPos = ufbx_transform_position(&node->geometry_to_world, localPos);

                ufbx_vec3 worldNorm = { 0.0f, 1.0f, 0.0f };
                if (mesh->vertex_normal.exists) {
                    ufbx_vec3 localNorm = ufbx_get_vertex_vec3(&mesh->vertex_normal, meshIdx);
                    worldNorm = ufbx_transform_direction(&normMat, localNorm);
                }

                ufbx_vec2 uv = { 0.0f, 0.0f };
                if (mesh->vertex_uv.exists) {
                    uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, meshIdx);
                }

                Vertex vert;
                vert.position = { (float)worldPos.x, (float)worldPos.y, (float)worldPos.z };
                vert.normal = { (float)worldNorm.x, (float)worldNorm.y, (float)worldNorm.z };
                vert.uv = { (float)uv.x, (float)uv.y };

                triU += vert.uv.u;
                triV += vert.uv.v;

                uint32_t newIdx = (uint32_t)outMesh.vertices.size();
                outMesh.vertices.push_back(vert);
                faceIndices[v] = newIdx;
            }

            // Normal fallback
            Vertex& v0 = outMesh.vertices[faceIndices[0]];
            Vertex& v1 = outMesh.vertices[faceIndices[1]];
            Vertex& v2 = outMesh.vertices[faceIndices[2]];
            Vec3 edge1 = v1.position - v0.position;
            Vec3 edge2 = v2.position - v0.position;
            Vec3 faceNormal = edge1.Cross(edge2).Normalized();
            for (uint32_t v = 0; v < face.num_indices; ++v) {
                if (outMesh.vertices[faceIndices[v]].normal.Length() < 1e-4f) {
                    outMesh.vertices[faceIndices[v]].normal = faceNormal;
                }
            }

            triU /= (float)face.num_indices;
            triV /= (float)face.num_indices;
            int tileId = CalculateUDIMTile(triU, triV);
            udimSet.insert(tileId);

            // 1. Wireframe lines: Perimeter edges of original polygon face (NO diagonal splits!)
            for (uint32_t v = 0; v < face.num_indices; ++v) {
                outMesh.wireframeLineIndices.push_back(faceIndices[v]);
                outMesh.wireframeLineIndices.push_back(faceIndices[(v + 1) % face.num_indices]);
            }

            // 2. Count original topology and store polygon faces
            std::vector<uint32_t> polyFace(face.num_indices);
            for (uint32_t v = 0; v < face.num_indices; ++v) {
                polyFace[v] = faceIndices[v];
            }
            outMesh.polygonFaces.push_back(std::move(polyFace));

            if (face.num_indices == 4) {
                outMesh.quadCount++;
            } else if (face.num_indices == 3) {
                outMesh.triCount++;
            }
            outMesh.polyCount++;

            // 3. Triangulation for baking ray-tracing and MikkTSpace
            uint32_t triIndices[64];
            uint32_t numTris = ufbx_triangulate_face(triIndices, 64, mesh, face);
            for (uint32_t t = 0; t < numTris; ++t) {
                LowPolyTriangle tri;
                tri.meshIndex = nodeIdx;
                tri.groupMask = nodeMask;
                tri.udimTile = tileId;

                for (uint32_t c = 0; c < 3; ++c) {
                    uint32_t cornerIdx = triIndices[t * 3 + c] - face.index_begin;
                    uint32_t vIdx = faceIndices[cornerIdx];
                    tri.indices[c] = vIdx;
                    outMesh.indices.push_back(vIdx);
                }
                outMesh.triangles.push_back(tri);
            }
        }
    }

    ufbx_free_scene(scene);

    if (outMesh.vertices.empty()) {
        outError = "FBX contains no mesh triangles";
        return false;
    }

    outMesh.detectedUDIMs.assign(udimSet.begin(), udimSet.end());
    if (outMesh.detectedUDIMs.empty()) {
        outMesh.detectedUDIMs.push_back(1001);
    }

    outMesh.SmoothNormalsIfMissing(180.0f);
    MikkTSpaceBridge::CalculateTangents(outMesh);
    outMesh.ComputeWatertightCageNormals(180.0f);
    DeduplicateWireframeLines(outMesh);
    return true;
}

bool MeshLoader::LoadCageFBX(
    const std::string& filePath,
    LowPolyMesh& lowPoly,
    std::string& outError
) {
    if (lowPoly.vertices.empty()) {
        outError = "Low-poly mesh must be loaded before loading an explicit cage";
        return false;
    }

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(filePath.c_str(), &opts, &error);
    if (!scene) {
        outError = std::string("Failed to load cage FBX: ") + error.description.data;
        return false;
    }

    std::vector<Vec3> cageVerts;

    for (size_t n = 0; n < scene->nodes.count; ++n) {
        ufbx_node* node = scene->nodes.data[n];
        if (!node->mesh) continue;
        ufbx_mesh* mesh = node->mesh;

        for (size_t f = 0; f < mesh->faces.count; ++f) {
            ufbx_face face = mesh->faces.data[f];
            if (face.num_indices < 3) continue;

            for (uint32_t v = 0; v < face.num_indices; ++v) {
                uint32_t meshIdx = face.index_begin + v;
                ufbx_vec3 localPos = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIdx);
                ufbx_vec3 worldPos = ufbx_transform_position(&node->geometry_to_world, localPos);
                cageVerts.push_back({ (float)worldPos.x, (float)worldPos.y, (float)worldPos.z });
            }
        }
    }

    ufbx_free_scene(scene);

    if (cageVerts.size() != lowPoly.vertices.size()) {
        outError = "Explicit cage FBX vertex count (" + std::to_string(cageVerts.size()) +
            ") does not match low-poly vertex count (" + std::to_string(lowPoly.vertices.size()) + "). Topology must match.";
        return false;
    }

    lowPoly.cagePositions = std::move(cageVerts);
    return true;
}

bool MeshLoader::LoadHighPolyFBX(
    const std::string& filePath,
    HighPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(filePath.c_str(), &opts, &error);
    if (!scene) {
        outError = std::string("Failed to load high-poly FBX: ") + error.description.data;
        return false;
    }

    outMesh.name = filePath;

    for (size_t n = 0; n < scene->nodes.count; ++n) {
        ufbx_node* node = scene->nodes.data[n];
        if (!node->mesh) continue;
        ufbx_mesh* mesh = node->mesh;
        ufbx_matrix normMat = ufbx_matrix_for_normals(&node->geometry_to_world);

        for (size_t f = 0; f < mesh->faces.count; ++f) {
            ufbx_face face = mesh->faces.data[f];
            uint32_t triIndices[64];
            uint32_t numTris = ufbx_triangulate_face(triIndices, 64, mesh, face);
            int mtlIndex = (f < mesh->face_material.count) ? mesh->face_material.data[f] : (int)n;

            for (uint32_t t = 0; t < numTris; ++t) {
                outMesh.polygroupPerFace.push_back((uint32_t)(mtlIndex + 1));
                for (uint32_t c = 0; c < 3; ++c) {
                    uint32_t meshIdx = triIndices[t * 3 + c];
                    ufbx_vec3 localPos = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIdx);
                    ufbx_vec3 worldPos = ufbx_transform_position(&node->geometry_to_world, localPos);

                    ufbx_vec3 worldNorm = { 0.0f, 1.0f, 0.0f };
                    if (mesh->vertex_normal.exists) {
                        ufbx_vec3 localNorm = ufbx_get_vertex_vec3(&mesh->vertex_normal, meshIdx);
                        worldNorm = ufbx_transform_direction(&normMat, localNorm);
                    }

                    Vec3 vColor = { 1.0f, 1.0f, 1.0f };
                    if (mesh->vertex_color.exists) {
                        ufbx_vec4 clr = ufbx_get_vertex_vec4(&mesh->vertex_color, meshIdx);
                        vColor = { (float)clr.x, (float)clr.y, (float)clr.z };
                    }

                    uint32_t idx = (uint32_t)outMesh.positions.size();
                    outMesh.positions.push_back({ (float)worldPos.x, (float)worldPos.y, (float)worldPos.z });
                    outMesh.normals.push_back({ (float)worldNorm.x, (float)worldNorm.y, (float)worldNorm.z });
                    outMesh.colors.push_back(vColor);
                    outMesh.indices.push_back(idx);
                }
            }
        }
    }

    ufbx_free_scene(scene);

    if (outMesh.positions.empty()) {
        outError = "High-poly FBX contains no geometry";
        return false;
    }

    return true;
}

bool MeshLoader::LoadLowPolyOBJ(
    const std::string& filePath,
    LowPolyMesh& outMesh,
    std::string& outError
) {
    outMesh.Clear();

    tinyobj::ObjReaderConfig readerConfig;
    readerConfig.triangulate = false;

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

    uint32_t shapeIdx = 0;
    for (const auto& shape : shapes) {
        uint32_t shapeMask = (1u << (shapeIdx % 32));
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv < 3) {
                indexOffset += fv;
                continue;
            }

            std::vector<uint32_t> faceIndices(fv);
            float faceU = 0.0f;
            float faceV = 0.0f;

            for (size_t v = 0; v < fv; ++v) {
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

                faceU += vert.uv.u;
                faceV += vert.uv.v;

                uint32_t newIdx = static_cast<uint32_t>(outMesh.vertices.size());
                outMesh.vertices.push_back(vert);
                faceIndices[v] = newIdx;
            }

            Vertex& v0 = outMesh.vertices[faceIndices[0]];
            Vertex& v1 = outMesh.vertices[faceIndices[1]];
            Vertex& v2 = outMesh.vertices[faceIndices[2]];
            Vec3 edge1 = v1.position - v0.position;
            Vec3 edge2 = v2.position - v0.position;
            Vec3 faceNormal = edge1.Cross(edge2).Normalized();

            faceU /= (float)fv;
            faceV /= (float)fv;
            int tileId = CalculateUDIMTile(faceU, faceV);
            udimSet.insert(tileId);

            // 1. Wireframe lines: Perimeter of original polygon (NO diagonal splits!)
            for (size_t v = 0; v < fv; ++v) {
                outMesh.wireframeLineIndices.push_back(faceIndices[v]);
                outMesh.wireframeLineIndices.push_back(faceIndices[(v + 1) % fv]);
            }

            // 2. Count original topology and store polygon faces
            std::vector<uint32_t> polyFace(fv);
            for (size_t v = 0; v < fv; ++v) {
                polyFace[v] = faceIndices[v];
            }
            outMesh.polygonFaces.push_back(std::move(polyFace));

            if (fv == 4) {
                outMesh.quadCount++;
            } else if (fv == 3) {
                outMesh.triCount++;
            }
            outMesh.polyCount++;

            // 3. Triangulate for baking ray-tracing and MikkTSpace (fan triangulation: 0, k, k+1)
            for (size_t k = 1; k + 1 < fv; ++k) {
                LowPolyTriangle tri;
                tri.meshIndex = shapeIdx;
                tri.groupMask = shapeMask;
                tri.udimTile = tileId;
                tri.indices[0] = faceIndices[0];
                tri.indices[1] = faceIndices[k];
                tri.indices[2] = faceIndices[k + 1];

                outMesh.indices.push_back(tri.indices[0]);
                outMesh.indices.push_back(tri.indices[1]);
                outMesh.indices.push_back(tri.indices[2]);

                outMesh.triangles.push_back(tri);
            }

            indexOffset += fv;
        }
        shapeIdx++;
    }

    outMesh.detectedUDIMs.assign(udimSet.begin(), udimSet.end());
    if (outMesh.detectedUDIMs.empty()) {
        outMesh.detectedUDIMs.push_back(1001);
    }

    outMesh.SmoothNormalsIfMissing(180.0f);
    MikkTSpaceBridge::CalculateTangents(outMesh);
    outMesh.ComputeWatertightCageNormals(180.0f);
    DeduplicateWireframeLines(outMesh);
    return true;
}

bool MeshLoader::SaveLowPolyOBJWithNormals(
    const std::string& filePath,
    const LowPolyMesh& mesh,
    std::string& outError
) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        outError = "Cannot save empty mesh to OBJ";
        return false;
    }

    std::ofstream out(filePath);
    if (!out.is_open()) {
        outError = "Failed to open destination OBJ file: " + filePath;
        return false;
    }

    // Deduplicate geometric vertex positions so adjacent faces share 'v' indices,
    // preserving watertight topology and smooth edge continuity in DCCs / Blender
    std::unordered_map<PosKey, uint32_t, PosHasher> posMap;
    std::vector<Vec3> uniquePositions;
    std::vector<uint32_t> vertToPos(mesh.vertices.size());

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        PosKey k = QuantizePos(mesh.vertices[i].position);
        auto it = posMap.find(k);
        if (it == posMap.end()) {
            uint32_t pIdx = static_cast<uint32_t>(uniquePositions.size());
            posMap[k] = pIdx;
            uniquePositions.push_back(mesh.vertices[i].position);
            vertToPos[i] = pIdx;
        } else {
            vertToPos[i] = it->second;
        }
    }

    out << "# Low-Poly Mesh with matching vertex normals baked by ZBrush UDIM Baker\n";
    out << "# Geometric Vertex Count: " << uniquePositions.size() << "\n";
    out << "# Corner Vertex Count: " << mesh.vertices.size() << "\n";
    out << "# Triangle Count: " << mesh.triangles.size() << "\n";

    // 1. Shared Geometric Vertices (v)
    for (const auto& pos : uniquePositions) {
        out << "v " << pos.x << " " << pos.y << " " << pos.z << "\n";
    }

    // 2. Texture Coordinates per face corner (vt)
    for (const auto& v : mesh.vertices) {
        out << "vt " << v.uv.u << " " << v.uv.v << "\n";
    }

    // 3. Vertex Normals per face corner (vn)
    for (const auto& v : mesh.vertices) {
        out << "vn " << v.normal.x << " " << v.normal.y << " " << v.normal.z << "\n";
    }

    // 4. Triangles with 1-based indexing f v/vt/vn
    out << "s 1\n";
    for (const auto& tri : mesh.triangles) {
        uint32_t c0 = tri.indices[0];
        uint32_t c1 = tri.indices[1];
        uint32_t c2 = tri.indices[2];

        uint32_t p0 = vertToPos[c0] + 1;
        uint32_t p1 = vertToPos[c1] + 1;
        uint32_t p2 = vertToPos[c2] + 1;

        uint32_t l0 = c0 + 1;
        uint32_t l1 = c1 + 1;
        uint32_t l2 = c2 + 1;

        out << "f " << p0 << "/" << l0 << "/" << l0 << " "
                    << p1 << "/" << l1 << "/" << l1 << " "
                    << p2 << "/" << l2 << "/" << l2 << "\n";
    }

    out.close();
    return true;
}

void MeshLoader::DeduplicateWireframeLines(LowPolyMesh& mesh) {
    if (mesh.vertices.empty()) return;

    std::unordered_map<PosKey, uint32_t, PosHasher> posMap;
    posMap.reserve(mesh.vertices.size());
    std::unordered_set<uint64_t> uniqueEdges;

    std::vector<uint32_t> cleanLines;
    cleanLines.reserve(mesh.wireframeLineIndices.size());

    // 1. If wireframeLineIndices was generated from polygon perimeter edges:
    if (!mesh.wireframeLineIndices.empty()) {
        for (size_t i = 0; i + 1 < mesh.wireframeLineIndices.size(); i += 2) {
            uint32_t v0 = mesh.wireframeLineIndices[i];
            uint32_t v1 = mesh.wireframeLineIndices[i + 1];
            if (v0 >= mesh.vertices.size() || v1 >= mesh.vertices.size()) continue;

            PosKey pk0 = QuantizePos(mesh.vertices[v0].position);
            PosKey pk1 = QuantizePos(mesh.vertices[v1].position);
            if (pk0 == pk1) continue;

            uint32_t id0 = posMap.emplace(pk0, static_cast<uint32_t>(posMap.size())).first->second;
            uint32_t id1 = posMap.emplace(pk1, static_cast<uint32_t>(posMap.size())).first->second;

            uint32_t minId = (std::min)(id0, id1);
            uint32_t maxId = (std::max)(id0, id1);
            uint64_t edgeKey = (static_cast<uint64_t>(minId) << 32) | static_cast<uint64_t>(maxId);

            if (uniqueEdges.insert(edgeKey).second) {
                cleanLines.push_back(v0);
                cleanLines.push_back(v1);
            }
        }
        mesh.wireframeLineIndices = std::move(cleanLines);
    } else if (!mesh.triangles.empty()) {
        // 2. Fallback: extract unique geometric edges from triangles
        for (const auto& tri : mesh.triangles) {
            for (int e = 0; e < 3; ++e) {
                uint32_t v0 = tri.indices[e];
                uint32_t v1 = tri.indices[(e + 1) % 3];
                if (v0 >= mesh.vertices.size() || v1 >= mesh.vertices.size()) continue;

                PosKey pk0 = QuantizePos(mesh.vertices[v0].position);
                PosKey pk1 = QuantizePos(mesh.vertices[v1].position);
                if (pk0 == pk1) continue;

                uint32_t id0 = posMap.emplace(pk0, static_cast<uint32_t>(posMap.size())).first->second;
                uint32_t id1 = posMap.emplace(pk1, static_cast<uint32_t>(posMap.size())).first->second;

                uint32_t minId = (std::min)(id0, id1);
                uint32_t maxId = (std::max)(id0, id1);
                uint64_t edgeKey = (static_cast<uint64_t>(minId) << 32) | static_cast<uint64_t>(maxId);

                if (uniqueEdges.insert(edgeKey).second) {
                    cleanLines.push_back(v0);
                    cleanLines.push_back(v1);
                }
            }
        }
        mesh.wireframeLineIndices = std::move(cleanLines);
    }
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
    readerConfig.triangulate = false;

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
            if (fv < 3) {
                indexOffset += fv;
                continue;
            }

            for (size_t v = 0; v < fv; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                Vec3 p(0, 0, 0);
                if (idx.vertex_index >= 0) {
                    p.x = attrib.vertices[3 * idx.vertex_index + 0];
                    p.y = attrib.vertices[3 * idx.vertex_index + 1];
                    p.z = attrib.vertices[3 * idx.vertex_index + 2];
                }
                cageVerts.push_back(p);
            }
            indexOffset += fv;
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

    if (attrib.colors.size() == attrib.vertices.size()) {
        outMesh.colors.resize(numVerts);
        for (size_t i = 0; i < numVerts; ++i) {
            outMesh.colors[i] = Vec3(
                attrib.colors[3 * i + 0],
                attrib.colors[3 * i + 1],
                attrib.colors[3 * i + 2]
            );
        }
    }

    uint32_t currentGroup = 1;
    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv == 3) {
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 0].vertex_index));
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 1].vertex_index));
                outMesh.indices.push_back(static_cast<uint32_t>(shape.mesh.indices[indexOffset + 2].vertex_index));

                int mtlId = (f < shape.mesh.material_ids.size()) ? shape.mesh.material_ids[f] : -1;
                uint32_t pg = (mtlId >= 0) ? static_cast<uint32_t>(mtlId + 1) : currentGroup;
                outMesh.polygroupPerFace.push_back(pg);
            }
            indexOffset += fv;
        }
        currentGroup++;
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
        float len = outMesh.normals[i].Length();
        if (len > 1e-20f) {
            outMesh.normals[i] = outMesh.normals[i] * (1.0f / len);
        } else {
            outMesh.normals[i] = Vec3(0, 1, 0);
        }
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
