#include "MikkTSpaceBridge.h"
#include <mikktspace.h>
#include <vector>
#include <iostream>

namespace Baker {

struct MikkUserData {
    LowPolyMesh* pMesh = nullptr;
    std::vector<Vec4> generatedTangents; // [iFace * 3 + iVert]
};

static int MikkGetNumFaces(const SMikkTSpaceContext* pContext) {
    auto* data = static_cast<MikkUserData*>(pContext->m_pUserData);
    if (!data || !data->pMesh) return 0;
    return static_cast<int>(data->pMesh->indices.size() / 3);
}

static int MikkGetNumVerticesOfFace(const SMikkTSpaceContext* pContext, const int iFace) {
    return 3;
}

static void MikkGetPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert) {
    auto* data = static_cast<MikkUserData*>(pContext->m_pUserData);
    uint32_t idx = data->pMesh->indices[iFace * 3 + iVert];
    const Vec3& p = data->pMesh->vertices[idx].position;
    fvPosOut[0] = p.x;
    fvPosOut[1] = p.y;
    fvPosOut[2] = p.z;
}

static void MikkGetNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert) {
    auto* data = static_cast<MikkUserData*>(pContext->m_pUserData);
    uint32_t idx = data->pMesh->indices[iFace * 3 + iVert];
    const Vec3& n = data->pMesh->vertices[idx].normal;
    fvNormOut[0] = n.x;
    fvNormOut[1] = n.y;
    fvNormOut[2] = n.z;
}

static void MikkGetTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert) {
    auto* data = static_cast<MikkUserData*>(pContext->m_pUserData);
    uint32_t idx = data->pMesh->indices[iFace * 3 + iVert];
    const Vec2& uv = data->pMesh->vertices[idx].uv;
    fvTexcOut[0] = uv.u;
    fvTexcOut[1] = uv.v;
}

static void MikkSetTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert) {
    auto* data = static_cast<MikkUserData*>(pContext->m_pUserData);
    size_t cornerIdx = static_cast<size_t>(iFace * 3 + iVert);
    if (cornerIdx < data->generatedTangents.size()) {
        data->generatedTangents[cornerIdx] = Vec4(fvTangent[0], fvTangent[1], fvTangent[2], fSign);
    }
}

bool MikkTSpaceBridge::CalculateTangents(LowPolyMesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty() || (mesh.indices.size() % 3 != 0)) {
        return false;
    }

    MikkUserData userData;
    userData.pMesh = &mesh;
    userData.generatedTangents.resize(mesh.indices.size());

    SMikkTSpaceInterface iface;
    iface.m_getNumFaces = MikkGetNumFaces;
    iface.m_getNumVerticesOfFace = MikkGetNumVerticesOfFace;
    iface.m_getPosition = MikkGetPosition;
    iface.m_getNormal = MikkGetNormal;
    iface.m_getTexCoord = MikkGetTexCoord;
    iface.m_setTSpaceBasic = MikkSetTSpaceBasic;
    iface.m_setTSpace = nullptr;

    SMikkTSpaceContext context;
    context.m_pInterface = &iface;
    context.m_pUserData = &userData;

    tbool res = genTangSpaceDefault(&context);
    if (!res) {
        return false;
    }

    // If mesh is already unrolled per-face, assign directly to vertex
    if (mesh.vertices.size() == mesh.indices.size()) {
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            mesh.vertices[i].tangent = userData.generatedTangents[i];
        }
    } else {
        // Average / assign tangents into indexed vertices
        std::vector<Vec3> tangentSums(mesh.vertices.size(), Vec3(0, 0, 0));
        std::vector<float> signSums(mesh.vertices.size(), 0.0f);
        std::vector<int> counts(mesh.vertices.size(), 0);

        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            uint32_t vIdx = mesh.indices[i];
            const Vec4& t = userData.generatedTangents[i];
            tangentSums[vIdx] = tangentSums[vIdx] + Vec3(t.x, t.y, t.z);
            signSums[vIdx] += t.w;
            counts[vIdx]++;
        }

        for (size_t v = 0; v < mesh.vertices.size(); ++v) {
            if (counts[v] > 0) {
                Vec3 avgT = tangentSums[v].Normalized();
                float s = (signSums[v] >= 0.0f) ? 1.0f : -1.0f;
                mesh.vertices[v].tangent = Vec4(avgT.x, avgT.y, avgT.z, s);
            }
        }
    }

    return true;
}

} // namespace Baker
