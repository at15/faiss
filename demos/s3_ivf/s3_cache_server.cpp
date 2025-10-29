#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Faiss headers
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>

// S3 inverted lists
#include "S3InvertedLists.h"

// AWS SDK headers
#include <aws/core/Aws.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/s3-crt/S3CrtClient.h>
#include <aws/s3-crt/model/GetObjectRequest.h>

// Helper: Download byte range from S3
static std::vector<uint8_t> DownloadRangeFromS3(
        std::shared_ptr<Aws::S3Crt::S3CrtClient> client,
        const std::string& bucket,
        const std::string& key,
        size_t offset,
        size_t size) {
    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    // S3 range format: "bytes=start-end" (end is inclusive)
    std::ostringstream range_stream;
    range_stream << "bytes=" << offset << "-" << (offset + size - 1);
    request.SetRange(range_stream.str());

    auto outcome = client->GetObject(request);

    if (!outcome.IsSuccess()) {
        throw std::runtime_error(
                "S3 GetObject failed: " +
                outcome.GetError().GetMessage());
    }

    auto& stream = outcome.GetResultWithOwnership().GetBody();
    std::vector<uint8_t> data(size);
    stream.read(reinterpret_cast<char*>(data.data()), size);

    if (stream.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error(
                "S3 read size mismatch: expected " + std::to_string(size) +
                        ", got " + std::to_string(stream.gcount()));
    }

    return data;
}

// Helper: Create S3 client
static std::shared_ptr<Aws::S3Crt::S3CrtClient> create_s3_client() {
    Aws::S3Crt::ClientConfiguration config;

    // Check for custom endpoint (for S3Mock or MinIO)
    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint) {
        config.endpointOverride = endpoint;
        std::cout << "[S3] Using custom endpoint: " << endpoint << std::endl;
    }

    // Check for region
    const char* region = std::getenv("AWS_REGION");
    if (region) {
        config.region = region;
    } else {
        config.region = "us-east-1"; // Default
    }

    return std::make_shared<Aws::S3Crt::S3CrtClient>(config);
}

// Global state for the server
struct IndexState {
    int id;
    std::string bucket;
    std::string key;
    int64_t cluster_data_offset;

    // Faiss components
    std::shared_ptr<faiss::Index> index;
    std::shared_ptr<Aws::S3Crt::S3CrtClient> s3_client;
    faiss_s3::S3OnDemandInvertedLists* s3_invlists = nullptr;
};

class ServerState {
public:
    Aws::SDKOptions sdk_options;

    ServerState() {
        // Initialize AWS SDK
        std::cout << "[Server] Initializing AWS SDK..." << std::endl;
        Aws::InitAPI(sdk_options);

        // Register S3 IO hook for Faiss
        std::cout << "[Server] Registering S3 IO hook..." << std::endl;
        faiss_s3::register_s3_io_hook();
    }

    ~ServerState() {
        // Cleanup indexes
        {
            std::lock_guard<std::mutex> lock(indexes_mutex);
            std::cout << "[Server] Cleaning up " << loaded_indexes.size()
                      << " loaded indexes..." << std::endl;
            loaded_indexes.clear();
        }

        // Shutdown AWS SDK
        std::cout << "[Server] Shutting down AWS SDK..." << std::endl;
        Aws::ShutdownAPI(sdk_options);
    }

    std::atomic<int> next_index_id{1};
    std::map<int, IndexState> loaded_indexes;
    std::mutex indexes_mutex;

    int add_index(
            const std::string& bucket,
            const std::string& key,
            int64_t cluster_data_offset,
            std::shared_ptr<faiss::Index> index,
            std::shared_ptr<Aws::S3Crt::S3CrtClient> s3_client,
            faiss_s3::S3OnDemandInvertedLists* s3_invlists) {
        std::lock_guard<std::mutex> lock(indexes_mutex);
        int id = next_index_id.fetch_add(1);
        IndexState state;
        state.id = id;
        state.bucket = bucket;
        state.key = key;
        state.cluster_data_offset = cluster_data_offset;
        state.index = index;
        state.s3_client = s3_client;
        state.s3_invlists = s3_invlists;
        loaded_indexes[id] = state;
        return id;
    }

    bool get_index(int id, IndexState** out_state) {
        std::lock_guard<std::mutex> lock(indexes_mutex);
        auto it = loaded_indexes.find(id);
        if (it == loaded_indexes.end()) {
            return false;
        }
        *out_state = &it->second;
        return true;
    }

    int get_index_count() {
        std::lock_guard<std::mutex> lock(indexes_mutex);
        return loaded_indexes.size();
    }

    // Find existing index by S3 location
    int find_index_by_location(
            const std::string& bucket,
            const std::string& key,
            int64_t cluster_data_offset) {
        std::lock_guard<std::mutex> lock(indexes_mutex);
        for (const auto& kv : loaded_indexes) {
            const IndexState& state = kv.second;
            if (state.bucket == bucket && state.key == key &&
                state.cluster_data_offset == cluster_data_offset) {
                return state.id;
            }
        }
        return -1; // Not found
    }
};

// Protocol utilities
class ProtocolParser {
public:
    static std::map<std::string, std::string> parse_command(
            const std::string& line) {
        std::map<std::string, std::string> params;
        std::istringstream iss(line);
        std::string token;

        // First token is the command
        if (iss >> token) {
            params["__command"] = token;
        }

        // Parse key=value pairs
        while (iss >> token) {
            size_t eq_pos = token.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = token.substr(0, eq_pos);
                std::string value = token.substr(eq_pos + 1);
                params[key] = value;
            }
        }

        return params;
    }

    static std::string format_response(
            const std::map<std::string, std::string>& params) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& kv : params) {
            if (!first)
                oss << " ";
            oss << kv.first << "=" << kv.second;
            first = false;
        }
        oss << "\n";
        return oss.str();
    }

    static std::string format_error(
            const std::string& code,
            const std::string& msg) {
        return "ERROR code=" + code + " msg=" + msg + "\n";
    }
};

// Binary data I/O
class BinaryIO {
public:
    static bool read_exact(int socket, void* buffer, size_t length) {
        size_t total_read = 0;
        uint8_t* buf = static_cast<uint8_t*>(buffer);

        while (total_read < length) {
            ssize_t n =
                    recv(socket, buf + total_read, length - total_read, 0);
            if (n <= 0) {
                return false; // Connection closed or error
            }
            total_read += n;
        }

        return true;
    }

    static bool write_exact(int socket, const void* buffer, size_t length) {
        size_t total_written = 0;
        const uint8_t* buf = static_cast<const uint8_t*>(buffer);

        while (total_written < length) {
            ssize_t n = send(
                    socket, buf + total_written, length - total_written, 0);
            if (n <= 0) {
                return false; // Connection closed or error
            }
            total_written += n;
        }

        return true;
    }

    static bool read_binary_array(int socket, std::vector<uint8_t>& data) {
        uint32_t length;
        if (!read_exact(socket, &length, sizeof(length))) {
            return false;
        }

        data.resize(length);
        return read_exact(socket, data.data(), length);
    }

    static bool write_binary_array(
            int socket,
            const void* data,
            uint32_t length) {
        if (!write_exact(socket, &length, sizeof(length))) {
            return false;
        }
        return write_exact(socket, data, length);
    }
};

// Client handler
class ClientHandler {
private:
    int client_socket;
    ServerState* server_state;
    bool running;

public:
    ClientHandler(int socket, ServerState* state)
            : client_socket(socket), server_state(state), running(true) {}

    ~ClientHandler() {
        if (client_socket >= 0) {
            close(client_socket);
        }
    }

    void run() {
        std::cout << "[Client " << client_socket << "] Connected"
                  << std::endl;

        while (running) {
            // Read command line
            std::string line;
            if (!read_line(line)) {
                std::cout << "[Client " << client_socket << "] Disconnected"
                          << std::endl;
                break;
            }

            if (line.empty()) {
                continue;
            }

            // Parse and handle command
            auto params = ProtocolParser::parse_command(line);
            if (params.find("__command") == params.end()) {
                send_error("INVALID_COMMAND", "Empty command");
                continue;
            }

            std::string command = params["__command"];
            std::cout << "[Client " << client_socket << "] Command: " << command
                      << std::endl;

            if (command == "ECHO") {
                handle_echo(params);
            } else if (command == "LOAD") {
                handle_load(params);
            } else if (command == "SEARCH") {
                handle_search(params);
            } else if (command == "INFO") {
                handle_info(params);
            } else {
                send_error("INVALID_COMMAND", "Unknown command: " + command);
            }
        }
    }

private:
    bool read_line(std::string& line) {
        line.clear();
        char ch;
        while (true) {
            ssize_t n = recv(client_socket, &ch, 1, 0);
            if (n <= 0) {
                return false; // Connection closed or error
            }
            if (ch == '\n') {
                break;
            }
            if (line.size() >= 8192) { // Max line length
                return false;
            }
            line += ch;
        }
        return true;
    }

    bool send_response(const std::string& response) {
        return BinaryIO::write_exact(
                client_socket, response.c_str(), response.size());
    }

    bool send_error(const std::string& code, const std::string& msg) {
        std::string error = ProtocolParser::format_error(code, msg);
        std::cout << "[Client " << client_socket << "] Error: " << code
                  << " - " << msg << std::endl;
        return send_response(error);
    }

    void handle_echo(const std::map<std::string, std::string>& params) {
        auto it = params.find("msg");
        if (it == params.end()) {
            send_error("MISSING_PARAM", "Missing msg parameter");
            return;
        }

        std::map<std::string, std::string> response;
        response["msg"] = it->second;
        send_response(ProtocolParser::format_response(response));
    }

    void handle_load(const std::map<std::string, std::string>& params) {
        // Parse parameters
        auto bucket_it = params.find("bucket");
        auto key_it = params.find("key");
        auto offset_it = params.find("cluster_data_offset");

        if (bucket_it == params.end() || key_it == params.end() ||
            offset_it == params.end()) {
            send_error(
                    "MISSING_PARAM",
                    "Missing required parameters (bucket, key, "
                    "cluster_data_offset)");
            return;
        }

        std::string bucket = bucket_it->second;
        std::string key = key_it->second;
        int64_t cluster_data_offset;

        try {
            cluster_data_offset = std::stoll(offset_it->second);
        } catch (...) {
            send_error("INVALID_PARAM", "Invalid cluster_data_offset");
            return;
        }

        std::cout << "[Client " << client_socket << "] Loading index: s3://"
                  << bucket << "/" << key
                  << " (offset=" << cluster_data_offset << ")" << std::endl;

        // Check if this index is already loaded
        int existing_id = server_state->find_index_by_location(
                bucket, key, cluster_data_offset);

        if (existing_id != -1) {
            std::cout << "[Client " << client_socket
                      << "] Index already loaded with ID=" << existing_id
                      << ", returning existing" << std::endl;

            // Get the existing index to return metadata
            IndexState* existing_state;
            if (server_state->get_index(existing_id, &existing_state)) {
                auto* ivf_index = dynamic_cast<faiss::IndexIVFFlat*>(
                        existing_state->index.get());

                std::map<std::string, std::string> response;
                response["index"] = std::to_string(existing_id);
                response["d"] = std::to_string(ivf_index->d);
                response["ntotal"] = std::to_string(ivf_index->ntotal);
                response["nlist"] = std::to_string(ivf_index->nlist);
                response["metric_type"] =
                        std::to_string(ivf_index->metric_type);

                send_response(ProtocolParser::format_response(response));
                return;
            }
        }

        try {
            // Create S3 client
            auto s3_client = create_s3_client();

            // Download index metadata
            std::cout << "[Client " << client_socket
                      << "] Downloading metadata (0-"
                      << cluster_data_offset - 1 << ")" << std::endl;

            auto metadata_bytes = DownloadRangeFromS3(
                    s3_client, bucket, key, 0, cluster_data_offset);

            // Parse index with S3 flag
            faiss::VectorIOReader reader;
            reader.data = std::move(metadata_bytes);

            faiss::Index* raw_index =
                    faiss::read_index(&reader, faiss_s3::IO_FLAG_S3);

            // Cast to IVF index
            auto* ivf_index = dynamic_cast<faiss::IndexIVFFlat*>(raw_index);
            if (!ivf_index) {
                delete raw_index;
                send_error("LOAD_FAILED", "Index is not IndexIVFFlat type");
                return;
            }

            std::cout << "[Client " << client_socket << "] Parsed index: d="
                      << ivf_index->d << ", ntotal=" << ivf_index->ntotal
                      << ", nlist=" << ivf_index->nlist << std::endl;

            // Extract placeholder inverted lists
            auto* placeholder =
                    dynamic_cast<faiss_s3::S3ReadNothingInvertedLists*>(
                            ivf_index->invlists);

            if (!placeholder) {
                delete raw_index;
                send_error("LOAD_FAILED", "Invalid inverted lists type");
                return;
            }

            // Create S3OnDemandInvertedLists
            auto* s3_invlists = new faiss_s3::S3OnDemandInvertedLists(
                    s3_client,
                    bucket,
                    key,
                    cluster_data_offset,
                    ivf_index->nlist,
                    ivf_index->code_size,
                    placeholder->cluster_sizes);

            // Replace inverted lists
            ivf_index->replace_invlists(s3_invlists, true); // owns=true

            // Store in server state
            std::shared_ptr<faiss::Index> index_ptr(raw_index);
            int index_id = server_state->add_index(
                    bucket,
                    key,
                    cluster_data_offset,
                    index_ptr,
                    s3_client,
                    s3_invlists);

            std::cout << "[Client " << client_socket
                      << "] Index loaded with ID=" << index_id << std::endl;

            // Send response with metadata
            std::map<std::string, std::string> response;
            response["index"] = std::to_string(index_id);
            response["d"] = std::to_string(ivf_index->d);
            response["ntotal"] = std::to_string(ivf_index->ntotal);
            response["nlist"] = std::to_string(ivf_index->nlist);
            response["metric_type"] = std::to_string(ivf_index->metric_type);

            send_response(ProtocolParser::format_response(response));

        } catch (const std::exception& e) {
            send_error(
                    "LOAD_FAILED",
                    std::string("Failed to load index: ") + e.what());
        }
    }

    void handle_search(const std::map<std::string, std::string>& params) {
        // Parse parameters
        auto index_it = params.find("index");
        auto k_it = params.find("k");
        auto d_it = params.find("d");

        if (index_it == params.end() || k_it == params.end() ||
            d_it == params.end()) {
            send_error(
                    "MISSING_PARAM",
                    "Missing required parameters (index, k, d)");
            return;
        }

        int index_id, k, d;
        try {
            index_id = std::stoi(index_it->second);
            k = std::stoi(k_it->second);
            d = std::stoi(d_it->second);
        } catch (...) {
            send_error("INVALID_PARAM", "Invalid parameter values");
            return;
        }

        // Read query vector (binary) - MUST read before validation
        std::vector<uint8_t> query_data;
        if (!BinaryIO::read_binary_array(client_socket, query_data)) {
            send_error("INVALID_BINARY", "Failed to read query vector");
            return;
        }

        // Validate query vector size
        size_t expected_size = d * sizeof(float);
        if (query_data.size() != expected_size) {
            send_error(
                    "INVALID_BINARY",
                    "Expected " + std::to_string(expected_size) + " bytes, got " +
                            std::to_string(query_data.size()));
            return;
        }

        // Get index from server state
        IndexState* index_state;
        if (!server_state->get_index(index_id, &index_state)) {
            send_error(
                    "INDEX_NOT_FOUND",
                    "Index " + std::to_string(index_id) + " not loaded");
            return;
        }

        // Cast to IVF index
        auto* ivf_index =
                dynamic_cast<faiss::IndexIVFFlat*>(index_state->index.get());

        if (!ivf_index) {
            send_error("INTERNAL_ERROR", "Index is not IVF type");
            return;
        }

        // Validate dimension
        if (ivf_index->d != static_cast<size_t>(d)) {
            send_error(
                    "INVALID_PARAM",
                    "Dimension mismatch: index has d=" +
                            std::to_string(ivf_index->d) +
                            ", query has d=" + std::to_string(d));
            return;
        }

        std::cout << "[Client " << client_socket << "] Search: index="
                  << index_id << ", k=" << k << ", d=" << d << std::endl;

        try {
            // Perform Faiss search
            const float* query =
                    reinterpret_cast<const float*>(query_data.data());

            std::vector<float> distances(k);
            std::vector<faiss::idx_t> labels(k);

            // This triggers on-demand S3 fetching
            ivf_index->search(1, query, k, distances.data(), labels.data());

            std::cout << "[Client " << client_socket
                      << "] Search completed: cache_hits="
                      << index_state->s3_invlists->cache_hits()
                      << ", cache_misses="
                      << index_state->s3_invlists->cache_misses() << std::endl;

            // Send text response
            std::map<std::string, std::string> response;
            response["k"] = std::to_string(k);
            if (!send_response(ProtocolParser::format_response(response))) {
                return;
            }

            // Send binary data: IDs (int64)
            if (!BinaryIO::write_binary_array(
                        client_socket,
                        labels.data(),
                        k * sizeof(faiss::idx_t))) {
                std::cout << "[Client " << client_socket
                          << "] Failed to send result IDs" << std::endl;
                return;
            }

            // Send binary data: distances (float32)
            if (!BinaryIO::write_binary_array(
                        client_socket, distances.data(), k * sizeof(float))) {
                std::cout << "[Client " << client_socket
                          << "] Failed to send distances" << std::endl;
                return;
            }

            std::cout << "[Client " << client_socket
                      << "] Results sent successfully" << std::endl;

        } catch (const std::exception& e) {
            send_error(
                    "SEARCH_FAILED",
                    std::string("Search failed: ") + e.what());
        }
    }

    void handle_info(const std::map<std::string, std::string>& params) {
        auto about_it = params.find("about");
        if (about_it == params.end()) {
            send_error("MISSING_PARAM", "Missing about parameter");
            return;
        }

        std::map<std::string, std::string> response;

        if (about_it->second == "cache") {
            // Global cache statistics
            int index_count = server_state->get_index_count();

            // Aggregate statistics from all indexes
            int64_t total_hits = 0;
            int64_t total_misses = 0;

            {
                std::lock_guard<std::mutex> lock(
                        server_state->indexes_mutex);
                for (const auto& kv : server_state->loaded_indexes) {
                    const IndexState& state = kv.second;
                    if (state.s3_invlists) {
                        total_hits += state.s3_invlists->cache_hits();
                        total_misses += state.s3_invlists->cache_misses();
                    }
                }
            }

            response["index_count"] = std::to_string(index_count);
            response["cache_hits"] = std::to_string(total_hits);
            response["cache_misses"] = std::to_string(total_misses);
            send_response(ProtocolParser::format_response(response));

        } else if (about_it->second == "index") {
            // Per-index statistics
            auto id_it = params.find("id");
            if (id_it == params.end()) {
                send_error(
                        "MISSING_PARAM",
                        "Missing id parameter for index query");
                return;
            }

            int index_id;
            try {
                index_id = std::stoi(id_it->second);
            } catch (...) {
                send_error("INVALID_PARAM", "Invalid id parameter");
                return;
            }

            IndexState* index_state;
            if (!server_state->get_index(index_id, &index_state)) {
                send_error(
                        "INDEX_NOT_FOUND",
                        "Index " + std::to_string(index_id) + " not loaded");
                return;
            }

            auto* ivf = dynamic_cast<faiss::IndexIVFFlat*>(
                    index_state->index.get());

            response["cluster_count"] = std::to_string(ivf->nlist);
            response["cache_hits"] =
                    std::to_string(index_state->s3_invlists->cache_hits());
            response["cache_misses"] =
                    std::to_string(index_state->s3_invlists->cache_misses());
            response["cached_clusters"] =
                    std::to_string(index_state->s3_invlists->cache_size());
            response["nprobe"] = std::to_string(ivf->nprobe);

            send_response(ProtocolParser::format_response(response));

        } else {
            send_error(
                    "INVALID_PARAM",
                    "Invalid about value: " + about_it->second);
        }
    }
};

// Server class
class TCPServer {
private:
    int server_socket;
    int port;
    ServerState server_state;
    std::atomic<bool> running{true};
    std::vector<std::thread> client_threads;

public:
    TCPServer(int port) : server_socket(-1), port(port) {}

    ~TCPServer() {
        stop();
    }

    bool start() {
        // Create socket
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        // Set socket options
        int opt = 1;
        if (setsockopt(
                    server_socket,
                    SOL_SOCKET,
                    SO_REUSEADDR,
                    &opt,
                    sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            close(server_socket);
            return false;
        }

        // Bind to port
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_socket,
                 (struct sockaddr*)&address,
                 sizeof(address)) < 0) {
            std::cerr << "Failed to bind to port " << port << std::endl;
            close(server_socket);
            return false;
        }

        // Listen
        if (listen(server_socket, 10) < 0) {
            std::cerr << "Failed to listen on port " << port << std::endl;
            close(server_socket);
            return false;
        }

        std::cout << "Server listening on port " << port << std::endl;
        return true;
    }

    void run() {
        while (running) {
            // Accept client connection
            struct sockaddr_in client_address;
            socklen_t client_len = sizeof(client_address);

            int client_socket = accept(
                    server_socket,
                    (struct sockaddr*)&client_address,
                    &client_len);
            if (client_socket < 0) {
                if (running) {
                    std::cerr << "Failed to accept connection" << std::endl;
                }
                continue;
            }

            // Spawn thread for client
            client_threads.emplace_back([this, client_socket]() {
                ClientHandler handler(client_socket, &server_state);
                handler.run();
            });
        }
    }

    void stop() {
        running = false;

        if (server_socket >= 0) {
            close(server_socket);
            server_socket = -1;
        }

        // Wait for all client threads to finish
        for (auto& thread : client_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        std::cout << "Server stopped" << std::endl;
    }
};

// Signal handler for graceful shutdown
TCPServer* g_server = nullptr;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..."
              << std::endl;
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    int port = 9001;

    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    TCPServer server(port);
    g_server = &server;

    // Register signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!server.start()) {
        return 1;
    }

    server.run();

    return 0;
}
