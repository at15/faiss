#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Global state for the server
struct IndexState {
    int id;
    std::string bucket;
    std::string key;
    int64_t cluster_data_offset;
    int64_t cache_hits = 0;
    int64_t cache_misses = 0;
};

class ServerState {
public:
    std::atomic<int> next_index_id{1};
    std::map<int, IndexState> loaded_indexes;
    std::mutex indexes_mutex;
    std::atomic<int64_t> global_cache_hits{0};
    std::atomic<int64_t> global_cache_misses{0};

    int add_index(const std::string& bucket, const std::string& key, int64_t offset) {
        std::lock_guard<std::mutex> lock(indexes_mutex);
        int id = next_index_id.fetch_add(1);
        IndexState state;
        state.id = id;
        state.bucket = bucket;
        state.key = key;
        state.cluster_data_offset = offset;
        state.cache_hits = 0;
        state.cache_misses = 0;
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
};

// Protocol utilities
class ProtocolParser {
public:
    static std::map<std::string, std::string> parse_command(const std::string& line) {
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

    static std::string format_response(const std::map<std::string, std::string>& params) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) oss << " ";
            oss << key << "=" << value;
            first = false;
        }
        oss << "\n";
        return oss.str();
    }

    static std::string format_error(const std::string& code, const std::string& msg) {
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
            ssize_t n = recv(socket, buf + total_read, length - total_read, 0);
            if (n <= 0) {
                return false;  // Connection closed or error
            }
            total_read += n;
        }

        return true;
    }

    static bool write_exact(int socket, const void* buffer, size_t length) {
        size_t total_written = 0;
        const uint8_t* buf = static_cast<const uint8_t*>(buffer);

        while (total_written < length) {
            ssize_t n = send(socket, buf + total_written, length - total_written, 0);
            if (n <= 0) {
                return false;  // Connection closed or error
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

    static bool write_binary_array(int socket, const void* data, uint32_t length) {
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
        std::cout << "[Client " << client_socket << "] Connected" << std::endl;

        while (running) {
            // Read command line
            std::string line;
            if (!read_line(line)) {
                std::cout << "[Client " << client_socket << "] Disconnected" << std::endl;
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
            std::cout << "[Client " << client_socket << "] Command: " << command << std::endl;

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
                return false;  // Connection closed or error
            }
            if (ch == '\n') {
                break;
            }
            if (line.size() >= 8192) {  // Max line length
                return false;
            }
            line += ch;
        }
        return true;
    }

    bool send_response(const std::string& response) {
        return BinaryIO::write_exact(client_socket, response.c_str(), response.size());
    }

    bool send_error(const std::string& code, const std::string& msg) {
        std::string error = ProtocolParser::format_error(code, msg);
        std::cout << "[Client " << client_socket << "] Error: " << code << " - " << msg << std::endl;
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
        // Check required parameters
        auto bucket_it = params.find("bucket");
        auto key_it = params.find("key");
        auto offset_it = params.find("cluster_data_offset");

        if (bucket_it == params.end() || key_it == params.end() || offset_it == params.end()) {
            send_error("MISSING_PARAM", "Missing required parameters (bucket, key, cluster_data_offset)");
            return;
        }

        // Parse offset
        int64_t offset;
        try {
            offset = std::stoll(offset_it->second);
        } catch (...) {
            send_error("INVALID_PARAM", "Invalid cluster_data_offset");
            return;
        }

        // Add index and assign ID
        int index_id = server_state->add_index(bucket_it->second, key_it->second, offset);

        std::cout << "[Client " << client_socket << "] Loaded index " << index_id
                  << " (bucket=" << bucket_it->second << ", key=" << key_it->second << ")" << std::endl;

        // Send response
        std::map<std::string, std::string> response;
        response["index"] = std::to_string(index_id);
        send_response(ProtocolParser::format_response(response));
    }

    void handle_search(const std::map<std::string, std::string>& params) {
        // Check required parameters
        auto index_it = params.find("index");
        auto k_it = params.find("k");
        auto d_it = params.find("d");

        if (index_it == params.end() || k_it == params.end() || d_it == params.end()) {
            send_error("MISSING_PARAM", "Missing required parameters (index, k, d)");
            return;
        }

        // Parse parameters
        int index_id, k, d;
        try {
            index_id = std::stoi(index_it->second);
            k = std::stoi(k_it->second);
            d = std::stoi(d_it->second);
        } catch (...) {
            send_error("INVALID_PARAM", "Invalid parameter values");
            return;
        }

        // Read query vector (binary) - must read before validating to consume data from socket
        std::vector<uint8_t> query_data;
        if (!BinaryIO::read_binary_array(client_socket, query_data)) {
            send_error("INVALID_BINARY", "Failed to read query vector");
            return;
        }

        // Validate query vector size
        size_t expected_size = d * sizeof(float);
        if (query_data.size() != expected_size) {
            send_error("INVALID_BINARY", "Expected " + std::to_string(expected_size) +
                      " bytes, got " + std::to_string(query_data.size()));
            return;
        }

        // Verify index exists
        IndexState* index_state;
        if (!server_state->get_index(index_id, &index_state)) {
            send_error("INDEX_NOT_FOUND", "Index " + std::to_string(index_id) + " not loaded");
            return;
        }

        std::cout << "[Client " << client_socket << "] Search index=" << index_id
                  << " k=" << k << " d=" << d << std::endl;

        // DUMMY IMPLEMENTATION: Return hardcoded results
        // In real implementation, this would call Faiss search

        // Create dummy result IDs (42, 43, 44, ...)
        std::vector<int64_t> result_ids(k);
        for (int i = 0; i < k; i++) {
            result_ids[i] = 42 + i;
        }

        // Create dummy distances (0.5, 0.7, 0.9, ...)
        std::vector<float> result_distances(k);
        for (int i = 0; i < k; i++) {
            result_distances[i] = 0.5f + i * 0.2f;
        }

        // Send text response
        std::map<std::string, std::string> response;
        response["k"] = std::to_string(k);
        if (!send_response(ProtocolParser::format_response(response))) {
            return;
        }

        // Send binary data: IDs
        if (!BinaryIO::write_binary_array(client_socket, result_ids.data(), k * sizeof(int64_t))) {
            std::cout << "[Client " << client_socket << "] Failed to send result IDs" << std::endl;
            return;
        }

        // Send binary data: distances
        if (!BinaryIO::write_binary_array(client_socket, result_distances.data(), k * sizeof(float))) {
            std::cout << "[Client " << client_socket << "] Failed to send distances" << std::endl;
            return;
        }

        std::cout << "[Client " << client_socket << "] Search completed, returned " << k << " results" << std::endl;
    }

    void handle_info(const std::map<std::string, std::string>& params) {
        auto about_it = params.find("about");
        if (about_it == params.end()) {
            send_error("MISSING_PARAM", "Missing about parameter");
            return;
        }

        std::map<std::string, std::string> response;

        if (about_it->second == "cache") {
            // Global cache statistics (dummy)
            response["index_count"] = std::to_string(server_state->get_index_count());
            response["cache_hits"] = std::to_string(server_state->global_cache_hits.load());
            response["cache_misses"] = std::to_string(server_state->global_cache_misses.load());
            send_response(ProtocolParser::format_response(response));

        } else if (about_it->second == "index") {
            // Per-index statistics
            auto id_it = params.find("id");
            if (id_it == params.end()) {
                send_error("MISSING_PARAM", "Missing id parameter for index query");
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
                send_error("INDEX_NOT_FOUND", "Index " + std::to_string(index_id) + " not loaded");
                return;
            }

            // DUMMY IMPLEMENTATION: Return hardcoded statistics
            response["cluster_count"] = "4096";  // Dummy value
            response["cache_hits"] = std::to_string(index_state->cache_hits);
            response["cache_misses"] = std::to_string(index_state->cache_misses);
            response["cached_clusters"] = "128";  // Dummy value
            send_response(ProtocolParser::format_response(response));

        } else {
            send_error("INVALID_PARAM", "Invalid about value: " + about_it->second);
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
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            close(server_socket);
            return false;
        }

        // Bind to port
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_socket, (struct sockaddr*)&address, sizeof(address)) < 0) {
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

            int client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_len);
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
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
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
