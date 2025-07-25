# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

velox_add_library(
  velox_aggregates
  ApproxDistinctAggregate.cpp
  ApproxMostFrequentAggregate.cpp
  ApproxPercentileAggregate.cpp
  ArbitraryAggregate.cpp
  ArrayAggAggregate.cpp
  AverageAggregate.cpp
  BitwiseAggregates.cpp
  BitwiseXorAggregate.cpp
  BoolAggregates.cpp
  CentralMomentsAggregates.cpp
  CountAggregate.cpp
  CountIfAggregate.cpp
  CovarianceAggregates.cpp
  ChecksumAggregate.cpp
  ClassificationAggregation.cpp
  EntropyAggregates.cpp
  GeometricMeanAggregate.cpp
  HistogramAggregate.cpp
  MapAggAggregate.cpp
  MapUnionAggregate.cpp
  MapUnionSumAggregate.cpp
  MaxSizeForStatsAggregate.cpp
  MergeAggregate.cpp
  MinMaxAggregates.cpp
  MaxByAggregate.cpp
  MinByAggregate.cpp
  MultiMapAggAggregate.cpp
  PrestoHasher.cpp
  QDigestAggAggregate.cpp
  ReduceAgg.cpp
  RegisterAggregateFunctions.cpp
  NoisyAvgGaussianAggregate.cpp
  NoisyCountIfGaussianAggregate.cpp
  NoisyCountGaussianAggregate.cpp
  NoisyHelperFunctionFactory.cpp
  NoisySumGaussianAggregate.cpp
  NoisyApproxSfmAggregate.cpp
  SetAggregates.cpp
  SumAggregate.cpp
  SumDataSizeForStatsAggregate.cpp
  TDigestAggregate.cpp
  VarianceAggregates.cpp)

velox_link_libraries(
  velox_aggregates
  velox_common_hyperloglog
  velox_exec
  velox_expression
  velox_presto_serializer
  velox_presto_types
  velox_functions_aggregates
  velox_functions_lib
  velox_functions_util
  velox_presto_types
  velox_functions_prestosql_aggregates_sfm
  Folly::folly)

add_subdirectory(sfm)

if(${VELOX_BUILD_TESTING})
  add_subdirectory(tests)
endif()

if(${VELOX_ENABLE_BENCHMARKS})
  add_subdirectory(benchmarks)
endif()
