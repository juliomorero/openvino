// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <string>
#include <vector>

#include "shared_test_classes/base/ov_subgraph.hpp"
#include "ngraph/function.hpp"
#include "ngraph_functions/subgraph_builders.hpp"
#include "functional_test_utils/plugin_cache.hpp"
#include "common_test_utils/unicode_utils.hpp"
#include "openvino/util/common_util.hpp"
#include "base/ov_behavior_test_utils.hpp"

#include <ie_core.hpp>
#include <ie_common.h>

namespace ov {
namespace test {
namespace behavior {

using ovModelGenerator = std::function<std::shared_ptr<ov::Model>(ov::element::Type, std::size_t)>;
using ovModelWithName = std::tuple<ovModelGenerator, std::string>;

using compileModelCacheParams = std::tuple<
        ovModelWithName,        // openvino model with friendly name
        ov::element::Type,      // element type
        size_t,                 // batch size
        std::string,            // device name
        ov::AnyMap              // device configuration
>;

using ovModelIS = std::function<std::shared_ptr<ov::Model>(std::vector<size_t> inputShape,
                                                                    ov::element::Type_t type)>;

class CompileModelCacheTestBase : public testing::WithParamInterface<compileModelCacheParams>,
                                  virtual public SubgraphBaseTest,
                                  virtual public OVPluginTestBase {
    std::string m_cacheFolderName;
    std::string m_functionName;
    ov::element::Type m_precision;
    size_t m_batchSize;

public:
    static std::string getTestCaseName(testing::TestParamInfo<compileModelCacheParams> obj);

    void SetUp() override;
    void TearDown() override;
    void run() override;

    bool importExportSupported(ov::Core &core) const;

    // Wrapper of most part of available builder functions
    static ovModelGenerator inputShapeWrapper(ovModelIS fun, std::vector<size_t> inputShape);
    // Default functions and precisions that can be used as test parameters
    static std::vector<ovModelWithName> getAnyTypeOnlyFunctions();
    static std::vector<ovModelWithName> getNumericTypeOnlyFunctions();
    static std::vector<ovModelWithName> getNumericAnyTypeFunctions();
    static std::vector<ovModelWithName> getFloatingPointOnlyFunctions();
    static std::vector<ovModelWithName> getStandardFunctions();
};

using compileModelLoadFromFileParams = std::tuple<
        std::string,            // device name
        ov::AnyMap              // device configuration
>;
class CompileModelLoadFromFileTestBase : public testing::WithParamInterface<compileModelLoadFromFileParams>,
                                  virtual public SubgraphBaseTest,
                                  virtual public OVPluginTestBase {
    std::string m_cacheFolderName;
    std::string m_modelName;
    std::string m_weightsName;

public:
    static std::string getTestCaseName(testing::TestParamInfo<compileModelLoadFromFileParams> obj);

    void SetUp() override;
    void TearDown() override;
    void run() override;
};

using compileModelLoadFromMemoryParams = std::tuple<std::string,  // device name
                                                    ov::AnyMap    // device configuration
>;
class CompileModelLoadFromMemoryTestBase : public testing::WithParamInterface<compileModelLoadFromMemoryParams>,
                                           virtual public SubgraphBaseTest,
                                           virtual public OVPluginTestBase {
    std::string m_cacheFolderName;
    std::string m_modelName;
    std::string m_weightsName;
    std::string m_model;
    ov::Tensor m_weights;
    std::vector<std::uint8_t> weights_vector;

public:
    static std::string getTestCaseName(testing::TestParamInfo<compileModelLoadFromMemoryParams> obj);

    void SetUp() override;
    void TearDown() override;
    void run() override;
    bool importExportSupported(ov::Core &core) const;
};

using compileKernelsCacheParams = std::tuple<
        std::string,                          // device name
        std::pair<ov::AnyMap, std::string>    // device and cache configuration
>;
class CompiledKernelsCacheTest : virtual public SubgraphBaseTest,
                                 virtual public OVPluginTestBase,
                                 public testing::WithParamInterface<compileKernelsCacheParams> {
public:
    static std::string getTestCaseName(testing::TestParamInfo<compileKernelsCacheParams> obj);
protected:
    std::string test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::string cache_path;
    std::vector<std::string> m_extList;

    void SetUp() override;
    void TearDown() override;
};
} // namespace behavior
} // namespace test
} // namespace ov
