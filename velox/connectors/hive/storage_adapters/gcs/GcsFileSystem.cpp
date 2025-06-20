/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/connectors/hive/storage_adapters/gcs/GcsFileSystem.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/config/Config.h"
#include "velox/common/file/File.h"
#include "velox/connectors/hive/HiveConfig.h"
#include "velox/connectors/hive/storage_adapters/gcs/GcsUtil.h"
#include "velox/core/QueryConfig.h"

#include <fmt/format.h>
#include <glog/logging.h>
#include <memory>
#include <stdexcept>

#include <google/cloud/storage/client.h>

namespace facebook::velox {
namespace {
namespace gcs = ::google::cloud::storage;
namespace gc = ::google::cloud;
// Reference: https://github.com/apache/arrow/issues/29916
// Change the default upload buffer size. In general, sending larger buffers is
// more efficient with GCS, as each buffer requires a roundtrip to the service.
// With formatted output (when using `operator<<`), keeping a larger buffer in
// memory before uploading makes sense.  With unformatted output (the only
// choice given gcs::io::OutputStream's API) it is better to let the caller
// provide as large a buffer as they want. The GCS C++ client library will
// upload this buffer with zero copies if possible.
auto constexpr kUploadBufferSize = 256 * 1024;

inline void checkGcsStatus(
    const gc::Status outcome,
    const std::string_view& errorMsgPrefix,
    const std::string& bucket,
    const std::string& key) {
  if (!outcome.ok()) {
    const auto errMsg = fmt::format(
        "{} due to: Path:'{}', SDK Error Type:{}, GCS Status Code:{},  Message:'{}'",
        errorMsgPrefix,
        gcsURI(bucket, key),
        outcome.error_info().domain(),
        getErrorStringFromGcsError(outcome.code()),
        outcome.message());
    if (outcome.code() == gc::StatusCode::kNotFound) {
      VELOX_FILE_NOT_FOUND_ERROR(errMsg);
    }
    VELOX_FAIL(errMsg);
  }
}

class GcsReadFile final : public ReadFile {
 public:
  GcsReadFile(const std::string& path, std::shared_ptr<gcs::Client> client)
      : client_(std::move(client)) {
    // assumption it's a proper path
    setBucketAndKeyFromGcsPath(path, bucket_, key_);
  }

  // Gets the length of the file.
  // Checks if there are any issues reading the file.
  void initialize(const filesystems::FileOptions& options) {
    if (options.fileSize.has_value()) {
      VELOX_CHECK_GE(
          options.fileSize.value(), 0, "File size must be non-negative");
      length_ = options.fileSize.value();
    }

    // Make it a no-op if invoked twice.
    if (length_ != -1) {
      return;
    }
    // get metadata and initialize length
    auto metadata = client_->GetObjectMetadata(bucket_, key_);
    if (!metadata.ok()) {
      checkGcsStatus(
          metadata.status(),
          "Failed to get metadata for GCS object",
          bucket_,
          key_);
    }
    length_ = (*metadata).size();
    VELOX_CHECK_GE(length_, 0);
  }

  std::string_view pread(
      uint64_t offset,
      uint64_t length,
      void* buffer,
      filesystems::File::IoStats* stats = nullptr) const override {
    preadInternal(offset, length, static_cast<char*>(buffer));
    return {static_cast<char*>(buffer), length};
  }

  std::string pread(
      uint64_t offset,
      uint64_t length,
      filesystems::File::IoStats* stats = nullptr) const override {
    std::string result(length, 0);
    char* position = result.data();
    preadInternal(offset, length, position);
    return result;
  }

  uint64_t preadv(
      uint64_t offset,
      const std::vector<folly::Range<char*>>& buffers,
      filesystems::File::IoStats* stats = nullptr) const override {
    // 'buffers' contains Ranges(data, size)  with some gaps (data = nullptr) in
    // between. This call must populate the ranges (except gap ranges)
    // sequentially starting from 'offset'. If a range pointer is nullptr, the
    // data from stream of size range.size() will be skipped.
    size_t length = 0;
    for (const auto range : buffers) {
      length += range.size();
    }
    std::string result(length, 0);
    preadInternal(offset, length, static_cast<char*>(result.data()));
    size_t resultOffset = 0;
    for (auto range : buffers) {
      if (range.data()) {
        memcpy(range.data(), &(result.data()[resultOffset]), range.size());
      }
      resultOffset += range.size();
    }
    return length;
  }

  uint64_t size() const override {
    return length_;
  }

  uint64_t memoryUsage() const override {
    return sizeof(GcsReadFile) // this class
        + sizeof(gcs::Client) // pointee
        + kUploadBufferSize; // buffer size
  }

  bool shouldCoalesce() const final {
    return false;
  }

  std::string getName() const override {
    return key_;
  }

  uint64_t getNaturalReadSize() const override {
    return kUploadBufferSize;
  }

 private:
  // The assumption here is that "position" has space for at least "length"
  // bytes.
  void preadInternal(uint64_t offset, uint64_t length, char* position) const {
    gcs::ObjectReadStream stream = client_->ReadObject(
        bucket_, key_, gcs::ReadRange(offset, offset + length));
    if (!stream) {
      checkGcsStatus(
          stream.status(), "Failed to get GCS object", bucket_, key_);
    }

    stream.read(position, length);
    if (!stream) {
      checkGcsStatus(
          stream.status(), "Failed to get read object", bucket_, key_);
    }
    bytesRead_ += length;
  }

  std::shared_ptr<gcs::Client> client_;
  std::string bucket_;
  std::string key_;
  std::atomic<int64_t> length_ = -1;
};

class GcsWriteFile final : public WriteFile {
 public:
  explicit GcsWriteFile(
      const std::string& path,
      std::shared_ptr<gcs::Client> client)
      : client_(client) {
    setBucketAndKeyFromGcsPath(path, bucket_, key_);
  }

  ~GcsWriteFile() {
    close();
  }

  void initialize() {
    // Make it a no-op if invoked twice.
    if (size_ != -1) {
      return;
    }

    // Check that it doesn't exist, if it does throw an error
    auto object_metadata = client_->GetObjectMetadata(bucket_, key_);
    VELOX_CHECK(!object_metadata.ok(), "File already exists");

    auto stream = client_->WriteObject(bucket_, key_);
    checkGcsStatus(
        stream.last_status(),
        "Failed to open GCS object for writing",
        bucket_,
        key_);
    stream_ = std::move(stream);
    size_ = 0;
  }

  void append(const std::string_view data) override {
    VELOX_CHECK(isFileOpen(), "File is not open");
    stream_ << data;
    size_ += data.size();
  }

  void flush() override {
    if (isFileOpen()) {
      stream_.flush();
    }
  }

  void close() override {
    if (isFileOpen()) {
      stream_.flush();
      stream_.Close();
      closed_ = true;
    }
  }

  uint64_t size() const override {
    return size_;
  }

 private:
  inline bool isFileOpen() {
    return (!closed_ && stream_.IsOpen());
  }

  gcs::ObjectWriteStream stream_;
  std::shared_ptr<gcs::Client> client_;
  std::string bucket_;
  std::string key_;
  std::atomic<int64_t> size_{-1};
  std::atomic<bool> closed_{false};
};
} // namespace

namespace filesystems {
using namespace connector::hive;

auto constexpr kGcsInvalidPath = "File {} is not a valid gcs file";

class GcsFileSystem::Impl {
 public:
  Impl(const config::ConfigBase* config)
      : hiveConfig_(std::make_shared<HiveConfig>(
            std::make_shared<config::ConfigBase>(config->rawConfigsCopy()))) {}

  ~Impl() = default;

  // Use the input Config parameters and initialize the GcsClient.
  void initializeClient() {
    constexpr std::string_view kHttpsScheme{"https://"};
    auto options = gc::Options{};
    auto endpointOverride = hiveConfig_->gcsEndpoint();
    // Use secure credentials by default.
    if (!endpointOverride.empty()) {
      options.set<gcs::RestEndpointOption>(endpointOverride);
      // Use Google default credentials if endpoint has https scheme.
      if (endpointOverride.find(kHttpsScheme) == 0) {
        options.set<gc::UnifiedCredentialsOption>(
            gc::MakeGoogleDefaultCredentials());
      } else {
        options.set<gc::UnifiedCredentialsOption>(
            gc::MakeInsecureCredentials());
      }
    } else {
      options.set<gc::UnifiedCredentialsOption>(
          gc::MakeGoogleDefaultCredentials());
    }
    options.set<gcs::UploadBufferSizeOption>(kUploadBufferSize);

    auto max_retry_count = hiveConfig_->gcsMaxRetryCount();
    if (max_retry_count) {
      options.set<gcs::RetryPolicyOption>(
          gcs::LimitedErrorCountRetryPolicy(max_retry_count.value()).clone());
    }

    auto max_retry_time = hiveConfig_->gcsMaxRetryTime();
    if (max_retry_time) {
      auto retry_time = std::chrono::duration_cast<std::chrono::milliseconds>(
          facebook::velox::config::toDuration(max_retry_time.value()));
      options.set<gcs::RetryPolicyOption>(
          gcs::LimitedTimeRetryPolicy(retry_time).clone());
    }

    auto credFile = hiveConfig_->gcsCredentialsPath();
    if (!credFile.empty() && std::filesystem::exists(credFile)) {
      std::ifstream jsonFile(credFile, std::ios::in);
      if (!jsonFile.is_open()) {
        LOG(WARNING) << "Error opening file " << credFile;
      } else {
        std::stringstream credsBuffer;
        credsBuffer << jsonFile.rdbuf();
        auto creds = credsBuffer.str();
        auto credentials = gc::MakeServiceAccountCredentials(std::move(creds));
        options.set<gc::UnifiedCredentialsOption>(credentials);
      }
    } else {
      LOG(WARNING)
          << "Config hive.gcs.json-key-file-path is empty or key file path not found";
    }

    client_ = std::make_shared<gcs::Client>(options);
  }

  std::shared_ptr<gcs::Client> getClient() const {
    return client_;
  }

 private:
  const std::shared_ptr<HiveConfig> hiveConfig_;
  std::shared_ptr<gcs::Client> client_;
};

GcsFileSystem::GcsFileSystem(std::shared_ptr<const config::ConfigBase> config)
    : FileSystem(config) {
  impl_ = std::make_shared<Impl>(config.get());
}

void GcsFileSystem::initializeClient() {
  impl_->initializeClient();
}

std::unique_ptr<ReadFile> GcsFileSystem::openFileForRead(
    std::string_view path,
    const FileOptions& options) {
  const auto gcspath = gcsPath(path);
  auto gcsfile = std::make_unique<GcsReadFile>(gcspath, impl_->getClient());
  gcsfile->initialize(options);
  return gcsfile;
}

std::unique_ptr<WriteFile> GcsFileSystem::openFileForWrite(
    std::string_view path,
    const FileOptions& /*unused*/) {
  const auto gcspath = gcsPath(path);
  auto gcsfile = std::make_unique<GcsWriteFile>(gcspath, impl_->getClient());
  gcsfile->initialize();
  return gcsfile;
}

void GcsFileSystem::remove(std::string_view path) {
  if (!isGcsFile(path)) {
    VELOX_FAIL(kGcsInvalidPath, path);
  }

  // We assume 'path' is well-formed here.
  std::string bucket;
  std::string object;
  const auto file = gcsPath(path);
  setBucketAndKeyFromGcsPath(file, bucket, object);

  if (!object.empty()) {
    auto stat = impl_->getClient()->GetObjectMetadata(bucket, object);
    if (!stat.ok()) {
      checkGcsStatus(
          stat.status(),
          "Failed to get metadata for GCS object",
          bucket,
          object);
    }
  }
  auto ret = impl_->getClient()->DeleteObject(bucket, object);
  if (!ret.ok()) {
    checkGcsStatus(
        ret, "Failed to get metadata for GCS object", bucket, object);
  }
}

bool GcsFileSystem::exists(std::string_view path) {
  std::vector<std::string> result;
  if (!isGcsFile(path))
    VELOX_FAIL(kGcsInvalidPath, path);

  // We assume 'path' is well-formed here.
  const auto file = gcsPath(path);
  std::string bucket;
  std::string object;
  setBucketAndKeyFromGcsPath(file, bucket, object);
  using ::google::cloud::StatusOr;
  StatusOr<gcs::BucketMetadata> metadata =
      impl_->getClient()->GetBucketMetadata(bucket);

  return metadata.ok();
}

std::vector<std::string> GcsFileSystem::list(std::string_view path) {
  std::vector<std::string> result;
  if (!isGcsFile(path))
    VELOX_FAIL(kGcsInvalidPath, path);

  // We assume 'path' is well-formed here.
  const auto file = gcsPath(path);
  std::string bucket;
  std::string object;
  setBucketAndKeyFromGcsPath(file, bucket, object);
  for (auto&& metadata : impl_->getClient()->ListObjects(bucket)) {
    if (!metadata.ok()) {
      checkGcsStatus(
          metadata.status(),
          "Failed to get metadata for GCS object",
          bucket,
          object);
    }
    result.push_back(metadata->name());
  }

  return result;
}

std::string GcsFileSystem::name() const {
  return "GCS";
}

void GcsFileSystem::rename(
    std::string_view originPath,
    std::string_view newPath,
    bool overwrite) {
  if (!isGcsFile(originPath)) {
    VELOX_FAIL(kGcsInvalidPath, originPath);
  }

  if (!isGcsFile(newPath)) {
    VELOX_FAIL(kGcsInvalidPath, newPath);
  }

  std::string originBucket;
  std::string originObject;
  const auto originFile = gcsPath(originPath);
  setBucketAndKeyFromGcsPath(originFile, originBucket, originObject);

  std::string newBucket;
  std::string newObject;
  const auto newFile = gcsPath(newPath);
  setBucketAndKeyFromGcsPath(newFile, newBucket, newObject);

  if (!overwrite) {
    auto objects = list(newPath);
    if (std::find(objects.begin(), objects.end(), newObject) != objects.end()) {
      VELOX_USER_FAIL(
          "Failed to rename object {} to {} with as {} exists.",
          originObject,
          newObject,
          newObject);
      return;
    }
  }

  // Copy the object to the new name.
  auto copyStats = impl_->getClient()->CopyObject(
      originBucket, originObject, newBucket, newObject);
  if (!copyStats.ok()) {
    checkGcsStatus(
        copyStats.status(),
        fmt::format(
            "Failed to rename for GCS object {}/{}",
            originBucket,
            originObject),
        originBucket,
        originObject);
  }

  // Delete the original object.
  auto delStatus = impl_->getClient()->DeleteObject(originBucket, originObject);
  if (!delStatus.ok()) {
    checkGcsStatus(
        delStatus,
        fmt::format(
            "Failed to delete for GCS object {}/{} after copy when renaming. And the copied object is at {}/{}",
            originBucket,
            originObject,
            newBucket,
            newObject),
        originBucket,
        originObject);
  }
}

void GcsFileSystem::mkdir(
    std::string_view path,
    const DirectoryOptions& options) {
  if (!isGcsFile(path)) {
    VELOX_FAIL(kGcsInvalidPath, path);
  }

  std::string bucket;
  std::string object;
  const auto file = gcsPath(path);
  setBucketAndKeyFromGcsPath(file, bucket, object);

  // Create an empty object to represent the directory.
  auto status = impl_->getClient()->InsertObject(bucket, object, "");

  checkGcsStatus(
      status.status(),
      fmt::format("Failed to mkdir for GCS object {}/{}", bucket, object),
      bucket,
      object);
}

void GcsFileSystem::rmdir(std::string_view path) {
  if (!isGcsFile(path)) {
    VELOX_FAIL(kGcsInvalidPath, path);
  }

  const auto file = gcsPath(path);
  std::string bucket;
  std::string object;
  setBucketAndKeyFromGcsPath(file, bucket, object);
  for (auto&& metadata : impl_->getClient()->ListObjects(bucket)) {
    checkGcsStatus(
        metadata.status(),
        fmt::format("Failed to rmdir for GCS object {}/{}", bucket, object),
        bucket,
        object);

    auto status = impl_->getClient()->DeleteObject(bucket, metadata->name());
    checkGcsStatus(
        metadata.status(),
        fmt::format(
            "Failed to delete for GCS object {}/{} when rmdir.",
            bucket,
            metadata->name()),
        bucket,
        metadata->name());
  }
}

} // namespace filesystems
} // namespace facebook::velox
