// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include <common/blocked_desc_creator.h>
#include <cpu_types.h>
#include <edge.h>
#include <gtest/gtest.h>
#include <ie_common.h>
#include <memory_desc/cpu_memory_desc_utils.h>
#include <memory_desc/dnnl_memory_desc.h>
#include <node.h>
#include <nodes/reorder.h>

#include <common/memory_desc_wrapper.hpp>
#include <dnnl.hpp>
#include <utility>

#include "../../../ie_test_utils/common_test_utils/common_utils.hpp"
#include "cache/multi_cache.h"
#include "nodes/input.h"

using namespace InferenceEngine;
using namespace ov::intel_cpu;
namespace ReorderCPUTest {
inline void checkReorder(const ov::intel_cpu::Memory& inputMemory,
                         const ov::intel_cpu::Memory& outputMemory,
                         const InferenceEngine::Precision& prescision) {
    auto srcData = inputMemory.GetData();
    auto dstData = outputMemory.GetData();
    auto mdInput = inputMemory.GetDescWithType<DnnlMemoryDesc>()->getDnnlDesc();
    auto mdOutput = outputMemory.GetDescWithType<DnnlMemoryDesc>()->getDnnlDesc();

    const dnnl::impl::memory_desc_wrapper mdwInput(mdInput.data);
    const dnnl::impl::memory_desc_wrapper mdwOutput(mdOutput.data);
    auto nelems = mdwInput.nelems();

    for (size_t i = 0; i < nelems; ++i) {
        auto srcOffset = mdwInput.off_l(i, false);
        auto dstOffset = mdwOutput.off_l(i, false);
        switch (prescision) {
        case InferenceEngine::Precision::FP32: {
            auto s = *(static_cast<float*>(srcData) + srcOffset);
            auto d = *(static_cast<float*>(dstData) + dstOffset);
            ASSERT_EQ(s, d) << "mismatch at position " << i;
            break;
        }
        case InferenceEngine::Precision::I8: {
            auto s = *(static_cast<int8_t*>(srcData) + srcOffset);
            auto d = *(static_cast<int8_t*>(dstData) + dstOffset);
            ASSERT_EQ(s, d) << "mismatch at position " << i;
            break;
        }
        default:
            FAIL() << "Unsupported data precision in the test" << prescision.name();
        }
    }
}

inline std::string layoutName(const LayoutType& layout) {
    if (layout == LayoutType::nspc)
        return "nspc";
    if (layout == LayoutType::ncsp)
        return "ncsp";
    if (layout == LayoutType::nCsp8c)
        return "nCsp8c";
    if (layout == LayoutType::nCsp16c)
        return "nCsp16c";
    return "Unsupported layout type";
}

inline void fillData(const ov::intel_cpu::Memory& inputMemory, const InferenceEngine::Precision& prec) {
    ov::intel_cpu::DnnlMemoryDescPtr dnnlMdInput = inputMemory.GetDescWithType<DnnlMemoryDesc>();
    const dnnl::impl::memory_desc_wrapper mdInput{dnnlMdInput->getDnnlDesc().data};
    auto elemNum = mdInput.nelems();
    auto inputReorderData = inputMemory.GetData();
    switch (prec) {
    case InferenceEngine::Precision::FP32:
        for (size_t i = 0; i < elemNum; ++i)
            *(static_cast<float*>(inputReorderData) + mdInput.off_l(i, false)) = static_cast<float>(i);
        break;
    case InferenceEngine::Precision::I8:
        for (size_t i = 0; i < elemNum; ++i)
            *(static_cast<int8_t*>(inputReorderData) + mdInput.off_l(i, false)) = static_cast<int8_t>(i);
        break;
    default:
        FAIL() << "Unsupported data precision in the test" << prec.name();
    }
}
struct ReorderCustomImplTestParamSet {
    // logical dimension of input
    std::vector<size_t> srcDims;
    bool isNspc2Ncsp;
    uint32_t strideFactor;
    InferenceEngine::Precision prec;
    size_t stridedAxis;
};

struct ReorderCPUTestParamSet {
    ngraph::PartialShape inputPartialShape;
    // logical dimension vector  of input
    std::vector<std::vector<size_t>> inputShapes;
    LayoutType srcLayout;
    LayoutType dstLayout;
    InferenceEngine::Precision prec;
};

class ReorderCPUTestGraph {
public:
    void buildReorderGraph(const ov::intel_cpu::CpuBlockedMemoryDesc& inputDesc,
                    const ov::intel_cpu::CpuBlockedMemoryDesc& outputDesc) {
        Config conf;
        conf.rtCacheCapacity = 100;
        auto context = std::make_shared<GraphContext>(conf,
                                                      nullptr,
                                                      std::make_shared<WeightsSharing>(),
                                                      std::make_shared<std::mutex>(),
                                                      false);
        const dnnl::engine cpuEngine = context->getEngine();

        inputNode = std::make_shared<ov::intel_cpu::node::Input>(inputDesc.clone(),
                                                                      "Reorder_Input",
                                                                      "Parameter",
                                                                      context);
        reorderNode = std::make_shared<ov::intel_cpu::node::Reorder>("Reorder", context);
        outputNode = std::make_shared<ov::intel_cpu::node::Input>(outputDesc.clone(),
                                                                       "Reorder_Output",
                                                                       "Result",
                                                                       context);

        parentEdge = std::make_shared<ov::intel_cpu::Edge>(inputNode, reorderNode, 0, 0);
        childEdge = std::make_shared<ov::intel_cpu::Edge>(reorderNode, outputNode, 0, 0);
        parentEdge->changeStatus(ov::intel_cpu::Edge::Status::NeedAllocation);
        childEdge->changeStatus(ov::intel_cpu::Edge::Status::NeedAllocation);
        reorderNode->addEdge(parentEdge);
        reorderNode->addEdge(childEdge);

        auto parentMemory = std::make_shared<ov::intel_cpu::Memory>(cpuEngine);
        auto childMemory = std::make_shared<ov::intel_cpu::Memory>(cpuEngine);
        parentMemory->Create(inputDesc, nullptr);
        childMemory->Create(outputDesc, nullptr);

        parentEdge->reuse(parentMemory);
        childEdge->reuse(childMemory);

        reorderNode->setDescs(inputDesc, outputDesc);
        std::array<std::shared_ptr<ov::intel_cpu::Node>, 3> nodes{inputNode, reorderNode, outputNode};
        for (auto& n : nodes) {
            n->init();
            n->getSupportedDescriptors();
            n->initSupportedPrimitiveDescriptors();
            n->selectPrimitiveDescriptorByIndex(0);
        }
        stream = dnnl::stream{cpuEngine};
    }

protected:
    dnnl::stream stream;
    std::shared_ptr<ov::intel_cpu::node::Input> inputNode;
    std::shared_ptr<ov::intel_cpu::node::Reorder> reorderNode;
    std::shared_ptr<ov::intel_cpu::node::Input> outputNode;
    std::shared_ptr<ov::intel_cpu::Edge> parentEdge;
    std::shared_ptr<ov::intel_cpu::Edge> childEdge;
    InferenceEngine::Precision prec;
};

}// namespace ReorderCPUTest

using namespace ReorderCPUTest;

/*
 * Test Reorder::optimizedNcsp2Nspc() and Reorder::optimizedNspc2Ncsp() for
 * inPlace and non-inPlace cases. Specifically, the test checks that dst batch strides are
 * correctly taken into account by the custom impls (the case when the reorder is followed by an inplace concat).
 */
class ReorderCustomizedStrideTest : public ::testing::Test,
                                    public ::testing::WithParamInterface<ReorderCustomImplTestParamSet>,
                                    public ::ReorderCPUTest::ReorderCPUTestGraph {
public:
    static std::string getTestCaseName(const testing::TestParamInfo<ReorderCustomImplTestParamSet>& obj) {
        ReorderCustomImplTestParamSet p = obj.param;
        std::ostringstream result;
        result << "IS:(";
        result << CommonTestUtils::vec2str(p.srcDims);
        result << (p.isNspc2Ncsp ? "_NSPC2NCSP" : "_NCSP2NSPC");
        result << "_InputDataType:" << p.prec.name();
        result << "_OutputDataType:" << p.prec.name();
        result << "_StrideFactor:" << p.strideFactor;
        result << "_StridedLogicChannelIndice:" << p.stridedAxis;
        result << ")";
        return result.str();
    }

    void Run() {
        buildCustomizedReorderGraph();
        infer();
        validate();
    }

protected:
    void SetUp() override {
        ReorderCustomImplTestParamSet p = ::testing::TestWithParam<ReorderCustomImplTestParamSet>::GetParam();
        srcDims = p.srcDims;

        if (p.isNspc2Ncsp) {
            // The custom NSPC2NCSP  impl is used only if an input shape complies with:
            ASSERT_TRUE(srcDims[1] <= 64 && srcDims[1] >= 16 && (getNumElems(srcDims) / srcDims[1]) >= 128);
            // The custom NSPC2NCSP impl is used only for FP32
            prec = InferenceEngine::Precision::FP32;
            srcOrder = std::vector<size_t>{0, 2, 3, 1};
            dstOrder = std::vector<size_t>{0, 1, 2, 3};
        } else {
            ASSERT_LE(getNumElems(srcDims), 256);
            srcOrder = std::vector<size_t>{0, 1, 2, 3};
            dstOrder = std::vector<size_t>{0, 2, 3, 1};
            // The custom NSPC2NCSP  impl is used only for U8
            prec = InferenceEngine::Precision::I8;
        }
        dstDims = srcDims;
        // Create strided dst layout for the inPlace case,
        // For example: If need channel axis stride changes, need to set the height axis dimension.
        dstDims[p.stridedAxis + 1] *= p.strideFactor;
    }

    void buildCustomizedReorderGraph() {
        auto getBlockedDims = [](const std::vector<size_t>& dims, const std::vector<size_t>& order) {
            std::vector<size_t> result;
            result.reserve(order.size());
            for (auto i : order)
                result.push_back(dims[i]);
            return result;
        };
        auto getStrides = [](const std::vector<size_t>& dims) {
            std::vector<size_t> result(dims.size());
            result[dims.size() - 1] = 1;
            for (int i = dims.size() - 2; i >= 0; --i) {
                result[i] = result[i + 1] * dims[i + 1];
            }
            return result;
        };
        const std::vector<size_t> srcBlockedDims = getBlockedDims(srcDims, srcOrder);
        const std::vector<size_t> srcStrides = getStrides(srcBlockedDims);
        const std::vector<size_t> offsetPaddingToData(srcDims.size(), 0);
        const std::vector<size_t> dstBlockedDims = getBlockedDims(dstDims, dstOrder);
        const std::vector<size_t> dstStrides = getStrides(dstBlockedDims);

        const ov::intel_cpu::CpuBlockedMemoryDesc inputDesc(prec,
                                                            ov::intel_cpu::Shape(srcDims),
                                                            srcBlockedDims,
                                                            srcOrder,
                                                            0,
                                                            offsetPaddingToData,
                                                            srcStrides);

        const ov::intel_cpu::CpuBlockedMemoryDesc outputDesc(prec,
                                                             ov::intel_cpu::Shape(srcDims),
                                                             getBlockedDims(srcDims, dstOrder),
                                                             dstOrder,
                                                             0,
                                                             offsetPaddingToData,
                                                             dstStrides);
        buildReorderGraph(inputDesc, outputDesc);
    }

    void infer() {
        generateInput();
        reorderNode->createPrimitive();
        reorderNode->execute(stream);
    }

    void validate(void) {
        checkReorder(parentEdge->getMemory(), childEdge->getMemory(), prec);
    }

    // Fill srcData so that the results of NSPC2NCSP and NCSP2NSPC reorders are incremental numbers 0,1,2,...
    // Fill dstData with zeros
    void generateInput() {
        fillData(parentEdge->getMemory(), prec);
        memset(childEdge->getMemory().GetData(), 0, childEdge->getMemory().GetSize());
    }

    size_t getNumElems(const std::vector<size_t>& dims) {
        size_t result = 1;
        for (auto d : dims)
            result *= d;
        return result;
    }

private:
    std::vector<size_t> srcDims;
    std::vector<size_t> srcOrder;
    std::vector<size_t> dstDims;
    std::vector<size_t> dstOrder;
};

TEST_P(ReorderCustomizedStrideTest, OutputIsStrided) {
    Run();
}

const auto stridedParameter =
    ::testing::Values(ReorderCustomImplTestParamSet{{2, 16, 8, 8}, true, 2, InferenceEngine::Precision::FP32, 0},
                      ReorderCustomImplTestParamSet{{2, 16, 8, 8}, true, 4, InferenceEngine::Precision::FP32, 1},
                      ReorderCustomImplTestParamSet{{2, 16, 8, 8}, true, 3, InferenceEngine::Precision::FP32, 1},
                      ReorderCustomImplTestParamSet{{2, 16, 8, 8}, true, 1, InferenceEngine::Precision::FP32, 2},
                      ReorderCustomImplTestParamSet{{2, 8, 4, 4}, false, 2, InferenceEngine::Precision::I8, 0},
                      ReorderCustomImplTestParamSet{{2, 8, 4, 4}, false, 5, InferenceEngine::Precision::I8, 1},
                      ReorderCustomImplTestParamSet{{2, 8, 4, 4}, false, 1, InferenceEngine::Precision::I8, 2});

INSTANTIATE_TEST_SUITE_P(smoke_ReorderTestCustomStrideWithFactor,
                         ReorderCustomizedStrideTest,
                         stridedParameter,
                         ReorderCustomizedStrideTest::getTestCaseName);

/*
 * ReorderCPUTest to test the CPU plugin-in dynamism and RT cache
 */
class ReorderDynamismCPUTest : public ::testing::Test,
                       public ::testing::WithParamInterface<ReorderCPUTestParamSet>,
                       public ::ReorderCPUTest::ReorderCPUTestGraph {
public:
    static std::string getTestCaseName(const testing::TestParamInfo<ReorderCPUTestParamSet>& obj) {
        ReorderCPUTestParamSet p = obj.param;
        std::ostringstream result;
        result << "IS:(";
        result << "InputPartialShape:" << CommonTestUtils::partialShape2str({p.inputPartialShape});
        for (const auto inputShape : p.inputShapes) {
            result << CommonTestUtils::vec2str(inputShape);
        }
        result << "_InputLayoutType:" << layoutName(p.srcLayout) << ".";
        result << "_OutputLayoutType:" << layoutName(p.dstLayout) << ".";
        result << "_InputDataType:" << p.prec.name();
        result << "_OutputDataType:" << p.prec.name();
        result << ")";
        return result.str();
    }

    void Run() {
        for (auto inputshape : inputShapes) {
            generate_inputs(inputshape);
            infer();
            validate();
        }
    }

protected:
    void generate_inputs(const std::vector<size_t>& inputShape) {
        parentEdge->getParent()->redefineOutputMemory({inputShape});
        fillData(parentEdge->getMemory(), prec);
    }
    void infer() {
        reorderNode->updateShapes();
        reorderNode->updateDynamicParams();
        reorderNode->executeDynamic(stream);
    }
    void validate(void) {
        checkReorder(parentEdge->getMemory(), childEdge->getMemory(), prec);
    }

    struct BuildReorderParams {
        ov::intel_cpu::Shape srcShape;
        ov::intel_cpu::Shape dstShape;
        LayoutType srcLayout;
        LayoutType dstLayout;
    };

    void SetUp() override {
        ReorderCPUTestParamSet reorderTestParam = this->GetParam();
        BuildReorderParams reorderParams;
        reorderParams.srcLayout = reorderTestParam.srcLayout;
        reorderParams.dstLayout = reorderTestParam.dstLayout;
        reorderParams.srcShape = ov::intel_cpu::Shape(reorderTestParam.inputPartialShape);
        reorderParams.dstShape = reorderParams.srcShape;
        inputShapes = reorderTestParam.inputShapes;
        prec = reorderTestParam.prec;

        buildReorderDynamismGraph(reorderParams);
    }

    void buildReorderDynamismGraph(const BuildReorderParams& reorderParams) {
        BlockedDescCreator::CreatorsMap blockCreatorMap = BlockedDescCreator::getCommonCreators();
        auto srcBlockedDescCreator = blockCreatorMap[reorderParams.srcLayout];
        auto dstBlockedDescCreator = blockCreatorMap[reorderParams.dstLayout];

        const ov::intel_cpu::CpuBlockedMemoryDesc inputDesc =
            srcBlockedDescCreator->createDesc(prec, reorderParams.srcShape);

        const ov::intel_cpu::CpuBlockedMemoryDesc outputDesc =
            dstBlockedDescCreator->createDesc(prec, reorderParams.dstShape);

        buildReorderGraph(inputDesc, outputDesc);
    }

private:
    std::vector<std::vector<size_t>> inputShapes;
};

TEST_P(ReorderDynamismCPUTest, CompareResult) {
    Run();
}

const auto reorderCpuTestDynamismParams =
    ::testing::Values(ReorderCPUTestParamSet{{2, 16, 8, -1},
                                             {{2, 16, 8, 8}, {2, 16, 8, 16}, {2, 16, 8, 8}},
                                             LayoutType::nspc,
                                             LayoutType::ncsp,
                                             InferenceEngine::Precision::FP32},
                      ReorderCPUTestParamSet{{-1, -1, -1, -1},
                                             {{2, 8, 4, 4}, {2, 8, 8, 4}, {2, 8, 4, 4}},
                                             LayoutType::ncsp,
                                             LayoutType::nspc,
                                             InferenceEngine::Precision::FP32},
                      ReorderCPUTestParamSet{{2, 32, -1, 4},
                                             {{2, 32, 3, 4}, {2, 32, 6, 4}, {2, 32, 3, 4}},
                                             LayoutType::ncsp,
                                             LayoutType::nCsp8c,
                                             InferenceEngine::Precision::FP32},
                      ReorderCPUTestParamSet{{-1, 32, -1, -1},
                                             {{2, 32, 3, 4}, {2, 32, 6, 4}, {2, 32, 3, 4}},
                                             LayoutType::nCsp16c,
                                             LayoutType::nspc,
                                             InferenceEngine::Precision::I8});

INSTANTIATE_TEST_SUITE_P(smoke_ReorderTestDynamism,
                         ReorderDynamismCPUTest,
                         reorderCpuTestDynamismParams,
                         ReorderDynamismCPUTest::getTestCaseName);
