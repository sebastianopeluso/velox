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

#include <memory>
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/exec/Driver.h"
#include "velox/exec/MemoryReclaimer.h"
#include "velox/exec/Task.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

namespace facebook::velox::exec::test {

constexpr int64_t KB = 1024L;
constexpr int64_t MB = 1024L * KB;

constexpr uint64_t kMemoryCapacity = 512 * MB;
constexpr uint64_t kMemoryPoolInitCapacity = 16 * MB;

class FakeMemoryReclaimer : public exec::MemoryReclaimer {
 public:
  FakeMemoryReclaimer() : exec::MemoryReclaimer(0) {}

  static std::unique_ptr<MemoryReclaimer> create() {
    return std::make_unique<FakeMemoryReclaimer>();
  }

  void enterArbitration() override {
    auto* driverThreadCtx = driverThreadContext();
    if (driverThreadCtx == nullptr) {
      return;
    }
    auto* driver = driverThreadCtx->driverCtx()->driver;
    ASSERT_TRUE(driver != nullptr);
    if (driver->task()->enterSuspended(driver->state()) != StopReason::kNone) {
      VELOX_FAIL("Terminate detected when entering suspension");
    }
  }

  void leaveArbitration() noexcept override {
    auto* driverThreadCtx = driverThreadContext();
    if (driverThreadCtx == nullptr) {
      return;
    }
    auto* driver = driverThreadCtx->driverCtx()->driver;
    ASSERT_TRUE(driver != nullptr);
    driver->task()->leaveSuspended(driver->state());
  }
};

struct TestAllocation {
  memory::MemoryPool* pool{nullptr};
  void* buffer{nullptr};
  size_t size{0};

  size_t free() {
    const size_t freedBytes = size;
    if (pool == nullptr) {
      VELOX_CHECK_EQ(freedBytes, 0);
      return freedBytes;
    }
    VELOX_CHECK_GT(freedBytes, 0);
    pool->free(buffer, freedBytes);
    pool = nullptr;
    buffer = nullptr;
    size = 0;
    return freedBytes;
  }
};

/// Begins and ends a section where a thread is running but not counted in its
/// Task. Using this, a Driver thread can for example stop its own Task. For
/// arbitrating memory overbooking, the contending threads go suspended and each
/// in turn enters a global critical section. When running the arbitration
/// strategy, a thread can stop and restart Tasks, including its own. When a
/// Task is stopped, its drivers are blocked or suspended and the strategy
/// thread can alter the Task's memory including spilling or killing the whole
/// Task. Other threads waiting to run the arbitration, are in a suspended state
/// which also means that they are instantaneously killable or spillable.
class TestSuspendedSection {
 public:
  explicit TestSuspendedSection(Driver* driver);
  ~TestSuspendedSection();

 private:
  Driver* driver_;
};

std::shared_ptr<core::QueryCtx> newQueryCtx(
    facebook::velox::memory::MemoryManager* memoryManager,
    folly::Executor* executor,
    int64_t memoryCapacity = facebook::velox::memory::kMaxMemory,
    const std::string& queryId = "");

std::unique_ptr<memory::MemoryManager> createMemoryManager(
    int64_t arbitratorCapacity = kMemoryCapacity,
    uint64_t memoryPoolInitCapacity = kMemoryPoolInitCapacity,
    uint64_t maxReclaimWaitMs = 5 * 60 * 1'000,
    uint64_t fastExponentialGrowthCapacityLimit = 0,
    double slowCapacityGrowPct = 0);

// Contains the query result.
struct QueryTestResult {
  std::shared_ptr<Task> task;
  RowVectorPtr data;
  core::PlanNodeId planNodeId;
};

core::PlanNodePtr hashJoinPlan(
    const std::vector<RowVectorPtr>& vectors,
    core::PlanNodeId& joinNodeId);

QueryTestResult runHashJoinTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    bool enableSpilling,
    const RowVectorPtr& expectedResult = nullptr);

core::PlanNodePtr aggregationPlan(
    const std::vector<RowVectorPtr>& vectors,
    core::PlanNodeId& aggregateNodeId);

QueryTestResult runAggregateTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    bool enableSpilling,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    const RowVectorPtr& expectedResult = nullptr);

core::PlanNodePtr orderByPlan(
    const std::vector<RowVectorPtr>& vectors,
    core::PlanNodeId& orderNodeId);

QueryTestResult runOrderByTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    bool enableSpilling,
    const RowVectorPtr& expectedResult = nullptr);

core::PlanNodePtr rowNumberPlan(
    const std::vector<RowVectorPtr>& vectors,
    core::PlanNodeId& rowNumberNodeId);

QueryTestResult runRowNumberTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    bool enableSpilling,
    const RowVectorPtr& expectedResult = nullptr);

core::PlanNodePtr topNPlan(
    const std::vector<RowVectorPtr>& vectors,
    core::PlanNodeId& topNodeId);

QueryTestResult runTopNTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    bool enableSpilling,
    const RowVectorPtr& expectedResult = nullptr);

core::PlanNodePtr writePlan(
    const std::vector<RowVectorPtr>& vectors,
    const std::string& outputDirPath,
    core::PlanNodeId& writeNodeId);

QueryTestResult runWriteTask(
    const std::vector<RowVectorPtr>& vectors,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool serialExecution,
    uint32_t numDrivers,
    memory::MemoryPool* pool,
    const std::string& kHiveConnectorId,
    bool enableSpilling,
    const RowVectorPtr& expectedResult = nullptr);
} // namespace facebook::velox::exec::test
