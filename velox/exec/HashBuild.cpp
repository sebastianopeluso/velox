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

#include "velox/exec/HashBuild.h"
#include "velox/common/base/Counters.h"
#include "velox/common/base/StatsReporter.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/exec/OperatorUtils.h"
#include "velox/exec/Task.h"
#include "velox/expression/FieldReference.h"

using facebook::velox::common::testutil::TestValue;

namespace facebook::velox::exec {
namespace {
// Map HashBuild 'state' to the corresponding driver blocking reason.
BlockingReason fromStateToBlockingReason(HashBuild::State state) {
  switch (state) {
    case HashBuild::State::kRunning:
      [[fallthrough]];
    case HashBuild::State::kFinish:
      return BlockingReason::kNotBlocked;
    case HashBuild::State::kYield:
      return BlockingReason::kYield;
    case HashBuild::State::kWaitForBuild:
      return BlockingReason::kWaitForJoinBuild;
    case HashBuild::State::kWaitForProbe:
      return BlockingReason::kWaitForJoinProbe;
    default:
      VELOX_UNREACHABLE(HashBuild::stateName(state));
  }
}
} // namespace

HashBuild::HashBuild(
    int32_t operatorId,
    DriverCtx* driverCtx,
    std::shared_ptr<const core::HashJoinNode> joinNode)
    : Operator(
          driverCtx,
          nullptr,
          operatorId,
          joinNode->id(),
          "HashBuild",
          joinNode->canSpill(driverCtx->queryConfig())
              ? driverCtx->makeSpillConfig(operatorId)
              : std::nullopt),
      joinNode_(std::move(joinNode)),
      joinType_{joinNode_->joinType()},
      nullAware_{joinNode_->isNullAware()},
      needProbedFlagSpill_{needRightSideJoin(joinType_)},
      joinBridge_(operatorCtx_->task()->getHashJoinBridgeLocked(
          operatorCtx_->driverCtx()->splitGroupId,
          planNodeId())),
      keyChannelMap_(joinNode_->rightKeys().size()) {
  VELOX_CHECK(pool()->trackUsage());
  VELOX_CHECK_NOT_NULL(joinBridge_);

  joinBridge_->addBuilder();

  const auto& inputType = joinNode_->sources()[1]->outputType();

  const auto numKeys = joinNode_->rightKeys().size();
  keyChannels_.reserve(numKeys);

  for (int i = 0; i < numKeys; ++i) {
    auto& key = joinNode_->rightKeys()[i];
    auto channel = exprToChannel(key.get(), inputType);
    keyChannelMap_[channel] = i;
    keyChannels_.emplace_back(channel);
  }

  // Identify the non-key build side columns and make a decoder for each.
  const int32_t numDependents = inputType->size() - numKeys;
  if (numDependents > 0) {
    // Number of join keys (numKeys) may be less then number of input columns
    // (inputType->size()). In this case numDependents is negative and cannot be
    // used to call 'reserve'. This happens when we join different probe side
    // keys with the same build side key: SELECT * FROM t LEFT JOIN u ON t.k1 =
    // u.k AND t.k2 = u.k.
    dependentChannels_.reserve(numDependents);
    decoders_.reserve(numDependents);
  }
  for (auto i = 0; i < inputType->size(); ++i) {
    if (keyChannelMap_.find(i) == keyChannelMap_.end()) {
      dependentChannels_.emplace_back(i);
      decoders_.emplace_back(std::make_unique<DecodedVector>());
    }
  }

  tableType_ = hashJoinTableType(joinNode_);
  setupTable();
  setupSpiller();
  stateCleared_ = false;
}

void HashBuild::initialize() {
  Operator::initialize();

  if (isAntiJoin(joinType_) && joinNode_->filter()) {
    setupFilterForAntiJoins(keyChannelMap_);
  }
}

void HashBuild::setupTable() {
  VELOX_CHECK_NULL(table_);

  const auto numKeys = keyChannels_.size();
  std::vector<std::unique_ptr<VectorHasher>> keyHashers;
  keyHashers.reserve(numKeys);
  for (vector_size_t i = 0; i < numKeys; ++i) {
    keyHashers.emplace_back(
        VectorHasher::create(tableType_->childAt(i), keyChannels_[i]));
  }

  const auto numDependents = tableType_->size() - numKeys;
  std::vector<TypePtr> dependentTypes;
  dependentTypes.reserve(numDependents);
  for (int i = numKeys; i < tableType_->size(); ++i) {
    dependentTypes.emplace_back(tableType_->childAt(i));
  }
  if (joinNode_->isRightJoin() || joinNode_->isFullJoin() ||
      joinNode_->isRightSemiProjectJoin()) {
    // Do not ignore null keys.
    table_ = HashTable<false>::createForJoin(
        std::move(keyHashers),
        dependentTypes,
        true, // allowDuplicates
        true, // hasProbedFlag
        operatorCtx_->driverCtx()
            ->queryConfig()
            .minTableRowsForParallelJoinBuild(),
        pool());
  } else {
    // (Left) semi and anti join with no extra filter only needs to know whether
    // there is a match. Hence, no need to store entries with duplicate keys.
    const bool dropDuplicates = !joinNode_->filter() &&
        (joinNode_->isLeftSemiFilterJoin() ||
         joinNode_->isLeftSemiProjectJoin() || isAntiJoin(joinType_));
    // Right semi join needs to tag build rows that were probed.
    const bool needProbedFlag = joinNode_->isRightSemiFilterJoin();
    if (isLeftNullAwareJoinWithFilter(joinNode_)) {
      // We need to check null key rows in build side in case of null-aware anti
      // or left semi project join with filter set.
      table_ = HashTable<false>::createForJoin(
          std::move(keyHashers),
          dependentTypes,
          !dropDuplicates, // allowDuplicates
          needProbedFlag, // hasProbedFlag
          operatorCtx_->driverCtx()
              ->queryConfig()
              .minTableRowsForParallelJoinBuild(),
          pool());
    } else {
      // Ignore null keys
      table_ = HashTable<true>::createForJoin(
          std::move(keyHashers),
          dependentTypes,
          !dropDuplicates, // allowDuplicates
          needProbedFlag, // hasProbedFlag
          operatorCtx_->driverCtx()
              ->queryConfig()
              .minTableRowsForParallelJoinBuild(),
          pool());
    }
  }
  analyzeKeys_ = table_->hashMode() != BaseHashTable::HashMode::kHash;
}

void HashBuild::setupSpiller(SpillPartition* spillPartition) {
  VELOX_CHECK_NULL(spiller_);
  VELOX_CHECK_NULL(spillInputReader_);

  if (!canSpill()) {
    return;
  }
  if (spillType_ == nullptr) {
    spillType_ = hashJoinTableSpillType(tableType_, joinType_);
    if (needProbedFlagSpill_) {
      spillProbedFlagChannel_ = spillType_->size() - 1;
      VELOX_CHECK_NULL(spillProbedFlagVector_);
      // Creates a constant probed flag vector with all values false for build
      // side table spilling.
      spillProbedFlagVector_ = std::make_shared<ConstantVector<bool>>(
          pool(), 0, /*isNull=*/false, BOOLEAN(), false);
    }
  }

  const auto* config = spillConfig();
  uint8_t startPartitionBit = config->startPartitionBit;
  if (spillPartition != nullptr) {
    spillInputReader_ = spillPartition->createUnorderedReader(
        config->readBufferSize, pool(), spillStats_.get());
    VELOX_CHECK(!restoringPartitionId_.has_value());
    restoringPartitionId_ = spillPartition->id();
    const auto numPartitionBits = config->numPartitionBits;
    startPartitionBit =
        partitionBitOffset(
            spillPartition->id(), startPartitionBit, numPartitionBits) +
        numPartitionBits;
    // Disable spilling if exceeding the max spill level and the query might run
    // out of memory if the restored partition still can't fit in memory.
    if (config->exceedSpillLevelLimit(startPartitionBit)) {
      RECORD_METRIC_VALUE(kMetricMaxSpillLevelExceededCount);
      LOG(WARNING) << "Exceeded spill level limit: " << config->maxSpillLevel
                   << ", and disable spilling for memory pool: "
                   << pool()->name();
      ++spillStats_->wlock()->spillMaxLevelExceededCount;
      exceededMaxSpillLevelLimit_ = true;
      return;
    }
    exceededMaxSpillLevelLimit_ = false;
  }

  spiller_ = std::make_unique<HashBuildSpiller>(
      joinType_,
      restoringPartitionId_,
      table_->rows(),
      spillType_,
      HashBitRange(
          startPartitionBit, startPartitionBit + config->numPartitionBits),
      config,
      spillStats_.get());

  const int32_t numPartitions = spiller_->hashBits().numPartitions();
  spillInputIndicesBuffers_.resize(numPartitions);
  rawSpillInputIndicesBuffers_.resize(numPartitions);
  numSpillInputs_.resize(numPartitions, 0);
  spillChildVectors_.resize(spillType_->size());
}

bool HashBuild::isInputFromSpill() const {
  return spillInputReader_ != nullptr;
}

RowTypePtr HashBuild::inputType() const {
  return isInputFromSpill() ? tableType_
                            : joinNode_->sources()[1]->outputType();
}

void HashBuild::setupFilterForAntiJoins(
    const folly::F14FastMap<column_index_t, column_index_t>& keyChannelMap) {
  VELOX_DCHECK(
      std::is_sorted(dependentChannels_.begin(), dependentChannels_.end()));

  ExprSet exprs({joinNode_->filter()}, operatorCtx_->execCtx());
  VELOX_DCHECK_EQ(exprs.exprs().size(), 1);
  const auto& expr = exprs.expr(0);
  filterPropagatesNulls_ = expr->propagatesNulls();
  if (filterPropagatesNulls_) {
    const auto inputType = joinNode_->sources()[1]->outputType();
    for (const auto& field : expr->distinctFields()) {
      const auto index = inputType->getChildIdxIfExists(field->field());
      if (!index.has_value()) {
        continue;
      }
      auto keyIter = keyChannelMap.find(*index);
      if (keyIter != keyChannelMap.end()) {
        keyFilterChannels_.push_back(keyIter->second);
      } else {
        auto dependentIter = std::lower_bound(
            dependentChannels_.begin(), dependentChannels_.end(), *index);
        VELOX_DCHECK(
            dependentIter != dependentChannels_.end() &&
            *dependentIter == *index);
        dependentFilterChannels_.push_back(
            dependentIter - dependentChannels_.begin());
      }
    }
  }
}

void HashBuild::removeInputRowsForAntiJoinFilter() {
  bool changed = false;
  auto* rawActiveRows = activeRows_.asMutableRange().bits();
  auto removeNulls = [&](DecodedVector& decoded) {
    if (decoded.mayHaveNulls()) {
      changed = true;
      // NOTE: the true value of a raw null bit indicates non-null so we AND
      // 'rawActiveRows' with the raw bit.
      bits::andBits(
          rawActiveRows, decoded.nulls(&activeRows_), 0, activeRows_.end());
    }
  };
  for (auto channel : keyFilterChannels_) {
    removeNulls(table_->hashers()[channel]->decodedVector());
  }
  for (auto channel : dependentFilterChannels_) {
    removeNulls(*decoders_[channel]);
  }
  if (changed) {
    activeRows_.updateBounds();
  }
}

void HashBuild::addInput(RowVectorPtr input) {
  checkRunning();
  ensureInputFits(input);

  TestValue::adjust("facebook::velox::exec::HashBuild::addInput", this);

  activeRows_.resize(input->size());
  activeRows_.setAll();

  auto& hashers = table_->hashers();

  for (auto i = 0; i < hashers.size(); ++i) {
    auto key = input->childAt(hashers[i]->channel())->loadedVector();
    hashers[i]->decode(*key, activeRows_);
  }

  // Update statistics for null keys in join operator.
  // We use activeRows_ to store which rows have some null keys,
  // and reset it after using it.
  // Only process when input is not spilled, to avoid overcounting.
  if (!isInputFromSpill()) {
    auto lockedStats = stats_.wlock();
    deselectRowsWithNulls(hashers, activeRows_);
    lockedStats->numNullKeys +=
        activeRows_.size() - activeRows_.countSelected();
    activeRows_.setAll();
  }

  if (!isRightJoin(joinType_) && !isFullJoin(joinType_) &&
      !isRightSemiProjectJoin(joinType_) &&
      !isLeftNullAwareJoinWithFilter(joinNode_)) {
    deselectRowsWithNulls(hashers, activeRows_);
    if (nullAware_ && !joinHasNullKeys_ &&
        activeRows_.countSelected() < input->size()) {
      joinHasNullKeys_ = true;
    }
  } else if (nullAware_ && !joinHasNullKeys_) {
    for (auto& hasher : hashers) {
      auto& decoded = hasher->decodedVector();
      if (decoded.mayHaveNulls()) {
        auto* nulls = decoded.nulls(&activeRows_);
        if (nulls && bits::countNulls(nulls, 0, activeRows_.end()) > 0) {
          joinHasNullKeys_ = true;
          break;
        }
      }
    }
  }

  for (auto i = 0; i < dependentChannels_.size(); ++i) {
    decoders_[i]->decode(
        *input->childAt(dependentChannels_[i])->loadedVector(), activeRows_);
  }

  if (isAntiJoin(joinType_) && joinNode_->filter()) {
    if (filterPropagatesNulls_) {
      removeInputRowsForAntiJoinFilter();
    }
  } else if (isAntiJoin(joinType_) && nullAware_ && joinHasNullKeys_) {
    // Null-aware anti join with no extra filter returns no rows if build side
    // has nulls in join keys. Hence, we can stop processing on first null.
    noMoreInput();
    return;
  }

  spillInput(input);
  if (!activeRows_.hasSelections()) {
    return;
  }

  if (analyzeKeys_ && hashes_.size() < activeRows_.end()) {
    hashes_.resize(activeRows_.end());
  }

  // As long as analyzeKeys is true, we keep running the keys through
  // the Vectorhashers so that we get a possible mapping of the keys
  // to small ints for array or normalized key. When mayUseValueIds is
  // false for the first time we stop. We do not retain the value ids
  // since the final ones will only be known after all data is
  // received.
  for (auto& hasher : hashers) {
    // TODO: Load only for active rows, except if right/full outer join.
    if (analyzeKeys_) {
      hasher->computeValueIds(activeRows_, hashes_);
      analyzeKeys_ = hasher->mayUseValueIds();
    }
  }
  auto rows = table_->rows();
  auto nextOffset = rows->nextOffset();
  FlatVector<bool>* spillProbedFlagVector{nullptr};
  if (isInputFromSpill() && needProbedFlagSpill_) {
    spillProbedFlagVector =
        input->childAt(spillProbedFlagChannel_)->asFlatVector<bool>();
  }

  activeRows_.applyToSelected([&](auto rowIndex) {
    char* newRow = rows->newRow();
    if (nextOffset) {
      *reinterpret_cast<char**>(newRow + nextOffset) = nullptr;
    }
    // Store the columns for each row in sequence. At probe time
    // strings of the row will probably be in consecutive places, so
    // reading one will prime the cache for the next.
    for (auto i = 0; i < hashers.size(); ++i) {
      rows->store(hashers[i]->decodedVector(), rowIndex, newRow, i);
    }
    for (auto i = 0; i < dependentChannels_.size(); ++i) {
      rows->store(*decoders_[i], rowIndex, newRow, i + hashers.size());
    }
    if (spillProbedFlagVector != nullptr) {
      VELOX_CHECK(!spillProbedFlagVector->isNullAt(rowIndex));
      if (spillProbedFlagVector->valueAt(rowIndex)) {
        rows->setProbedFlag(&newRow, 1);
      }
    }
  });
}

void HashBuild::ensureInputFits(RowVectorPtr& input) {
  // NOTE: we don't need memory reservation if all the partitions are spilling
  // as we spill all the input rows to disk directly.
  if (!canSpill() || spiller_ == nullptr || spiller_->spillTriggered()) {
    return;
  }

  // NOTE: we simply reserve memory all inputs even though some of them are
  // spilling directly. It is okay as we will accumulate the extra reservation
  // in the operator's memory pool, and won't make any new reservation if there
  // is already sufficient reservations.
  VELOX_CHECK(canSpill());

  auto* rows = table_->rows();
  const auto numRows = rows->numRows();

  auto [freeRows, outOfLineFreeBytes] = rows->freeSpace();
  const auto outOfLineBytes =
      rows->stringAllocator().retainedSize() - outOfLineFreeBytes;
  const auto currentUsage = pool()->usedBytes();

  if (numRows != 0) {
    // Test-only spill path.
    if (testingTriggerSpill(pool()->name())) {
      Operator::ReclaimableSectionGuard guard(this);
      memory::testingRunArbitration(pool());
      return;
    }
  }

  const auto minReservationBytes =
      currentUsage * spillConfig_->minSpillableReservationPct / 100;
  const auto availableReservationBytes = pool()->availableReservation();
  const auto tableIncrementBytes = table_->hashTableSizeIncrease(input->size());
  const int64_t flatBytes = input->estimateFlatSize();
  const auto rowContainerIncrementBytes = numRows == 0
      ? flatBytes * 2
      : rows->sizeIncrement(
            input->size(), outOfLineBytes > 0 ? flatBytes * 2 : 0);
  const auto incrementBytes = rowContainerIncrementBytes + tableIncrementBytes;

  // First to check if we have sufficient minimal memory reservation.
  if (availableReservationBytes >= minReservationBytes) {
    if (freeRows > input->size() &&
        (outOfLineBytes == 0 || outOfLineFreeBytes >= flatBytes)) {
      // Enough free rows for input rows and enough variable length free
      // space for the flat size of the whole vector. If outOfLineBytes
      // is 0 there is no need for variable length space.
      return;
    }

    // If there is variable length data we take the flat size of the
    // input as a cap on the new variable length data needed. There must be at
    // least 2x the increments in reservation.
    if (pool()->availableReservation() > 2 * incrementBytes) {
      return;
    }
  }

  // Check if we can increase reservation. The increment is the larger of
  // twice the maximum increment from this input and
  // 'spillableReservationGrowthPct_' of the current reservation.
  const auto targetIncrementBytes = std::max<int64_t>(
      incrementBytes * 2,
      currentUsage * spillConfig_->spillableReservationGrowthPct / 100);

  {
    Operator::ReclaimableSectionGuard guard(this);
    if (pool()->maybeReserve(targetIncrementBytes)) {
      // If above reservation triggers the spilling of 'HashBuild' operator
      // itself, we will no longer need the reserved memory for building hash
      // table as the table is spilled, and the input will be directly spilled,
      // too.
      if (spiller_->spillTriggered()) {
        pool()->release();
      }
      return;
    }
  }
  LOG(WARNING) << "Failed to reserve " << succinctBytes(targetIncrementBytes)
               << " for memory pool " << pool()->name()
               << ", usage: " << succinctBytes(pool()->usedBytes())
               << ", reservation: " << succinctBytes(pool()->reservedBytes());
}

void HashBuild::spillInput(const RowVectorPtr& input) {
  VELOX_CHECK_EQ(input->size(), activeRows_.size());

  if (!canSpill() || spiller_ == nullptr || !spiller_->spillTriggered() ||
      !activeRows_.hasSelections()) {
    return;
  }

  const auto numInput = input->size();
  prepareInputIndicesBuffers(numInput);
  computeSpillPartitions(input);

  vector_size_t numSpillInputs = 0;
  for (auto row = 0; row < numInput; ++row) {
    const auto partition = spillPartitions_[row];
    if (FOLLY_UNLIKELY(!activeRows_.isValid(row))) {
      continue;
    }
    activeRows_.setValid(row, false);
    ++numSpillInputs;
    rawSpillInputIndicesBuffers_[partition][numSpillInputs_[partition]++] = row;
  }
  if (numSpillInputs == 0) {
    return;
  }

  maybeSetupSpillChildVectors(input);

  for (uint32_t partition = 0; partition < numSpillInputs_.size();
       ++partition) {
    const int numInputs = numSpillInputs_[partition];
    if (numInputs == 0) {
      continue;
    }
    spillPartition(
        partition, numInputs, spillInputIndicesBuffers_[partition], input);
    VELOX_CHECK(
        spiller_->state().isPartitionSpilled(SpillPartitionId(partition)));
  }
  activeRows_.updateBounds();
}

void HashBuild::maybeSetupSpillChildVectors(const RowVectorPtr& input) {
  if (isInputFromSpill()) {
    return;
  }
  int32_t spillChannel = 0;
  for (const auto& channel : keyChannels_) {
    spillChildVectors_[spillChannel++] = input->childAt(channel);
  }
  for (const auto& channel : dependentChannels_) {
    spillChildVectors_[spillChannel++] = input->childAt(channel);
  }
  if (needProbedFlagSpill_) {
    VELOX_CHECK_NOT_NULL(spillProbedFlagVector_);
    spillProbedFlagVector_->resize(input->size());
    spillChildVectors_[spillChannel] = spillProbedFlagVector_;
  }
}

void HashBuild::prepareInputIndicesBuffers(vector_size_t numInput) {
  const auto maxIndicesBufferBytes = numInput * sizeof(vector_size_t);
  for (auto partition = 0; partition < (1UL << spillConfig_->numPartitionBits);
       ++partition) {
    if (spillInputIndicesBuffers_[partition] == nullptr ||
        (spillInputIndicesBuffers_[partition]->size() <
         maxIndicesBufferBytes)) {
      spillInputIndicesBuffers_[partition] = allocateIndices(numInput, pool());
      rawSpillInputIndicesBuffers_[partition] =
          spillInputIndicesBuffers_[partition]->asMutable<vector_size_t>();
    }
  }
  std::fill(numSpillInputs_.begin(), numSpillInputs_.end(), 0);
}

void HashBuild::computeSpillPartitions(const RowVectorPtr& input) {
  if (hashes_.size() < activeRows_.end()) {
    hashes_.resize(activeRows_.end());
  }
  const auto& hashers = table_->hashers();
  for (auto i = 0; i < hashers.size(); ++i) {
    auto& hasher = hashers[i];
    if (hasher->channel() != kConstantChannel) {
      hashers[i]->hash(activeRows_, i > 0, hashes_);
    } else {
      hashers[i]->hashPrecomputed(activeRows_, i > 0, hashes_);
    }
  }

  spillPartitions_.resize(input->size());
  activeRows_.applyToSelected([&](int32_t row) {
    spillPartitions_[row] = spiller_->hashBits().partition(hashes_[row]);
  });
}

void HashBuild::spillPartition(
    uint32_t partition,
    vector_size_t size,
    const BufferPtr& indices,
    const RowVectorPtr& input) {
  VELOX_DCHECK(canSpill());

  if (isInputFromSpill()) {
    spiller_->spill(SpillPartitionId(partition), wrap(size, indices, input));
  } else {
    spiller_->spill(
        SpillPartitionId(partition),
        wrap(size, indices, spillType_, spillChildVectors_, input->pool()));
  }
}

void HashBuild::noMoreInput() {
  checkRunning();

  if (noMoreInput_) {
    return;
  }
  Operator::noMoreInput();

  noMoreInputInternal();
}

void HashBuild::noMoreInputInternal() {
  if (!finishHashBuild()) {
    return;
  }

  postHashBuildProcess();
}

bool HashBuild::finishHashBuild() {
  checkRunning();

  // Release the unused memory reservation before building the merged join
  // table.
  pool()->release();

  std::vector<ContinuePromise> promises;
  std::vector<std::shared_ptr<Driver>> peers;
  // The last Driver to hit HashBuild::finish gathers the data from
  // all build Drivers and hands it over to the probe side. At this
  // point all build Drivers are continued and will free their
  // state. allPeersFinished is true only for the last Driver of the
  // build pipeline.
  if (!operatorCtx_->task()->allPeersFinished(
          planNodeId(), operatorCtx_->driver(), &future_, promises, peers)) {
    setState(State::kWaitForBuild);
    return false;
  }

  TestValue::adjust("facebook::velox::exec::HashBuild::finishHashBuild", this);

  SCOPE_EXIT {
    // Realize the promises so that the other Drivers (which were not
    // the last to finish) can continue and finish.
    peers.clear();
    for (auto& promise : promises) {
      promise.setValue();
    }
  };

  if (joinHasNullKeys_ && isAntiJoin(joinType_) && nullAware_ &&
      !joinNode_->filter()) {
    joinBridge_->setAntiJoinHasNullKeys();
    return true;
  }

  std::vector<HashBuild*> otherBuilds;
  otherBuilds.reserve(peers.size());
  uint64_t numRows{0};
  {
    std::lock_guard<std::mutex> l(mutex_);
    numRows += table_->rows()->numRows();
  }
  for (auto& peer : peers) {
    auto op = peer->findOperator(planNodeId());
    HashBuild* build = dynamic_cast<HashBuild*>(op);
    VELOX_CHECK_NOT_NULL(build);
    if (build->joinHasNullKeys_) {
      joinHasNullKeys_ = true;
      if (isAntiJoin(joinType_) && nullAware_ && !joinNode_->filter()) {
        joinBridge_->setAntiJoinHasNullKeys();
        return true;
      }
    }
    {
      std::lock_guard<std::mutex> l(build->mutex_);
      VELOX_CHECK(
          !build->stateCleared_,
          "Internal state for a peer is empty. It might have already"
          " been closed.");
      numRows += build->table_->rows()->numRows();
    }
    otherBuilds.push_back(build);
  }

  ensureTableFits(numRows);

  std::vector<std::unique_ptr<BaseHashTable>> otherTables;
  otherTables.reserve(peers.size());
  SpillPartitionSet spillPartitions;
  for (auto* build : otherBuilds) {
    std::unique_ptr<HashBuildSpiller> spiller;
    {
      std::lock_guard<std::mutex> l(build->mutex_);
      VELOX_CHECK(
          !build->stateCleared_,
          "Internal state for a peer is empty. It might have already"
          " been closed.");
      build->stateCleared_ = true;
      VELOX_CHECK_NOT_NULL(build->table_);
      otherTables.push_back(std::move(build->table_));
      spiller = std::move(build->spiller_);
    }
    if (spiller != nullptr) {
      spiller->finishSpill(spillPartitions);
    }
  }

  if (spiller_ != nullptr) {
    spiller_->finishSpill(spillPartitions);
    removeEmptyPartitions(spillPartitions);
  }

  // TODO: Get accurate signal if parallel join build is going to be applied
  //  from hash table. Currently there is still a chance inside hash table that
  //  it might decide it is not going to trigger parallel join build.
  const bool allowParallelJoinBuild =
      !otherTables.empty() && spillPartitions.empty();

  SCOPE_EXIT {
    // Make a guard to release the unused memory reservation since we have
    // finished the merged table build.
    pool()->release();
  };

  // TODO: Re-enable parallel join build with spilling triggered after
  //  https://github.com/facebookincubator/velox/issues/3567 is fixed.
  CpuWallTiming timing;
  {
    CpuWallTimer cpuWallTimer{timing};
    table_->prepareJoinTable(
        std::move(otherTables),
        isInputFromSpill() ? spillConfig()->startPartitionBit
                           : BaseHashTable::kNoSpillInputStartPartitionBit,
        allowParallelJoinBuild ? operatorCtx_->task()->queryCtx()->executor()
                               : nullptr);
  }
  stats_.wlock()->addRuntimeStat(
      BaseHashTable::kBuildWallNanos,
      RuntimeCounter(timing.wallNanos, RuntimeCounter::Unit::kNanos));

  addRuntimeStats();

  // Setup spill function for spilling hash table directly from hash join
  // bridge after transferring of table ownership.
  HashJoinTableSpillFunc tableSpillFunc;
  if (canReclaim()) {
    VELOX_CHECK_NOT_NULL(spiller_);
    tableSpillFunc =
        [hashBitRange = spiller_->hashBits(),
         restoringPartitionId = restoringPartitionId_,
         joinNode = joinNode_,
         spillConfig = spillConfig(),
         spillStats = spillStats_.get()](std::shared_ptr<BaseHashTable> table) {
          return spillHashJoinTable(
              table,
              restoringPartitionId,
              hashBitRange,
              joinNode,
              spillConfig,
              spillStats);
        };
  }
  joinBridge_->setHashTable(
      std::move(table_),
      std::move(spillPartitions),
      joinHasNullKeys_,
      std::move(tableSpillFunc));
  if (canSpill()) {
    stateCleared_ = true;
  }
  return true;
}

void HashBuild::ensureTableFits(uint64_t numRows) {
  // NOTE: we don't need memory reservation if all the partitions have been
  // spilled as nothing need to be built.
  if (!canSpill() || spiller_ == nullptr || spiller_->spillTriggered() ||
      numRows == 0) {
    return;
  }

  // Test-only spill path.
  if (testingTriggerSpill(pool()->name())) {
    Operator::ReclaimableSectionGuard guard(this);
    memory::testingRunArbitration(pool());
    return;
  }

  TestValue::adjust("facebook::velox::exec::HashBuild::ensureTableFits", this);

  // NOTE: reserve a bit more memory to consider the extra memory used for
  // parallel table build operation.
  //
  // TODO: make this query configurable.
  const uint64_t memoryBytesToReserve =
      table_->estimateHashTableSize(numRows) * 1.1;
  {
    Operator::ReclaimableSectionGuard guard(this);
    if (pool()->maybeReserve(memoryBytesToReserve)) {
      // If reservation triggers the spilling of 'HashBuild' operator itself, we
      // will no longer need the reserved memory for building hash table as the
      // table is spilled.
      if (spiller_->spillTriggered()) {
        pool()->release();
      }
      return;
    }
  }

  LOG(WARNING) << "Failed to reserve " << succinctBytes(memoryBytesToReserve)
               << " for join table build from last hash build operator "
               << pool()->name()
               << ", usage: " << succinctBytes(pool()->usedBytes())
               << ", reservation: " << succinctBytes(pool()->reservedBytes());
}

void HashBuild::postHashBuildProcess() {
  checkRunning();

  if (!canSpill()) {
    setState(State::kFinish);
    return;
  }

  auto spillInput = joinBridge_->spillInputOrFuture(&future_);
  if (!spillInput.has_value()) {
    VELOX_CHECK(future_.valid());
    setState(State::kWaitForProbe);
    return;
  }
  setupSpillInput(std::move(spillInput.value()));
}

void HashBuild::setupSpillInput(HashJoinBridge::SpillInput spillInput) {
  checkRunning();

  if (spillInput.spillPartition == nullptr) {
    setState(State::kFinish);
    return;
  }

  table_.reset();
  spiller_.reset();
  spillInputReader_.reset();
  restoringPartitionId_.reset();

  // Reset the key and dependent channels as the spilled data columns have
  // already been ordered.
  std::iota(keyChannels_.begin(), keyChannels_.end(), 0);
  std::iota(
      dependentChannels_.begin(),
      dependentChannels_.end(),
      keyChannels_.size());

  setupTable();
  setupSpiller(spillInput.spillPartition.get());
  stateCleared_ = false;

  // Start to process spill input.
  processSpillInput();
}

void HashBuild::processSpillInput() {
  checkRunning();

  while (spillInputReader_->nextBatch(spillInput_)) {
    addInput(std::move(spillInput_));
    if (!isRunning()) {
      return;
    }
    if (operatorCtx_->driver()->shouldYield()) {
      state_ = State::kYield;
      future_ = ContinueFuture{folly::Unit{}};
      return;
    }
  }
  noMoreInputInternal();
}

void HashBuild::addRuntimeStats() {
  // Report range sizes and number of distinct values for the join keys.
  const auto& hashers = table_->hashers();
  const auto hashTableStats = table_->stats();
  uint64_t asRange{0};
  uint64_t asDistinct{0};
  auto lockedStats = stats_.wlock();

  for (const auto& timing : table_->parallelJoinBuildStats().partitionTimings) {
    lockedStats->getOutputTiming.add(timing);
    lockedStats->addRuntimeStat(
        BaseHashTable::kParallelJoinPartitionWallNanos,
        RuntimeCounter(timing.wallNanos, RuntimeCounter::Unit::kNanos));
    lockedStats->addRuntimeStat(
        BaseHashTable::kParallelJoinPartitionCpuNanos,
        RuntimeCounter(timing.cpuNanos, RuntimeCounter::Unit::kNanos));
  }

  for (const auto& timing : table_->parallelJoinBuildStats().buildTimings) {
    lockedStats->getOutputTiming.add(timing);
    lockedStats->addRuntimeStat(
        BaseHashTable::kParallelJoinBuildWallNanos,
        RuntimeCounter(timing.wallNanos, RuntimeCounter::Unit::kNanos));
    lockedStats->addRuntimeStat(
        BaseHashTable::kParallelJoinBuildCpuNanos,
        RuntimeCounter(timing.cpuNanos, RuntimeCounter::Unit::kNanos));
  }

  for (auto i = 0; i < hashers.size(); i++) {
    hashers[i]->cardinality(0, asRange, asDistinct);
    if (asRange != VectorHasher::kRangeTooLarge) {
      lockedStats->addRuntimeStat(
          fmt::format("rangeKey{}", i), RuntimeCounter(asRange));
    }
    if (asDistinct != VectorHasher::kRangeTooLarge) {
      lockedStats->addRuntimeStat(
          fmt::format("distinctKey{}", i), RuntimeCounter(asDistinct));
    }
  }

  lockedStats->runtimeStats[BaseHashTable::kCapacity] =
      RuntimeMetric(hashTableStats.capacity);
  lockedStats->runtimeStats[BaseHashTable::kNumRehashes] =
      RuntimeMetric(hashTableStats.numRehashes);
  lockedStats->runtimeStats[BaseHashTable::kNumDistinct] =
      RuntimeMetric(hashTableStats.numDistinct);
  if (hashTableStats.numTombstones != 0) {
    lockedStats->runtimeStats[BaseHashTable::kNumTombstones] =
        RuntimeMetric(hashTableStats.numTombstones);
  }

  // Add max spilling level stats if spilling has been triggered.
  if (spiller_ != nullptr && spiller_->spillTriggered()) {
    lockedStats->addRuntimeStat(
        "maxSpillLevel",
        RuntimeCounter(
            spillConfig()->spillLevel(spiller_->hashBits().begin())));
  }
}

BlockingReason HashBuild::isBlocked(ContinueFuture* future) {
  switch (state_) {
    case State::kRunning:
      if (isInputFromSpill()) {
        processSpillInput();
      }
      break;
    case State::kYield:
      setRunning();
      VELOX_CHECK(isInputFromSpill());
      processSpillInput();
      break;
    case State::kFinish:
      break;
    case State::kWaitForBuild:
      [[fallthrough]];
    case State::kWaitForProbe:
      if (!future_.valid()) {
        setRunning();
        postHashBuildProcess();
      }
      break;
    default:
      VELOX_UNREACHABLE("Unexpected state: {}", stateName(state_));
      break;
  }
  if (future_.valid()) {
    VELOX_CHECK(!isRunning() && !isFinished());
    *future = std::move(future_);
  }
  return fromStateToBlockingReason(state_);
}

bool HashBuild::isFinished() {
  return state_ == State::kFinish;
}

bool HashBuild::isRunning() const {
  return state_ == State::kRunning;
}

void HashBuild::checkRunning() const {
  VELOX_CHECK(isRunning(), stateName(state_));
}

void HashBuild::setRunning() {
  setState(State::kRunning);
}

void HashBuild::setState(State state) {
  checkStateTransition(state);
  state_ = state;
}

void HashBuild::checkStateTransition(State state) {
  VELOX_CHECK_NE(state_, state);
  switch (state) {
    case State::kRunning:
      if (!canSpill()) {
        VELOX_CHECK_EQ(state_, State::kWaitForBuild);
      } else {
        VELOX_CHECK_NE(state_, State::kFinish);
      }
      break;
    case State::kWaitForBuild:
      [[fallthrough]];
    case State::kWaitForProbe:
      [[fallthrough]];
    case State::kFinish:
      VELOX_CHECK_EQ(state_, State::kRunning);
      break;
    default:
      VELOX_UNREACHABLE(stateName(state_));
      break;
  }
}

std::string HashBuild::stateName(State state) {
  switch (state) {
    case State::kRunning:
      return "RUNNING";
    case State::kYield:
      return "YIELD";
    case State::kWaitForBuild:
      return "WAIT_FOR_BUILD";
    case State::kWaitForProbe:
      return "WAIT_FOR_PROBE";
    case State::kFinish:
      return "FINISH";
    default:
      return fmt::format("UNKNOWN: {}", static_cast<int>(state));
  }
}

bool HashBuild::canSpill() const {
  if (!Operator::canSpill()) {
    return false;
  }
  if (operatorCtx_->task()->hasMixedExecutionGroupJoin(joinNode_.get())) {
    return operatorCtx_->driverCtx()
               ->queryConfig()
               .mixedGroupedModeHashJoinSpillEnabled() &&
        operatorCtx_->task()->concurrentSplitGroups() == 1;
  }
  return true;
}

bool HashBuild::canReclaim() const {
  return canSpill() && !exceededMaxSpillLevelLimit_;
}

void HashBuild::reclaim(
    uint64_t /*unused*/,
    memory::MemoryReclaimer::Stats& stats) {
  TestValue::adjust("facebook::velox::exec::HashBuild::reclaim", this);
  VELOX_CHECK(canSpill());
  auto* driver = operatorCtx_->driver();
  VELOX_CHECK_NOT_NULL(driver);
  VELOX_CHECK(!nonReclaimableSection_);

  const auto* config = spillConfig();
  VELOX_CHECK_NOT_NULL(config);
  if (UNLIKELY(exceededMaxSpillLevelLimit_)) {
    // 'canReclaim()' already checks the spill limit is not exceeding max, there
    // is only a small chance from the time 'canReclaim()' is checked to the
    // actual reclaim happens that the operator has spilled such that the spill
    // level exceeds max.
    LOG(WARNING)
        << "Can't reclaim from hash build operator, exceeded maximum spill "
           "level of "
        << config->maxSpillLevel << ", " << pool()->name() << ", usage "
        << succinctBytes(pool()->usedBytes());
    return;
  }

  // NOTE: a hash build operator is reclaimable if it is in the middle of table
  // build processing and is not under non-reclaimable execution section.
  if (nonReclaimableState()) {
    // TODO: reduce the log frequency if it is too verbose.
    RECORD_METRIC_VALUE(kMetricMemoryNonReclaimableCount);
    ++stats.numNonReclaimableAttempts;
    LOG(WARNING) << "Can't reclaim from hash build operator, state_["
                 << stateName(state_) << "], nonReclaimableSection_["
                 << nonReclaimableSection_ << "], spiller_["
                 << (stateCleared_ ? "cleared"
                                   : (spiller_->finalized() ? "finalized"
                                                            : "non-finalized"))
                 << "] " << pool()->name()
                 << ", usage: " << succinctBytes(pool()->usedBytes());
    return;
  }

  const auto& task = driver->task();
  VELOX_CHECK(task->pauseRequested());
  const std::vector<Operator*> operators =
      task->findPeerOperators(operatorCtx_->driverCtx()->pipelineId, this);

  for (auto* op : operators) {
    HashBuild* buildOp = dynamic_cast<HashBuild*>(op);
    VELOX_CHECK_NOT_NULL(buildOp);
    VELOX_CHECK(buildOp->canSpill());
    if (buildOp->nonReclaimableState()) {
      // TODO: reduce the log frequency if it is too verbose.
      RECORD_METRIC_VALUE(kMetricMemoryNonReclaimableCount);
      ++stats.numNonReclaimableAttempts;
      LOG(WARNING) << "Can't reclaim from hash build operator, state_["
                   << stateName(buildOp->state_) << "], nonReclaimableSection_["
                   << buildOp->nonReclaimableSection_ << "], "
                   << buildOp->pool()->name() << ", usage: "
                   << succinctBytes(buildOp->pool()->usedBytes());
      return;
    }
  }

  std::vector<HashBuildSpiller*> spillers;
  for (auto* op : operators) {
    HashBuild* buildOp = static_cast<HashBuild*>(op);
    spillers.push_back(buildOp->spiller_.get());
  }

  spillHashJoinTable(spillers, config);

  for (auto* op : operators) {
    HashBuild* buildOp = static_cast<HashBuild*>(op);
    buildOp->table_->clear(true);
    buildOp->pool()->release();
  }
}

bool HashBuild::nonReclaimableState() const {
  // Apart from being in the nonReclaimable section, it's also not reclaimable
  // if:
  // 1) the hash table has been built by the last build thread (indicated by
  //    state_)
  // 2) the last build operator has transferred ownership of 'this operator's
  //    internal state (table_ and spiller_) to itself.
  // 3) it has completed spilling before reaching either of the previous
  //    two states.
  return ((state_ != State::kRunning) && (state_ != State::kWaitForBuild) &&
          (state_ != State::kYield)) ||
      nonReclaimableSection_ || !spiller_ || spiller_->finalized();
}

void HashBuild::close() {
  Operator::close();

  {
    // Free up major memory usage. Gate access to them as they can be accessed
    // by the last build thread that finishes building the hash table.
    std::lock_guard<std::mutex> l(mutex_);
    stateCleared_ = true;
    joinBridge_.reset();
    spiller_.reset();
    table_.reset();
  }
}

HashBuildSpiller::HashBuildSpiller(
    core::JoinType joinType,
    std::optional<SpillPartitionId> parentId,
    RowContainer* container,
    RowTypePtr rowType,
    HashBitRange bits,
    const common::SpillConfig* spillConfig,
    folly::Synchronized<common::SpillStats>* spillStats)
    : SpillerBase(
          container,
          std::move(rowType),
          bits,
          {},
          spillConfig->maxFileSize,
          spillConfig->maxSpillRunRows,
          parentId,
          spillConfig,
          spillStats),
      spillProbeFlag_(needRightSideJoin(joinType)) {
  VELOX_CHECK(container_->accumulators().empty());
}

void HashBuildSpiller::spill() {
  spillTriggered_ = true;
  SpillerBase::spill(nullptr);
}

void HashBuildSpiller::spill(
    const SpillPartitionId& partitionId,
    const RowVectorPtr& spillVector) {
  VELOX_CHECK(spillTriggered_);
  VELOX_CHECK(!finalized_);
  if (FOLLY_UNLIKELY(spillVector == nullptr)) {
    return;
  }
  if (!state_.isPartitionSpilled(partitionId)) {
    state_.setPartitionSpilled(partitionId);
  }
  state_.appendToPartition(partitionId, spillVector);
}

void HashBuildSpiller::extractSpill(
    folly::Range<char**> rows,
    facebook::velox::RowVectorPtr& resultPtr) {
  if (resultPtr == nullptr) {
    resultPtr = BaseVector::create<RowVector>(
        rowType_, rows.size(), memory::spillMemoryPool());
  } else {
    resultPtr->prepareForReuse();
    resultPtr->resize(rows.size());
  }

  auto* result = resultPtr.get();
  const auto& types = container_->columnTypes();
  for (auto i = 0; i < types.size(); ++i) {
    container_->extractColumn(rows.data(), rows.size(), i, result->childAt(i));
  }
  if (spillProbeFlag_) {
    container_->extractProbedFlags(
        rows.data(), rows.size(), false, false, result->childAt(types.size()));
  }
}
} // namespace facebook::velox::exec
