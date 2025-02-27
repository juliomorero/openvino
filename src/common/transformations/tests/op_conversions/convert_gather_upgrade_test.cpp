// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gtest/gtest.h>

#include <memory>
#include <ngraph/function.hpp>
#include <ngraph/op/constant.hpp>
#include <ngraph/opsets/opset1.hpp>
#include <ngraph/opsets/opset7.hpp>
#include <ngraph/opsets/opset8.hpp>
#include <ngraph/pass/manager.hpp>
#include <string>
#include <transformations/init_node_info.hpp>
#include <transformations/op_conversions/convert_gather_upgrade.hpp>

#include "common_test_utils/ngraph_test_utils.hpp"

using namespace testing;

TEST_F(TransformationTestsF, ConvertGather1toGather7) {
    {
        auto data = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::f32, ngraph::Shape{2, 3});
        auto indices = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::i32, ngraph::Shape{2, 2});
        auto axis = ngraph::opset1::Constant::create(ngraph::element::i32, ngraph::Shape{1}, {0});

        auto gather_v1 = std::make_shared<ngraph::opset1::Gather>(data, indices, axis);

        function =
            std::make_shared<ngraph::Function>(ngraph::NodeVector{gather_v1}, ngraph::ParameterVector{data, indices});
        manager.register_pass<ngraph::pass::ConvertGather1ToGather7>();
    }

    {
        auto data = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::f32, ngraph::Shape{2, 3});
        auto indices = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::i32, ngraph::Shape{2, 2});
        auto axis = ngraph::opset1::Constant::create(ngraph::element::i32, ngraph::Shape{1}, {0});

        auto gather_v7 = std::make_shared<ngraph::opset7::Gather>(data, indices, axis, 0);

        function_ref =
            std::make_shared<ngraph::Function>(ngraph::NodeVector{gather_v7}, ngraph::ParameterVector{data, indices});
    }
}

TEST_F(TransformationTestsF, ConvertGather7toGather8) {
    {
        auto data = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::f32, ngraph::Shape{2, 3});
        auto indices = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::i32, ngraph::Shape{2, 2});
        auto axis = ngraph::opset1::Constant::create(ngraph::element::i32, ngraph::Shape{1}, {1});
        int64_t batch_dims = 1;
        auto gather_v7 = std::make_shared<ngraph::opset7::Gather>(data, indices, axis, batch_dims);

        function =
            std::make_shared<ngraph::Function>(ngraph::NodeVector{gather_v7}, ngraph::ParameterVector{data, indices});

        manager.register_pass<ngraph::pass::ConvertGather7ToGather8>();
    }

    {
        auto data = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::f32, ngraph::Shape{2, 3});
        auto indices = std::make_shared<ngraph::opset1::Parameter>(ngraph::element::i32, ngraph::Shape{2, 2});
        auto axis = ngraph::opset1::Constant::create(ngraph::element::i32, ngraph::Shape{1}, {1});
        int64_t batch_dims = 1;

        auto gather_v8 = std::make_shared<ngraph::opset8::Gather>(data, indices, axis, batch_dims);

        function_ref =
            std::make_shared<ngraph::Function>(ngraph::NodeVector{gather_v8}, ngraph::ParameterVector{data, indices});
    }
}
