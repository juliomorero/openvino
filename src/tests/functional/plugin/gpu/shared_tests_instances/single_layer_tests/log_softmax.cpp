// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <vector>

#include "single_layer_tests/log_softmax.hpp"
#include "common_test_utils/test_constants.hpp"

using namespace LayerTestsDefinitions;

namespace {

const std::vector<InferenceEngine::Precision> netPrecisions = {
    InferenceEngine::Precision::FP32,
};

const std::vector<InferenceEngine::SizeVector> inputShapes2D = {
    InferenceEngine::SizeVector {1, 100},
    InferenceEngine::SizeVector {100, 1},
    InferenceEngine::SizeVector {10, 10},
};

const std::vector<int64_t> axis2D = {
    -1, 1
};

INSTANTIATE_TEST_SUITE_P(smoke_LogSoftmax2D,
                         LogSoftmaxLayerTest,
                         testing::Combine(testing::ValuesIn(netPrecisions),
                                          testing::Values(InferenceEngine::Precision::UNSPECIFIED),
                                          testing::Values(InferenceEngine::Precision::UNSPECIFIED),
                                          testing::Values(InferenceEngine::Layout::ANY),
                                          testing::Values(InferenceEngine::Layout::ANY),
                                          testing::ValuesIn(inputShapes2D),
                                          testing::ValuesIn(axis2D),
                                          testing::Values(CommonTestUtils::DEVICE_GPU),
                                          testing::Values(std::map<std::string, std::string>())),
                         LogSoftmaxLayerTest::getTestCaseName);

const std::vector<InferenceEngine::SizeVector> inputShapes4D = {
    InferenceEngine::SizeVector {1, 100, 1, 1},
    InferenceEngine::SizeVector {1, 3, 4, 3},
    InferenceEngine::SizeVector {2, 3, 4, 5},
};

const std::vector<int64_t> axis4D = {
    -3, -2, -1, 1, 2, 3
};

INSTANTIATE_TEST_SUITE_P(smoke_LogSoftmax4D,
                         LogSoftmaxLayerTest,
                         testing::Combine(testing::ValuesIn(netPrecisions),
                                          testing::Values(InferenceEngine::Precision::UNSPECIFIED),
                                          testing::Values(InferenceEngine::Precision::UNSPECIFIED),
                                          testing::Values(InferenceEngine::Layout::ANY),
                                          testing::Values(InferenceEngine::Layout::ANY),
                                          testing::ValuesIn(inputShapes4D),
                                          testing::ValuesIn(axis4D),
                                          testing::Values(CommonTestUtils::DEVICE_GPU),
                                          testing::Values(std::map<std::string, std::string>())),
                         LogSoftmaxLayerTest::getTestCaseName);

}  // namespace
