#include "game/content/CommercialVisualAssets.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include <stb/stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace OpenYAMM::Game
{
namespace
{
constexpr size_t LodDirectoryOffset = 0x120;
constexpr size_t LodEntrySize = 32;
constexpr uint32_t MaxImageDimension = 4096;
constexpr std::string_view CacheVersion = "original-visuals-v1";

struct WorldSource
{
    std::string id;
    std::filesystem::path sprites;
    std::filesystem::path bitmaps;
};

uint16_t u16(const std::vector<uint8_t> &data, size_t at)
{
    return uint16_t(data[at]) | uint16_t(uint16_t(data[at + 1]) << 8);
}

uint32_t u32(const std::vector<uint8_t> &data, size_t at)
{
    return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16)
        | (uint32_t(data[at + 3]) << 24);
}

void put16(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(uint8_t(value));
    data.push_back(uint8_t(value >> 8));
}

void put32(std::vector<uint8_t> &data, uint32_t value)
{
    data.push_back(uint8_t(value));
    data.push_back(uint8_t(value >> 8));
    data.push_back(uint8_t(value >> 16));
    data.push_back(uint8_t(value >> 24));
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::optional<std::filesystem::path> child(
    const std::filesystem::path &folder,
    std::string_view name,
    bool wantDirectory)
{
    std::error_code error;
    const std::string expected = lower(std::string(name));
    for (std::filesystem::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error))
    {
        std::error_code typeError;
        const bool typeMatches = wantDirectory ? entry->is_directory(typeError) : entry->is_regular_file(typeError);
        if (!typeError && typeMatches && lower(entry->path().filename().string()) == expected)
        {
            return entry->path();
        }
    }
    return std::nullopt;
}

bool locateSources(const std::filesystem::path &root, std::array<WorldSource, 3> &sources, std::string &error)
{
    const std::array<std::pair<const char *, const char *>, 3> games = {{
        {"mm6", "Might and Magic 6"}, {"mm7", "Might and Magic 7"}, {"mm8", "Might and Magic 8"}
    }};
    std::error_code rootError;
    if (!std::filesystem::is_directory(root, rootError))
    {
        error = "Purchased games folder does not exist: " + root.string();
        return false;
    }
    for (size_t index = 0; index < games.size(); ++index)
    {
        const auto game = child(root, games[index].second, true);
        const auto data = game ? child(*game, "data", true) : std::nullopt;
        const auto sprites = data ? child(*data, "sprites.lod", false) : std::nullopt;
        const auto bitmaps = data ? child(*data, "bitmaps.lod", false) : std::nullopt;
        if (!sprites || !bitmaps)
        {
            error = "Missing " + std::string(games[index].second)
                + " Data\\SPRITES.LOD or Data\\BITMAPS.LOD below " + root.string();
            return false;
        }
        sources[index] = {games[index].first, *sprites, *bitmaps};
    }
    error.clear();
    return true;
}

void hashText(uint64_t &hash, std::string_view value)
{
    for (unsigned char c : value)
    {
        hash = (hash ^ c) * 1099511628211ull;
    }
}

std::string fingerprint(const std::array<WorldSource, 3> &sources, std::string &error)
{
    uint64_t hash = 1469598103934665603ull;
    hashText(hash, CacheVersion);
    for (const WorldSource &source : sources)
    {
        for (const std::filesystem::path &path : {source.sprites, source.bitmaps})
        {
            std::error_code sizeError, timeError, canonicalError;
            const uintmax_t size = std::filesystem::file_size(path, sizeError);
            const auto time = std::filesystem::last_write_time(path, timeError).time_since_epoch().count();
            const auto canonical = std::filesystem::weakly_canonical(path, canonicalError);
            if (sizeError || timeError)
            {
                error = "Cannot inspect purchased asset file: " + path.string();
                return {};
            }
            hashText(hash, lower((canonicalError ? path : canonical).generic_string()));
            hashText(hash, std::to_string(size));
            hashText(hash, std::to_string(time));
        }
    }
    std::ostringstream value;
    value << CacheVersion << '-' << std::hex << std::setfill('0') << std::setw(16) << hash;
    return value.str();
}

bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes, std::string &error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "Cannot read purchased asset file: " + path.string();
        return false;
    }
    const std::streamoff length = std::streamoff(file.tellg());
    if (length <= 0 || uint64_t(length) > uint64_t(std::numeric_limits<size_t>::max()))
    {
        error = "Cannot read purchased asset file: " + path.string();
        return false;
    }
    bytes.resize(size_t(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), length))
    {
        error = "Cannot read purchased asset file: " + path.string();
        return false;
    }
    return true;
}

bool inflate(const uint8_t *encoded, size_t encodedSize, size_t expectedSize, std::vector<uint8_t> &decoded)
{
    if (encodedSize > size_t(INT_MAX) || expectedSize > size_t(INT_MAX))
    {
        return false;
    }
    decoded.resize(expectedSize);
    const int count = stbi_zlib_decode_buffer(reinterpret_cast<char *>(decoded.data()), int(decoded.size()),
                                               reinterpret_cast<const char *>(encoded), int(encodedSize));
    if (count < 0 || size_t(count) < expectedSize)
    {
        return false;
    }
    decoded.resize(size_t(count));
    return true;
}

bool validDimensions(uint32_t width, uint32_t height)
{
    return width > 0 && height > 0 && width <= MaxImageDimension && height <= MaxImageDimension;
}

bool decodeSprite(const std::vector<uint8_t> &lod, size_t at, size_t size,
                  uint32_t &width, uint32_t &height, std::vector<uint8_t> &pixels)
{
    if (size < 32 || at > lod.size() || size > lod.size() - at)
    {
        return false;
    }
    width = u16(lod, at + 16);
    height = u16(lod, at + 18);
    const size_t packedSize = u32(lod, at + 28);
    const size_t rowBytes = size_t(height) * 8;
    if (!validDimensions(width, height) || rowBytes > size - 32 || packedSize == 0)
    {
        return false;
    }
    const size_t encodedAt = at + 32 + rowBytes;
    const size_t encodedSize = at + size - encodedAt;
    std::vector<uint8_t> packed;
    if (encodedSize > 0 && lod[encodedAt] == 0x78)
    {
        if (!inflate(lod.data() + encodedAt, encodedSize, packedSize, packed))
        {
            return false;
        }
    }
    else if (encodedSize >= packedSize)
    {
        packed.assign(lod.begin() + ptrdiff_t(encodedAt), lod.begin() + ptrdiff_t(encodedAt + packedSize));
    }
    else
    {
        return false;
    }
    pixels.assign(size_t(width) * height, 0);
    for (uint32_t row = 0; row < height; ++row)
    {
        const size_t span = at + 32 + size_t(row) * 8;
        const uint32_t left = u16(lod, span);
        const uint32_t right = u16(lod, span + 2);
        const size_t source = u32(lod, span + 4);
        if (left == 0xffff || right < left)
        {
            continue;
        }
        const size_t count = size_t(right - left) + 1;
        if (right >= width || source > packed.size() || count > packed.size() - source)
        {
            return false;
        }
        std::copy_n(packed.begin() + ptrdiff_t(source), count,
                    pixels.begin() + ptrdiff_t(size_t(row) * width + left));
    }
    return true;
}

bool decodeBitmap(const std::vector<uint8_t> &lod, size_t at, size_t size,
                  uint32_t &width, uint32_t &height, std::vector<uint8_t> &pixels,
                  std::array<uint8_t, 768> &palette)
{
    if (size < 48 || at > lod.size() || size > lod.size() - at)
    {
        return false;
    }
    const size_t topSize = u32(lod, at + 16);
    const size_t encodedSize = u32(lod, at + 20);
    width = u16(lod, at + 24);
    height = u16(lod, at + 26);
    const size_t mipSize = u32(lod, at + 40);
    const size_t pixelCount = size_t(width) * height;
    const size_t encodedAt = at + 48;
    if (!validDimensions(width, height) || topSize < pixelCount || encodedSize > size - 48
        || 768 > size - 48 - encodedSize)
    {
        return false;
    }
    std::vector<uint8_t> decoded;
    if (encodedSize > 0 && lod[encodedAt] == 0x78)
    {
        if (!inflate(lod.data() + encodedAt, encodedSize, std::max(pixelCount, mipSize), decoded))
        {
            return false;
        }
    }
    else if (encodedSize >= pixelCount)
    {
        decoded.assign(lod.begin() + ptrdiff_t(encodedAt), lod.begin() + ptrdiff_t(encodedAt + encodedSize));
    }
    else
    {
        return false;
    }
    pixels.assign(decoded.begin(), decoded.begin() + ptrdiff_t(pixelCount));
    std::copy_n(lod.begin() + ptrdiff_t(encodedAt + encodedSize), palette.size(), palette.begin());
    return true;
}

std::optional<std::string> assetName(const uint8_t *name)
{
    std::string result;
    for (size_t index = 0; index < 16 && name[index] != 0; ++index)
    {
        const unsigned char c = name[index];
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.'))
        {
            return std::nullopt;
        }
        result.push_back(char(std::tolower(c)));
    }
    const size_t extension = result.find_last_of('.');
    if (extension != std::string::npos)
    {
        result.resize(extension);
    }
    return result.empty() ? std::nullopt : std::optional<std::string>(result);
}

std::vector<uint8_t> makeBmp(uint32_t width, uint32_t height, const std::vector<uint8_t> &pixels,
                             const std::array<uint8_t, 768> *palette)
{
    const uint32_t stride = (width + 3) & ~uint32_t(3);
    constexpr uint32_t PixelAt = 14 + 40 + 256 * 4;
    std::vector<uint8_t> bmp;
    bmp.reserve(PixelAt + stride * height);
    bmp.insert(bmp.end(), {'B', 'M'});
    put32(bmp, PixelAt + stride * height);
    put16(bmp, 0); put16(bmp, 0); put32(bmp, PixelAt); put32(bmp, 40);
    put32(bmp, width); put32(bmp, height); put16(bmp, 1); put16(bmp, 8); put32(bmp, 0);
    put32(bmp, stride * height); put32(bmp, 2835); put32(bmp, 2835); put32(bmp, 256); put32(bmp, 256);
    for (size_t index = 0; index < 256; ++index)
    {
        const uint8_t red = palette ? (*palette)[index * 3] : uint8_t(index);
        const uint8_t green = palette ? (*palette)[index * 3 + 1] : uint8_t(index);
        const uint8_t blue = palette ? (*palette)[index * 3 + 2] : uint8_t(index);
        bmp.insert(bmp.end(), {blue, green, red, 0});
    }
    bmp.resize(PixelAt + stride * height, 0);
    for (uint32_t row = 0; row < height; ++row)
    {
        std::copy_n(pixels.begin() + ptrdiff_t(size_t(row) * width), width,
                    bmp.begin() + ptrdiff_t(PixelAt + size_t(height - row - 1) * stride));
    }
    return bmp;
}

bool writeAtomic(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, std::string &error)
{
    std::error_code fileError;
    if (std::filesystem::is_regular_file(path, fileError))
    {
        return true;
    }
    std::filesystem::create_directories(path.parent_path(), fileError);
    if (fileError)
    {
        error = "Cannot create original visuals cache: " + path.parent_path().string();
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::filesystem::remove(temporary, fileError);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size())))
    {
        error = "Cannot write original visuals cache: " + temporary.string();
        return false;
    }
    file.close();
    std::filesystem::rename(temporary, path, fileError);
    if (fileError)
    {
        error = "Cannot finish original visuals cache file: " + path.string();
        return false;
    }
    return true;
}

bool importLod(const std::filesystem::path &path, const std::filesystem::path &target, bool sprites,
               size_t &imported, size_t &skipped, std::string &error)
{
    std::vector<uint8_t> lod;
    if (!readFile(path, lod, error))
    {
        return false;
    }
    if (lod.size() < LodDirectoryOffset || lod[0] != 'L' || lod[1] != 'O' || lod[2] != 'D')
    {
        error = "Unsupported purchased LOD: " + path.string();
        return false;
    }
    const size_t base = u32(lod, 0x110);
    const size_t count = u32(lod, 0x11c);
    if (base > lod.size() || count > (lod.size() - LodDirectoryOffset) / LodEntrySize)
    {
        error = "Invalid purchased LOD directory: " + path.string();
        return false;
    }
    for (size_t index = 0; index < count; ++index)
    {
        const size_t entry = LodDirectoryOffset + index * LodEntrySize;
        const auto name = assetName(lod.data() + entry);
        const size_t relative = u32(lod, entry + 16);
        const size_t payloadSize = u32(lod, entry + 20);
        if (!name || relative > lod.size() - base || payloadSize > lod.size() - base - relative)
        {
            ++skipped;
            continue;
        }
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixels;
        std::array<uint8_t, 768> palette = {};
        const bool decoded = sprites
            ? decodeSprite(lod, base + relative, payloadSize, width, height, pixels)
            : decodeBitmap(lod, base + relative, payloadSize, width, height, pixels, palette);
        if (!decoded)
        {
            ++skipped;
            continue;
        }
        if (!writeAtomic(target / (*name + ".bmp"), makeBmp(width, height, pixels, sprites ? nullptr : &palette), error))
        {
            return false;
        }
        ++imported;
    }
    return true;
}
}

bool validateCommercialVisualAssetsRoot(const std::filesystem::path &gamesRoot, std::string &error)
{
    std::array<WorldSource, 3> sources;
    return locateSources(gamesRoot, sources, error);
}

bool validateCommercialMusicRoot(const std::filesystem::path &gamesRoot, std::string &error)
{
    const std::array<std::pair<const char *, const char *>, 3> games = {{
        {"Might and Magic 6", "Sounds"},
        {"Might and Magic 7", "Music"},
        {"Might and Magic 8", "Music"},
    }};
    std::error_code rootError;
    if (!std::filesystem::is_directory(gamesRoot, rootError))
    {
        error = "Purchased games folder does not exist: " + gamesRoot.string();
        return false;
    }
    for (const auto &[gameName, musicName] : games)
    {
        const std::optional<std::filesystem::path> game = child(gamesRoot, gameName, true);
        const std::optional<std::filesystem::path> music = game ? child(*game, musicName, true) : std::nullopt;
        const std::optional<std::filesystem::path> firstTrack = music ? child(*music, "2.mp3", false) : std::nullopt;
        if (!firstTrack)
        {
            error = "Missing " + std::string(gameName) + "\\" + musicName + "\\2.mp3 below "
                + gamesRoot.string();
            return false;
        }
    }
    error.clear();
    return true;
}

bool prepareCommercialVisualAssets(const std::filesystem::path &gamesRoot,
                                   const std::filesystem::path &cacheParent,
                                   CommercialVisualAssetsResult &result, std::string &error)
{
    result = {};
    std::array<WorldSource, 3> sources;
    if (!locateSources(gamesRoot, sources, error))
    {
        return false;
    }
    const std::string sourceId = fingerprint(sources, error);
    if (sourceId.empty())
    {
        return false;
    }
    result.cacheRoot = cacheParent / sourceId;
    const std::filesystem::path ready = result.cacheRoot / "ready.txt";
    std::error_code readyError;
    if (std::filesystem::is_regular_file(ready, readyError))
    {
        std::cout << "Original visuals: using cache " << result.cacheRoot << '\n';
        return true;
    }
    std::cout << "Original visuals: decoding purchased art into " << result.cacheRoot << " (first launch only)\n";
    size_t skipped = 0;
    for (const WorldSource &source : sources)
    {
        size_t sprites = 0, textures = 0;
        const std::filesystem::path world = result.cacheRoot / "worlds" / source.id;
        if (!importLod(source.sprites, world / "sprites", true, sprites, skipped, error)
            || !importLod(source.bitmaps, world / "textures", false, textures, skipped, error))
        {
            return false;
        }
        result.spriteCount += sprites;
        result.textureCount += textures;
        std::cout << "Original visuals: " << source.id << ": " << sprites << " sprites, " << textures << " textures\n";
    }
    if (result.spriteCount == 0 || result.textureCount == 0)
    {
        error = "The purchased LODs contained no supported visual assets.";
        return false;
    }
    const std::string marker = std::string(CacheVersion) + "\nsprites=" + std::to_string(result.spriteCount)
        + "\ntextures=" + std::to_string(result.textureCount) + "\n";
    if (!writeAtomic(ready, std::vector<uint8_t>(marker.begin(), marker.end()), error))
    {
        return false;
    }
    std::cout << "Original visuals: ready; skipped " << skipped << " unsupported records\n";
    return true;
}
}
