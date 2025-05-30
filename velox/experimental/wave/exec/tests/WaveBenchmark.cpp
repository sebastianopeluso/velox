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

#include "velox/benchmarks/QueryBenchmarkBase.h"
#include "velox/common/process/TraceContext.h"
#include "velox/dwio/dwrf/writer/Writer.h"
#include "velox/dwio/dwrf/writer/WriterContext.h"
#include "velox/dwio/parquet/writer/Writer.h"
#include "velox/experimental/wave/common/Cuda.h"
#include "velox/experimental/wave/exec/ToWave.h"
#include "velox/experimental/wave/exec/WaveHiveDataSource.h"
#include "velox/experimental/wave/exec/tests/utils/FileFormat.h"
#include "velox/experimental/wave/exec/tests/utils/WaveTestSplitReader.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;
using namespace facebook::velox::dwio::common;

DEFINE_string(
    data_path,
    "",
    "Root path of test data. Data layout must follow Hive-style partitioning. "
    "If the content are directories, they contain the data files for "
    "each table. If they are files, they contain a file system path for each "
    "data file, one per line. This allows running against cloud storage or "
    "HDFS");

DEFINE_bool(
    generate,
    true,
    "Generate input data. If false, data_path must "
    "contain a directory with a subdirectory per table.");

DEFINE_bool(dwrf_vints, true, "Use vints in DWRF test dataset");

DEFINE_int64(min_card, 1000, "Lowest cardinality of column");

DEFINE_int64(max_card, 100000, "Highest cardinality of column");

DEFINE_bool(preload, false, "Preload Wave data into RAM before starting query");

DEFINE_bool(wave, true, "Run benchmark with Wave");

DEFINE_int32(num_columns, 10, "Number of columns in test table");

DEFINE_int32(num_keys, 0, "Number of grouping keys");

DEFINE_int32(key_mod, 10000, "Modulo for grouping keys");

DEFINE_int64(filter_pass_pct, 100, "Passing % for one filter");

DEFINE_int32(null_pct, 0, "Pct of null values in columns");

DEFINE_int32(num_column_filters, 0, "Number of columns wit a filter");

DEFINE_int32(num_expr_filters, 0, "Number of columns  with a filter expr");

DEFINE_int32(
    num_arithmetic,
    0,
    "Number of arithmetic ops per column after filters");

DEFINE_int32(rows_per_stripe, 200000, "Rows in a stripe");

DEFINE_int64(num_rows, 1000000000, "Rows in test table");

DEFINE_int32(
    run_query_verbose,
    -1,
    "Run a given query and print execution statistics");

DECLARE_string(data_format);

struct ColumnSpec {
  int64_t mod{1000000000};
  int64_t base{0};
  int64_t roundUp{1};
  bool notNull{true};
};

void printPlan(core::PlanNode* node) {
  auto str = node->toString(true, true);
  printf("%s\n", str.c_str());
}

class WaveBenchmark : public QueryBenchmarkBase {
 public:
  ~WaveBenchmark() {
    wave::test::Table::dropAll();
  }

  void initialize() override {
    QueryBenchmarkBase::initialize();
    if (FLAGS_wave) {
      wave::registerWave();
      wave::WaveHiveDataSource::registerConnector();
      wave::test::WaveTestSplitReader::registerTestSplitReader();
    }
    rootPool_ = memory::memoryManager()->addRootPool("WaveBenchmark");
    leafPool_ = rootPool_->addLeafChild("WaveBenchmark");
  }

  void makeData(
      const RowTypePtr& type,
      int32_t numVectors,
      int32_t vectorSize,
      float nullPct = 0) {
    auto vectors = makeVectors(type, numVectors, vectorSize, nullPct / 100);
    int32_t cnt = 0;

    for (auto i = 0; i < vectors.size(); ++i) {
      auto& vector = vectors[i];
      makeRange(vector, specs_);
      auto rn = vector->childAt(type->size() - 1)->as<FlatVector<int64_t>>();
      for (auto i = 0; i < rn->size(); ++i) {
        rn->set(i, cnt++);
      }
    }
    if (FLAGS_wave) {
      makeTable(FLAGS_data_path + "/test.wave", vectors);
      if (FLAGS_generate) {
        auto table =
            wave::test::Table::getTable(FLAGS_data_path + "/test.wave");
        table->toFile(FLAGS_data_path + "/test.wave");
      }
    } else {
      std::string temp = FLAGS_data_path + "/data." + FLAGS_data_format;
      writeToFile(temp, vectors, vectors.front()->type());
    }
  }

  std::vector<RowVectorPtr> makeVectors(
      const RowTypePtr& rowType,
      int32_t numVectors,
      int32_t rowsPerVector,
      float nullRatio = 0) {
    std::vector<RowVectorPtr> vectors;
    options_.vectorSize = rowsPerVector;
    options_.nullRatio = nullRatio;
    fuzzer_ = std::make_unique<VectorFuzzer>(options_, leafPool_.get());
    for (int32_t i = 0; i < numVectors; ++i) {
      auto vector = fuzzer_->fuzzInputFlatRow(rowType);
      vectors.push_back(vector);
    }
    return vectors;
  }

  void makeRange(RowVectorPtr row, const std::vector<ColumnSpec> specs) {
    for (auto i = 0; i < row->type()->size(); ++i) {
      auto& spec = specs[i];
      auto child = row->childAt(i);
      if (auto ints = child->as<FlatVector<int64_t>>()) {
        for (auto i = 0; i < child->size(); ++i) {
          if (!spec.notNull && ints->isNullAt(i)) {
            continue;
          }
          ints->set(
              i,
              spec.base +
                  bits::roundUp(ints->valueAt(i) % spec.mod, spec.roundUp));
        }
      }
      if (spec.notNull) {
        child->clearNulls(0, row->size());
      }
    }
  }

  wave::test::SplitVector makeTable(
      const std::string& name,
      std::vector<RowVectorPtr>& rows) {
    wave::test::Table::dropTable(name);
    return wave::test::Table::defineTable(name, rows)->splits();
  }

  void writeToFile(
      const std::string& filePath,
      const std::vector<RowVectorPtr>& vectors,
      const TypePtr& schema) {
    auto localWriteFile =
        std::make_unique<LocalWriteFile>(filePath, true, false);
    auto sink = std::make_unique<dwio::common::WriteFileSink>(
        std::move(localWriteFile), filePath);
    auto childPool =
        rootPool_->addAggregateChild("HiveConnectorTestBase.Writer");
    if (FLAGS_data_format == "dwrf") {
      auto config = std::make_shared<dwrf::Config>();
      config->set(dwrf::Config::COMPRESSION, common::CompressionKind_NONE);
      config->set(
          dwrf::Config::STRIPE_SIZE,
          static_cast<uint64_t>(FLAGS_rows_per_stripe * FLAGS_num_columns * 8));
      config->set(dwrf::Config::USE_VINTS, FLAGS_dwrf_vints);

      dwrf::WriterOptions options;
      options.config = config;
      options.schema = schema;

      options.memoryPool = childPool.get();
      facebook::velox::dwrf::Writer writer{std::move(sink), options};
      for (size_t i = 0; i < vectors.size(); ++i) {
        writer.write(vectors[i]);
      }
      writer.close();
    } else if (FLAGS_data_format == "parquet") {
      facebook::velox::parquet::WriterOptions options;
      options.memoryPool = childPool.get();
      int32_t flushCounter = 0;
      options.encoding = parquet::arrow::Encoding::type::BIT_PACKED;
      options.flushPolicyFactory = [&]() {
        return std::make_unique<facebook::velox::parquet::LambdaFlushPolicy>(
            1000000, 1000000000, [&]() { return (++flushCounter % 1 == 0); });
      };
      options.compressionKind = common::CompressionKind_NONE;
      auto writer = std::make_unique<facebook::velox::parquet::Writer>(
          std::move(sink), options, asRowType(schema));
      for (auto& batch : vectors) {
        writer->write(batch);
      }
      writer->flush();
      writer->close();

    } else {
      VELOX_FAIL("Bad file format {}", FLAGS_data_format);
    }
  }

  exec::test::TpchPlan getQueryPlan(int32_t query) {
    switch (query) {
      case 1: {
        if (!type_) {
          type_ = makeType();
        }

        exec::test::TpchPlan plan;
        if (FLAGS_wave) {
          plan.dataFiles["0"] = {FLAGS_data_path + "/test.wave"};
          plan.dataFileFormat = FileFormat::UNKNOWN;
        } else {
          plan.dataFiles["0"] = {
              FLAGS_data_path + "/data." + FLAGS_data_format};
          plan.dataFileFormat = toFileFormat(FLAGS_data_format);
        }
        float passRatio = FLAGS_filter_pass_pct / 100.0;
        std::vector<std::string> scanFilters;
        for (auto i = 0; i < FLAGS_num_column_filters; ++i) {
          scanFilters.push_back(fmt::format(
              "c{} < {}", i, static_cast<int64_t>(specs_[i].mod * passRatio)));
        }
        auto builder =
            PlanBuilder(leafPool_.get()).tableScan(type_, scanFilters);

        for (auto i = FLAGS_num_column_filters;
             i < FLAGS_num_column_filters + FLAGS_num_expr_filters;
             ++i) {
          builder = builder.filter(fmt::format(
              "c{} + 1 < {}",
              i,
              static_cast<int64_t>(specs_[i].mod * passRatio)));
        }

        std::vector<std::string> aggInputs;
        std::vector<std::string> keyProjections;
        std::vector<std::string> keys;
        for (auto i = 0; i < type_->size(); ++i) {
          if (i < FLAGS_num_keys) {
            keyProjections.push_back(fmt::format(
                "(c{} / {}) % {} as c{}",
                i,
                specs_[i].roundUp,
                FLAGS_key_mod,
                i));
            keys.push_back(fmt::format("c{}", i));
          } else {
            keyProjections.push_back(fmt::format("c{}", i));
          }
        }
        if (!keys.empty()) {
          builder.project(keyProjections);
        }

        if (FLAGS_num_arithmetic > 0) {
          std::vector<std::string> projects;
          for (auto c = 0; c < type_->size(); ++c) {
            std::string expr = fmt::format("c{} ", c);
            for (auto i = 0; i < FLAGS_num_arithmetic; ++i) {
              expr += fmt::format(" + c{}", c);
            }
            expr += fmt::format(" as f{}", c);
            projects.push_back(std::move(expr));
            aggInputs.push_back(fmt::format("f{}", c));
          }
          builder = builder.project(std::move(projects));
        } else {
          for (auto i = 0; i < type_->size(); ++i) {
            aggInputs.push_back(fmt::format("c{}", i));
          }
        }

        std::vector<std::string> aggs;
        for (auto i = FLAGS_num_keys; i < aggInputs.size(); ++i) {
          aggs.push_back(fmt::format("sum({})", aggInputs[i]));
        }

        if (!keys.empty() && !FLAGS_wave) {
          builder.localPartition(keys);
        }

        auto aggsAndCount = aggs;
        aggsAndCount.push_back("sum(1)");
        builder.singleAggregation(keys, aggsAndCount);

        if (!keys.empty()) {
          if (!FLAGS_wave) {
            builder.localPartition({});
          }
          auto aggType = builder.planNode()->outputType();
          auto sumCounts =
              fmt::format("sum({})", aggType->nameOf(aggType->size() - 1));
          builder.singleAggregation({}, {"sum(1)", sumCounts});
        }
        plan.plan = builder.planNode();
        return plan;
      }
      default:
        VELOX_FAIL("Bad query number");
    }
  }

  void prepareQuery(int32_t query) {
    switch (query) {
      case 1: {
        type_ = makeType();
        auto range = FLAGS_max_card - FLAGS_min_card;
        for (auto i = 0; i < type_->size(); ++i) {
          ColumnSpec spec;
          spec.mod = FLAGS_min_card + 10000 * (i + 1) * (range / type_->size());
          spec.roundUp = 10000;
          specs_.push_back(spec);
        }
        auto numVectors =
            std::max<int64_t>(1, FLAGS_num_rows / FLAGS_rows_per_stripe);
        if (FLAGS_generate) {
          makeData(
              type_, numVectors, FLAGS_num_rows / numVectors, FLAGS_null_pct);
        } else {
          loadData();
        }
        break;
      }
      default:
        VELOX_FAIL("Bad query number");
    }
  }

  void loadData() {
    if (FLAGS_wave) {
      auto table =
          wave::test::Table::getTable(FLAGS_data_path + "/test.wave", true);
      table->fromFile(FLAGS_data_path + "/test.wave");
      if (FLAGS_preload) {
        table->loadData(leafPool_);
      }
    }
  }

  std::vector<std::shared_ptr<connector::ConnectorSplit>> listSplits(
      const std::string& path,
      int32_t numSplitsPerFile,
      const TpchPlan& plan) override {
    if (plan.dataFileFormat == FileFormat::UNKNOWN) {
      auto table = wave::test::Table::getTable(path);
      return table->splits();
    }
    return QueryBenchmarkBase::listSplits(path, numSplitsPerFile, plan);
  }

  void runMain(std::ostream& out, RunStats& runStats) override {
    if (FLAGS_run_query_verbose == -1) {
      folly::runBenchmarks();
    } else {
      const auto queryPlan = getQueryPlan(FLAGS_run_query_verbose);
      auto [cursor, actualResults] = run(queryPlan);
      if (!cursor) {
        LOG(ERROR) << "Query terminated with error. Exiting";
        exit(1);
      }
      auto task = cursor->task();
      ensureTaskCompletion(task.get());
      if (FLAGS_include_results) {
        printResults(actualResults, out);
        out << std::endl;
      }
      const auto stats = task->taskStats();
      int64_t rawInputBytes = 0;
      for (auto& pipeline : stats.pipelineStats) {
        auto& first = pipeline.operatorStats[0];
        if (first.operatorType == "TableScan" || first.operatorType == "Wave") {
          rawInputBytes += first.rawInputBytes;
        }
      }
      runStats.rawInputBytes = rawInputBytes;
      out << fmt::format(
                 "Execution time: {}",
                 succinctMillis(
                     stats.executionEndTimeMs - stats.executionStartTimeMs))
          << std::endl;
      out << fmt::format(
                 "Splits total: {}, finished: {}",
                 stats.numTotalSplits,
                 stats.numFinishedSplits)
          << std::endl;
      out << printPlanWithStats(
                 *queryPlan.plan, stats, FLAGS_include_custom_stats)
          << std::endl;
    }
  }

  RowTypePtr makeType() {
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (auto i = 0; i < FLAGS_num_columns; ++i) {
      names.push_back(fmt::format("c{}", i));
      types.push_back(BIGINT());
    }
    return ROW(std::move(names), std::move(types));
  }

  std::shared_ptr<memory::MemoryPool> rootPool_;
  std::shared_ptr<memory::MemoryPool> leafPool_;
  RowTypePtr type_;
  VectorFuzzer::Options options_;
  std::unique_ptr<VectorFuzzer> fuzzer_;
  std::vector<ColumnSpec> specs_;
};

void waveBenchmarkMain() {
  auto benchmark = std::make_unique<WaveBenchmark>();
  benchmark->initialize();
  if (FLAGS_run_query_verbose != -1) {
    benchmark->prepareQuery(FLAGS_run_query_verbose);
  }
  if (FLAGS_test_flags_file.empty()) {
    RunStats stats;
    benchmark->runOne(std::cout, stats);
    std::cout << stats.toString(false);
  } else {
    benchmark->runAllCombinations();
  }
  benchmark->shutdown();
}

int main(int argc, char** argv) {
  std::string kUsage(
      "This program benchmarks Wave. Run 'velox_wave_benchmark -helpon=WaveBenchmark' for available options.\n");
  gflags::SetUsageMessage(kUsage);
  folly::Init init{&argc, &argv, false};
  if (FLAGS_wave) {
    auto device = facebook::velox::wave::getDevice();
    std::cout << device->toString() << std::endl;
    facebook::velox::wave::printKernels();
    facebook::velox::wave::CompiledKernel::initialize();
  }
  waveBenchmarkMain();
  return 0;
}
