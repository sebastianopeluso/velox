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

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"

#include "folly/experimental/EventCount.h"

using namespace facebook::velox;
using namespace facebook::velox::common::testutil;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;

class MergeJoinTest : public HiveConnectorTestBase {
 protected:
  using OperatorTestBase::assertQuery;

  CursorParameters makeCursorParameters(
      const std::shared_ptr<const core::PlanNode>& planNode,
      uint32_t preferredOutputBatchSize) {
    auto queryCtx = core::QueryCtx::create(executor_.get());

    CursorParameters params;
    params.planNode = planNode;
    params.queryCtx = queryCtx;
    params.queryCtx->testingOverrideConfigUnsafe(
        {{core::QueryConfig::kPreferredOutputBatchRows,
          std::to_string(preferredOutputBatchSize)}});
    return params;
  }

  std::vector<RowVectorPtr> generateInput(const std::vector<VectorPtr>& keys) {
    std::vector<RowVectorPtr> data;
    data.reserve(keys.size());
    vector_size_t startRow = 0;

    for (const auto& key : keys) {
      auto payload = makeFlatVector<int32_t>(
          key->size(), [startRow](auto row) { return (startRow + row) * 10; });
      auto constPayload = BaseVector::createConstant(
          DOUBLE(), (double)startRow, key->size(), pool());
      auto dictPayload = BaseVector::wrapInDictionary(
          {},
          makeIndicesInReverse(key->size()),
          key->size(),
          makeFlatVector<std::string>(key->size(), [startRow](auto row) {
            return fmt::format("{}", (startRow + row) * 10);
          }));
      data.push_back(makeRowVector({key, payload, constPayload, dictPayload}));
      startRow += key->size();
    }
    return data;
  }

  // Lazy vector loader class to ensure they get loaded in the correct order,
  // and only once.
  class MySimpleVectorLoader : public VectorLoader {
   public:
    explicit MySimpleVectorLoader(
        size_t batchId,
        const std::shared_ptr<size_t>& count,
        std::function<VectorPtr(RowSet)> loader)
        : batchId_(batchId), count_(count), loader_(loader) {}

    void loadInternal(
        RowSet rows,
        ValueHook* hook,
        vector_size_t resultSize,
        VectorPtr* result) override {
      if (batchId_ > *count_) {
        *count_ = batchId_;
      }
      VELOX_CHECK_GE(batchId_, *count_, "Lazy vectors loaded out of order.");
      VELOX_CHECK(!loaded_, "Trying to load a lazy vector twice.");
      *result = loader_(rows);
      loaded_ = true;
    }

   private:
    const size_t batchId_;
    const std::shared_ptr<size_t> count_;
    bool loaded_{false};
    std::function<VectorPtr(RowSet)> loader_;
  };

  // Generates lazy vectors to ensure the merge join operator is loading them as
  // expected.
  std::vector<RowVectorPtr> generateLazyInput(
      const std::vector<RowVectorPtr>& input) {
    std::vector<RowVectorPtr> data;
    data.reserve(input.size());

    size_t batchId = 0;
    auto counter = std::make_shared<size_t>(0);

    for (const auto& row : input) {
      std::vector<VectorPtr> children;
      for (const auto& child : row->children()) {
        children.push_back(std::make_shared<LazyVector>(
            pool(),
            child->type(),
            child->size(),
            std::make_unique<MySimpleVectorLoader>(
                batchId, counter, [=, this](RowSet) { return child; })));
      }

      data.push_back(makeRowVector(children));
      ++batchId;
    }

    return data;
  }

  template <typename T>
  void testJoin(
      std::function<T(vector_size_t /*row*/)> leftKeyAt,
      std::function<T(vector_size_t /*row*/)> rightKeyAt,
      std::function<bool(vector_size_t /*row*/)> leftNullAt = nullptr,
      std::function<bool(vector_size_t /*row*/)> rightNullAt = nullptr) {
    // Single batch on the left and right sides of the join.
    {
      auto leftKeys = makeFlatVector<T>(1'234, leftKeyAt, leftNullAt);
      auto rightKeys = makeFlatVector<T>(1'234, rightKeyAt, rightNullAt);

      testJoin({leftKeys}, {rightKeys});
    }

    // Multiple batches on one side. Single batch on the other side.
    {
      std::vector<VectorPtr> leftKeys = {
          makeFlatVector<T>(1024, leftKeyAt, leftNullAt),
          makeFlatVector<T>(
              1024,
              [&](auto row) { return leftKeyAt(1024 + row); },
              [&](auto row) {
                return leftNullAt ? leftNullAt(1024 + row) : false;
              }),
      };
      std::vector<VectorPtr> rightKeys = {
          makeFlatVector<T>(2048, rightKeyAt, rightNullAt)};

      testJoin(leftKeys, rightKeys);

      // Swap left and right side keys.
      testJoin(rightKeys, leftKeys);
    }

    // Multiple batches on each side.
    {
      std::vector<VectorPtr> leftKeys = {
          makeFlatVector<T>(512, leftKeyAt, leftNullAt),
          makeFlatVector<T>(
              1024,
              [&](auto row) { return leftKeyAt(512 + row); },
              [&](auto row) {
                return leftNullAt ? leftNullAt(512 + row) : false;
              }),
          makeFlatVector<T>(
              16,
              [&](auto row) { return leftKeyAt(512 + 1024 + row); },
              [&](auto row) {
                return leftNullAt ? leftNullAt(512 + 1024 + row) : false;
              }),
      };
      std::vector<VectorPtr> rightKeys = {
          makeFlatVector<T>(123, rightKeyAt),
          makeFlatVector<T>(
              1024,
              [&](auto row) { return rightKeyAt(123 + row); },
              [&](auto row) {
                return rightNullAt ? rightNullAt(123 + row) : false;
              }),
          makeFlatVector<T>(
              1234,
              [&](auto row) { return rightKeyAt(123 + 1024 + row); },
              [&](auto row) {
                return rightNullAt ? rightNullAt(123 + 1024 + row) : false;
              }),
      };

      testJoin(leftKeys, rightKeys);

      // Swap left and right side keys.
      testJoin(rightKeys, leftKeys);
    }
  }

  void testJoins(
      const std::vector<RowVectorPtr>& leftInput,
      const std::vector<RowVectorPtr>& rightInput,
      const std::function<std::vector<RowVectorPtr>(
          const std::vector<RowVectorPtr>&)>& inputTransform) {
    // Test INNER join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodePtr mergeJoinNode;
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .values(inputTransform(leftInput))
                    .mergeJoin(
                        {"c0"},
                        {"u_c0"},
                        PlanBuilder(planNodeIdGenerator)
                            .values(inputTransform(rightInput))
                            .project(
                                {"c1 AS u_c1",
                                 "c0 AS u_c0",
                                 "c2 AS u_c2",
                                 "c3 AS u_c3"})
                            .planNode(),
                        "",
                        {"c0", "c1", "u_c1", "c2", "u_c2", "c3", "u_c3"},
                        core::JoinType::kInner)
                    .capturePlanNode(mergeJoinNode)
                    .planNode();
    ASSERT_TRUE(mergeJoinNode->supportsBarrier());

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(plan, 16),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t, u WHERE t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(plan, 1024),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t, u WHERE t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(plan, 10'000),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t, u WHERE t.c0 = u.c0");

    // Test LEFT join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto leftPlan = PlanBuilder(planNodeIdGenerator)
                        .values(inputTransform(leftInput))
                        .mergeJoin(
                            {"c0"},
                            {"u_c0"},
                            PlanBuilder(planNodeIdGenerator)
                                .values(inputTransform(rightInput))
                                .project(
                                    {"c1 as u_c1",
                                     "c0 as u_c0",
                                     "c2 AS u_c2",
                                     "c3 AS u_c3"})
                                .planNode(),
                            "",
                            {"c0", "c1", "u_c1", "c2", "u_c2", "c3", "u_c3"},
                            core::JoinType::kLeft)
                        .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 16),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 1024),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Test RIGHT join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto rightPlan = PlanBuilder(planNodeIdGenerator)
                         .values(inputTransform(rightInput))
                         .mergeJoin(
                             {"c0"},
                             {"u_c0"},
                             PlanBuilder(planNodeIdGenerator)
                                 .values(inputTransform(leftInput))
                                 .project(
                                     {"c1 as u_c1",
                                      "c0 as u_c0",
                                      "c2 AS u_c2",
                                      "c3 AS u_c3"})
                                 .planNode(),
                             "",
                             {"u_c0", "u_c1", "c1", "u_c2", "c2", "u_c3", "c3"},
                             core::JoinType::kRight)
                         .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 16),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 1024),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Test right join and left join with same result.
    auto expectedResult = AssertQueryBuilder(leftPlan).copyResults(pool_.get());
    AssertQueryBuilder(rightPlan).assertResults(expectedResult);

    // Test FULL join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto fullPlan = PlanBuilder(planNodeIdGenerator)
                        .values(inputTransform(rightInput))
                        .mergeJoin(
                            {"c0"},
                            {"u_c0"},
                            PlanBuilder(planNodeIdGenerator)
                                .values(inputTransform(leftInput))
                                .project(
                                    {"c1 as u_c1",
                                     "c0 as u_c0",
                                     "c2 AS u_c2",
                                     "c3 AS u_c3"})
                                .planNode(),
                            "",
                            {"u_c0", "u_c1", "c1", "u_c2", "c2", "u_c3", "c3"},
                            core::JoinType::kFull)
                        .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 16),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 1024),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1, t.c2, u.c2, t.c3, u.c3 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");
  }

  void testJoin(
      const std::vector<VectorPtr>& leftKeys,
      const std::vector<VectorPtr>& rightKeys) {
    const auto leftInput = generateInput(leftKeys);
    const auto rightInput = generateInput(rightKeys);
    createDuckDbTable("t", leftInput);
    createDuckDbTable("u", rightInput);

    testJoins(leftInput, rightInput, folly::identity);
    testJoins(
        leftInput,
        rightInput,
        std::bind(
            &MergeJoinTest::generateLazyInput, this, std::placeholders::_1));
  }
};

TEST_F(MergeJoinTest, oneToOneAllMatch) {
  testJoin<int32_t>([](auto row) { return row; }, [](auto row) { return row; });
}

TEST_F(MergeJoinTest, someDontMatch) {
  testJoin<int32_t>(
      [](auto row) { return row % 5 == 0 ? row - 1 : row; },
      [](auto row) { return row % 7 == 0 ? row - 1 : row; });
}

TEST_F(MergeJoinTest, fewMatch) {
  testJoin<int32_t>(
      [](auto row) { return row * 5; }, [](auto row) { return row * 7; });
}

TEST_F(MergeJoinTest, duplicateMatch) {
  testJoin<int32_t>(
      [](auto row) { return row / 2; }, [](auto row) { return row / 3; });
}

TEST_F(MergeJoinTest, someNulls) {
  testJoin<int32_t>(
      [](auto row) { return row; },
      [](auto row) { return row; },
      [](auto row) { return row > 7; },
      [](auto row) { return false; });
}

TEST_F(MergeJoinTest, someNullsOtherSideFinishesEarly) {
  testJoin<int32_t>(
      [](auto row) { return row; },
      [](auto row) { return std::min(row, 7); },
      [](auto row) { return row > 7; },
      [](auto row) { return false; });
}

TEST_F(MergeJoinTest, someNullsOnBothSides) {
  testJoin<int32_t>(
      [](auto row) { return row; },
      [](auto row) { return row; },
      [](auto row) { return row > 7; },
      [](auto row) { return row > 8; });
}

TEST_F(MergeJoinTest, allRowsMatch) {
  std::vector<VectorPtr> leftKeys = {
      makeFlatVector<int32_t>(2, [](auto /* row */) { return 5; }),
      makeFlatVector<int32_t>(3, [](auto /* row */) { return 5; }),
      makeFlatVector<int32_t>(4, [](auto /* row */) { return 5; }),
  };
  std::vector<VectorPtr> rightKeys = {
      makeFlatVector<int32_t>(7, [](auto /* row */) { return 5; })};

  testJoin(leftKeys, rightKeys);
  testJoin(rightKeys, leftKeys);
}

TEST_F(MergeJoinTest, keySkew) {
  testJoin<int32_t>(
      [](auto row) { return row; },
      [](auto row) { return row < 10 ? row : row + 10240; });
}

TEST_F(MergeJoinTest, aggregationOverJoin) {
  auto left =
      makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3, 4, 5})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({2, 4, 6})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .singleAggregation({}, {"count(1)"})
          .planNode();

  auto result = readSingleValue(plan);
  ASSERT_FALSE(result.isNull());
  ASSERT_EQ(2, result.value<int64_t>());
}

TEST_F(MergeJoinTest, nonFirstJoinKeys) {
  auto left = makeRowVector(
      {"t_data", "t_key"},
      {
          makeFlatVector<int32_t>({50, 40, 30, 20, 10}),
          makeFlatVector<int32_t>({1, 2, 3, 4, 5}),
      });
  auto right = makeRowVector(
      {"u_data", "u_key"},
      {
          makeFlatVector<int32_t>({23, 22, 21}),
          makeFlatVector<int32_t>({2, 4, 6}),
      });

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_key"},
              {"u_key"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_key", "t_data", "u_data"},
              core::JoinType::kInner)
          .planNode();

  assertQuery(plan, "VALUES (2, 40, 23), (4, 20, 22)");
}

TEST_F(MergeJoinTest, innerJoinFilter) {
  vector_size_t size = 1'000;
  // Join keys on the left side: 0, 10, 20,..
  // Payload on the left side: 0, 1, 2, 3,..
  auto left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>(size, [](auto row) { return row * 10; }),
          makeFlatVector<int64_t>(
              size, [](auto row) { return row; }, nullEvery(13)),
      });

  // Join keys on the right side: 0, 5, 10, 15, 20,..
  // Payload on the right side: 0, 1, 2, 3, 4, 5, 6, 0, 1, 2,..
  auto right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>(size, [](auto row) { return row * 5; }),
          makeFlatVector<int64_t>(
              size, [](auto row) { return row % 7; }, nullEvery(17)),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto plan = [&](const std::string& filter) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    return PlanBuilder(planNodeIdGenerator)
        .values({left})
        .mergeJoin(
            {"t_c0"},
            {"u_c0"},
            PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
            filter,
            {"t_c0", "u_c0", "u_c1"},
            core::JoinType::kInner)
        .planNode();
  };

  assertQuery(
      plan("(t_c1 + u_c1) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");

  assertQuery(
      plan("(t_c1 + u_c1) % 2 = 1"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 1");

  // No rows pass filter.
  assertQuery(
      plan("(t_c1 + u_c1) % 2 < 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 < 0");

  // All rows pass filter.
  assertQuery(
      plan("(t_c1 + u_c1) % 2 >= 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 >= 0");

  // Filter expressions over join keys.
  assertQuery(
      plan("(t_c0 + u_c1) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c0 + u_c1) % 2 = 0");

  assertQuery(
      plan("(t_c1 + u_c0) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c0) % 2 = 0");

  // Very small output batch size.
  assertQuery(
      makeCursorParameters(plan("(t_c1 + u_c1) % 2 = 0"), 16),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");
}

TEST_F(MergeJoinTest, leftAndRightJoinFilter) {
  // Each row on the left side has at most one match on the right side.
  auto left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>({0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}),
      });

  auto right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>({0, 10, 20, 30, 40, 50}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto leftPlan = [&](const std::string& filter) {
    return PlanBuilder(planNodeIdGenerator)
        .values({left})
        .mergeJoin(
            {"t_c0"},
            {"u_c0"},
            PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
            filter,
            {"t_c0", "t_c1", "u_c1"},
            core::JoinType::kLeft)
        .planNode();
  };

  auto rightPlan = [&](const std::string& filter) {
    return PlanBuilder(planNodeIdGenerator)
        .values({right})
        .mergeJoin(
            {"u_c0"},
            {"t_c0"},
            PlanBuilder(planNodeIdGenerator).values({left}).planNode(),
            filter,
            {"t_c0", "t_c1", "u_c1"},
            core::JoinType::kRight)
        .planNode();
  };

  // Test with different output batch sizes.
  for (auto batchSize : {1, 3, 16}) {
    assertQuery(
        makeCursorParameters(leftPlan("(t_c1 + u_c1) % 2 = 0"), batchSize),
        "SELECT t_c0, t_c1, u_c1 FROM t LEFT JOIN u ON t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");

    assertQuery(
        makeCursorParameters(rightPlan("(t_c1 + u_c1) % 2 = 0"), batchSize),
        "SELECT t_c0, t_c1, u_c1 FROM u RIGHT JOIN t ON t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");
  }

  // A left-side row with multiple matches on the right side.
  left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>({5, 10}),
          makeFlatVector<int32_t>({0, 0}),
      });

  right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>({10, 10, 10, 10, 10, 10}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Test with different filters and output batch sizes.
  for (auto batchSize : {1, 3, 16}) {
    for (auto filter :
         {"t_c1 + u_c1 > 3",
          "t_c1 + u_c1 < 3",
          "t_c1 + u_c1 > 100",
          "t_c1 + u_c1 < 100"}) {
      assertQuery(
          makeCursorParameters(leftPlan(filter), batchSize),
          fmt::format(
              "SELECT t_c0, t_c1, u_c1 FROM t LEFT JOIN u ON t_c0 = u_c0 AND {}",
              filter));
      assertQuery(
          makeCursorParameters(rightPlan(filter), batchSize),
          fmt::format(
              "SELECT t_c0, t_c1, u_c1 FROM u RIGHT JOIN t ON t_c0 = u_c0 AND {}",
              filter));
    }
  }
}

TEST_F(MergeJoinTest, rightJoinWithDuplicateMatch) {
  // Each row on the left side has at most one match on the right side.
  auto left = makeRowVector(
      {"a", "b"},
      {
          makeNullableFlatVector<int32_t>({1, 2, 2, 2, 3, 5, 6, std::nullopt}),
          makeNullableFlatVector<double>(
              {2.0, 100.0, 1.0, 1.0, 3.0, 1.0, 6.0, std::nullopt}),
      });

  auto right = makeRowVector(
      {"c", "d"},
      {
          makeNullableFlatVector<int32_t>(
              {0, 2, 2, 2, 2, 3, 4, 5, 7, std::nullopt}),
          makeNullableFlatVector<double>(
              {0.0, 3.0, -1.0, -1.0, 3.0, 2.0, 1.0, 3.0, 7.0, std::nullopt}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();

  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b", "c", "d"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults("SELECT * from t RIGHT JOIN u ON a = c AND b < d");
}

TEST_F(MergeJoinTest, rightJoinFilterWithNull) {
  auto left = makeRowVector(
      {"a", "b"},
      {
          makeNullableFlatVector<int32_t>({std::nullopt, std::nullopt}),
          makeNullableFlatVector<double>({std::nullopt, std::nullopt}),
      });

  auto right = makeRowVector(
      {"c", "d"},
      {
          makeNullableFlatVector<int32_t>(
              {std::nullopt, std::nullopt, std::nullopt}),
          makeNullableFlatVector<double>(
              {std::nullopt, std::nullopt, std::nullopt}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();

  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b", "c", "d"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults("SELECT * from t RIGHT JOIN u ON a = c AND b < d");
}

TEST_F(MergeJoinTest, fisrtRowsNull) {
  auto left = makeRowVector(
      {"a", "b"},
      {
          makeNullableFlatVector<int32_t>({std::nullopt, 3}),
          makeNullableFlatVector<double>({std::nullopt, 3}),
      });

  auto right = makeRowVector(
      {"c", "d"},
      {
          makeNullableFlatVector<int32_t>({std::nullopt, std::nullopt, 3}),
          makeNullableFlatVector<double>({std::nullopt, std::nullopt, 4}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();

  auto plan = [&](core::JoinType type) {
    return PlanBuilder(planNodeIdGenerator)
        .values({left})
        .mergeJoin(
            {"a"},
            {"c"},
            PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
            "b < d",
            {"a", "b", "c", "d"},
            type)
        .planNode();
  };

  // Right Join
  AssertQueryBuilder(plan(core::JoinType::kRight), duckDbQueryRunner_)
      .assertResults("SELECT * from t RIGHT JOIN u ON a = c AND b < d");

  // Left Join
  AssertQueryBuilder(plan(core::JoinType::kLeft), duckDbQueryRunner_)
      .assertResults("SELECT * from t Left JOIN u ON a = c AND b < d");

  // Inner Join
  AssertQueryBuilder(plan(core::JoinType::kInner), duckDbQueryRunner_)
      .assertResults("SELECT * from t, u where a = c AND b < d");
}

// Verify that both left-side and right-side pipelines feeding the merge join
// always run single-threaded.
TEST_F(MergeJoinTest, numDrivers) {
  auto left = makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({0, 2, 5})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left}, true)
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}, true).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .planNode();

  auto task = AssertQueryBuilder(plan, duckDbQueryRunner_)
                  .maxDrivers(5)
                  .assertResults("SELECT 2, 2");

  // We have two pipelines in the task and each must have 1 driver.
  EXPECT_EQ(2, task->numTotalDrivers());
  EXPECT_EQ(2, task->numFinishedDrivers());
}

TEST_F(MergeJoinTest, lazyVectors) {
  // A dataset of multiple row groups with multiple columns. We create
  // different dictionary wrappings for different columns and load the
  // rows in scope at different times. We make 11000 repeats of 300
  // followed by ascending rows. These will hit one 300 from the
  // right side and cover more than one batch, so that we test lazy
  // loading where we buffer multiple batches of input.
  auto leftVectors = makeRowVector({
      makeFlatVector<int32_t>(
          30'000, [](auto row) { return row < 11000 ? 300 : row; }),
      makeFlatVector<int64_t>(30'000, [](auto row) { return row % 23; }),
      makeFlatVector<int32_t>(30'000, [](auto row) { return row % 31; }),
      makeFlatVector<StringView>(
          30'000,
          [](auto row) {
            return StringView::makeInline(fmt::format("{}   string", row % 43));
          }),
  });

  auto rightVectors = makeRowVector(
      {"rc0", "rc1"},
      {
          makeFlatVector<int32_t>(10'000, [](auto row) { return row * 3; }),
          makeFlatVector<int64_t>(10'000, [](auto row) { return row % 31; }),
      });

  auto leftFile = TempFilePath::create();
  writeToFile(leftFile->getPath(), leftVectors);
  createDuckDbTable("t", {leftVectors});

  auto rightFile = TempFilePath::create();
  writeToFile(rightFile->getPath(), rightVectors);
  createDuckDbTable("u", {rightVectors});

  auto joinTypes = {
      core::JoinType::kInner,
      core::JoinType::kLeft,
      core::JoinType::kRight,
  };

  for (auto joinType : joinTypes) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftScanId;
    core::PlanNodeId rightScanId;
    auto op = PlanBuilder(planNodeIdGenerator)
                  .tableScan(
                      ROW({"c0", "c1", "c2", "c3"},
                          {INTEGER(), BIGINT(), INTEGER(), VARCHAR()}))
                  .capturePlanNodeId(leftScanId)
                  .mergeJoin(
                      {"c0"},
                      {"rc0"},
                      PlanBuilder(planNodeIdGenerator)
                          .tableScan(ROW({"rc0", "rc1"}, {INTEGER(), BIGINT()}))
                          .capturePlanNodeId(rightScanId)
                          .planNode(),
                      "c1 + rc1 < 30",
                      {"c0", "rc0", "c1", "rc1", "c2", "c3"},
                      joinType)
                  .planNode();

    AssertQueryBuilder(op, duckDbQueryRunner_)
        .split(rightScanId, makeHiveConnectorSplit(rightFile->getPath()))
        .split(leftScanId, makeHiveConnectorSplit(leftFile->getPath()))
        .assertResults(fmt::format(
            "SELECT c0, rc0, c1, rc1, c2, c3 FROM t {} JOIN u "
            "ON t.c0 = u.rc0 AND c1 + rc1 < 30",
            core::JoinTypeName::toName(joinType)));
  }
}

// Ensures the output of merge joins are dictionaries.
TEST_F(MergeJoinTest, dictionaryOutput) {
  auto left =
      makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3, 4, 5})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({2, 4, 6})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .planFragment();

  // Run task with special callback so we can capture results without them being
  // copied/flattened.
  RowVectorPtr output;
  auto task = Task::create(
      "0",
      std::move(plan),
      0,
      core::QueryCtx::create(driverExecutor_.get()),
      Task::ExecutionMode::kParallel,
      [&](const RowVectorPtr& vector, bool drained, ContinueFuture* future) {
        VELOX_CHECK(!drained);
        if (vector) {
          output = vector;
        }
        return BlockingReason::kNotBlocked;
      });

  task->start(2);
  waitForTaskCompletion(task.get());

  for (const auto& child : output->children()) {
    EXPECT_TRUE(isDictionary(child->encoding()));
  }

  // Output can't outlive the task.
  output.reset();
}

TEST_F(MergeJoinTest, semiJoin) {
  auto left = makeRowVector(
      {"t0"}, {makeNullableFlatVector<int64_t>({1, 2, 2, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 2, 7, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto testSemiJoin = [&](const std::string& filter,
                          const std::string& sql,
                          const std::vector<std::string>& outputLayout,
                          core::JoinType joinType) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .values({left})
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                filter,
                outputLayout,
                joinType)
            .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_).assertResults(sql);
  };

  testSemiJoin(
      "t0 >1",
      "SELECT t0 FROM t where t0 IN (SELECT u0 from u) and t0 > 1",
      {"t0"},
      core::JoinType::kLeftSemiFilter);
  testSemiJoin(
      "u0 > 1",
      "SELECT u0 FROM u where u0 IN (SELECT t0 from t) and u0 > 1",
      {"u0"},
      core::JoinType::kRightSemiFilter);
}

TEST_F(MergeJoinTest, semiJoinWithMultipleMatchVectors) {
  std::vector<RowVectorPtr> leftVectors;
  for (int i = 0; i < 10; ++i) {
    leftVectors.push_back(makeRowVector(
        {"t0"}, {makeFlatVector<int64_t>({i / 2, i / 2, i / 2})}));
  }
  std::vector<RowVectorPtr> rightVectors;
  for (int i = 0; i < 10; ++i) {
    rightVectors.push_back(makeRowVector(
        {"u0"}, {makeFlatVector<int64_t>({i / 2, i / 2, i / 2})}));
  }

  createDuckDbTable("t", leftVectors);
  createDuckDbTable("u", rightVectors);

  auto testSemiJoin = [&](const std::string& filter,
                          const std::string& sql,
                          const std::vector<std::string>& outputLayout,
                          core::JoinType joinType) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .values(leftVectors)
                    .mergeJoin(
                        {"t0"},
                        {"u0"},
                        PlanBuilder(planNodeIdGenerator)
                            .values(rightVectors)
                            .planNode(),
                        filter,
                        outputLayout,
                        joinType)
                    .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .config(core::QueryConfig::kMaxOutputBatchRows, "1")
        .assertResults(sql);
  };

  testSemiJoin(
      "u0 > 1",
      "SELECT u0 FROM u where u0 IN (SELECT t0 from t) and u0 > 1",
      {"u0"},
      core::JoinType::kRightSemiFilter);
  testSemiJoin(
      "t0 >1",
      "SELECT t0 FROM t where t0 IN (SELECT u0 from u) and t0 > 1",
      {"t0"},
      core::JoinType::kLeftSemiFilter);
}

TEST_F(MergeJoinTest, semiJoinWithMultiMatchedRowsWithFilter) {
  auto left = makeRowVector(
      {"t0", "t1"},
      {makeNullableFlatVector<int64_t>({2, 2, 2, 2, 2}),
       makeNullableFlatVector<int64_t>({3, 2, 3, 2, 2})});

  auto right = makeRowVector(
      {"u0", "u1"},
      {makeNullableFlatVector<int64_t>({2, 2, 2, 2, 2, 2}),
       makeNullableFlatVector<int64_t>({2, 2, 2, 2, 2, 4})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto testSemiJoin = [&](const std::string& filter,
                          const std::string& sql,
                          const std::vector<std::string>& outputLayout,
                          core::JoinType joinType) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .values(split(left, 2))
                    .mergeJoin(
                        {"t0"},
                        {"u0"},
                        PlanBuilder(planNodeIdGenerator)
                            .values(split(right, 2))
                            .planNode(),
                        filter,
                        outputLayout,
                        joinType)
                    .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .config(core::QueryConfig::kPreferredOutputBatchRows, "2")
        .config(core::QueryConfig::kMaxOutputBatchRows, "2")
        .assertResults(sql);
  };

  // Left Semi join With filter
  testSemiJoin(
      "t1 > u1",
      "SELECT t0, t1 FROM t where t0 IN (SELECT u0 from u where t1 > u1)",
      {"t0", "t1"},
      core::JoinType::kLeftSemiFilter);

  // Right Semi join With filter
  testSemiJoin(
      "u1 > t1",
      "SELECT u0, u1 FROM u where u0 IN (SELECT t0 from t where u1 > t1)",
      {"u0", "u1"},
      core::JoinType::kRightSemiFilter);
}

TEST_F(MergeJoinTest, semiJoinWithOneMatchedRowWithFilter) {
  auto left = makeRowVector(
      {"t0", "t1"},
      {makeNullableFlatVector<int64_t>({2, 2}),
       makeNullableFlatVector<int64_t>({3, 5})});

  auto right = makeRowVector(
      {"u0", "u1"},
      {makeNullableFlatVector<int64_t>({2, 2}),
       makeNullableFlatVector<int64_t>({1, 4})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto testSemiJoin = [&](const std::string& filter,
                          const std::string& sql,
                          const std::vector<std::string>& outputLayout,
                          core::JoinType joinType) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .values(split(left, 2))
                    .mergeJoin(
                        {"t0"},
                        {"u0"},
                        PlanBuilder(planNodeIdGenerator)
                            .values(split(right, 2))
                            .planNode(),
                        filter,
                        outputLayout,
                        joinType)
                    .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_)
        .config(core::QueryConfig::kPreferredOutputBatchRows, "2")
        .config(core::QueryConfig::kMaxOutputBatchRows, "2")
        .assertResults(sql);
  };

  // Left Semi join With filter
  testSemiJoin(
      "t1 > u1",
      "SELECT t0, t1 FROM t where t0 IN (SELECT u0 from u where t1 > u1)",
      {"t0", "t1"},
      core::JoinType::kLeftSemiFilter);

  // Right Semi join With filter
  testSemiJoin(
      "u1 > t1",
      "SELECT u0, u1 FROM u where u0 IN (SELECT t0 from t where u1 > t1)",
      {"u0", "u1"},
      core::JoinType::kRightSemiFilter);
}

TEST_F(MergeJoinTest, rightJoin) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, std::nullopt, 5, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 8, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Right join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t RIGHT JOIN u ON t.t0 = u.u0 AND t.t0 > 2");

  auto leftPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({right})
          .mergeJoin(
              {"u0"},
              {"t0"},
              PlanBuilder(planNodeIdGenerator).values({left}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kLeft)
          .planNode();
  auto expectedResult = AssertQueryBuilder(leftPlan).copyResults(pool_.get());
  AssertQueryBuilder(rightPlan).assertResults(expectedResult);
}

TEST_F(MergeJoinTest, nullKeys) {
  auto left = makeRowVector(
      {"t0"}, {makeNullableFlatVector<int64_t>({1, 2, 5, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>({1, 5, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Inner join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "u0"},
              core::JoinType::kInner)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults("SELECT * FROM t, u WHERE t.t0 = u.u0");

  // Left join.
  plan = PlanBuilder(planNodeIdGenerator)
             .values({left})
             .mergeJoin(
                 {"t0"},
                 {"u0"},
                 PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                 "",
                 {"t0", "u0"},
                 core::JoinType::kLeft)
             .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults("SELECT * FROM t LEFT JOIN u ON t.t0 = u.u0");
}

TEST_F(MergeJoinTest, antiJoinWithFilter) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 4, 5, 8, 9, std::nullopt, 10, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 7, std::nullopt, std::nullopt, 8, 9, 10})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0 AND t.t0 > 2 ) ");
}

TEST_F(MergeJoinTest, antiJoinFailed) {
  auto size = 1'00;
  auto left = makeRowVector(
      {"t0"}, {makeFlatVector<int64_t>(size, [](auto row) { return row; })});

  auto right = makeRowVector(
      {"u0"}, {makeFlatVector<int64_t>(size, [](auto row) { return row; })});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values(split(left, 10))
          .orderBy({"t0"}, false)
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kMaxOutputBatchRows, "10")
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0) ");
}

TEST_F(MergeJoinTest, antiJoinWithTwoJoinKeys) {
  auto left = makeRowVector(
      {"a", "b"},
      {makeNullableFlatVector<int32_t>(
           {1, 1, 2, 2, 3, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {2.0, 2.0, 1.0, 1.0, 3.0, std::nullopt, 5.0, std::nullopt})});

  auto right = makeRowVector(
      {"c", "d"},
      {makeNullableFlatVector<int32_t>(
           {2, 2, 3, 4, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {3.0, 3.0, 2.0, 1.0, std::nullopt, 5.0, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t WHERE NOT exists (select * from u where t.a = u.c and t.b < u.d)");
}

TEST_F(MergeJoinTest, antiJoinWithUniqueJoinKeys) {
  auto left = makeRowVector(
      {"a", "b"},
      {makeNullableFlatVector<int32_t>(
           {1, 1, 2, 2, 3, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {2.0, 2.0, 1.0, 1.0, 3.0, std::nullopt, 5.0, std::nullopt})});

  auto right = makeRowVector(
      {"c", "d"},
      {makeNullableFlatVector<int32_t>({2, 3, 4, std::nullopt, 6}),
       makeNullableFlatVector<double>({3.0, 2.0, 1.0, 5.0, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t WHERE NOT exists (select * from u where t.a = u.c and t.b < u.d)");
}

TEST_F(MergeJoinTest, antiJoinNoFilter) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 4, 5, 8, 9, std::nullopt, 10, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 7, std::nullopt, std::nullopt, 8, 9, 10})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0)");
}

TEST_F(MergeJoinTest, fullOuterJoin) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, std::nullopt, 5, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 8, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t FULL OUTER JOIN u ON t.t0 = u.u0 AND t.t0 > 2");
}

TEST_F(MergeJoinTest, fullOuterJoinNoFilter) {
  auto left = makeRowVector(
      {"t0", "t1", "t2", "t3"},
      {makeNullableFlatVector<int64_t>(
           {7854252584298216695,
            5874550437257860379,
            6694700278390749883,
            6952978413716179087,
            2785313305792069690,
            5306984336093303849,
            2249699434807719017,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            8814597374860168988}),
       makeNullableFlatVector<int64_t>(
           {1, 2, 3, 4, 5, 6, 7, std::nullopt, 8, 9, 10}),
       makeNullableFlatVector<bool>(
           {false,
            true,
            false,
            false,
            false,
            true,
            true,
            false,
            true,
            false,
            false}),
       makeNullableFlatVector<int64_t>(
           {58, 112, 125, 52, 69, 39, 73, 29, 101, std::nullopt, 51})});

  auto right = makeRowVector(
      {"u0", "u1", "u2", "u3"},
      {makeNullableFlatVector<int64_t>({std::nullopt}),
       makeNullableFlatVector<int64_t>({11}),
       makeNullableFlatVector<bool>({false}),
       makeNullableFlatVector<int64_t>({77})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0", "t1", "t2", "t3"},
              {"u0", "u1", "u2", "u3"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "t1"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0, t1 FROM t FULL OUTER JOIN u ON t3 = u3 and t2 = u2 and t1 = u1 and t.t0 = u.u0");
}

TEST_F(MergeJoinTest, fullOuterJoinWithNullCompare) {
  auto right = makeRowVector(
      {"u0", "u1"},
      {makeNullableFlatVector<bool>({false, true}),
       makeNullableFlatVector<int64_t>({std::nullopt, std::nullopt})});

  auto left = makeRowVector(
      {"t0", "t1"},
      {makeNullableFlatVector<bool>({false, false, std::nullopt}),
       makeNullableFlatVector<int64_t>(
           {std::nullopt, 1195665568, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0", "t1"},
              {"u0", "u1"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "t1", "u0", "u1"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0, t1, u0, u1 FROM t FULL OUTER JOIN u ON t.t0 = u.u0 and t1 = u1");
}

TEST_F(MergeJoinTest, complexTypedFilter) {
  constexpr vector_size_t size{1000};

  auto right = makeRowVector(
      {"u_c0"},
      {makeFlatVector<int32_t>(size, [](auto row) { return row * 2; })});

  auto testComplexTypedFilter =
      [&](const std::vector<RowVectorPtr>& left,
          const std::string& filter,
          const std::string& queryFilter,
          const std::vector<std::string>& outputLayout) {
        createDuckDbTable("t", left);
        createDuckDbTable("u", {right});
        auto planNodeIdGenerator =
            std::make_shared<core::PlanNodeIdGenerator>();
        auto plan =
            PlanBuilder(planNodeIdGenerator)
                .values(left)
                .mergeJoin(
                    {"t_c0"},
                    {"u_c0"},
                    PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                    filter,
                    outputLayout,
                    core::JoinType::kLeft)
                .planNode();

        std::string outputs;
        for (auto i = 0; i < outputLayout.size(); ++i) {
          outputs += std::move(outputLayout[i]);
          if (i + 1 < outputLayout.size()) {
            outputs += ", ";
          }
        }

        for (size_t outputBatchSize : {1000, 1024, 13}) {
          assertQuery(
              makeCursorParameters(plan, outputBatchSize),
              fmt::format(
                  "SELECT {} FROM t LEFT JOIN u ON t_c0 = u_c0 AND {}",
                  outputs,
                  queryFilter));
        }
      };

  std::vector<std::vector<std::string>> outputLayouts{
      {"t_c0", "u_c0"}, {"t_c0", "u_c0", "t_c1"}};

  {
    const std::vector<std::vector<int32_t>> pattern{
        {1},
        {1, 2},
        {1, 2, 4},
        {1, 2, 4, 8},
        {1, 2, 4, 8, 16},
    };
    std::vector<std::vector<int32_t>> arrayVector;
    arrayVector.reserve(size);
    for (auto i = 0; i < size / pattern.size(); ++i) {
      arrayVector.insert(arrayVector.end(), pattern.begin(), pattern.end());
    }
    auto left = {
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(size, [](auto row) { return row; }),
             makeArrayVector<int32_t>(arrayVector)}),
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(
                 size, [size](auto row) { return size + row * 2; }),
             makeArrayVector<int32_t>(arrayVector)})};

    for (const auto& outputLayout : outputLayouts) {
      testComplexTypedFilter(
          left, "array_max(t_c1) >= 8", "list_max(t_c1) >= 8", outputLayout);
    }
  }

  {
    auto sizeAt = [](vector_size_t row) { return row % 5; };
    auto keyAt = [](vector_size_t row) { return row % 11; };
    auto valueAt = [](vector_size_t row) { return row % 13; };
    auto keys = makeArrayVector<int64_t>(size, sizeAt, keyAt);
    auto values = makeArrayVector<int32_t>(size, sizeAt, valueAt);

    auto mapVector =
        makeMapVector<int64_t, int32_t>(size, sizeAt, keyAt, valueAt);

    auto left = {
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(size, [](auto row) { return row; }),
             mapVector}),
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(
                 size, [size](auto row) { return size + row * 2; }),
             mapVector})};

    for (const auto& outputLayout : outputLayouts) {
      testComplexTypedFilter(
          left, "cardinality(t_c1) > 4", "cardinality(t_c1) > 4", outputLayout);
    }
  }
}

DEBUG_ONLY_TEST_F(MergeJoinTest, failureOnRightSide) {
  // Test that the Task terminates cleanly when the right side of the join
  // throws an exception.

  auto leftKeys = makeFlatVector<int32_t>(1'234, [](auto row) { return row; });
  auto rightKeys = makeFlatVector<int32_t>(1'234, [](auto row) { return row; });
  std::vector<RowVectorPtr> left;
  auto payload = makeFlatVector<int32_t>(
      leftKeys->size(), [](auto row) { return row * 10; });
  left.push_back(makeRowVector({leftKeys, payload}));

  std::vector<RowVectorPtr> right;
  payload = makeFlatVector<int32_t>(
      rightKeys->size(), [](auto row) { return row * 20; });
  right.push_back(makeRowVector({rightKeys, payload}));

  createDuckDbTable("t", left);
  createDuckDbTable("u", right);

  // Test INNER join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan = PlanBuilder(planNodeIdGenerator)
                  .values(left)
                  .mergeJoin(
                      {"c0"},
                      {"u_c0"},
                      PlanBuilder(planNodeIdGenerator)
                          .values(right)
                          .project({"c1 AS u_c1", "c0 AS u_c0"})
                          .planNode(),
                      "",
                      {"c0", "c1", "u_c1"},
                      core::JoinType::kInner)
                  .planNode();

  std::atomic_bool nextCalled = false;
  folly::EventCount nextCalledWait;
  std::atomic_bool enqueueCalled = false;

  // The left side will call next to fetch data from the right side.  We want
  // this to be called at least once to ensure consumerPromise_ is created in
  // the MergeSource.
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::MergeJoinSource::next",
      std::function<void(const MergeJoinSource*)>([&](const MergeJoinSource*) {
        nextCalled = true;
        nextCalledWait.notifyAll();
      }));

  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::MergeJoinSource::enqueue",
      std::function<void(const MergeJoinSource*)>([&](const MergeJoinSource*) {
        // Only call this the first time, otherwise if we throw an exception
        // during Driver.close the process will crash.
        if (!enqueueCalled.load()) {
          // The first time the right side calls enqueue, wait for the left side
          // to call next.  Since enqueue never finished executing there won't
          // be any data available and enqueue will create a consumerPromise_.
          enqueueCalled = true;
          nextCalledWait.await([&]() { return nextCalled.load(); });
          // Throw an exception so that the task terminates and consumerPromise_
          // is not fulfilled.
          VELOX_FAIL("Expected");
        }
      }));

  // Use very small output batch size.
  VELOX_ASSERT_THROW(
      assertQuery(
          makeCursorParameters(plan, 16),
          "SELECT t.c0, t.c1, u.c1 FROM t, u WHERE t.c0 = u.c0"),
      "Expected");

  waitForAllTasksToBeDeleted();
}

TEST_F(MergeJoinTest, barrier) {
  auto right = makeRowVector(
      {"u0", "u1"},
      {makeFlatVector<int32_t>(1'024, [](auto row) { return row / 3; }),
       makeFlatVector<int32_t>(1'024, [](auto row) { return row; })});

  auto left = makeRowVector(
      {"t0", "t1"},
      {makeFlatVector<int32_t>(1'024, [](auto row) { return row / 6; }),
       makeFlatVector<int32_t>(1'024, [](auto row) { return row; })});

  auto leftFile = TempFilePath::create();
  HiveConnectorTestBase::writeToFile(leftFile->getPath(), left);
  auto rightFile = TempFilePath::create();
  HiveConnectorTestBase::writeToFile(rightFile->getPath(), right);

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  {
    // Inner join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftNodeId;
    core::PlanNodeId rightNodeId;
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .startTableScan()
            .outputType(std::static_pointer_cast<const RowType>(left->type()))
            .endTableScan()
            .capturePlanNodeId(leftNodeId)
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator)
                    .startTableScan()
                    .outputType(
                        std::static_pointer_cast<const RowType>(right->type()))
                    .endTableScan()
                    .capturePlanNodeId(rightNodeId)
                    .planNode(),
                "",
                {"t0", "t1", "u0", "u1"},
                core::JoinType::kInner)
            .planNode();
    for (const auto hasBarrier : {false, true}) {
      SCOPED_TRACE(fmt::format("hasBarrier {}", hasBarrier));
      AssertQueryBuilder queryBuilder(plan, duckDbQueryRunner_);
      queryBuilder.barrierExecution(hasBarrier).serialExecution(true);
      queryBuilder.split(
          leftNodeId, makeHiveConnectorSplit(leftFile->getPath()));
      queryBuilder.split(
          rightNodeId, makeHiveConnectorSplit(rightFile->getPath()));
      queryBuilder.config(core::QueryConfig::kMaxOutputBatchRows, "32");

      const auto task = queryBuilder.assertResults(
          "SELECT t0, t1, u0, u1 FROM t INNER JOIN u ON t.t0 = u.u0");
      ASSERT_EQ(task->taskStats().numBarriers, hasBarrier ? 1 : 0);
      ASSERT_EQ(task->taskStats().numFinishedSplits, hasBarrier ? 2 : 1);
    }
  }

  {
    // Full join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftNodeId;
    core::PlanNodeId rightNodeId;
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .startTableScan()
            .outputType(std::static_pointer_cast<const RowType>(left->type()))
            .endTableScan()
            .capturePlanNodeId(leftNodeId)
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator)
                    .startTableScan()
                    .outputType(
                        std::static_pointer_cast<const RowType>(right->type()))
                    .endTableScan()
                    .capturePlanNodeId(rightNodeId)
                    .planNode(),
                "",
                {"t0", "t1", "u0", "u1"},
                core::JoinType::kFull)
            .planNode();
    for (const auto hasBarrier : {false, true}) {
      SCOPED_TRACE(fmt::format("hasBarrier {}", hasBarrier));
      AssertQueryBuilder queryBuilder(plan, duckDbQueryRunner_);
      queryBuilder.barrierExecution(hasBarrier).serialExecution(true);
      queryBuilder.split(
          leftNodeId, makeHiveConnectorSplit(leftFile->getPath()));
      queryBuilder.split(
          rightNodeId, makeHiveConnectorSplit(rightFile->getPath()));
      queryBuilder.config(core::QueryConfig::kMaxOutputBatchRows, "32");

      const auto task = queryBuilder.assertResults(
          "SELECT t0, t1, u0, u1 FROM t FULL OUTER JOIN u ON t.t0 = u.u0");
      ASSERT_EQ(task->taskStats().numBarriers, hasBarrier ? 1 : 0);
      ASSERT_EQ(task->taskStats().numFinishedSplits, 2);
    }
  }

  {
    // Right join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftNodeId;
    core::PlanNodeId rightNodeId;
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .startTableScan()
            .outputType(std::static_pointer_cast<const RowType>(left->type()))
            .endTableScan()
            .capturePlanNodeId(leftNodeId)
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator)
                    .startTableScan()
                    .outputType(
                        std::static_pointer_cast<const RowType>(right->type()))
                    .endTableScan()
                    .capturePlanNodeId(rightNodeId)
                    .planNode(),
                "",
                {"t0", "t1", "u0", "u1"},
                core::JoinType::kRight)
            .planNode();
    for (const auto hasBarrier : {false, true}) {
      SCOPED_TRACE(fmt::format("hasBarrier {}", hasBarrier));
      AssertQueryBuilder queryBuilder(plan, duckDbQueryRunner_);
      queryBuilder.barrierExecution(hasBarrier).serialExecution(true);
      queryBuilder.split(
          leftNodeId, makeHiveConnectorSplit(leftFile->getPath()));
      queryBuilder.split(
          rightNodeId, makeHiveConnectorSplit(rightFile->getPath()));
      queryBuilder.config(core::QueryConfig::kMaxOutputBatchRows, "32");

      const auto task = queryBuilder.assertResults(
          "SELECT t0, t1, u0, u1 FROM t RIGHT JOIN u ON t.t0 = u.u0");
      ASSERT_EQ(task->taskStats().numBarriers, hasBarrier ? 1 : 0);
      ASSERT_EQ(task->taskStats().numFinishedSplits, 2);
    }
  }

  {
    // Left join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftNodeId;
    core::PlanNodeId rightNodeId;
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .startTableScan()
            .outputType(std::static_pointer_cast<const RowType>(left->type()))
            .endTableScan()
            .capturePlanNodeId(leftNodeId)
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator)
                    .startTableScan()
                    .outputType(
                        std::static_pointer_cast<const RowType>(right->type()))
                    .endTableScan()
                    .capturePlanNodeId(rightNodeId)
                    .planNode(),
                "",
                {"t0", "t1", "u0", "u1"},
                core::JoinType::kLeft)
            .planNode();
    for (const auto hasBarrier : {true}) {
      SCOPED_TRACE(fmt::format("hasBarrier {}", hasBarrier));
      AssertQueryBuilder queryBuilder(plan, duckDbQueryRunner_);
      queryBuilder.barrierExecution(hasBarrier).serialExecution(true);
      queryBuilder.split(
          leftNodeId, makeHiveConnectorSplit(leftFile->getPath()));
      queryBuilder.split(
          rightNodeId, makeHiveConnectorSplit(rightFile->getPath()));
      queryBuilder.config(core::QueryConfig::kMaxOutputBatchRows, "32");

      const auto task = queryBuilder.assertResults(
          "SELECT t0, t1, u0, u1 FROM t LEFT JOIN u ON t.t0 = u.u0");
      ASSERT_EQ(task->taskStats().numBarriers, hasBarrier ? 1 : 0);
      ASSERT_EQ(task->taskStats().numFinishedSplits, hasBarrier ? 2 : 1);
    }
  }

  {
    // Anti join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    core::PlanNodeId leftNodeId;
    core::PlanNodeId rightNodeId;
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .startTableScan()
            .outputType(std::static_pointer_cast<const RowType>(left->type()))
            .endTableScan()
            .capturePlanNodeId(leftNodeId)
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator)
                    .startTableScan()
                    .outputType(
                        std::static_pointer_cast<const RowType>(right->type()))
                    .endTableScan()
                    .capturePlanNodeId(rightNodeId)
                    .planNode(),
                "",
                {"t0", "t1"},
                core::JoinType::kAnti)
            .planNode();
    for (const auto hasBarrier : {true}) {
      SCOPED_TRACE(fmt::format("hasBarrier {}", hasBarrier));
      AssertQueryBuilder queryBuilder(plan, duckDbQueryRunner_);
      queryBuilder.barrierExecution(hasBarrier).serialExecution(true);
      queryBuilder.split(
          leftNodeId, makeHiveConnectorSplit(leftFile->getPath()));
      queryBuilder.split(
          rightNodeId, makeHiveConnectorSplit(rightFile->getPath()));
      queryBuilder.config(core::QueryConfig::kMaxOutputBatchRows, "32");

      const auto task = queryBuilder.assertResults(
          "SELECT t0, t1 FROM t WHERE NOT exists (select u0, u1 from u where t0 = u0)");
      ASSERT_EQ(task->taskStats().numBarriers, hasBarrier ? 1 : 0);
      ASSERT_EQ(task->taskStats().numFinishedSplits, hasBarrier ? 2 : 1);
    }
  }
}

TEST_F(MergeJoinTest, antiJoinWithFilterWithMultiMatchedRows) {
  auto left = makeRowVector({"t0"}, {makeNullableFlatVector<int64_t>({1, 2})});

  auto right =
      makeRowVector({"u0"}, {makeNullableFlatVector<int64_t>({1, 2, 2, 2})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0 AND t.t0 > 2 ) ");
}

TEST_F(MergeJoinTest, antiJoinWithTwoJoinKeysInDifferentBatch) {
  auto left = makeRowVector(
      {"a", "b"},
      {makeNullableFlatVector<int32_t>({1, 1, 1, 1}),
       makeNullableFlatVector<double>({3.0, 3.0, 3.0, 3.0})});

  auto right = makeRowVector(
      {"c", "d"},
      {makeNullableFlatVector<int32_t>({1, 1, 1}),
       makeNullableFlatVector<double>({2.0, 2.0, 4.0})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan = PlanBuilder(planNodeIdGenerator)
                  .values({split(left, 2)})
                  .mergeJoin(
                      {"a"},
                      {"c"},
                      PlanBuilder(planNodeIdGenerator)
                          .values({split(right, 2)})
                          .planNode(),
                      "b < d",
                      {"a", "b"},
                      core::JoinType::kAnti)
                  .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t WHERE NOT exists (select * from u where t.a = u.c and t.b < u.d)");
}
