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

add_executable(
  velox_plan_conversion_test
  FunctionTest.cpp
  JsonToProtoConverter.cpp
  Substrait2VeloxPlanConversionTest.cpp
  Substrait2VeloxValuesNodeConversionTest.cpp
  SubstraitExtensionCollectorTest.cpp
  VeloxSubstraitRoundTripTest.cpp
  VeloxToSubstraitTypeTest.cpp
  VeloxSubstraitSignatureTest.cpp)

add_dependencies(velox_plan_conversion_test velox_substrait_plan_converter)

add_test(
  NAME velox_plan_conversion_test
  COMMAND velox_plan_conversion_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  velox_plan_conversion_test
  velox_substrait_plan_converter
  velox_functions_test_lib
  velox_exec
  velox_dwio_common
  velox_aggregates
  velox_functions_aggregates_test_lib
  velox_functions_lib
  velox_functions_prestosql
  velox_hive_connector
  velox_type
  velox_serialization
  velox_exec_test_lib
  velox_functions_json
  velox_vector
  velox_memory
  velox_dwio_common_exception
  Boost::atomic
  Boost::context
  Boost::date_time
  Boost::filesystem
  Boost::program_options
  Boost::regex
  Boost::thread
  Boost::system
  GTest::gtest
  GTest::gtest_main
  Folly::folly
  gflags::gflags
  glog::glog
  fmt::fmt)
