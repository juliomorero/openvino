// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "test_utils/cpu_test_utils.hpp"
#include "ngraph_functions/builders.hpp"
#include "shared_test_classes/base/ov_subgraph.hpp"
#include <common_test_utils/ov_tensor_utils.hpp>

using namespace InferenceEngine;
using namespace ngraph;
using namespace CPUTestUtils;
using namespace ov::test;

namespace CPULayerTestsDefinitions {
using inputShapes = std::tuple<ov::test::InputShape,              // dynamic data shape
                               std::vector<SizeVector>>;          // range input shapes

using fqSpecificParams = std::tuple<int64_t,                  // 'data' input low bounds
                                    int64_t,                  // 'data' input high bounds
                                    std::vector<float>,       // output low
                                    std::vector<float>,       // output high
                                    size_t>;                  // levels

using fqLayerTestParamsSet = std::tuple<fqSpecificParams,
                                        inputShapes,                                       // input shapes
                                        Precision,                                         // input precision
                                        std::pair<std::vector<float>, std::vector<float>>, // il and ih values
                                        bool,                                              // should be decomposed
                                        CPUSpecificParams>;

class FakeQuantizeLayerCPUTest : public testing::WithParamInterface<fqLayerTestParamsSet>,
                                 virtual public SubgraphBaseTest, public CPUTestsBase {
public:
    static std::string getTestCaseName(testing::TestParamInfo<fqLayerTestParamsSet> obj) {
        fqSpecificParams fqParams;
        inputShapes testShapes;
        Precision inPrec;
        std::pair<std::vector<float>, std::vector<float>> inputRangesValues;
        bool shouldBeDecomposed;
        CPUSpecificParams cpuParams;
        std::tie(fqParams, testShapes, inPrec, inputRangesValues, shouldBeDecomposed, cpuParams) = obj.param;

        InputShape shapes;
        std::vector<SizeVector> ranges;
        std::tie(shapes, ranges) = testShapes;

        int64_t inDataLowBounds, inDataHighBounds;
        std::vector<float> inputLow, inputHigh, outputLow, outputHigh;
        size_t levels;
        inputLow = inputRangesValues.first;
        inputHigh = inputRangesValues.second;
        std::tie(inDataLowBounds, inDataHighBounds, outputLow, outputHigh, levels) = fqParams;

        std::ostringstream result;

        result << "IS=" << CommonTestUtils::partialShape2str({shapes.first}) << "_";
        result << "TS=";
        for (const auto& shape : shapes.second) {
            result << "(" << CommonTestUtils::vec2str(shape) << ")_";
        }
        result << "RS=";
        for (const auto& data : ranges) {
            result << "(" << CommonTestUtils::vec2str(data) << ")_";
        }
        result << "inPrec=" << inPrec.name() << "_";

        result << "LOW_BOUNDS=" << inDataLowBounds << "_";
        result << "HIGH_BOUNDS=" << inDataHighBounds << "_";
        result << "IL=" << CommonTestUtils::vec2str(inputLow) << "_";
        result << "IH=" << CommonTestUtils::vec2str(inputHigh) << "_";
        result << "OL=" << CommonTestUtils::vec2str(outputLow) << "_";
        result << "OH=" << CommonTestUtils::vec2str(outputHigh) << "_";
        result << "LEVELS=" << levels;

        result << CPUTestsBase::getTestCaseName(cpuParams);

        return result.str();
    }



protected:
    std::string layerName;

    void SetUp() override {
        targetDevice = CommonTestUtils::DEVICE_CPU;
        fqSpecificParams fqParams;
        inputShapes testShapes;
        Precision inPrec;
        std::pair<std::vector<float>, std::vector<float>> inputRangesValues;
        bool shouldBeDecomposed;
        CPUSpecificParams cpuParams;
        std::tie(fqParams, testShapes, inPrec, inputRangesValues, shouldBeDecomposed, cpuParams) = this->GetParam();

        std::tie(inFmts, outFmts, priority, selectedType) = cpuParams;

        InputShape shapes;
        std::vector<SizeVector> ranges;
        std::tie(shapes, ranges) = testShapes;

        inputDynamicShapes.push_back(shapes.first);
        for (size_t i = 0; i < shapes.second.size(); i++) {
            targetStaticShapes.push_back(std::vector<ov::Shape>{shapes.second[i]});
        }

        size_t levels;
        std::vector<std::vector<float>> rangesBounds(RANGES_INPUT_NUMBER);
        rangesBounds[0] = inputRangesValues.first;
        rangesBounds[1] = inputRangesValues.second;
        std::tie(inDataLowBounds, inDataHighBounds, rangesBounds[2], rangesBounds[3], levels) = fqParams;

        auto ngInPrec = FuncTestUtils::PrecisionUtils::convertIE2nGraphPrc(inPrec);
        ParameterVector params = builder::makeDynamicParams(ngInPrec, inputDynamicShapes);
        auto paramOuts = helpers::convert2OutputVector(helpers::castOps2Nodes<opset5::Parameter>(params));

        auto il = builder::makeConstant(ngInPrec, ranges[0], rangesBounds[0], rangesBounds[0].empty());
        auto ih = builder::makeConstant(ngInPrec, ranges[1], rangesBounds[1], rangesBounds[1].empty());
        auto ol = builder::makeConstant(ngInPrec, ranges[2], rangesBounds[2], rangesBounds[2].empty());
        auto oh = builder::makeConstant(ngInPrec, ranges[3], rangesBounds[3], rangesBounds[3].empty());
        auto fq = std::make_shared<opset5::FakeQuantize>(paramOuts[0], il, ih, ol, oh, levels);

        layerName = shouldBeDecomposed ? "" : "FakeQuantize";

        if (selectedType.empty()) {
           selectedType = getPrimitiveType() + "_" + inPrec.name();
        }

        function = makeNgraphFunction(ngInPrec, params, fq, "FakeQuantizeCPU");
    }

    void generate_inputs(const std::vector<ov::Shape>& targetInputStaticShapes) override {
        inputs.clear();
        const auto& funcInputs = function->inputs();
        ASSERT_EQ(funcInputs.size(), 1);
        const auto& funcInput = funcInputs[0];
        ov::Tensor tensor;
        tensor = ov::test::utils::create_and_fill_tensor(funcInput.get_element_type(),
                                                         targetInputStaticShapes[0],
                                                         inDataHighBounds - inDataLowBounds,
                                                         inDataLowBounds);
        inputs.insert({funcInput.get_node_shared_ptr(), tensor});
    }

private:
    const size_t RANGES_INPUT_NUMBER = 4;

    int64_t inDataLowBounds, inDataHighBounds;
};

TEST_P(FakeQuantizeLayerCPUTest, CompareWithRefs) {
    run();

    CheckPluginRelatedResults(compiledModel, layerName);
}


const std::vector<size_t> levels = {16, 255, 256};

int64_t dataLowBounds{-10}, dataHighBounds{10};

const std::vector<std::pair<std::vector<float>, std::vector<float>>> input_ranges = {
    {{0.0f}, {5.f}},
    {{0.0f}, {}},
    {{-10.0f}, {-5.f}}
};

const std::vector<float> outputLow{5.0f}, outputHigh{25.0f};

const auto specificParams = ::testing::Combine(::testing::Values(dataLowBounds),
                                               ::testing::Values(dataHighBounds),
                                               ::testing::Values(outputLow),
                                               ::testing::Values(outputHigh),
                                               ::testing::ValuesIn(levels));

namespace fqImpl {

std::vector<CPUSpecificParams> memForm4D_jit = {
        CPUSpecificParams({nchw}, {nchw}, {}, {}),
        CPUSpecificParams({nhwc}, {nhwc}, {}, {}),
//        CPUSpecificParams({nChw16c}, {nChw16c}, {}, {}) comment out due to post ops optimizations in lpt plugin.cpp
};

std::vector<inputShapes> rangesShapes4D_jit = {
    inputShapes{
        InputShape{{{4, 5, 6, 7}}, {{4, 5, 6, 7}}},
        {{1, 5, 1, 1}, {1, 5, 1, 1}, {1, 5, 1, 1}, {1, 5, 1, 1}}
    },
    inputShapes{
        InputShape{{{4, 5, 6, 7}}, {{4, 5, 6, 7}}},
        {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1}, {{4, 5, 6, 7}, {1, 12, 1, 1}, {4, 1, 8, 2}, {1, 16, 6, 1}, {4, 5, 6, 7}}},
        {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1}, {{4, 16, 6, 7}, {1, 16, 1, 1}, {7, 16, 1, 2}, {1, 16, 6, 1}, {4, 16, 6, 7}}},
        {{1, 16, 1, 1}, {1, 16, 1, 1}, {1, 16, 1, 1}, {1, 16, 1, 1}}
    },
};

const auto testParams4D_jit = ::testing::Combine(specificParams,
                                                 ::testing::ValuesIn(rangesShapes4D_jit),
                                                 ::testing::Values(Precision::FP32),
                                                 ::testing::ValuesIn(input_ranges),
                                                 ::testing::Values(false),
                                                 ::testing::ValuesIn(filterCPUSpecificParams(memForm4D_jit)));
INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_4D_jit, FakeQuantizeLayerCPUTest, testParams4D_jit, FakeQuantizeLayerCPUTest::getTestCaseName);


std::vector<CPUSpecificParams> memForm4D_ref = {
        CPUSpecificParams({nchw}, {nchw}, {"ref_FP32"}, {"ref_FP32"})
};

std::vector<inputShapes> rangesShapes4D_ref = {
    inputShapes{
        InputShape{{{4, 5, 6, 7}}, {{4, 5, 6, 7}}},
        {{4, 1, 1, 1}, {4, 1, 1, 1}, {4, 1, 1, 1}, {4, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1}, {{4, 16, 6, 7}, {4, 1, 1, 1}, {4, 16, 1, 2}, {4, 16, 6, 1}, {4, 16, 6, 7}}},
        {{4, 1, 1, 1}, {4, 1, 1, 1}, {4, 1, 1, 1}, {4, 1, 1, 1}}
    },
};

const auto testParams4D_ref = ::testing::Combine(specificParams,
                                                 ::testing::ValuesIn(rangesShapes4D_ref),
                                                 ::testing::Values(Precision::FP32),
                                                 ::testing::ValuesIn(input_ranges),
                                                 ::testing::Values(false),
                                                 ::testing::ValuesIn(memForm4D_ref));
INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_4D_ref, FakeQuantizeLayerCPUTest, testParams4D_ref, FakeQuantizeLayerCPUTest::getTestCaseName);


std::vector<CPUSpecificParams> memForm5D_jit = {
        CPUSpecificParams({ncdhw}, {ncdhw}, {}, {}),
        CPUSpecificParams({ndhwc}, {ndhwc}, {}, {}),
//        CPUSpecificParams({nCdhw16c}, {nCdhw16c}, {}, {}) comment out due to post ops optimizations in lpt plugin.cpp
};

std::vector<inputShapes> rangesShapes5D_jit = {
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 4, 1, 1, 1}, {1, 4, 1, 1, 1}, {1, 4, 1, 1, 1}, {1, 4, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1, -1}, {{3, 4, 5, 6, 7}, {1, 12, 1, 1, 1}, {4, 1, 8, 2, 7}, {3, 4, 5, 6, 7}, {1, 16, 6, 5, 1}}},
        {{1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}, {1, 1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1, -1}, {{4, 16, 6, 7, 8}, {1, 16, 1, 1, 1}, {7, 16, 1, 2, 5}, {4, 16, 6, 7, 8}, {1, 16, 6, 1, 7}}},
        {{1, 16, 1, 1, 1}, {1, 16, 1, 1, 1}, {1, 16, 1, 1, 1}, {1, 16, 1, 1, 1}}
    },
};

const auto testParams5D_jit = ::testing::Combine(specificParams,
                                                 ::testing::ValuesIn(rangesShapes5D_jit),
                                                 ::testing::Values(Precision::FP32),
                                                 ::testing::ValuesIn(input_ranges),
                                                 ::testing::Values(false),
                                                 ::testing::ValuesIn(filterCPUSpecificParams(memForm5D_jit)));

INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_5D_jit, FakeQuantizeLayerCPUTest, testParams5D_jit, FakeQuantizeLayerCPUTest::getTestCaseName);


std::vector<CPUSpecificParams> memForm5D_ref = {
        CPUSpecificParams({ncdhw}, {ncdhw}, {"ref_FP32"}, {"ref_FP32"})
};

std::vector<inputShapes> rangesShapes5D_ref = {
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1, -1}, {{3, 16, 6, 7, 8}, {3, 16, 1, 1, 1}, {3, 16, 1, 2, 5}, {3, 16, 6, 1, 7}, {3, 16, 6, 7, 8}}},
        {{3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}, {3, 1, 1, 1, 1}}
    },
};

const auto testParams5D_ref = ::testing::Combine(specificParams,
                                                 ::testing::ValuesIn(rangesShapes5D_ref),
                                                 ::testing::Values(Precision::FP32),
                                                 ::testing::ValuesIn(input_ranges),
                                                 ::testing::Values(false),
                                                 ::testing::ValuesIn(memForm5D_ref));

INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_5D_ref, FakeQuantizeLayerCPUTest, testParams5D_ref, FakeQuantizeLayerCPUTest::getTestCaseName);

const auto specificParamsBin = ::testing::Combine(::testing::Values(dataLowBounds),
                                                  ::testing::Values(dataHighBounds),
                                                  ::testing::Values(std::vector<float>{0.0f}),
                                                  ::testing::Values(std::vector<float>{1.0f}),
                                                  ::testing::Values(2));

const auto testParamsBin4D = ::testing::Combine(specificParamsBin,
                                                 ::testing::ValuesIn(rangesShapes4D_jit),
                                                 ::testing::Values(Precision::FP32),
                                                 ::testing::Values(std::pair<std::vector<float>, std::vector<float>>{{3.0f}, {3.f}}),
                                                 ::testing::Values(false),
                                                 ::testing::Values(CPUSpecificParams()));

INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_4D_bin, FakeQuantizeLayerCPUTest, testParamsBin4D, FakeQuantizeLayerCPUTest::getTestCaseName);

} // namespace fqImpl

namespace fqDecompos {

std::vector<inputShapes> decomposeShapes = {
    inputShapes{
        InputShape{{4, 5, 6, 7}, {{4, 5, 6, 7}}},
        {{4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}}
    },
    inputShapes{
        InputShape{{4, 5, 6, 7}, {{4, 5, 6, 7}}},
        {{1, 5, 1, 1}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{4, 5, 6, 7}, {{4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{4, 5, 6, 7}, {{4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{4, 5, 6, 7}, {{4, 5, 6, 7}}},
        {{1, 1, 6, 1}, {1, 5, 6, 7}, {1, 1, 6, 1}, {1, 1, 6, 1}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 5, 1, 1}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{3, 4, 5, 6, 7}, {{3, 4, 5, 6, 7}}},
        {{1, 1, 6, 1}, {1, 5, 6, 7}, {1, 1, 6, 1}, {1, 1, 6, 1}}
    },
    inputShapes{
        InputShape{{2, 3, 4, 5, 6, 7}, {{2, 3, 4, 5, 6, 7}}},
        {{4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}}
    },
    inputShapes{
        InputShape{{2, 3, 4, 5, 6, 7}, {{2, 3, 4, 5, 6, 7}}},
        {{1, 5, 1, 1}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{2, 3, 4, 5, 6, 7}, {{2, 3, 4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 6, 7}}
    },
    inputShapes{
        InputShape{{2, 3, 4, 5, 6, 7}, {{2, 3, 4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
    inputShapes{
        InputShape{{2, 3, 4, 5, 6, 7}, {{2, 3, 4, 5, 6, 7}}},
        {{1, 1, 6, 1}, {1, 5, 6, 7}, {1, 1, 6, 1}, {1, 1, 6, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1}, {{4, 5, 6, 7}, {1, 5, 6, 7}, {7, 5, 6, 7}, {4, 5, 6, 7}}},
        {{1, 1, 6, 1}, {1, 5, 6, 7}, {1, 1, 6, 1}, {1, 1, 6, 1}}
    },
    inputShapes{
        InputShape{{-1, -1, -1, -1, -1}, {{8, 4, 5, 6, 7}, {1, 1, 5, 6, 7}, {1, 1, 1, 6, 7}, {8, 4, 5, 6, 7}}},
        {{1, 1, 6, 7}, {1, 1, 6, 7}, {1, 1, 1, 1}, {1, 1, 1, 1}}
    },
};

const auto testParams = ::testing::Combine(specificParams,
                                           ::testing::ValuesIn(decomposeShapes),
                                           ::testing::Values(Precision::FP32),
                                           ::testing::ValuesIn(input_ranges),
                                           ::testing::Values(true),
                                           ::testing::Values(CPUSpecificParams{}));

INSTANTIATE_TEST_SUITE_P(smoke_FakeQuantizeLayerCPUTest_Decompos, FakeQuantizeLayerCPUTest, testParams, FakeQuantizeLayerCPUTest::getTestCaseName);

} // namespace fqDecompos

} // namespace CPULayerTestsDefinitions
