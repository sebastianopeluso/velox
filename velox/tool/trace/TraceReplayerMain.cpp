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

#include "velox/tool/trace/TraceReplayRunner.h"

#include <folly/init/Init.h>
#include <gflags/gflags.h>

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  facebook::velox::tool::trace::TraceReplayRunner runner;
  runner.init();
  runner.run();
  return 0;
}
