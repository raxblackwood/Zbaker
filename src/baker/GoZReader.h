#pragma once

#include "MeshTypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <future>

namespace Baker {

class GoZReader {
public:
    // Read GoZ binary file directly into HighPolyMesh via Memory-Mapped IO (zero-copy)
    static bool ReadGoZFile(
        const std::string& filePath,
        HighPolyMesh& outMesh,
        std::string& outError
    );

    // Asynchronous background thread loader for responsive UI
    static std::future<bool> ReadGoZFileAsync(
        const std::string& filePath,
        std::shared_ptr<HighPolyMesh> outMesh,
        std::shared_ptr<std::string> outError
    );

    // SubTool memory cache management (Decision 4: Cache per SubTool, reload only what changed)
    struct CachedSubTool {
        std::string subToolId;
        std::string filePath;
        uint64_t fileTimestamp = 0;
        uint64_t fileSize = 0;
        HighPolyMesh mesh;
    };

    static bool GetOrLoadSubTool(
        const std::string& filePath,
        HighPolyMesh& outMesh,
        bool forceReload,
        std::string& outError
    );

    static void ClearCache();
    static size_t GetCachedSubToolCount();

private:
    static std::unordered_map<std::string, CachedSubTool> s_subToolCache;
};

} // namespace Baker
