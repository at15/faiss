// Version 2, only has a reader and rely on python script to generate the meta
// for the index
#include <iostream>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>
#include <faiss/utils/random.h>

#include <aws/core/Aws.h>
#include <aws/s3-crt/S3CrtClient.h>
#include <aws/s3-crt/S3CrtClientConfiguration.h>
#include <aws/s3-crt/model/GetObjectRequest.h>

#include "S3InvertedLists.h"

void read_local_file() {
    auto index_name =
            "quora-index-quora-distilbert-multilingual-size-100000.idx";
    faiss::Index* idx = faiss::read_index(index_name);
    auto* index = dynamic_cast<faiss::IndexIVFFlat*>(idx);
    if (!index) {
        throw std::runtime_error("Loaded index is not IndexIVFFlat");
    }

    std::cout << "Index loaded (d=" << index->d << ", ntotal=" << index->ntotal
              << ", nlist=" << index->nlist << ", nprobe=" << index->nprobe
              << ")" << std::endl;

    // Try to query it
    const size_t NQ = 10;
    const size_t K = 5;
    std::vector<float> queries(NQ * index->d);
    faiss::float_rand(queries.data(), NQ * index->d, 12345);
    std::vector<float> distances(NQ * K);
    std::vector<faiss::idx_t> labels(NQ * K);
    index->search(NQ, queries.data(), K, distances.data(), labels.data());

    for (size_t i = 0; i < NQ; i++) {
        std::cout << "Query " << i << ": ";
        for (size_t j = 0; j < K; j++) {
            std::cout << labels[i * K + j] << "(" << distances[i * K + j]
                      << ") ";
        }
        std::cout << std::endl;
    }

    delete index;
}

/**
 * Create S3 CRT client with optional custom endpoint for local testing.
 */
std::shared_ptr<Aws::S3Crt::S3CrtClient> create_s3_client() {
    std::cout << "Creating S3 CRT client" << std::endl;

    Aws::S3Crt::ClientConfiguration config;

    // Check for custom endpoint (for local s3mock testing)
    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint != nullptr) {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP; // Use HTTP for local mock
        config.verifySSL = false;

        std::cout << "Using custom S3 endpoint: " << endpoint << std::endl;
    }

    // TODO: configure part size and throughput target
    // https://github.com/awsdocs/aws-doc-sdk-examples/blob/main/cpp/example_code/s3-crt/s3-crt-demo.cpp#L226-L240

    // TODO: Do we need shared ptr here?
    return Aws::MakeShared<Aws::S3Crt::S3CrtClient>("S3CrtClient", config);
}

void read_s3_file() {
    // First download the parts before cluster data into memory and read it
    auto s3_client = create_s3_client();

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(Aws::String("test-bucket"));
    request.SetKey(Aws::String("quora/index.idx"));
    // HTTP Range header: "bytes=start-end" (inclusive on both ends)
    size_t offset = 0;
    size_t size = 3154059;
    std::string range = "bytes=" + std::to_string(offset) + "-" +
            std::to_string(offset + size - 1);
    request.SetRange(range);

    std::cout << "Sending request to S3" << std::endl;

    Aws::S3Crt::Model::GetObjectOutcome outcome = s3_client->GetObject(request);

    if (outcome.IsSuccess()) {
        auto& stream = outcome.GetResultWithOwnership().GetBody();
        std::cout
                << "Successfully retrieved object 'quora/index.idx' from bucket 'test-bucket'"
                << std::endl;

        // Read the entire stream into a VectorIOReader
        faiss::VectorIOReader reader;

        // Pre-allocate the vector since we know the size
        reader.data.resize(size);

        // Read stream directly into the pre-allocated vector
        stream.read(reinterpret_cast<char*>(reader.data.data()), size);
        std::streamsize bytes_read = stream.gcount();

        if (bytes_read != static_cast<std::streamsize>(size)) {
            std::cerr << "Warning: Expected " << size << " bytes but read "
                      << bytes_read << " bytes" << std::endl;
            reader.data.resize(bytes_read);
        }

        std::cout << "Downloaded " << reader.data.size() << " bytes into memory"
                  << std::endl;

        // Now read the index from the VectorIOReader
        faiss::Index* idx = faiss::read_index(&reader, faiss_s3::IO_FLAG_S3);
        // TODO: proper type cast and support more IVF formats
        auto* index = dynamic_cast<faiss::IndexIVFFlat*>(idx);
        if (!index) {
            throw std::runtime_error("Loaded index is not IndexIVFFlat");
        }

        std::cout << "Index loaded (d=" << index->d
                  << ", ntotal=" << index->ntotal << ", nlist=" << index->nlist
                  << ")" << std::endl;

        // Replace placeholder with S3OnDemandInvertedLists
        auto* placeholder = dynamic_cast<faiss_s3::S3ReadNothingInvertedLists*>(
                index->invlists);
        if (!placeholder) {
            throw std::runtime_error(
                    "Inverted lists is not S3ReadNothingInvertedLists");
        }

        // Create cache configuration (cluster_data_offset = size = 3154059)
        faiss_s3::S3ClusterCache::Config cache_config;
        cache_config.bucket = "test-bucket";
        cache_config.key = "quora/index.idx";
        cache_config.cluster_data_offset = size; // Same as downloaded header size
        cache_config.cluster_sizes = placeholder->cluster_sizes;
        cache_config.code_size = index->code_size;

        // Create cache and fetch function
        auto cache = std::make_shared<faiss_s3::S3ClusterCache>(
                s3_client, cache_config);
        auto fetch_fn = faiss_s3::make_fetch_fn(cache);

        // Create on-demand inverted lists
        auto s3_invlists = new faiss_s3::S3OnDemandInvertedLists(
                index->nlist, index->code_size, placeholder->cluster_sizes, fetch_fn);

        index->replace_invlists(s3_invlists, true);

        std::cout << "✓ Replaced with S3OnDemandInvertedLists" << std::endl;

        // Perform search
        index->nprobe = 2;
        const size_t NQ = 10;
        const size_t K = 5;

        std::vector<float> queries(NQ * index->d);
        faiss::float_rand(queries.data(), NQ * index->d, 12345);
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

        std::cout << "Cache statistics: " << cache->cache_size()
                  << " clusters cached" << std::endl;

        delete idx;
    } else {
        std::cerr << "Error: GetObject: " << outcome.GetError().GetMessage()
                  << std::endl;
    }
}

void test_s3() {
    // Initialize AWS SDK
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    // Register S3 IO so we can skip loading cluster data when reading
    faiss_s3::register_s3_io_hook();

    read_s3_file();

    // Shutdown AWS SDK
    Aws::ShutdownAPI(options);
}

int main() {
    std::cout << "demo_v2" << std::endl;

    // read_local_file();
    test_s3();

    return 0;
}
