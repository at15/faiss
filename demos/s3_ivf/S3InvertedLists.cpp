#include "S3InvertedLists.h"

#include <aws/core/Aws.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/s3-crt/S3CrtClient.h>
#include <aws/s3-crt/S3CrtClientConfiguration.h>
#include <aws/s3-crt/model/GetObjectRequest.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include <faiss/impl/FaissAssert.h>
#include <faiss/invlists/InvertedListsIOHook.h>

#include "json.hpp"

using json = nlohmann::json;

namespace faiss_s3 {

// Helper function to create S3 client (supports localhost mock)
static std::shared_ptr<Aws::S3Crt::S3CrtClient> CreateS3Client() {
    Aws::S3Crt::ClientConfiguration config;

    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint != nullptr) {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
    }

    const char* region = std::getenv("AWS_REGION");
    if (region != nullptr) {
        config.region = region;
    }

    // Disable multipart upload for S3 mock server compatibility
    config.partSize = SIZE_MAX;

    return Aws::MakeShared<Aws::S3Crt::S3CrtClient>("S3CrtClient", config);
}

// Helper function to download byte range from S3
static std::vector<uint8_t> DownloadRangeFromS3(
        std::shared_ptr<void> client_ptr,
        const std::string& bucket,
        const std::string& key,
        size_t offset,
        size_t size) {

    // Cast opaque pointer to actual S3 client type
    auto client = std::static_pointer_cast<Aws::S3Crt::S3CrtClient>(client_ptr);

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    // S3 range format: "bytes=start-end" (inclusive on both ends)
    // Use Aws::String and Aws::Utils::StringUtils::to_string like test_s3.cpp does
    Aws::String range = "bytes=" +
                        Aws::Utils::StringUtils::to_string(offset) + "-" +
                        Aws::Utils::StringUtils::to_string(offset + size - 1);
    request.SetRange(range);

    auto outcome = client->GetObject(request);

    if (outcome.IsSuccess()) {
        std::cout << "Downloaded range from S3: " << bucket << "/" << key << " [" << offset << ":" << offset + size - 1 << "]" << std::endl;
        auto& stream = outcome.GetResultWithOwnership().GetBody();
        std::vector<uint8_t> data(size);
        stream.read(reinterpret_cast<char*>(data.data()), size);

        size_t bytes_read = stream.gcount();
        if (bytes_read != size) {
            std::cerr << "Warning: Expected " << size << " bytes, got " << bytes_read << std::endl;
        }

        return data;
    } else {
        std::cerr << "Failed to download range from S3: " << bucket << "/" << key << " [" << offset << ":" << offset + size - 1 << "]: " << outcome.GetError().GetMessage() << std::endl;
        FAISS_THROW_FMT(
            "Failed to download range from s3://%s/%s [%zu:%zu]: %s",
            bucket.c_str(),
            key.c_str(),
            offset,
            offset + size,
            outcome.GetError().GetMessage().c_str());
    }
}

// Type alias for Faiss index type to avoid polluting namespace
using idx_t = faiss::idx_t;

// ============================================================================
// S3ReadNothingInvertedLists - Placeholder
// ============================================================================

S3ReadNothingInvertedLists::S3ReadNothingInvertedLists(
        size_t nlist,
        size_t code_size,
        const std::vector<size_t>& sizes)
        : faiss::InvertedLists(nlist, code_size), cluster_sizes(sizes) {}

size_t S3ReadNothingInvertedLists::list_size(size_t list_no) const {
    // Return actual size so index stats work before replacement
    return cluster_sizes[list_no];
}

const uint8_t* S3ReadNothingInvertedLists::get_codes(size_t list_no) const {
    FAISS_THROW_MSG("S3ReadNothingInvertedLists: not initialized, use replace_invlists");
}

const idx_t* S3ReadNothingInvertedLists::get_ids(size_t list_no) const {
    FAISS_THROW_MSG("S3ReadNothingInvertedLists: not initialized, use replace_invlists");
}

size_t S3ReadNothingInvertedLists::add_entries(
        size_t, size_t, const idx_t*, const uint8_t*) {
    FAISS_THROW_MSG("S3ReadNothingInvertedLists: read-only");
}

void S3ReadNothingInvertedLists::update_entries(
        size_t, size_t, size_t, const idx_t*, const uint8_t*) {
    FAISS_THROW_MSG("S3ReadNothingInvertedLists: read-only");
}

void S3ReadNothingInvertedLists::resize(size_t, size_t) {
    FAISS_THROW_MSG("S3ReadNothingInvertedLists: read-only");
}

// ============================================================================
// S3ReadOnlyInvertedLists - Main Implementation
// ============================================================================

S3ReadOnlyInvertedLists::S3ReadOnlyInvertedLists(
        std::shared_ptr<void> s3_client,
        const std::string& bucket,
        const std::string& key,
        const std::string& metadata_json,
        const std::vector<size_t>& sizes)
        : faiss::InvertedLists(0, 0),
          s3_bucket(bucket),
          s3_key(key),
          s3_client_(s3_client),  // Store passed-in client
          cluster_sizes(sizes) {

    load_metadata(metadata_json);
    // Sizes already provided from placeholder, no need to download
    calculate_cluster_offsets();

    std::cout << "S3ReadOnlyInvertedLists initialized:" << std::endl;
    std::cout << "  Bucket: " << s3_bucket << std::endl;
    std::cout << "  Key: " << s3_key << std::endl;
    std::cout << "  Clusters: " << nlist << std::endl;
    std::cout << "  Code size: " << code_size << " bytes" << std::endl;
}

void S3ReadOnlyInvertedLists::load_metadata(const std::string& metadata_json) {
    json meta = json::parse(metadata_json);

    nlist = meta["n_clusters"];
    code_size = meta["code_size"];
    cluster_data_offset = meta["cluster_data_offset"];
    sizes_array_offset = meta["sizes_array_offset"];
    sizes_array_count = meta["sizes_array_count"];
    sizes_array_format = meta["sizes_array_format"];
}

void S3ReadOnlyInvertedLists::load_sizes_from_s3() {
    std::cout << "Loading cluster sizes from S3..." << std::endl;

    // Download sizes array from S3
    size_t count_size = sizeof(size_t);
    size_t data_size = sizes_array_count * sizeof(size_t);
    size_t total_size = count_size + data_size;

    auto data = DownloadRangeFromS3(
        s3_client_, s3_bucket, s3_key, sizes_array_offset, total_size);

    // Parse count
    size_t count;
    memcpy(&count, data.data(), sizeof(size_t));

    // Parse sizes array
    std::vector<size_t> raw_sizes(count);
    memcpy(raw_sizes.data(), data.data() + count_size, data_size);

    // Convert based on format
    cluster_sizes.resize(nlist, 0);

    if (sizes_array_format == "full") {
        cluster_sizes = raw_sizes;
    } else {
        // Sparse: pairs of (cluster_id, size)
        for (size_t i = 0; i < count; i += 2) {
            size_t cluster_id = raw_sizes[i];
            size_t size = raw_sizes[i + 1];
            cluster_sizes[cluster_id] = size;
        }
    }

    std::cout << "✓ Loaded cluster sizes (" << sizes_array_format << " format)" << std::endl;
}

void S3ReadOnlyInvertedLists::calculate_cluster_offsets() {
    cluster_offsets.resize(nlist);
    size_t offset = cluster_data_offset;

    for (size_t i = 0; i < nlist; i++) {
        cluster_offsets[i] = offset;
        size_t n = cluster_sizes[i];
        if (n > 0) {
            offset += n * code_size;  // codes
            offset += n * sizeof(idx_t);  // ids
        }
    }
}

void S3ReadOnlyInvertedLists::fetch_cluster(size_t list_no) const {
    size_t n = cluster_sizes[list_no];
    if (n == 0) {
        // Empty cluster
        codes_cache[list_no] = {};
        ids_cache[list_no] = {};
        return;
    }

    size_t offset = cluster_offsets[list_no];
    size_t codes_bytes = n * code_size;
    size_t ids_bytes = n * sizeof(idx_t);
    size_t total_bytes = codes_bytes + ids_bytes;

    std::cout << "→ Fetching cluster " << list_no << " (" << n << " vectors, "
              << total_bytes << " bytes)" << std::endl;

    // Download cluster data from S3 using stored client
    auto data = DownloadRangeFromS3(s3_client_, s3_bucket, s3_key, offset, total_bytes);

    // Split into codes and ids
    std::vector<uint8_t> codes(codes_bytes);
    std::vector<idx_t> ids(n);

    memcpy(codes.data(), data.data(), codes_bytes);
    memcpy(ids.data(), data.data() + codes_bytes, ids_bytes);

    // Cache
    codes_cache[list_no] = std::move(codes);
    ids_cache[list_no] = std::move(ids);

    std::cout << "✓ Cached cluster " << list_no << std::endl;
}

const uint8_t* S3ReadOnlyInvertedLists::get_codes(size_t list_no) const {
    std::lock_guard<std::mutex> lock(cache_mutex);

    if (codes_cache.find(list_no) == codes_cache.end()) {
        fetch_cluster(list_no);
    }

    return codes_cache.at(list_no).data();
}

const idx_t* S3ReadOnlyInvertedLists::get_ids(size_t list_no) const {
    std::lock_guard<std::mutex> lock(cache_mutex);

    if (ids_cache.find(list_no) == ids_cache.end()) {
        fetch_cluster(list_no);
    }

    return ids_cache.at(list_no).data();
}

size_t S3ReadOnlyInvertedLists::list_size(size_t list_no) const {
    return cluster_sizes[list_no];
}

size_t S3ReadOnlyInvertedLists::add_entries(
        size_t, size_t, const idx_t*, const uint8_t*) {
    FAISS_THROW_MSG("S3ReadOnlyInvertedLists: read-only");
}

void S3ReadOnlyInvertedLists::update_entries(
        size_t, size_t, size_t, const idx_t*, const uint8_t*) {
    FAISS_THROW_MSG("S3ReadOnlyInvertedLists: read-only");
}

void S3ReadOnlyInvertedLists::resize(size_t, size_t) {
    FAISS_THROW_MSG("S3ReadOnlyInvertedLists: read-only");
}

// ============================================================================
// S3BuildOnlyInvertedLists - Original Implementation
// ============================================================================

S3BuildOnlyInvertedLists::S3BuildOnlyInvertedLists(
        size_t nlist,
        size_t code_size)
        : faiss::InvertedLists(nlist, code_size) {
    codes.resize(nlist);
    ids.resize(nlist);
}

void S3BuildOnlyInvertedLists::resize(size_t list_no, size_t new_size) {
    ids[list_no].resize(new_size);
    codes[list_no].resize(new_size * code_size);
}

size_t S3BuildOnlyInvertedLists::list_size(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].size();
}

bool S3BuildOnlyInvertedLists::is_empty(
        size_t list_no,
        void* inverted_list_context) const {
    FAISS_THROW_IF_NOT(inverted_list_context == nullptr);
    return ids[list_no].size() == 0;
}

void S3BuildOnlyInvertedLists::permute_invlists(const idx_t* map) {
    // Shuffle the clusters according to the map
    // map[new_index] = old_index (where to get data from)
    std::vector<std::vector<uint8_t>> new_codes(nlist);
    std::vector<std::vector<idx_t>> new_ids(nlist);

    for (size_t i = 0; i < nlist; i++) {
        size_t o = map[i];
        // Must be a valid cluster index
        FAISS_THROW_IF_NOT(o < nlist);
        // Use swap instead of copy for O(1) performance (just swaps pointers)
        std::swap(new_codes[i], codes[o]);
        std::swap(new_ids[i], ids[o]);
    }

    // Swap the new permuted data back into the member variables
    std::swap(codes, new_codes);
    std::swap(ids, new_ids);
}

const uint8_t* S3BuildOnlyInvertedLists::get_codes(size_t list_no) const {
    assert(list_no < nlist);
    return codes[list_no].data();
}

const idx_t* S3BuildOnlyInvertedLists::get_ids(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].data();
}

size_t S3BuildOnlyInvertedLists::add_entries(
        size_t list_no,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* code) {
    if (n_entry == 0) {
        // NOTE: return 0 instead of ids[list_no].size() because the return
        // value is not used when n_entry is 0 ... Exit early, though for in
        // memory implementation, it is not providing much performance benefits,
        // would be useful for disk/remote implementation to skip visting a
        // cluster.
        return 0;
    }
    assert(list_no < nlist);

    size_t o = ids[list_no].size();
    // Resize and copy ids
    ids[list_no].resize(o + n_entry);
    memcpy(&ids[list_no][o], ids_in, n_entry * sizeof(idx_t));
    // Resize and copy vectors
    codes[list_no].resize((o + n_entry) * code_size);
    memcpy(&codes[list_no][o * code_size], code, n_entry * code_size);
    return o;
}

void S3BuildOnlyInvertedLists::update_entries(
        size_t list_no,
        size_t offset,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* code) {
    // Cluster exists
    assert(list_no < nlist);
    // Valid offset and n_entry
    assert(n_entry + offset <= ids[list_no].size());
    // Copy ids
    memcpy(&ids[list_no][offset], ids_in, n_entry * sizeof(idx_t));
    // Copy vectors
    memcpy(&codes[list_no][offset * code_size], code, n_entry * code_size);
}

// ============================================================================
// S3InvertedListsIOHook - IO Hook Registration
// ============================================================================

struct S3InvertedListsIOHook : faiss::InvertedListsIOHook {
    S3InvertedListsIOHook()
        : InvertedListsIOHook("ils3", typeid(S3ReadNothingInvertedLists).name()) {
        std::cout << "[S3Hook] Registering S3InvertedListsIOHook (fourcc: ils3)" << std::endl;
    }

    void write(const faiss::InvertedLists* ils, faiss::IOWriter* f) const override {
        FAISS_THROW_MSG("S3InvertedLists is read-only, cannot write");
    }

    faiss::InvertedLists* read(faiss::IOReader* f, int io_flags) const override {
        FAISS_THROW_MSG("Use read_ArrayInvertedLists instead");
    }

    faiss::InvertedLists* read_ArrayInvertedLists(
            faiss::IOReader* f,
            int io_flags,
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& sizes) const override {

        std::cout << "[S3Hook] Creating S3ReadNothingInvertedLists placeholder" << std::endl;
        std::cout << "[S3Hook] nlist=" << nlist << ", code_size=" << code_size << std::endl;

        // Create placeholder with sizes
        auto s3il = new S3ReadNothingInvertedLists(nlist, code_size, sizes);

        // Calculate total data size to skip
        size_t total_bytes = 0;
        for (size_t s : sizes) {
            total_bytes += s * (code_size + sizeof(faiss::idx_t));
        }

        std::cout << "[S3Hook] Skipping " << total_bytes << " bytes of cluster data" << std::endl;

        // Skip cluster data in file stream
        auto* reader = dynamic_cast<faiss::FileIOReader*>(f);
        if (reader) {
            fseek(reader->f, total_bytes, SEEK_CUR);
        }

        return s3il;
    }
};

// Register hook during static initialization
static bool register_s3_hook_internal() {
    faiss::InvertedListsIOHook::add_callback(new S3InvertedListsIOHook());
    std::cout << "[S3Hook] S3InvertedListsIOHook registered successfully (static init)" << std::endl;
    return true;
}

static bool _s3_hook_registered = register_s3_hook_internal();

// Public function to manually register hook
void register_s3_io_hook() {
    static bool registered = false;
    if (!registered) {
        faiss::InvertedListsIOHook::add_callback(new S3InvertedListsIOHook());
        std::cout << "[S3Hook] S3InvertedListsIOHook registered (manual)" << std::endl;
        registered = true;
    }
}

} // namespace faiss_s3