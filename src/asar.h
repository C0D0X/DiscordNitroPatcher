// Electron asar archive parser
#pragma once

#include "json_lite.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dnp {

class Asar {
public:
    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;

    std::optional<std::vector<uint8_t>> read_file(const std::string& rel_path) const;
    bool write_file(const std::string& rel_path, std::vector<uint8_t> data);
    bool rename_file(const std::string& from, const std::string& to);
    bool has_file(const std::string& rel_path) const;

    Json*       root_field(const std::string& key);
    const Json* root_field(const std::string& key) const;
    void        set_root_field(const std::string& key, Json v);

    bool verify_in_memory() const;
    bool has_sentinel(int min_version) const;
    void write_sentinel(int version);

private:
    Json header_;
    std::vector<uint8_t> orig_blob_;
    std::unordered_map<std::string, std::vector<uint8_t>> overrides_;
    std::unordered_map<std::string, bool> removed_;

    Json*       resolve_node(const std::string& rel_path);
    const Json* resolve_node(const std::string& rel_path) const;

    Json* ensure_parent_files(const std::string& rel_path, std::string& out_leaf_name);

    struct LeafInfo { uint64_t size; uint64_t offset; std::string offset_str; };
    void collect_leaves(const Json& node, const std::string& prefix,
                        std::vector<std::pair<std::string, LeafInfo>>& out) const;
};

} // namespace dnp
