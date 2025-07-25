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
#pragma once

#include "velox/connectors/hive/HiveConnectorSplit.h"
#include "velox/connectors/hive/HiveDataSink.h"
#include "velox/connectors/hive/TableHandle.h"
#include "velox/dwio/dwrf/common/Config.h"
#include "velox/dwio/dwrf/writer/FlushPolicy.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/TempFilePath.h"

namespace facebook::velox::exec::test {

static const std::string kHiveConnectorId = "test-hive";

class HiveConnectorTestBase : public OperatorTestBase {
 public:
  HiveConnectorTestBase();

  void SetUp() override;
  void TearDown() override;

  void resetHiveConnector(
      const std::shared_ptr<const config::ConfigBase>& config);

  void writeToFiles(
      const std::vector<std::string>& filePaths,
      std::vector<RowVectorPtr> vectors);

  void writeToFile(const std::string& filePath, RowVectorPtr vector);

  void writeToFile(
      const std::string& filePath,
      const std::vector<RowVectorPtr>& vectors,
      std::shared_ptr<dwrf::Config> config =
          std::make_shared<facebook::velox::dwrf::Config>(),
      const std::function<std::unique_ptr<dwrf::DWRFFlushPolicy>()>&
          flushPolicyFactory = nullptr);

  void writeToFile(
      const std::string& filePath,
      const std::vector<RowVectorPtr>& vectors,
      std::shared_ptr<dwrf::Config> config,
      const TypePtr& schema,
      const std::function<std::unique_ptr<dwrf::DWRFFlushPolicy>()>&
          flushPolicyFactory = nullptr);

  // Creates a directory using matching file system based on directoryPath.
  // No throw when directory already exists.
  void createDirectory(const std::string& directoryPath);

  // Removes a directory using matching file system based on directoryPath.
  // No op when directory does not exist.
  void removeDirectory(const std::string& directoryPath);

  // Removes a file using matching file system based on filePath.
  // No op when file does not exist.
  void removeFile(const std::string& filePath);

  std::vector<RowVectorPtr> makeVectors(
      const RowTypePtr& rowType,
      int32_t numVectors,
      int32_t rowsPerVector);

  using OperatorTestBase::assertQuery;

  /// Assumes plan has a single TableScan node.
  std::shared_ptr<exec::Task> assertQuery(
      const core::PlanNodePtr& plan,
      const std::vector<std::shared_ptr<TempFilePath>>& filePaths,
      const std::string& duckDbSql);

  std::shared_ptr<Task> assertQuery(
      const core::PlanNodePtr& plan,
      const std::vector<std::shared_ptr<connector::ConnectorSplit>>& splits,
      const std::string& duckDbSql,
      const int32_t numPrefetchSplit);

  static std::vector<std::shared_ptr<TempFilePath>> makeFilePaths(int count);

  static std::vector<std::shared_ptr<connector::ConnectorSplit>>
  makeHiveConnectorSplits(
      const std::vector<std::shared_ptr<TempFilePath>>& filePaths);

  static std::shared_ptr<connector::hive::HiveConnectorSplit>
  makeHiveConnectorSplit(
      const std::string& filePath,
      uint64_t start = 0,
      uint64_t length = std::numeric_limits<uint64_t>::max(),
      int64_t splitWeight = 0,
      bool cacheable = true);

  static std::shared_ptr<connector::hive::HiveConnectorSplit>
  makeHiveConnectorSplit(
      const std::string& filePath,
      int64_t fileSize,
      int64_t fileModifiedTime,
      uint64_t start,
      uint64_t length);

  /// Split file at path 'filePath' into 'splitCount' splits. If not local file,
  /// file size can be given as 'externalSize'.
  static std::vector<std::shared_ptr<connector::hive::HiveConnectorSplit>>
  makeHiveConnectorSplits(
      const std::string& filePath,
      uint32_t splitCount,
      dwio::common::FileFormat format,
      const std::optional<
          std::unordered_map<std::string, std::optional<std::string>>>&
          partitionKeys = {},
      const std::optional<std::unordered_map<std::string, std::string>>&
          infoColumns = {});

  static std::shared_ptr<connector::hive::HiveTableHandle> makeTableHandle(
      common::SubfieldFilters subfieldFilters = {},
      const core::TypedExprPtr& remainingFilter = nullptr,
      const std::string& tableName = "hive_table",
      const RowTypePtr& dataColumns = nullptr,
      bool filterPushdownEnabled = true,
      const std::unordered_map<std::string, std::string>& tableParameters =
          {}) {
    return std::make_shared<connector::hive::HiveTableHandle>(
        kHiveConnectorId,
        tableName,
        filterPushdownEnabled,
        std::move(subfieldFilters),
        remainingFilter,
        dataColumns,
        tableParameters);
  }

  /// @param name Column name.
  /// @param type Column type.
  /// @param Required subfields of this column.
  static std::unique_ptr<connector::hive::HiveColumnHandle> makeColumnHandle(
      const std::string& name,
      const TypePtr& type,
      const std::vector<std::string>& requiredSubfields);

  /// @param name Column name.
  /// @param type Column type.
  /// @param type Hive type.
  /// @param Required subfields of this column.
  static std::unique_ptr<connector::hive::HiveColumnHandle> makeColumnHandle(
      const std::string& name,
      const TypePtr& dataType,
      const TypePtr& hiveType,
      const std::vector<std::string>& requiredSubfields,
      connector::hive::HiveColumnHandle::ColumnType columnType =
          connector::hive::HiveColumnHandle::ColumnType::kRegular);

  /// @param targetDirectory Final directory of the target table after commit.
  /// @param writeDirectory Write directory of the target table before commit.
  /// @param tableType Whether to create a new table, insert into an existing
  /// table, or write a temporary table.
  /// @param writeMode How to write to the target directory.
  static std::shared_ptr<connector::hive::LocationHandle> makeLocationHandle(
      std::string targetDirectory,
      std::optional<std::string> writeDirectory = std::nullopt,
      connector::hive::LocationHandle::TableType tableType =
          connector::hive::LocationHandle::TableType::kNew) {
    return std::make_shared<connector::hive::LocationHandle>(
        targetDirectory, writeDirectory.value_or(targetDirectory), tableType);
  }

  /// Build a HiveInsertTableHandle.
  /// @param tableColumnNames Column names of the target table. Corresponding
  /// type of tableColumnNames[i] is tableColumnTypes[i].
  /// @param tableColumnTypes Column types of the target table. Corresponding
  /// name of tableColumnTypes[i] is tableColumnNames[i].
  /// @param partitionedBy A list of partition columns of the target table.
  /// @param bucketProperty if not nulll, specifies the property for a bucket
  /// table.
  /// @param locationHandle Location handle for the table write.
  /// @param compressionKind compression algorithm to use for table write.
  /// @param serdeParameters Table writer configuration parameters.
  /// @param ensureFiles When this option is set the HiveDataSink will always
  /// create a file even if there is no data.
  static std::shared_ptr<connector::hive::HiveInsertTableHandle>
  makeHiveInsertTableHandle(
      const std::vector<std::string>& tableColumnNames,
      const std::vector<TypePtr>& tableColumnTypes,
      const std::vector<std::string>& partitionedBy,
      std::shared_ptr<connector::hive::HiveBucketProperty> bucketProperty,
      std::shared_ptr<connector::hive::LocationHandle> locationHandle,
      const dwio::common::FileFormat tableStorageFormat =
          dwio::common::FileFormat::DWRF,
      const std::optional<common::CompressionKind> compressionKind = {},
      const std::unordered_map<std::string, std::string>& serdeParameters = {},
      const std::shared_ptr<dwio::common::WriterOptions>& writerOptions =
          nullptr,
      const bool ensureFiles = false);

  static std::shared_ptr<connector::hive::HiveInsertTableHandle>
  makeHiveInsertTableHandle(
      const std::vector<std::string>& tableColumnNames,
      const std::vector<TypePtr>& tableColumnTypes,
      const std::vector<std::string>& partitionedBy,
      std::shared_ptr<connector::hive::LocationHandle> locationHandle,
      const dwio::common::FileFormat tableStorageFormat =
          dwio::common::FileFormat::DWRF,
      const std::optional<common::CompressionKind> compressionKind = {},
      const std::shared_ptr<dwio::common::WriterOptions>& writerOptions =
          nullptr,
      const bool ensureFiles = false);

  static std::shared_ptr<connector::hive::HiveColumnHandle> regularColumn(
      const std::string& name,
      const TypePtr& type);

  static std::shared_ptr<connector::hive::HiveColumnHandle> partitionKey(
      const std::string& name,
      const TypePtr& type);

  static std::shared_ptr<connector::hive::HiveColumnHandle> synthesizedColumn(
      const std::string& name,
      const TypePtr& type);

  static connector::ColumnHandleMap allRegularColumns(
      const RowTypePtr& rowType) {
    connector::ColumnHandleMap assignments;
    assignments.reserve(rowType->size());
    for (uint32_t i = 0; i < rowType->size(); ++i) {
      const auto& name = rowType->nameOf(i);
      assignments[name] = regularColumn(name, rowType->childAt(i));
    }
    return assignments;
  }
};

/// Same as connector::hive::HiveConnectorBuilder, except that this defaults
/// connectorId to kHiveConnectorId.
class HiveConnectorSplitBuilder
    : public connector::hive::HiveConnectorSplitBuilder {
 public:
  explicit HiveConnectorSplitBuilder(std::string filePath)
      : connector::hive::HiveConnectorSplitBuilder(std::move(filePath)) {
    connectorId(kHiveConnectorId);
  }
};

} // namespace facebook::velox::exec::test
