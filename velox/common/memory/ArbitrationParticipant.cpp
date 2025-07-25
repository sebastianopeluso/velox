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

#include "velox/common/memory/ArbitrationParticipant.h"
#include <mutex>

#include "velox/common/base/Exceptions.h"
#include "velox/common/base/RuntimeMetrics.h"
#include "velox/common/memory/ArbitrationOperation.h"
#include "velox/common/memory/Memory.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/common/time/Timer.h"

using facebook::velox::common::testutil::TestValue;

namespace facebook::velox::memory {
using namespace facebook::velox::memory;

std::string ArbitrationParticipant::Config::toString() const {
  return fmt::format(
      "initCapacity {}, minCapacity {}, fastExponentialGrowthCapacityLimit {}, slowCapacityGrowRatio {}, minFreeCapacity {}, minFreeCapacityRatio {}, minReclaimBytes {}, minReclaimPct {}",
      succinctBytes(initCapacity),
      succinctBytes(minCapacity),
      succinctBytes(fastExponentialGrowthCapacityLimit),
      slowCapacityGrowRatio,
      succinctBytes(minFreeCapacity),
      minFreeCapacityRatio,
      succinctBytes(minReclaimBytes),
      minReclaimPct);
}

ArbitrationParticipant::Config::Config(
    uint64_t _initCapacity,
    uint64_t _minCapacity,
    uint64_t _fastExponentialGrowthCapacityLimit,
    double _slowCapacityGrowRatio,
    uint64_t _minFreeCapacity,
    double _minFreeCapacityRatio,
    uint64_t _minReclaimBytes,
    double _minReclaimPct)
    : initCapacity(_initCapacity),
      minCapacity(_minCapacity),
      fastExponentialGrowthCapacityLimit(_fastExponentialGrowthCapacityLimit),
      slowCapacityGrowRatio(_slowCapacityGrowRatio),
      minFreeCapacity(_minFreeCapacity),
      minFreeCapacityRatio(_minFreeCapacityRatio),
      minReclaimBytes(_minReclaimBytes),
      minReclaimPct(_minReclaimPct) {
  VELOX_CHECK_GE(slowCapacityGrowRatio, 0);
  VELOX_CHECK_EQ(
      fastExponentialGrowthCapacityLimit == 0,
      slowCapacityGrowRatio == 0,
      "fastExponentialGrowthCapacityLimit {} and slowCapacityGrowRatio {} "
      "both need to be set (non-zero) at the same time to enable growth capacity "
      "adjustment.",
      fastExponentialGrowthCapacityLimit,
      slowCapacityGrowRatio);

  VELOX_CHECK_GE(minFreeCapacityRatio, 0);
  VELOX_CHECK_LE(minFreeCapacityRatio, 1);
  VELOX_CHECK_EQ(
      minFreeCapacity == 0,
      minFreeCapacityRatio == 0,
      "minFreeCapacity {} and minFreeCapacityRatio {} both "
      "need to be set (non-zero) at the same time to enable shrink capacity "
      "adjustment.",
      minFreeCapacity,
      minFreeCapacityRatio);
  VELOX_CHECK(
      0 <= minReclaimPct && minReclaimPct <= 1,
      "minReclaimPct {} must be in [0, 1]",
      minReclaimPct);
}

std::shared_ptr<ArbitrationParticipant> ArbitrationParticipant::create(
    uint64_t id,
    const std::shared_ptr<MemoryPool>& pool,
    const Config* config) {
  return std::shared_ptr<ArbitrationParticipant>(
      new ArbitrationParticipant(id, pool, config));
}

ArbitrationParticipant::ArbitrationParticipant(
    uint64_t id,
    const std::shared_ptr<MemoryPool>& pool,
    const Config* config)
    : id_(id),
      poolWeakPtr_(pool),
      pool_(pool.get()),
      config_(config),
      maxCapacity_(pool_->maxCapacity()),
      createTimeNs_(getCurrentTimeNano()) {
  VELOX_CHECK_LE(
      config_->minCapacity,
      maxCapacity_,
      "The min capacity is larger than the max capacity for memory pool {}.",
      pool_->name());
}

ArbitrationParticipant::~ArbitrationParticipant() {
  VELOX_CHECK_NULL(runningOp_);
  VELOX_CHECK(waitOps_.empty());
}

std::optional<ScopedArbitrationParticipant> ArbitrationParticipant::lock() {
  auto sharedPtr = poolWeakPtr_.lock();
  if (sharedPtr == nullptr) {
    return {};
  }
  return ScopedArbitrationParticipant(shared_from_this(), std::move(sharedPtr));
}

uint64_t ArbitrationParticipant::maxGrowCapacity() const {
  const auto capacity = pool_->capacity();
  VELOX_CHECK_LE(capacity, maxCapacity_);
  return maxCapacity_ - capacity;
}

uint64_t ArbitrationParticipant::minGrowCapacity() const {
  const auto capacity = pool_->capacity();
  if (capacity >= config_->minCapacity) {
    return 0;
  }
  return config_->minCapacity - capacity;
}

bool ArbitrationParticipant::inactivePool() const {
  // Checks if a query memory pool is actively used by query execution or not.
  // If not, then we don't have to respect the memory pool min limit or reserved
  // capacity check.
  //
  // NOTE: for query system like Prestissimo, it holds a finished query
  // state in minutes for query stats fetch request from the Presto
  // coordinator.
  return pool_->reservedBytes() == 0 && pool_->peakBytes() != 0;
}

uint64_t ArbitrationParticipant::reclaimableFreeCapacity() const {
  return std::min(maxShrinkCapacity(), maxReclaimableCapacity());
}

uint64_t ArbitrationParticipant::maxReclaimableCapacity() const {
  if (inactivePool()) {
    return pool_->capacity();
  }
  const uint64_t capacityBytes = pool_->capacity();
  if (capacityBytes < config_->minCapacity) {
    return 0;
  }
  return capacityBytes - config_->minCapacity;
}

uint64_t ArbitrationParticipant::reclaimableUsedCapacity() const {
  const auto maxReclaimableBytes = maxReclaimableCapacity();
  const auto reclaimableBytes = pool_->reclaimableBytes();
  return std::min<int64_t>(maxReclaimableBytes, reclaimableBytes.value_or(0));
}

uint64_t ArbitrationParticipant::maxShrinkCapacity() const {
  const uint64_t capacity = pool_->capacity();
  const uint64_t freeBytes = pool_->freeBytes();
  if (config_->minFreeCapacity != 0 && !inactivePool()) {
    const uint64_t minFreeBytes = std::min(
        static_cast<uint64_t>(capacity * config_->minFreeCapacityRatio),
        config_->minFreeCapacity);
    if (freeBytes <= minFreeBytes) {
      return 0;
    } else {
      return freeBytes - minFreeBytes;
    }
  } else {
    return freeBytes;
  }
}

bool ArbitrationParticipant::checkCapacityGrowth(uint64_t requestBytes) const {
  return maxGrowCapacity() >= requestBytes;
}

void ArbitrationParticipant::getGrowTargets(
    uint64_t requestBytes,
    uint64_t& maxGrowBytes,
    uint64_t& minGrowBytes) const {
  const uint64_t capacity = pool_->capacity();
  if (config_->fastExponentialGrowthCapacityLimit == 0 &&
      config_->slowCapacityGrowRatio == 0) {
    maxGrowBytes = requestBytes;
  } else {
    if (capacity * 2 <= config_->fastExponentialGrowthCapacityLimit) {
      maxGrowBytes = capacity;
    } else {
      maxGrowBytes = capacity * config_->slowCapacityGrowRatio;
    }
  }
  maxGrowBytes = std::max(requestBytes, maxGrowBytes);
  minGrowBytes = minGrowCapacity();
  maxGrowBytes = std::max(maxGrowBytes, minGrowBytes);
  maxGrowBytes = std::min(maxGrowCapacity(), maxGrowBytes);

  VELOX_CHECK_LE(minGrowBytes, maxGrowBytes);
  VELOX_CHECK_LE(requestBytes, maxGrowBytes);
}

void ArbitrationParticipant::startArbitration(ArbitrationOperation* op) {
  ContinueFuture waitPromise{ContinueFuture::makeEmpty()};
  {
    std::lock_guard<std::mutex> l(stateLock_);
    ++numRequests_;
    if (runningOp_ != nullptr) {
      op->setState(ArbitrationOperation::State::kWaiting);
      WaitOp waitOp{
          op,
          ContinuePromise{fmt::format(
              "Wait for arbitration on {}", op->participant()->name())}};
      waitPromise = waitOp.waitPromise.getSemiFuture();
      waitOps_.emplace_back(std::move(waitOp));
    } else {
      runningOp_ = op;
    }
  }

  TestValue::adjust(
      "facebook::velox::memory::ArbitrationParticipant::startArbitration",
      this);

  if (waitPromise.valid()) {
    waitPromise.wait();
  }
}

void ArbitrationParticipant::finishArbitration(ArbitrationOperation* op) {
  ContinuePromise resumePromise{ContinuePromise::makeEmpty()};
  {
    std::lock_guard<std::mutex> l(stateLock_);
    VELOX_CHECK_EQ(static_cast<void*>(op), static_cast<void*>(runningOp_));
    if (!waitOps_.empty()) {
      resumePromise = std::move(waitOps_.front().waitPromise);
      runningOp_ = waitOps_.front().op;
      waitOps_.pop_front();
    } else {
      runningOp_ = nullptr;
    }
  }
  if (resumePromise.valid()) {
    resumePromise.setValue();
  }
}

void ArbitrationParticipant::setPendingArbitrationGrowCapacity(
    int64_t growCapacity) {
  VELOX_CHECK_EQ(globalArbitrationGrowCapacity_, 0);
  globalArbitrationGrowCapacity_ = growCapacity;
}

void ArbitrationParticipant::clearGlobalArbitrationGrowCapacity() {
  VELOX_CHECK_NE(globalArbitrationGrowCapacity_, 0);
  globalArbitrationGrowCapacity_ = 0;
}

int64_t ArbitrationParticipant::globalArbitrationGrowCapacity() const {
  return globalArbitrationGrowCapacity_;
}

uint64_t ArbitrationParticipant::reclaim(
    uint64_t targetBytes,
    uint64_t maxWaitTimeNs,
    MemoryReclaimer::Stats& stats) noexcept {
  const auto minReclaimBytes = std::max(
      config_->minReclaimBytes,
      static_cast<uint64_t>(capacity() * config_->minReclaimPct));
  targetBytes = std::max(targetBytes, minReclaimBytes);
  if (targetBytes == 0) {
    return 0;
  }
  uint64_t reclaimedCapacity{0};
  try {
    ArbitrationTimedLock l(reclaimMutex_, maxWaitTimeNs);
    TestValue::adjust(
        "facebook::velox::memory::ArbitrationParticipant::reclaim", this);
    ++numReclaims_;
    VELOX_MEM_LOG(INFO) << "Reclaiming from memory pool " << pool_->name()
                        << " with target " << succinctBytes(targetBytes);
    auto reclaimedBytes =
        pool_->reclaim(targetBytes, maxWaitTimeNs / 1'000'000, stats);
    reclaimedCapacity = shrink(/*reclaimAll=*/false);
    VELOX_MEM_LOG(INFO) << "Reclaimed from memory pool " << pool_->name()
                        << " reserved memory " << succinctBytes(reclaimedBytes)
                        << ", capacity " << succinctBytes(reclaimedCapacity);
  } catch (const std::exception& e) {
    VELOX_MEM_LOG(ERROR) << "Failed to reclaim from memory pool "
                         << pool_->name() << ", aborting it: " << e.what();
    reclaimedCapacity = abortLocked(std::current_exception());
  }
  return reclaimedCapacity;
}

bool ArbitrationParticipant::grow(
    uint64_t growBytes,
    uint64_t reservationBytes) {
  std::lock_guard<std::mutex> l(stateLock_);
  ++numGrows_;
  const bool success = pool_->grow(growBytes, reservationBytes);
  if (success) {
    growBytes_ += growBytes;
  }
  return success;
}

uint64_t ArbitrationParticipant::shrink(bool reclaimAll) {
  std::lock_guard<std::mutex> l(stateLock_);
  return shrinkLocked(reclaimAll);
}

uint64_t ArbitrationParticipant::shrinkLocked(bool reclaimAll) {
  ++numShrinks_;

  uint64_t reclaimedBytes{0};
  if (reclaimAll) {
    reclaimedBytes = pool_->shrink(0);
  } else {
    const uint64_t reclaimTargetBytes = reclaimableFreeCapacity();
    if (reclaimTargetBytes > 0) {
      reclaimedBytes = pool_->shrink(reclaimTargetBytes);
    }
  }
  reclaimedBytes_ += reclaimedBytes;
  return reclaimedBytes;
}

uint64_t ArbitrationParticipant::abort(
    const std::exception_ptr& error) noexcept {
  std::lock_guard<std::timed_mutex> l(reclaimMutex_);
  return abortLocked(error);
}

uint64_t ArbitrationParticipant::abortLocked(
    const std::exception_ptr& error) noexcept {
  TestValue::adjust(
      "facebook::velox::memory::ArbitrationParticipant::abortLocked", this);
  {
    std::lock_guard<std::mutex> l(stateLock_);
    if (aborted_) {
      return 0;
    }
    aborted_ = true;
  }

  try {
    VELOX_MEM_LOG(WARNING) << "Memory pool " << pool_->name()
                           << " is being aborted";
    pool_->abort(error);
  } catch (const std::exception& e) {
    VELOX_MEM_LOG(WARNING) << "Failed to abort memory pool "
                           << pool_->toString() << ", error: " << e.what();
  }
  VELOX_MEM_LOG(WARNING) << "Memory pool " << pool_->name() << " aborted";
  // NOTE: no matter query memory pool abort throws or not, it should have been
  // marked as aborted to prevent any new memory arbitration operations.
  VELOX_CHECK(pool_->aborted());

  std::lock_guard<std::mutex> l(stateLock_);
  return shrinkLocked(/*reclaimAll=*/true);
}

bool ArbitrationParticipant::hasRunningOp() const {
  std::lock_guard<std::mutex> l(stateLock_);
  return runningOp_ != nullptr;
}

size_t ArbitrationParticipant::numWaitingOps() const {
  std::lock_guard<std::mutex> l(stateLock_);
  return waitOps_.size();
}

std::string ArbitrationParticipant::Stats::toString() const {
  return fmt::format(
      "numRequests: {}, numReclaims: {}, numShrinks: {}, numGrows: {}, reclaimedBytes: {}, growBytes: {}, aborted: {}, duration: {}",
      numRequests,
      numReclaims,
      numShrinks,
      numGrows,
      succinctBytes(reclaimedBytes),
      succinctBytes(growBytes),
      aborted,
      succinctNanos(durationNs));
}

ScopedArbitrationParticipant::ScopedArbitrationParticipant(
    std::shared_ptr<ArbitrationParticipant> ArbitrationParticipant,
    std::shared_ptr<MemoryPool> pool)
    : ArbitrationParticipant_(std::move(ArbitrationParticipant)),
      pool_(std::move(pool)) {
  VELOX_CHECK_NOT_NULL(ArbitrationParticipant_);
  VELOX_CHECK_NOT_NULL(pool_);
}

ArbitrationCandidate::ArbitrationCandidate(
    ScopedArbitrationParticipant&& _participant,
    bool freeCapacityOnly)
    : participant(std::move(_participant)),
      currentCapacity(participant->capacity()),
      reclaimableUsedCapacity(
          freeCapacityOnly ? 0 : participant->reclaimableUsedCapacity()),
      reclaimableFreeCapacity(participant->reclaimableFreeCapacity()) {}

std::string ArbitrationCandidate::toString() const {
  return fmt::format(
      "{} RECLAIMABLE_USED_CAPACITY {} RECLAIMABLE_FREE_CAPACITY {}",
      participant->name(),
      succinctBytes(reclaimableUsedCapacity),
      succinctBytes(reclaimableFreeCapacity));
}

#ifdef TSAN_BUILD
ArbitrationTimedLock::ArbitrationTimedLock(
    std::timed_mutex& mutex,
    uint64_t /* unused */)
    : mutex_(mutex) {
  mutex_.lock();
}

ArbitrationTimedLock::~ArbitrationTimedLock() {
  mutex_.unlock();
}
#else
ArbitrationTimedLock::ArbitrationTimedLock(
    std::timed_mutex& mutex,
    uint64_t timeoutNs)
    : mutex_(mutex) {
  if (!mutex_.try_lock_for(std::chrono::nanoseconds(timeoutNs))) {
    VELOX_MEM_ARBITRATION_TIMEOUT(fmt::format(
        "Memory arbitration lock timed out when reclaiming from arbitration participant."));
  }
}

ArbitrationTimedLock::~ArbitrationTimedLock() {
  mutex_.unlock();
}
#endif
} // namespace facebook::velox::memory
