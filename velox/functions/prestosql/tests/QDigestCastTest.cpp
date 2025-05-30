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

#include "velox/functions/prestosql/tests/CastBaseTest.h"
#include "velox/functions/prestosql/types/QDigestType.h"

using namespace facebook::velox;

class QDigestCastTest : public functions::test::CastBaseTest {};

TEST_F(QDigestCastTest, toQDigest) {
  auto test = [&](const TypePtr& parameterType) {
    testCast<StringView, StringView>(
        VARBINARY(),
        QDIGEST(parameterType),
        {"aaa"_sv, ""_sv, std::nullopt},
        {"aaa"_sv, ""_sv, std::nullopt});
    testCast<StringView, StringView>(
        VARBINARY(),
        QDIGEST(parameterType),
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  };
  test(BIGINT());
  test(REAL());
  test(DOUBLE());
}

TEST_F(QDigestCastTest, fromQDigest) {
  auto test = [&](const TypePtr& parameterType) {
    testCast<StringView, StringView>(
        QDIGEST(parameterType),
        VARBINARY(),
        {"aaa"_sv, ""_sv, std::nullopt},
        {"aaa"_sv, ""_sv, std::nullopt});
    testCast<StringView, StringView>(
        QDIGEST(parameterType),
        VARBINARY(),
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  };
  test(BIGINT());
  test(REAL());
  test(DOUBLE());
}
