#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <aws/core/Aws.h>
#include <aws/s3-crt/S3CrtClient.h>
#include <aws/s3-crt/S3CrtClientConfiguration.h>
#include <aws/s3-crt/model/GetObjectRequest.h>
#include <aws/s3-crt/model/PutObjectRequest.h>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissException.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>
#include <faiss/utils/random.h>

#include "S3InvertedLists.h"
#include "json.hpp"
#include "svf.h" // Read from simple vector format to use generated embeddings and text

using json = nlohmann::json;

// ============================================================================
// S3 Helper Functions
// ============================================================================

// Create S3 client (supports localhost:9000 mock server)
std::shared_ptr<Aws::S3Crt::S3CrtClient> CreateS3Client() {
    Aws::S3Crt::ClientConfiguration config;

    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint != nullptr) {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
        std::cout << "Using S3 endpoint: " << endpoint << std::endl;
    }

    const char* region = std::getenv("AWS_REGION");
    if (region != nullptr) {
        config.region = region;
    }
    // TODO: Config part size and throughput target
    // Seems AWS C++ SDK do parallel range requests automatically

    return Aws::MakeShared<Aws::S3Crt::S3CrtClient>("S3CrtClient", config);
}

// Upload file to S3
bool UploadFileToS3(
        const std::string& bucket,
        const std::string& key,
        const std::string& file_path) {
    auto client = CreateS3Client();

    // Read file into memory
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    Aws::S3Crt::Model::PutObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    auto data = Aws::MakeShared<Aws::StringStream>(
            "PutObjectInputStream",
            std::stringstream::in | std::stringstream::out |
                    std::stringstream::binary);
    *data << buffer.str();
    request.SetBody(data);

    auto outcome = client->PutObject(request);

    if (outcome.IsSuccess()) {
        std::cout << "✓ Uploaded " << key << " to s3://" << bucket << "/" << key
                  << " (" << buffer.str().size() << " bytes)" << std::endl;
        return true;
    } else {
        auto& error = outcome.GetError();
        std::cerr << "✗ Failed to upload " << key << ":" << std::endl;
        std::cerr << "  Error type: " << static_cast<int>(error.GetErrorType())
                  << std::endl;
        std::cerr << "  Message: " << error.GetMessage().c_str() << std::endl;
        std::cerr << "  Exception: " << error.GetExceptionName().c_str()
                  << std::endl;
        return false;
    }
}

// Download full object from S3
std::vector<uint8_t> DownloadFromS3(
        const std::string& bucket,
        const std::string& key) {
    auto client = CreateS3Client();

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    auto outcome = client->GetObject(request);

    if (outcome.IsSuccess()) {
        auto& stream = outcome.GetResultWithOwnership().GetBody();
        std::stringstream ss;
        ss << stream.rdbuf();
        std::string str = ss.str();

        return std::vector<uint8_t>(str.begin(), str.end());
    } else {
        FAISS_THROW_FMT(
                "Failed to download s3://%s/%s: %s",
                bucket.c_str(),
                key.c_str(),
                outcome.GetError().GetMessage().c_str());
    }
}

// Download byte range from S3
std::vector<uint8_t> DownloadRangeFromS3(
        const std::string& bucket,
        const std::string& key,
        size_t offset,
        size_t size) {
    auto client = CreateS3Client();

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    // S3 range format: "bytes=start-end" (inclusive on both ends)
    std::string range = "bytes=" + std::to_string(offset) + "-" +
            std::to_string(offset + size - 1);
    request.SetRange(range);

    auto outcome = client->GetObject(request);

    if (outcome.IsSuccess()) {
        auto& stream = outcome.GetResultWithOwnership().GetBody();
        std::stringstream ss;
        ss << stream.rdbuf();
        std::string str = ss.str();

        return std::vector<uint8_t>(str.begin(), str.end());
    } else {
        FAISS_THROW_FMT(
                "Failed to download range from s3://%s/%s [%zu:%zu]: %s",
                bucket.c_str(),
                key.c_str(),
                offset,
                offset + size,
                outcome.GetError().GetMessage().c_str());
    }
}

// ============================================================================
// Index Building
// ============================================================================

// Custom IOWriter that wraps FileIOWriter and tracks total bytes written
class TrackingIOWriter : public faiss::IOWriter {
   public:
    explicit TrackingIOWriter(const char* filename) : bytes_written(0) {
        file = fopen(filename, "wb");
        if (!file) {
            FAISS_THROW_FMT("Could not open %s for writing", filename);
        }
        name = filename;
    }

    ~TrackingIOWriter() override {
        if (file) {
            fclose(file);
        }
    }

    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        size_t ret = fwrite(ptr, size, nitems, file);
        bytes_written += size * ret;
        return ret;
    }

    size_t get_bytes_written() const {
        return bytes_written;
    }

   private:
    FILE* file = nullptr;
    size_t bytes_written;
};

void build_index(const char* index_file_name) {
    std::cout
            << "Building index in memory and writing to disk with offset tracking..."
            << std::endl;

    const size_t VEC_DIM = 128;       // Vector dimension
    const size_t N_CLUSTERS = 100;    // Number of IVF clusters
    const size_t N_VECTORS = 100'000; // Number of vectors to index

    // Create index, default invert list implementation is memory
    faiss::IndexFlatL2 quantizer(VEC_DIM);
    faiss::IndexIVFFlat index(&quantizer, VEC_DIM, N_CLUSTERS);
    index.verbose = true;

    // Create a new ArrayInvertedLists so that we can get number of vectors in
    // each cluster and calculate the offset for each cluster.
    faiss::ArrayInvertedLists array_inverted_lists(N_CLUSTERS, index.code_size);
    // false means the index doesn't own the inverted lists (we manage it
    // ourselves)
    index.replace_invlists(&array_inverted_lists, false);

    // Generate random vectors
    std::vector<float> xb(VEC_DIM * N_VECTORS);
    faiss::float_rand(xb.data(), VEC_DIM * N_VECTORS, 12345);

    // Train
    // TODO: We can use less vectors for training
    index.train(N_VECTORS, xb.data());

    // Add vectors
    index.add(N_VECTORS, xb.data());

    // Calculate inverted list section sizes before writing
    // This mirrors the write_InvertedLists implementation from index_write.cpp

    // Count non-empty clusters to determine format (full vs sparse)
    size_t n_non0 = 0;
    for (size_t i = 0; i < array_inverted_lists.nlist; i++) {
        if (array_inverted_lists.list_size(i) > 0) {
            n_non0++;
        }
    }

    bool is_full_format = (n_non0 > array_inverted_lists.nlist / 2);

    // Calculate sizes of different sections
    // Header: fourcc(4) + nlist(8) + code_size(8) + list_type(4) = 24 bytes
    size_t invlist_header_size = 4 + 8 + 8 + 4;

    // Sizes array: WRITEVECTOR writes count(8) + data
    size_t sizes_array_count;
    size_t sizes_array_data_size;
    if (is_full_format) {
        // Full format: one size_t per cluster
        sizes_array_count = array_inverted_lists.nlist;
        sizes_array_data_size = array_inverted_lists.nlist * sizeof(size_t);
    } else {
        // Sparse format: pairs of (cluster_id, size) for non-empty clusters
        sizes_array_count = n_non0 * 2;
        sizes_array_data_size = n_non0 * 2 * sizeof(size_t);
    }
    size_t sizes_array_size = 8 + sizes_array_data_size; // count + data

    // Calculate total size of cluster data (codes + ids)
    size_t inverted_list_data_size = 0;
    for (size_t i = 0; i < array_inverted_lists.nlist; i++) {
        size_t n = array_inverted_lists.list_size(i);
        if (n > 0) {
            inverted_list_data_size +=
                    n * array_inverted_lists.code_size;          // codes
            inverted_list_data_size += n * sizeof(faiss::idx_t); // ids
        }
    }

    size_t inverted_lists_total_size =
            invlist_header_size + sizes_array_size + inverted_list_data_size;

    std::cout << "Format: " << (is_full_format ? "full" : "sparse")
              << std::endl;
    std::cout << "Non-empty clusters: " << n_non0 << " / "
              << array_inverted_lists.nlist << std::endl;
    std::cout << "Inverted lists header size: " << invlist_header_size
              << " bytes" << std::endl;
    std::cout << "Sizes array size: " << sizes_array_size << " bytes"
              << std::endl;
    std::cout << "Cluster data size: " << inverted_list_data_size << " bytes"
              << std::endl;

    // Write index using custom tracking writer
    TrackingIOWriter writer(index_file_name);
    faiss::write_index(&index, &writer);

    size_t total_size = writer.get_bytes_written();

    // The inverted lists section starts at: total_size -
    // inverted_lists_total_size
    size_t inverted_lists_offset = total_size - inverted_lists_total_size;

    // The sizes array starts after the inverted lists header
    size_t sizes_array_offset = inverted_lists_offset + invlist_header_size;

    // The cluster data starts after the sizes array
    size_t cluster_data_offset = sizes_array_offset + sizes_array_size;

    std::cout << "Total file size: " << total_size << " bytes" << std::endl;
    std::cout << "Inverted lists offset: " << inverted_lists_offset << " bytes"
              << std::endl;
    std::cout << "Sizes array offset: " << sizes_array_offset << " bytes"
              << std::endl;
    std::cout << "Cluster data offset: " << cluster_data_offset << " bytes"
              << std::endl;

    // Generate metadata JSON
    json metadata;
    metadata["total_size"] = total_size;
    metadata["inverted_lists_offset"] = inverted_lists_offset;
    metadata["n_clusters"] = array_inverted_lists.nlist;
    metadata["code_size"] = array_inverted_lists.code_size;

    // Information about the sizes array in the file
    metadata["sizes_array_offset"] = sizes_array_offset;
    metadata["sizes_array_count"] = sizes_array_count;
    metadata["sizes_array_format"] = is_full_format ? "full" : "sparse";

    // Cluster data section
    metadata["cluster_data_offset"] = cluster_data_offset;

    // Write metadata to .meta.json file
    std::string metadata_file = std::string(index_file_name) + ".meta.json";
    std::ofstream meta_out(metadata_file);
    if (!meta_out) {
        FAISS_THROW_FMT("Could not open %s for writing", metadata_file.c_str());
    }
    meta_out << metadata.dump(2) << std::endl;
    meta_out.close();

    std::cout << "Metadata written to: " << metadata_file << std::endl;
}

// ============================================================================
// Upload Mode: Build Index and Upload to S3
// ============================================================================

void build_and_upload_to_s3(
        const char* index_file_name,
        const char* bucket,
        const char* key) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "=== BUILD AND UPLOAD MODE ===" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Build index locally first
    build_index(index_file_name);

    std::string metadata_file = std::string(index_file_name) + ".meta.json";

    std::cout << "\n=== Uploading to S3 ===" << std::endl;
    std::cout << "Bucket: " << bucket << std::endl;
    std::cout << "Key: " << key << std::endl;

    // Upload index file
    if (!UploadFileToS3(bucket, key, index_file_name)) {
        throw std::runtime_error("Failed to upload index file");
    }

    // Upload metadata
    std::string metadata_key = std::string(key) + ".meta.json";
    if (!UploadFileToS3(bucket, metadata_key, metadata_file)) {
        throw std::runtime_error("Failed to upload metadata file");
    }

    std::cout << "\n✓ Successfully uploaded to S3!" << std::endl;
    std::cout << "  Index: s3://" << bucket << "/" << key << std::endl;
    std::cout << "  Metadata: s3://" << bucket << "/" << metadata_key
              << std::endl;
}

// ============================================================================
// Search Mode: Load Index from S3 and Search
// ============================================================================

void search_from_s3(const char* bucket, const char* key) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "=== SEARCH FROM S3 MODE ===" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "Bucket: " << bucket << std::endl;
    std::cout << "Key: " << key << std::endl;

    // Download metadata (small file, download fully)
    std::cout << "\n[1/4] Downloading metadata..." << std::endl;
    std::string metadata_key = std::string(key) + ".meta.json";
    auto metadata_data = DownloadFromS3(bucket, metadata_key);
    std::string metadata_json(
            reinterpret_cast<char*>(metadata_data.data()),
            metadata_data.size());

    // Parse metadata
    json meta = json::parse(metadata_json);
    size_t inverted_lists_offset = meta["inverted_lists_offset"];
    size_t cluster_data_offset = meta["cluster_data_offset"];

    std::cout << "✓ Metadata loaded" << std::endl;
    std::cout << "  inverted_lists_offset: " << inverted_lists_offset
              << std::endl;
    std::cout << "  cluster_data_offset: " << cluster_data_offset << std::endl;

    // Download index header + inverted lists header (but not cluster data)
    // This includes: index structure + fourcc + nlist + code_size + sizes array
    std::cout << "\n[2/4] Downloading index header..." << std::endl;
    auto header_data = DownloadRangeFromS3(bucket, key, 0, cluster_data_offset);
    std::cout << "✓ Downloaded " << header_data.size() << " bytes" << std::endl;

    // Write to temporary file
    std::string temp_file = "/tmp/temp_index.faiss";
    std::ofstream out(temp_file, std::ios::binary);
    out.write(reinterpret_cast<char*>(header_data.data()), header_data.size());
    out.close();

    // Load index with S3 hook
    std::cout << "\n[3/4] Loading index with S3 hook..." << std::endl;
    // Ensure hook is registered (in case static init didn't run)
    faiss_s3::register_s3_io_hook();
    faiss::Index* idx =
            faiss::read_index(temp_file.c_str(), faiss_s3::IO_FLAG_S3);
    auto* index = dynamic_cast<faiss::IndexIVFFlat*>(idx);

    if (!index) {
        throw std::runtime_error("Loaded index is not IndexIVFFlat");
    }

    std::cout << "✓ Index loaded (d=" << index->d
              << ", ntotal=" << index->ntotal << ", nlist=" << index->nlist
              << ")" << std::endl;

    // Replace placeholder with S3 implementation
    std::cout << "\n[4/4] Replacing placeholder with S3ReadOnlyInvertedLists..."
              << std::endl;

    // Get sizes from placeholder
    auto* placeholder = dynamic_cast<faiss_s3::S3ReadNothingInvertedLists*>(
            index->invlists);
    if (!placeholder) {
        throw std::runtime_error(
                "Inverted lists is not S3ReadNothingInvertedLists");
    }

    // Create S3 client once for all operations
    auto s3_client = CreateS3Client();

    auto s3_invlists = new faiss_s3::S3ReadOnlyInvertedLists(
            s3_client, bucket, key, metadata_json, placeholder->cluster_sizes);
    index->replace_invlists(s3_invlists, true); // true = index owns it

    // Configure search
    index->nprobe = 2;
    const size_t NQ = 10;
    const size_t K = 5;

    std::cout << "\n=== Generating Query Vectors ===" << std::endl;
    std::vector<float> queries(NQ * index->d);
    faiss::float_rand(queries.data(), NQ * index->d, 12345);

    std::cout << "\n=== Performing Search (nprobe=" << index->nprobe
              << ") ===" << std::endl;
    std::vector<float> distances(NQ * K);
    std::vector<faiss::idx_t> labels(NQ * K);

    index->search(NQ, queries.data(), K, distances.data(), labels.data());

    std::cout << "\n=== Search Results ===" << std::endl;
    for (size_t i = 0; i < NQ; i++) {
        std::cout << "Query " << i << ": ";
        for (size_t j = 0; j < K; j++) {
            std::cout << labels[i * K + j] << "(" << distances[i * K + j]
                      << ") ";
        }
        std::cout << std::endl;
    }

    std::cout << "\n✓ Search completed successfully!" << std::endl;

    delete index;
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    // Initialize AWS SDK
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    try {
        std::cout << "========================================" << std::endl;
        std::cout << "=== Faiss S3 IVF Demo ===" << std::endl;
        std::cout << "========================================\n" << std::endl;

        if (argc < 2) {
            std::cerr << "Usage:" << std::endl;
            std::cerr << "  Build locally:  " << argv[0]
                      << " build <index_file>" << std::endl;
            std::cerr << "  Build & upload: " << argv[0]
                      << " upload <index_file> <bucket> <key>" << std::endl;
            std::cerr << "  Search from S3: " << argv[0]
                      << " search <bucket> <key>" << std::endl;
            std::cerr << "\nEnvironment variables:" << std::endl;
            std::cerr
                    << "  S3_ENDPOINT_URL - Custom S3 endpoint (e.g., http://localhost:9000)"
                    << std::endl;
            std::cerr << "  AWS_ACCESS_KEY_ID - AWS access key" << std::endl;
            std::cerr << "  AWS_SECRET_ACCESS_KEY - AWS secret key"
                      << std::endl;
            std::cerr << "  AWS_REGION - AWS region (e.g., us-east-1)"
                      << std::endl;
            std::cerr
                    << "  AWS_EC2_METADATA_DISABLED - Set to 'true' for local testing"
                    << std::endl;
            Aws::ShutdownAPI(options);
            return 1;
        }

        std::string mode = argv[1];

        if (mode == "build") {
            if (argc != 3) {
                std::cerr << "Usage: " << argv[0] << " build <index_file>"
                          << std::endl;
                Aws::ShutdownAPI(options);
                return 1;
            }
            build_index(argv[2]);
        } else if (mode == "upload") {
            if (argc != 5) {
                std::cerr << "Usage: " << argv[0]
                          << " upload <index_file> <bucket> <key>" << std::endl;
                Aws::ShutdownAPI(options);
                return 1;
            }
            build_and_upload_to_s3(argv[2], argv[3], argv[4]);
        } else if (mode == "search") {
            if (argc != 4) {
                std::cerr << "Usage: " << argv[0] << " search <bucket> <key>"
                          << std::endl;
                Aws::ShutdownAPI(options);
                return 1;
            }
            search_from_s3(argv[2], argv[3]);
        } else {
            std::cerr << "Unknown mode: " << mode << std::endl;
            std::cerr << "Valid modes: build, upload, search" << std::endl;
            Aws::ShutdownAPI(options);
            return 1;
        }

    } catch (faiss::FaissException& e) {
        std::cerr << "\n✗ Faiss Error: " << e.what() << std::endl;
        Aws::ShutdownAPI(options);
        return 1;
    } catch (std::exception& e) {
        std::cerr << "\n✗ Error: " << e.what() << std::endl;
        Aws::ShutdownAPI(options);
        return 1;
    }

    Aws::ShutdownAPI(options);
    return 0;
}