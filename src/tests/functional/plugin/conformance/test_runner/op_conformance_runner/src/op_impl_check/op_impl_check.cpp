// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "op_impl_check/op_impl_check.hpp"
#include "op_impl_check/single_op_graph.hpp"
#include "conformance.hpp"

namespace ConformanceTests {
using namespace ov::test::subgraph;

namespace {
INSTANTIATE_TEST_SUITE_P(conformance,
                         OpImplCheckTest,
                         ::testing::Combine(
                                 ::testing::ValuesIn(createFunctions()),
                                 ::testing::Values(targetDevice),
                                 ::testing::Values(std::map<std::string, std::string>())),
                         OpImplCheckTest::getTestCaseName);
} // namespace
} // namespace ConformanceTests