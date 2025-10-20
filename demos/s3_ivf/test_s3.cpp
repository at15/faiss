#include <aws/core/Aws.h>
#include <aws/s3-crt/S3CrtClient.h>
#include <aws/s3-crt/S3CrtClientConfiguration.h>
#include <aws/s3-crt/model/GetObjectRequest.h>
#include <aws/s3-crt/model/PutObjectRequest.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

/**
 * Create S3 CRT client with optional custom endpoint for local testing.
 */
std::shared_ptr<Aws::S3Crt::S3CrtClient> CreateS3Client() {
    Aws::S3Crt::ClientConfiguration config;

    // Check for custom endpoint (for local s3mock testing)
    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint != nullptr) {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP; // Use HTTP for local mock
        config.verifySSL = false;

        std::cout << "Using custom endpoint: " << endpoint << std::endl;
    }

    return Aws::MakeShared<Aws::S3Crt::S3CrtClient>("S3CrtClient", config);
}

/**
 * Put an object to an S3 bucket.
 */
bool PutObject(
        const Aws::String& bucketName,
        const Aws::String& objectKey,
        const Aws::String& content) {
    auto s3_crt_client = CreateS3Client();

    Aws::S3Crt::Model::PutObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);

    // Create a stringstream to hold the content
    auto data = Aws::MakeShared<Aws::StringStream>(
            "PutObjectInputStream",
            std::stringstream::in | std::stringstream::out |
                    std::stringstream::binary);
    *data << content;

    request.SetBody(data);

    Aws::S3Crt::Model::PutObjectOutcome outcome =
            s3_crt_client->PutObject(request);

    if (outcome.IsSuccess()) {
        std::cout << "Successfully put object '" << objectKey << "' to bucket '"
                  << bucketName << "'" << std::endl;
        return true;
    } else {
        std::cerr << "Error: PutObject: " << outcome.GetError().GetMessage()
                  << std::endl;
        return false;
    }
}

/**
 * Get an object from an S3 bucket.
 */
bool GetObject(const Aws::String& bucketName, const Aws::String& objectKey) {
    auto s3_crt_client = CreateS3Client();

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);

    Aws::S3Crt::Model::GetObjectOutcome outcome =
            s3_crt_client->GetObject(request);

    if (outcome.IsSuccess()) {
        std::cout << "Successfully retrieved object '" << objectKey
                  << "' from bucket '" << bucketName << "'" << std::endl;

        // Read the content
        auto& retrievedFile = outcome.GetResultWithOwnership().GetBody();
        std::stringstream ss;
        ss << retrievedFile.rdbuf();

        std::cout << "Content:" << std::endl;
        std::cout << ss.str() << std::endl;

        return true;
    } else {
        std::cerr << "Error: GetObject: " << outcome.GetError().GetMessage()
                  << std::endl;
        return false;
    }
}

bool GetObjectRange(
        const Aws::String& bucketName,
        const Aws::String& objectKey,
        size_t offset,
        size_t size) {
    auto s3_crt_client = CreateS3Client();

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);
    // Build Range header: "bytes=<start>-<end>"
    Aws::String rangeHeader =
            "bytes=" + Aws::Utils::StringUtils::to_string(offset) + "-" +
            Aws::Utils::StringUtils::to_string(offset + size - 1);
    request.SetRange(rangeHeader);

    Aws::S3Crt::Model::GetObjectOutcome outcome =
            s3_crt_client->GetObject(request);

    if (outcome.IsSuccess()) {
        std::cout << "Successfully retrieved object '" << objectKey
                  << "' from bucket '" << bucketName << "'" << std::endl;

        // Read the content
        auto& retrievedFile = outcome.GetResultWithOwnership().GetBody();
        std::stringstream ss;
        ss << retrievedFile.rdbuf();

        std::cout << "Content:" << std::endl;
        std::cout << ss.str() << std::endl;

        return true;
    } else {
        std::cerr << "Error: GetObjectRange: "
                  << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "Usage: " << argv[0]
                  << " <bucket_name> <object_key> <operation> [content]"
                  << std::endl;
        std::cout << "  operation: put or get" << std::endl;
        std::cout << "  content: required for 'put' operation" << std::endl;
        std::cout << std::endl;
        std::cout << "Environment variables:" << std::endl;
        std::cout
                << "  S3_ENDPOINT_URL - Custom S3 endpoint (e.g., http://localhost:9000 for s3mock)"
                << std::endl;
        std::cout
                << "  AWS_EC2_METADATA_DISABLED - Set to 'true' for local testing"
                << std::endl;
        std::cout << std::endl;
        std::cout << "Example (put): " << argv[0]
                  << " my-bucket test.txt put \"Hello, S3!\"" << std::endl;
        std::cout << "Example (get): " << argv[0] << " my-bucket test.txt get"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "Example (local s3mock):" << std::endl;
        std::cout
                << "  S3_ENDPOINT_URL=http://localhost:9000 AWS_EC2_METADATA_DISABLED=true \\"
                << std::endl;
        std::cout << "  " << argv[0] << " test-bucket test.txt put \"Hello!\""
                  << std::endl;
        return 1;
    }

    Aws::String bucketName = argv[1];
    Aws::String objectKey = argv[2];
    std::string operation = argv[3];

    Aws::SDKOptions options;
    Aws::InitAPI(options);
    {
        bool success = false;

        if (operation == "put") {
            if (argc < 5) {
                std::cerr << "Error: Content required for put operation"
                          << std::endl;
                Aws::ShutdownAPI(options);
                return 1;
            }
            Aws::String content = argv[4];
            success = PutObject(bucketName, objectKey, content);
        } else if (operation == "get") {
            // success = GetObject(bucketName, objectKey);
            success = GetObjectRange(bucketName, objectKey, 3, 10);
        } else {
            std::cerr << "Error: Invalid operation '" << operation
                      << "'. Use 'put' or 'get'" << std::endl;
            Aws::ShutdownAPI(options);
            return 1;
        }

        if (!success) {
            Aws::ShutdownAPI(options);
            return 1;
        }
    }
    Aws::ShutdownAPI(options);

    return 0;
}