// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "behavior/ov_plugin/caching_tests.hpp"
#include <ov_ops/nms_ie_internal.hpp>
#include "ov_api_conformance_helpers.hpp"

namespace {
using namespace ov::test::behavior;
using namespace ov::test::conformance;
using namespace ngraph;

static const std::vector<ov::element::Type> ovElemTypesTemplate = {
        ov::element::f64,
        ov::element::f32,
        ov::element::f16,
        ov::element::i64,
        ov::element::i32,
        ov::element::i16,
        ov::element::i8,
        ov::element::u64,
        ov::element::u32,
        ov::element::u16,
        ov::element::u8,
        ov::element::boolean,
};

static const std::vector<std::size_t> ovBatchSizesTemplate = {
        1, 2
};

static const std::vector<ov::element::Type> ovElemAnyNumericTypesTemplate(ovElemTypesTemplate.begin(),
                                                                          ovElemTypesTemplate.end() - 1);

static const std::vector<ov::element::Type> ovElemAnyFloatingPointTypesTemplate(ovElemTypesTemplate.begin(),
                                                                                ovElemTypesTemplate.begin() + 3);


INSTANTIATE_TEST_SUITE_P(ov_plugin, CompileModelCacheTestBase,
                         ::testing::Combine(
                                 ::testing::ValuesIn(CompileModelCacheTestBase::getAnyTypeOnlyFunctions()),
                                 ::testing::ValuesIn(ovElemTypesTemplate),
                                 ::testing::ValuesIn(ovBatchSizesTemplate),
                                 ::testing::ValuesIn(return_all_possible_device_combination()),
                                 ::testing::Values(ov::AnyMap{})),
                         CompileModelCacheTestBase::getTestCaseName);

// Convolution/UnaryElementwiseArithmetic/BinaryElementwiseArithmetic is not supported boolean elemnt type
INSTANTIATE_TEST_SUITE_P(ov_plugin_numeric, CompileModelCacheTestBase,
                         ::testing::Combine(
                                 ::testing::ValuesIn(CompileModelCacheTestBase::getNumericTypeOnlyFunctions()),
                                 ::testing::ValuesIn(ovElemAnyNumericTypesTemplate),
                                 ::testing::ValuesIn(ovBatchSizesTemplate),
                                 ::testing::ValuesIn(return_all_possible_device_combination()),
                                 ::testing::Values(ov::AnyMap{})),
                         CompileModelCacheTestBase::getTestCaseName);

// LSTMcell supported floating-point element type
INSTANTIATE_TEST_SUITE_P(ov_plugin_floating_point, CompileModelCacheTestBase,
                         ::testing::Combine(
                                 ::testing::ValuesIn(CompileModelCacheTestBase::getFloatingPointOnlyFunctions()),
                                 ::testing::ValuesIn(ovElemAnyFloatingPointTypesTemplate),
                                 ::testing::ValuesIn(ovBatchSizesTemplate),
                                 ::testing::ValuesIn(return_all_possible_device_combination()),
                                 ::testing::Values(ov::AnyMap{})),
                         CompileModelCacheTestBase::getTestCaseName);

} // namespace
