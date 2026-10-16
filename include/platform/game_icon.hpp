#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

/**
 * Describes a standard glTF icon and its presentation options.
 */
struct GameIconSource {
    std::vector<std::filesystem::path>                                               packs;              /**< Native asset directories relative to the data root, in priority order. */
    const char                                                                      *member = "";        /**< Actual standard glTF filename within each directory. */
    float                                                                            yaw = 25.0f;        /**< Rotation around the vertical axis, in degrees. */
    float                                                                            roll = -5.0f;       /**< Rotation around the viewing axis, in degrees. */
    const wchar_t                                                                   *ico_file = nullptr; /**< Optional Windows icon filename beside the data directory. */
    const wchar_t                                                                   *shortcut = nullptr; /**< Optional Windows shortcut name beside the executable. */
    std::function<std::vector<std::filesystem::path>(const std::filesystem::path &)> candidates;         /**< Optional physical glTF candidates in datapack priority order. */
};

/**
 * Draws standard glTF model bytes to RGBA pixels over a transparent background.
 */
std::vector<std::uint8_t> GameIconRender(const std::uint8_t *ico, std::size_t ico_size, int size, float yaw = 25.0f,
                                         float roll = -5.0f);

/**
 * Draws the first valid standard glTF model in the source's native directories.
 */
std::vector<std::uint8_t> GameIconFromData(const std::filesystem::path &data_root, const GameIconSource &source,
                                           int size);

/**
 * Installs the rendered window icon and optional Windows icon file and shortcut.
 */
void GameIconInstall(const std::filesystem::path &data_root, const GameIconSource &source);
