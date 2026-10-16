#include "platform/texture_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "platform/imagefile.hpp"

namespace texture_catalog {
namespace {
std::unordered_map<std::string, Source>                                          sources;
std::filesystem::path                                                            port_data_root;
std::function<std::vector<std::filesystem::path>(const std::filesystem::path &)> candidate_resolver;

bool RelativePath(const std::string &name, bool allow_parent = false) {
    const std::filesystem::path path(name);
    if (name.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return false;
    }
    for (const auto &part : path) {
        if (part.empty() || part == "." || (!allow_parent && part == "..")) {
            return false;
        }
    }
    return true;
}

bool Within(const std::filesystem::path &base, const std::filesystem::path &path);

std::filesystem::path AssetPath(const std::filesystem::path &base, const std::filesystem::path &root,
                                const nlohmann::json &value, bool allow_parent = false) {
    const auto name = value.get<std::string>();
    if (!RelativePath(name, allow_parent)) {
        throw std::runtime_error("invalid relative asset path: " + name);
    }
    const auto path = std::filesystem::weakly_canonical(base / name);
    if (!Within(root, path)) {
        throw std::runtime_error("asset path leaves catalog directory: " + name);
    }
    return path;
}

bool Within(const std::filesystem::path &base, const std::filesystem::path &path) {
    auto ancestor = base.begin();
    auto child = path.begin();
    for (; ancestor != base.end(); ++ancestor, ++child) {
        if (child == path.end() || *ancestor != *child) {
            return false;
        }
    }
    return child != path.end();
}

/**
 * Rejects fields outside the compact texture document vocabulary.
 */
void CheckKeys(const nlohmann::json &value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        throw std::runtime_error("expected JSON object");
    }
    for (const auto &[key, ignored] : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error("unsupported texture metadata: " + key);
        }
    }
}

/**
 * Reads a nonnegative integer without truncating fractions or large values.
 */
int NonnegativeInt(const nlohmann::json &value, int maximum = INT_MAX) {
    if (!value.is_number_integer()) {
        throw std::runtime_error("expected integer texture value");
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= static_cast<uint64_t>(maximum)) {
            return static_cast<int>(number);
        }
    } else {
        const auto number = value.get<int64_t>();
        if (number >= 0 && number <= maximum) {
            return static_cast<int>(number);
        }
    }
    throw std::runtime_error("texture integer is out of range");
}

/**
 * Reads a base PNG's logical width and height from its image header.
 */
std::pair<int, int> PngExtent(const std::filesystem::path &path) {
    std::ifstream                 input(path, std::ios::binary);
    std::array<unsigned char, 24> header{};
    input.read(reinterpret_cast<char *>(header.data()), header.size());
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (!input || !std::equal(signature.begin(), signature.end(), header.begin()) ||
        !std::equal(header.begin() + 12, header.begin() + 16, "IHDR")) {
        throw std::runtime_error("invalid base PNG: " + path.string());
    }
    const auto word = [&](int offset) {
        return (uint32_t{header[offset]} << 24) | (uint32_t{header[offset + 1]} << 16) |
               (uint32_t{header[offset + 2]} << 8) | header[offset + 3];
    };
    const auto width = word(16), height = word(20);
    if (!width || !height || width > 16384 || height > 16384) {
        throw std::runtime_error("invalid base PNG dimensions: " + path.string());
    }
    return {static_cast<int>(width), static_cast<int>(height)};
}

/**
 * Resolves the PNG chain belonging to one selectable image.
 */
void ReadLevels(Entry::Variant &variant, const nlohmann::json &levels, const std::filesystem::path &base,
                const std::filesystem::path &root, bool embedded) {
    if (!levels.is_array() || levels.empty() || levels.size() > 4) {
        throw std::runtime_error("invalid PNG mip chain");
    }
    for (const auto &level : levels) {
        if (embedded) {
            const auto reference = level.get<std::string>();
            constexpr std::string_view prefix = "#/images/";
            if (!reference.starts_with(prefix) || reference.size() == prefix.size() ||
                !std::all_of(reference.begin() + prefix.size(), reference.end(), [](unsigned char digit) {
                    return digit >= '0' && digit <= '9';
                })) {
                throw std::runtime_error("invalid embedded texture image reference: " + reference);
            }
            variant.levels.emplace_back(reference);
            continue;
        }
        const auto path = AssetPath(base, root, level);
        if (path.extension() != ".png") {
            throw std::runtime_error("texture level is not PNG: " + path.string());
        }
        variant.levels.push_back(path);
    }
}

Source ParseDetail(const nlohmann::json &detail, const std::filesystem::path &document_path, bool embedded,
                   const ImageExtentResolver &extent_resolver) {
    CheckKeys(detail, {"schema_version", "groups", "entries"});
    if (!detail.at("schema_version").is_number_integer() ||
        detail.at("schema_version").get<int>() != 3 || !detail.at("groups").is_array() ||
        !detail.at("entries").is_array()) {
        throw std::runtime_error("invalid source document: " + document_path.string());
    }
    Source source;
    source.source = embedded ? document_path.generic_string()
                             : document_path.lexically_relative(port_data_root).generic_string();
    for (const auto &group : detail.at("groups")) {
        const int value = NonnegativeInt(group);
        if (std::find(source.groups.begin(), source.groups.end(), value) != source.groups.end()) {
            throw std::runtime_error("duplicate or invalid texture group");
        }
        source.groups.push_back(value);
    }
    const auto asset_base = document_path.parent_path();
    for (const auto &item : detail.at("entries")) {
        if (item.contains("script")) {
            CheckKeys(item, {"name", "script"});
            Entry entry;
            entry.type = "cfg";
            entry.name = item.at("name").get<std::string>();
            if (!item.at("script").is_array()) {
                throw std::runtime_error("invalid animation script");
            }
            for (const auto &statement : item.at("script")) {
                CheckKeys(statement, {"command", "args"});
                Entry::Command command;
                command.name = statement.at("command").get<std::string>();
                if (!statement.at("args").is_array()) {
                    throw std::runtime_error("invalid animation arguments");
                }
                for (const auto &arg : statement.at("args")) {
                    CheckKeys(arg, {"text", "number"});
                    if (arg.size() != 1) {
                        throw std::runtime_error("invalid animation argument");
                    }
                    Entry::Argument value;
                    if (arg.contains("text")) {
                        value.text = arg.at("text").get<std::string>();
                    } else {
                        value.number = arg.at("number").get<double>();
                    }
                    command.args.push_back(std::move(value));
                }
                entry.script.push_back(std::move(command));
            }
            source.entries.push_back(std::move(entry));
            continue;
        }
        CheckKeys(item, {"name", "block", "pictures"});
        const auto  name = item.at("name").get<std::string>();
        const int   block = item.contains("block") ? NonnegativeInt(item.at("block")) : 0;
        const auto &pictures = item.at("pictures");
        if (name.empty() || !pictures.is_array() || pictures.empty()) {
            throw std::runtime_error("invalid texture entry");
        }
        if (std::find(source.groups.begin(), source.groups.end(), block) == source.groups.end()) {
            throw std::runtime_error("texture block missing from groups");
        }
        for (size_t index = 0; index < pictures.size(); ++index) {
            const auto &picture = pictures[index];
            CheckKeys(picture, {"levels", "canvas", "variants", "sampler", "animation"});
            Entry entry;
            entry.type = "texture";
            entry.name = name;
            entry.block = block;
            entry.picture = static_cast<int>(index);
            entry.bpp = 32;
            Entry::Variant normal;
            normal.selector = name;
            // The embedded exporter can materialize an unused optional field as JSON null.
            const bool has_levels = picture.contains("levels") && !(embedded && picture.at("levels").is_null());
            if (picture.contains("canvas") == has_levels) {
                throw std::runtime_error("picture requires either levels or canvas");
            }
            if (picture.contains("canvas")) {
                const auto &canvas = picture.at("canvas");
                CheckKeys(canvas, {"width", "height"});
                entry.width = NonnegativeInt(canvas.at("width"), 16384);
                entry.height = NonnegativeInt(canvas.at("height"), 16384);
                entry.no_image = true;
            } else {
                ReadLevels(normal, picture.at("levels"), asset_base, port_data_root, embedded);
                if (embedded) {
                    std::string extent_error;
                    if (!extent_resolver(normal.levels.front().generic_string(), entry.width, entry.height,
                                         extent_error)) {
                        throw std::runtime_error("cannot resolve embedded texture extent: " + extent_error);
                    }
                } else {
                    std::tie(entry.width, entry.height) = PngExtent(normal.levels.front());
                }
            }
            if (entry.width <= 0 || entry.height <= 0 || entry.width > 16384 || entry.height > 16384) {
                throw std::runtime_error("invalid texture extent");
            }
            entry.variants.push_back(std::move(normal));
            if (picture.contains("variants") && !(embedded && picture.at("variants").is_null())) {
                if (!picture.at("variants").is_array()) {
                    throw std::runtime_error("invalid texture variants");
                }
                std::unordered_set<std::string> selectors{name};
                for (const auto &record : picture.at("variants")) {
                    CheckKeys(record, {"selector", "levels"});
                    Entry::Variant variant;
                    variant.selector = record.at("selector").get<std::string>();
                    if (variant.selector.empty() || !selectors.insert(variant.selector).second) {
                        throw std::runtime_error("duplicate texture selector");
                    }
                    ReadLevels(variant, record.at("levels"), asset_base, port_data_root, embedded);
                    entry.variants.push_back(std::move(variant));
                }
            }
            if (picture.contains("sampler")) {
                const auto &sampler = picture.at("sampler");
                CheckKeys(sampler, {"wrap_u", "wrap_v", "filter"});
                const auto wrap_u = sampler.value("wrap_u", std::string{"repeat"});
                const auto wrap_v = sampler.value("wrap_v", std::string{"repeat"});
                const auto filter = sampler.value("filter", std::string{"linear"});
                if ((wrap_u != "repeat" && wrap_u != "clamp") ||
                    (wrap_v != "repeat" && wrap_v != "clamp") ||
                    (filter != "linear" && filter != "nearest")) {
                    throw std::runtime_error("invalid texture sampler");
                }
                entry.clamp = uint64_t{wrap_u == "clamp"} | (uint64_t{wrap_v == "clamp"} << 2);
                entry.nearest = filter == "nearest";
            }
            const auto animation = picture.value("animation", std::string{"draw"});
            if (animation == "copy") {
                entry.animation_mode = AnimationMode::Copy;
            } else if (animation == "copy_variant") {
                entry.animation_mode = AnimationMode::CopyVariant;
            } else if (animation != "draw") {
                throw std::runtime_error("invalid texture animation mode");
            }
            source.entries.push_back(std::move(entry));
        }
    }
    return source;
}

Source ParseDocument(const std::filesystem::path &document_path, const std::filesystem::path &candidate) {
    std::ifstream document(candidate);
    if (!document) {
        throw std::runtime_error("cannot open source document: " + candidate.string());
    }
    return ParseDetail(nlohmann::json::parse(document), document_path, false, {});
}
} // namespace

bool Load(const std::filesystem::path &root, std::string &error) {
    Clear();
    std::error_code path_error;
    const auto      canonical = std::filesystem::canonical(root, path_error);
    if (path_error || !std::filesystem::is_directory(canonical)) {
        error = "cannot open texture data directory: " + root.string();
        return false;
    }
    port_data_root = canonical;
    error.clear();
    return true;
}

void SetCandidateResolver(std::function<std::vector<std::filesystem::path>(const std::filesystem::path &)> resolver) {
    candidate_resolver = std::move(resolver);
    sources.clear();
}

void Clear() {
    sources.clear();
    port_data_root.clear();
    candidate_resolver = {};
}

const Source *Find(std::string_view path) {
    const std::string key(path);
    if (port_data_root.empty() || !RelativePath(key) || std::filesystem::path(key).extension() != ".json") {
        return nullptr;
    }
    if (const auto found = sources.find(key); found != sources.end()) {
        return &found->second;
    }
    std::error_code path_error;
    const auto      logical_path = std::filesystem::weakly_canonical(port_data_root / key, path_error);
    if (path_error || !Within(port_data_root, logical_path)) {
        return nullptr;
    }
    auto candidates = candidate_resolver ? candidate_resolver(key) : std::vector<std::filesystem::path>{};
    if (std::find(candidates.begin(), candidates.end(), logical_path) == candidates.end()) {
        candidates.push_back(logical_path);
    }
    for (const auto &candidate : candidates) {
        try {
            const auto result = sources.emplace(key, ParseDocument(logical_path, candidate));
            return &result.first->second;
        } catch (const std::exception &) {
            // Invalid replacements fall through to a lower-priority document.
        }
    }
    return nullptr;
}

bool Contains(std::string_view path) { return Find(path) != nullptr; }

bool DecodeEmbeddedSet(const nlohmann::json &document, const std::filesystem::path &gltf_document_path,
                       ImageExtentResolver extent_resolver, Source &source, std::string &error) {
    source = {};
    if (!extent_resolver) {
        error = "missing embedded image extent resolver";
        return false;
    }
    try {
        source = ParseDetail(document, gltf_document_path, true, extent_resolver);
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = std::string("invalid embedded texture document: ") + exception.what();
        return false;
    }
}

std::optional<Decoded> Decode(const Entry &entry, std::string_view selector, std::string &error) {
    if (entry.type != "texture" || entry.width <= 0 || entry.height <= 0 || entry.bpp != 32) {
        error = "invalid texture entry";
        return std::nullopt;
    }
    const auto variant = std::find_if(entry.variants.begin(), entry.variants.end(), [selector](const auto &value) {
        return value.selector == selector;
    });
    if (variant == entry.variants.end() || variant->levels.size() > 4) {
        error = "missing or invalid RGBA variant";
        return std::nullopt;
    }
    Decoded decoded;
    if (!entry.no_image && variant->levels.empty()) {
        error = "texture has no PNG levels";
        return std::nullopt;
    }
    for (size_t mip = 0; mip < variant->levels.size(); ++mip) {
        int                  width = 0, height = 0;
        std::vector<uint8_t> rgba;
        const bool           loaded = imagefile::LoadPng(variant->levels[mip], width, height, rgba);
        if (!loaded ||
            width != std::max(1, entry.width >> mip) || height != std::max(1, entry.height >> mip)) {
            error = "cannot load texture PNG " + variant->levels[mip].string();
            return std::nullopt;
        }
        decoded.levels.push_back(std::move(rgba));
    }
    error.clear();
    return decoded;
}

} // namespace texture_catalog
