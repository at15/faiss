#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>

namespace faiss_s3 {

// Metadata for calcuating offset of inverted lists so we
// can load each cluster from S3 on demand
struct IVFIndexMeta {
    // This is the only thing we need, other offsets can be calculated using
    // cluster id (list_no) and code size.
    size_t cluster_data_offset;
    // TODO: We can also save cluster data size
    // so we can load everything into memory if it is too small
};

// Type alias for Faiss index type to avoid polluting namespace
using idx_t = faiss::idx_t;

// Build index in memory and flush to S3.
// Base on the default ArrayInvertedLists
struct S3BuildOnlyInvertedLists : faiss::InvertedLists {
    // Stores vectors for all clusters.
    // Top level is the cluster, codes.size() == nlist.
    // std::vector<uint8_t> is a gaint vector to store all the vectors
    // in that cluster. No need for another vector because size
    // of each vector is same code_size. All we want is a large chunk
    // of memory. uint8_t is char, i.e. 1 byte.
    std::vector<std::vector<uint8_t>> codes;

    // Maps vectors within a cluster to their global id.
    // Top level is the cluster, ids.size() == nlist.
    // For std::vector<idx_t>
    // - the array offset is the index of vector within the cluster
    // - the value is the global id of the vector
    // NOTE: We need this mapping because the id is across clusters
    // and assigned base on insertion order (by default).
    // https://github.com/facebookresearch/faiss/wiki/Pre--and-post-processing#faiss-id-mapping
    std::vector<std::vector<idx_t>> ids;

    // nlist is number of clusters
    // code_size is size of single (encoded) vector e.g. 128*sizeof(float)
    // e.g. for IVFFlat this is the original vector size
    // for IVFPQ this is the PQ code size
    S3BuildOnlyInvertedLists(size_t nlist, size_t code_size);

    // Resize a cluster
    void resize(size_t list_no, size_t new_size) override;
    // Get number of vectors in a cluster
    size_t list_size(size_t list_no) const override;
    // inverted_list_context is for sharing centroid, only used by
    // DispatchingInvertedLists
    bool is_empty(size_t list_no, void* inverted_list_context = nullptr)
            const override;
    // Reorder the clusters, for example shuffle the centroids base on cluster
    // size. Both the centroids and the vectors should be shuffled at same time.
    void permute_invlists(const idx_t* map);

    // Get all the vectors for a cluster as a raw pointer
    const uint8_t* get_codes(size_t list_no) const override;
    // Get all ids for a cluster as a raw pointer
    const idx_t* get_ids(size_t list_no) const override;

    // Add vectors to a cluster
    // - list_no is the cluster index
    // - n_entry is the number of vectors to add
    // - ids_in is the raw pointer and visit base on n_entry and sizeof(idx_t)
    // - code is the raw pointer and visit base on n_entry and code_size
    // Returns the offset of the first new vector in the cluster
    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    // Update vectors in a cluster in place.
    // Similar to add_entries but gives offset to start updating from.
    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    // NOTE: get_iterator is for e.g. rocksdb where the entire vectors are
    // NOT loaded in memory and need to get vectors one by one. The default
    // implementation `CodeArrayIterator` wraps the raw memory addresses and
    // returns (id, vector) one by one.
    // TODO: Is this efficient? Why not get vectors in batches to compute
    // distance again the query vector?
};

// Placeholder created during read_index() with IO_FLAG_S3
// All methods throw errors until replaced with S3ReadOnlyInvertedLists
struct S3ReadNothingInvertedLists : faiss::InvertedLists {
    std::vector<size_t> cluster_sizes; // Store sizes from hook

    S3ReadNothingInvertedLists(
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& sizes);

    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void resize(size_t list_no, size_t new_size) override;
};

// Lazy load cluster data from S3 on demand
struct S3ReadOnlyInvertedLists : faiss::InvertedLists {
    // S3 configuration
    std::string s3_bucket;
    std::string s3_key; // Path to index file in S3
    std::shared_ptr<void>
            s3_client_; // Opaque pointer to S3 client (reused for all requests)

    // Metadata
    size_t cluster_data_offset;
    size_t sizes_array_offset;
    size_t sizes_array_count;
    std::string sizes_array_format; // "full" or "sparse"

    // Cluster information
    std::vector<size_t> cluster_sizes;   // Number of vectors per cluster
    std::vector<size_t> cluster_offsets; // Byte offset in file

    // Cache (no LRU for now, cache forever)
    mutable std::unordered_map<size_t, std::vector<uint8_t>> codes_cache;
    mutable std::unordered_map<size_t, std::vector<idx_t>> ids_cache;
    mutable std::mutex cache_mutex;

    // Constructor: loads metadata from JSON string and uses provided sizes
    // s3_client should be created by caller and passed in (shared across all
    // instances)
    S3ReadOnlyInvertedLists(
            std::shared_ptr<void> s3_client,
            const std::string& bucket,
            const std::string& key,
            const std::string& metadata_json,
            const std::vector<size_t>& sizes);

    // Read methods (lazy loading)
    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    // Write methods (throw errors)
    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void resize(size_t list_no, size_t new_size) override;

   private:
    void load_metadata(const std::string& metadata_json);
    void load_sizes_from_s3();
    void calculate_cluster_offsets();
    void fetch_cluster(size_t list_no) const;
};

// S3OnDemandInvertedLists - loads cluster data from S3 on-demand with caching
// Simplified version that merges S3 client and caching logic into one class
struct S3OnDemandInvertedLists : faiss::InvertedLists {
    // Constructor
    S3OnDemandInvertedLists(
            std::shared_ptr<void> s3_client,
            const std::string& bucket,
            const std::string& key,
            size_t cluster_data_offset,
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& cluster_sizes);

    // Read methods
    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;

    // Write methods (not supported, read-only)
    size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids_in,
            const uint8_t* code) override;

    void resize(size_t list_no, size_t new_size) override;

    // Cache statistics
    size_t cache_size() const;
    void clear_cache();
    size_t cache_hits() const;
    size_t cache_misses() const;

   private:
    // S3 configuration
    std::shared_ptr<void> s3_client_;
    std::string s3_bucket_;
    std::string s3_key_;
    size_t cluster_data_offset_;

    // Cluster metadata
    std::vector<size_t> cluster_sizes_;

    // Cache for cluster data
    struct ClusterData {
        std::vector<uint8_t> codes;
        std::vector<idx_t> ids;
    };
    mutable std::unordered_map<size_t, std::shared_ptr<ClusterData>> cache_;
    mutable std::mutex cache_mutex_;

    // Cache statistics
    mutable size_t cache_hits_ = 0;
    mutable size_t cache_misses_ = 0;

    // Helper methods
    std::shared_ptr<ClusterData> fetch_cluster(size_t list_no) const;
    size_t calculate_cluster_offset(size_t list_no) const;
};

// IO flag for S3 lazy loading
// 0x7333 = "s3" in hex (big-endian for fourcc)
// Combined with IO_FLAG_SKIP_IVF_DATA and "il" prefix → "ils3"
// IO flag for S3 on-demand loading
// fourcc("ils3") = 0x33736c69, so upper 16 bits = 0x3373
const int IO_FLAG_S3 = faiss::IO_FLAG_SKIP_IVF_DATA | 0x33730000;

// TODO: Check IndexIVFlatPanorama
// https://github.com/facebookresearch/faiss/pull/4606
// TODO: I remember claude code mentioned there is prefetch .... might need to
// check the rocksdb implementation

// Manually register S3 hook (call this before using IO_FLAG_S3)
void register_s3_io_hook();

} // namespace faiss_s3