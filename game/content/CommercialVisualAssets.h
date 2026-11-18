#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace OpenYAMM::Game
{
struct CommercialVisualAssetsResult
{
    std::filesystem::path cacheRoot;
    size_t spriteCount = 0;
    size_t textureCount = 0;
};

bool validateCommercialVisualAssetsRoot(const std::filesystem::path &gamesRoot, std::string &error);
bool validateCommercialMusicRoot(const std::filesystem::path &gamesRoot, std::string &error);
bool prepareCommercialVisualAssets(
    const std::filesystem::path &gamesRoot,
    const std::filesystem::path &cacheParent,
    CommercialVisualAssetsResult &result,
    std::string &error
);
}
