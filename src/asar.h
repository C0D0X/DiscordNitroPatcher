// asar.h — Electron asar archive parser and repacker.
//
// Format reference (Electron asar v0 / Chromium Pickle wrapper):
//   offset 0  (u32 LE): outer pickle payload size = 4
//   offset 4  (u32 LE): inner header pickle byte count (call this S)
//   offset 8  (u32 LE): inner pickle payload size  (= 4 + json_padded_len)
//   offset 12 (u32 LE): JSON byte length
//   offset 16 : JSON bytes (UTF-8)
//   offset 16 + json_padded_len : start of blob region
//
// JSON shape: {"files": {"name": {"size": N, "offset": "BASE_RELATIVE_DECIMAL"} | {"files": {...}}, ...}}
// Extra root-level keys (e.g. our "_dnp" sentinel) are ignored by Electron's asar reader.
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
    // ---- IO ----
    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;

    // ---- File tree operations (paths use '/' as separator, no leading slash) ----
    std::optional<std::vector<uint8_t>> read_file(const std::string& rel_path) const;

    // Creates or replaces. Parent directory nodes are created if missing.
    bool write_file(const std::string& rel_path, std::vector<uint8_t> data);

    // Rename a single file leaf (preserves data). Both args are full rel_paths.
    bool rename_file(const std::string& from, const std::string& to);

    bool has_file(const std::string& rel_path) const;

    // ---- Root header field access (for sentinel etc.) ----
    Json*       root_field(const std::string& key);
    const Json* root_field(const std::string& key) const;
    void        set_root_field(const std::string& key, Json v);

    // ---- Validation ----
    // Re-checks that on-disk-style invariants hold post-save (offsets in bounds, sizes match).
    bool verify_in_memory() const;

    // ---- Convenience for sentinel ----
    bool has_sentinel(int min_version) const;
    void write_sentinel(int version);

private:
    // The parsed JSON header. Source of truth for file tree structure + metadata.
    Json header_;

    // Original blob bytes (post-header). Used to resolve file content for files whose
    // bytes haven't been overridden via write_file.
    std::vector<uint8_t> orig_blob_;

    // Override map: rel_path -> new bytes. Takes precedence over orig_blob_ slice.
    std::unordered_map<std::string, std::vector<uint8_t>> overrides_;

    // Tombstone set for paths removed from the file tree (so reads correctly miss).
    std::unordered_map<std::string, bool> removed_;

    // Resolves a rel_path into a "files" object node (leaf or directory). Returns nullptr if missing.
    Json*       resolve_node(const std::string& rel_path);
    const Json* resolve_node(const std::string& rel_path) const;

    // Returns the parent directory's "files" object and the final segment name. Creates intermediates.
    Json* ensure_parent_files(const std::string& rel_path, std::string& out_leaf_name);

    // Walks the tree and collects every leaf path -> (size, offset_str).
    struct LeafInfo { uint64_t size; uint64_t offset; std::string offset_str; };
    void collect_leaves(const Json& node, const std::string& prefix,
                        std::vector<std::pair<std::string, LeafInfo>>& out) const;
};

} // namespace dnp
