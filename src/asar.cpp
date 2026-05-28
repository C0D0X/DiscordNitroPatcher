// asar.cpp — Electron asar archive parse + repack.
#include "asar.h"
#include "util.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace dnp {

// ============================================================================
// Path splitting
// ============================================================================
namespace {

std::vector<std::string> split_path(const std::string& rel_path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : rel_path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

size_t pad4(size_t n) { return (4 - (n & 3)) & 3; }

} // namespace

// ============================================================================
// Node resolution
// ============================================================================

Json* Asar::resolve_node(const std::string& rel_path) {
    Json* files = header_.find("files");
    if (!files || !files->is_obj()) return nullptr;
    auto parts = split_path(rel_path);
    Json* cur = files;
    for (size_t i = 0; i < parts.size(); ++i) {
        Json* next = cur->find(parts[i]);
        if (!next || !next->is_obj()) return nullptr;
        if (i + 1 == parts.size()) return next;
        Json* sub = next->find("files");
        if (!sub || !sub->is_obj()) return nullptr;
        cur = sub;
    }
    return cur;
}

const Json* Asar::resolve_node(const std::string& rel_path) const {
    return const_cast<Asar*>(this)->resolve_node(rel_path);
}

Json* Asar::ensure_parent_files(const std::string& rel_path, std::string& out_leaf_name) {
    Json* files = header_.find("files");
    if (!files) {
        header_.set("files", Json::make_obj());
        files = header_.find("files");
    }
    if (!files->is_obj()) return nullptr;

    auto parts = split_path(rel_path);
    if (parts.empty()) return nullptr;
    out_leaf_name = parts.back();

    Json* cur = files;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        Json* node = cur->find(parts[i]);
        if (!node) {
            cur->set(parts[i], Json::make_obj());
            node = cur->find(parts[i]);
            node->set("files", Json::make_obj());
        } else if (!node->find("files")) {
            // Was a file; converting to dir would lose data — fail.
            return nullptr;
        }
        cur = node->find("files");
        if (!cur || !cur->is_obj()) return nullptr;
    }
    return cur;
}

// ============================================================================
// Load
// ============================================================================

bool Asar::load(const std::wstring& path) {
    // Qualify to the free function in dnp:: — unqualified name resolves to Asar::read_file(string).
    auto buf_opt = ::dnp::read_file(path);
    if (!buf_opt) return false;
    const auto& buf = *buf_opt;
    if (buf.size() < 16) return false;

    // Header layout (little-endian uint32s at offsets 0/4/8/12):
    //   [0]  = 4                                <- outer pickle payload size
    //   [4]  = inner header pickle byte count (S)
    //   [8]  = inner pickle payload size = 4 + json_padded
    //   [12] = JSON byte length
    uint32_t outer_payload = read_u32_le(&buf[0]);
    uint32_t header_size   = read_u32_le(&buf[4]);
    uint32_t inner_payload = read_u32_le(&buf[8]);
    uint32_t json_len      = read_u32_le(&buf[12]);
    (void)outer_payload;

    if (outer_payload != 4) {
        // Not necessarily fatal; some pickles may report differently — be lenient but log.
    }

    if (buf.size() < (size_t)16 + json_len) return false;

    size_t json_padded   = json_len + pad4(json_len);
    size_t blob_offset   = 16 + json_padded;

    // Sanity: blob_offset must align with 8 + header_size (the inner pickle occupies header_size bytes
    // starting at offset 8). header_size includes the 4-byte inner-payload-size plus padded payload.
    if ((size_t)(8 + header_size) != blob_offset) {
        // Tolerate mismatch but use computed blob_offset.
    }
    (void)inner_payload;

    if (buf.size() < blob_offset) return false;

    std::string json_text((const char*)&buf[16], json_len);
    if (!header_.parse(json_text)) return false;
    if (!header_.is_obj()) return false;

    orig_blob_.assign(buf.begin() + (ptrdiff_t)blob_offset, buf.end());
    overrides_.clear();
    removed_.clear();
    return true;
}

// ============================================================================
// Save (atomic-friendly: builds in memory, writes once)
// ============================================================================

void Asar::collect_leaves(const Json& node, const std::string& prefix,
                          std::vector<std::pair<std::string, LeafInfo>>& out) const {
    if (!node.is_obj()) return;
    for (const auto& kv : node.as_obj()) {
        const Json& child = kv.second;
        if (!child.is_obj()) continue;
        const Json* sub = child.find("files");
        std::string child_path = prefix.empty() ? kv.first : (prefix + "/" + kv.first);
        if (sub && sub->is_obj()) {
            collect_leaves(*sub, child_path, out);
        } else {
            // Leaf file. Read size + offset (string).
            const Json* sz = child.find("size");
            const Json* off = child.find("offset");
            LeafInfo li{};
            if (sz && sz->is_int()) li.size = (uint64_t)sz->as_int();
            if (off && off->is_str()) {
                li.offset_str = off->as_str();
                li.offset = (uint64_t)strtoull(off->as_str().c_str(), nullptr, 10);
            }
            out.push_back({std::move(child_path), li});
        }
    }
}

bool Asar::save(const std::wstring& path) const {
    // Step 1: collect every leaf, in document order.
    const Json* files = header_.find("files");
    if (!files || !files->is_obj()) return false;

    std::vector<std::pair<std::string, LeafInfo>> leaves;
    collect_leaves(*files, "", leaves);

    // Step 2: build new blob and new offset mapping.
    std::vector<uint8_t> new_blob;
    new_blob.reserve(orig_blob_.size() + 4096);

    // Map: leaf path -> {new_offset, new_size}.
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> new_meta;
    new_meta.reserve(leaves.size());

    for (const auto& pr : leaves) {
        const std::string& path_str = pr.first;
        const LeafInfo&    li       = pr.second;

        const std::vector<uint8_t>* data_src = nullptr;
        std::vector<uint8_t> slice;
        auto it = overrides_.find(path_str);
        if (it != overrides_.end()) {
            data_src = &it->second;
        } else {
            // Slice from orig_blob_ using original offset/size.
            if (li.offset + li.size > orig_blob_.size()) return false;
            slice.assign(orig_blob_.begin() + (ptrdiff_t)li.offset,
                         orig_blob_.begin() + (ptrdiff_t)(li.offset + li.size));
            data_src = &slice;
        }

        uint64_t new_off = (uint64_t)new_blob.size();
        new_blob.insert(new_blob.end(), data_src->begin(), data_src->end());
        new_meta[path_str] = {new_off, (uint64_t)data_src->size()};
    }

    // Step 3: clone header and rewrite each leaf's size + offset using new_meta.
    Json new_header = header_;
    {
        // Recursive rewrite.
        struct R {
            const std::unordered_map<std::string, std::pair<uint64_t, uint64_t>>& meta;
            bool fix(Json& obj_files, const std::string& prefix) {
                if (!obj_files.is_obj()) return false;
                for (auto& kv : obj_files.as_obj()) {
                    Json& child = kv.second;
                    if (!child.is_obj()) continue;
                    Json* sub = child.find("files");
                    std::string p = prefix.empty() ? kv.first : (prefix + "/" + kv.first);
                    if (sub && sub->is_obj()) {
                        if (!fix(*sub, p)) return false;
                    } else {
                        auto it = meta.find(p);
                        if (it == meta.end()) return false;
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)it->second.first);
                        child.set("size",   Json::make_int((int64_t)it->second.second));
                        child.set("offset", Json::make_str(buf));
                    }
                }
                return true;
            }
        };
        Json* root_files = new_header.find("files");
        if (!root_files) return false;
        R r{new_meta};
        if (!r.fix(*root_files, "")) return false;
    }

    // Step 4: serialize JSON, compute padding, assemble final buffer.
    std::string json_text = new_header.dump();
    size_t json_len    = json_text.size();
    size_t json_padded = json_len + pad4(json_len);

    // Inner pickle payload size = 4 (string length) + json_padded.
    uint32_t inner_payload = (uint32_t)(4 + json_padded);
    // Header pickle byte count S = 4 (inner-payload-size prefix) + inner_payload (already padded).
    uint32_t header_size   = (uint32_t)(4 + inner_payload);

    std::vector<uint8_t> out_buf;
    out_buf.resize(16 + json_padded + new_blob.size());

    write_u32_le(&out_buf[0],  4);
    write_u32_le(&out_buf[4],  header_size);
    write_u32_le(&out_buf[8],  inner_payload);
    write_u32_le(&out_buf[12], (uint32_t)json_len);
    memcpy(&out_buf[16], json_text.data(), json_len);
    if (json_padded > json_len) {
        memset(&out_buf[16 + json_len], 0, json_padded - json_len);
    }
    if (!new_blob.empty()) {
        memcpy(&out_buf[16 + json_padded], new_blob.data(), new_blob.size());
    }

    // Qualify to the free function — Asar::write_file is the non-const member.
    return ::dnp::write_file(path, out_buf);
}

// ============================================================================
// File ops
// ============================================================================

bool Asar::has_file(const std::string& rel_path) const {
    auto it = removed_.find(rel_path);
    if (it != removed_.end() && it->second) return false;
    if (overrides_.find(rel_path) != overrides_.end()) return true;
    const Json* node = resolve_node(rel_path);
    if (!node) return false;
    // A file node has "size" + "offset" and no "files".
    return node->find("size") != nullptr && node->find("files") == nullptr;
}

std::optional<std::vector<uint8_t>> Asar::read_file(const std::string& rel_path) const {
    auto rit = removed_.find(rel_path);
    if (rit != removed_.end() && rit->second) return std::nullopt;

    auto oit = overrides_.find(rel_path);
    if (oit != overrides_.end()) return oit->second;

    const Json* node = resolve_node(rel_path);
    if (!node || !node->is_obj()) return std::nullopt;
    if (node->find("files") != nullptr) return std::nullopt; // it's a directory

    const Json* sz = node->find("size");
    const Json* off = node->find("offset");
    if (!sz || !sz->is_int() || !off || !off->is_str()) return std::nullopt;
    uint64_t size   = (uint64_t)sz->as_int();
    uint64_t offset = (uint64_t)strtoull(off->as_str().c_str(), nullptr, 10);
    if (offset + size > orig_blob_.size()) return std::nullopt;
    return std::vector<uint8_t>(orig_blob_.begin() + (ptrdiff_t)offset,
                                orig_blob_.begin() + (ptrdiff_t)(offset + size));
}

bool Asar::write_file(const std::string& rel_path, std::vector<uint8_t> data) {
    // Find or create parent.
    std::string leaf_name;
    Json* parent_files = ensure_parent_files(rel_path, leaf_name);
    if (!parent_files || leaf_name.empty()) return false;

    // Create or update the leaf node with placeholder size/offset; save() recomputes.
    Json* leaf = parent_files->find(leaf_name);
    if (!leaf) {
        parent_files->set(leaf_name, Json::make_obj());
        leaf = parent_files->find(leaf_name);
    }
    if (leaf->find("files")) return false; // would clobber a directory
    leaf->set("size", Json::make_int((int64_t)data.size()));
    leaf->set("offset", Json::make_str("0"));

    overrides_[rel_path] = std::move(data);
    removed_.erase(rel_path);
    return true;
}

bool Asar::rename_file(const std::string& from, const std::string& to) {
    if (!has_file(from)) return false;
    auto data_opt = read_file(from);
    if (!data_opt) return false;

    // Remove old leaf from header.
    auto from_parts = split_path(from);
    if (from_parts.empty()) return false;
    Json* files_root = header_.find("files");
    if (!files_root) return false;
    Json* cur = files_root;
    for (size_t i = 0; i + 1 < from_parts.size(); ++i) {
        Json* next = cur->find(from_parts[i]);
        if (!next) return false;
        Json* sub = next->find("files");
        if (!sub) return false;
        cur = sub;
    }
    cur->remove(from_parts.back());

    // Drop any override under old key.
    overrides_.erase(from);
    removed_[from] = true;

    // Insert under new path.
    return write_file(to, std::move(*data_opt));
}

// ============================================================================
// Root field access
// ============================================================================

Json* Asar::root_field(const std::string& key) {
    return header_.find(key);
}
const Json* Asar::root_field(const std::string& key) const {
    return header_.find(key);
}
void Asar::set_root_field(const std::string& key, Json v) {
    header_.set(key, std::move(v));
}

bool Asar::verify_in_memory() const {
    const Json* files = header_.find("files");
    if (!files || !files->is_obj()) return false;
    // Walk every leaf, ensure size+offset present.
    std::vector<std::pair<std::string, LeafInfo>> leaves;
    collect_leaves(*files, "", leaves);
    for (const auto& pr : leaves) {
        // Either overridden (size set in header, data in overrides_) or in orig_blob_.
        auto oit = overrides_.find(pr.first);
        if (oit != overrides_.end()) {
            if (oit->second.size() != pr.second.size) return false;
        } else {
            if (pr.second.offset + pr.second.size > orig_blob_.size()) return false;
        }
    }
    return true;
}

bool Asar::has_sentinel(int min_version) const {
    const Json* s = header_.find("_dnp");
    if (!s || !s->is_obj()) return false;
    const Json* v = s->find("v");
    if (!v || !v->is_int()) return false;
    return v->as_int() >= min_version;
}

void Asar::write_sentinel(int version) {
    Json o = Json::make_obj();
    o.set("v", Json::make_int(version));
    header_.set("_dnp", std::move(o));
}

} // namespace dnp
