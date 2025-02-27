// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/transpose_sinking.hpp"

#include <memory>
#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/rt_info.hpp>
#include <numeric>
#include <openvino/core/validation_util.hpp>
#include <openvino/opsets/opset6.hpp>
#include <openvino/opsets/opset7.hpp>
#include <vector>

#include "itt.hpp"
#include "transformations/common_optimizations/transpose_sinking_utils.hpp"
#include "transformations/utils/utils.hpp"

using namespace ov;

namespace {

std::shared_ptr<opset6::Constant> get_reduced_order_constant(const std::shared_ptr<opset6::Constant>& axes_const,
                                                             const std::shared_ptr<opset6::Constant>& order_const) {
    auto order = order_const->cast_vector<int64_t>();

    auto axes = axes_const->cast_vector<int64_t>();
    std::sort(axes.rbegin(), axes.rend());
    for (const auto& i : axes)
        order.erase(order.begin() + i);

    const auto& updated_order_size = static_cast<int64_t>(order.size());

    auto order_sorted = order;
    sort(order_sorted.begin(), order_sorted.end());
    for (int64_t i = 0; i < updated_order_size; ++i) {
        auto lowest_greater_eq_i = std::lower_bound(order_sorted.begin(), order_sorted.end(), i);
        std::replace(order.begin(), order.end(), *lowest_greater_eq_i, i);
        std::replace(order_sorted.begin(), order_sorted.end(), *lowest_greater_eq_i, i);
    }
    return std::make_shared<opset6::Constant>(ngraph::element::i64, ngraph::Shape{order.size()}, order);
}

std::shared_ptr<opset6::Constant> get_reversed_order_constant(const std::shared_ptr<opset6::Constant>& order_const) {
    const auto& order = order_const->cast_vector<size_t>();
    const auto& rank = order.size();
    const auto& default_order = ngraph::get_default_order(rank);
    std::vector<size_t> reverse_order(rank);
    for (size_t i = 0; i < rank; ++i)
        reverse_order[order[i]] = default_order[i];

    return std::make_shared<opset6::Constant>(ngraph::element::i64, ngraph::Shape{reverse_order.size()}, reverse_order);
}

}  // namespace

ov::pass::TransposeEltwise::TransposeEltwise() {
    MATCHER_SCOPE(TransposeEltwise);

    auto eltwise_data_input_p = pattern::any_input();
    auto eltwise_const_input_p = pattern::wrap_type<opset6::Constant>();
    auto eltwise_p = pattern::wrap_type<op::util::BinaryElementwiseArithmetic>(
        {eltwise_data_input_p, eltwise_const_input_p},
        [](const Output<Node>& output) {
            return ov::is_preprocesing_node(output.get_node_shared_ptr());
        });
    auto transpose_p = pattern::wrap_type<opset6::Transpose>({eltwise_p, pattern::wrap_type<opset6::Constant>()},
                                                             pattern::consumers_count(1));

    auto callback = [=](ngraph::pattern::Matcher& m) {
        const auto& pattern_to_output = m.get_pattern_value_map();
        auto eltwise = pattern_to_output.at(eltwise_p).get_node_shared_ptr();
        auto eltwise_const_input = pattern_to_output.at(eltwise_const_input_p);
        auto eltwise_data_input = pattern_to_output.at(eltwise_data_input_p);
        auto transpose = pattern_to_output.at(transpose_p).get_node_shared_ptr();

        const auto& order_size = transpose->get_input_shape(1).at(0);
        const auto& shape = eltwise_const_input.get_shape();
        if (shape.size() != order_size && ov::shape_size(shape) != 1) {
            // TODO: temporary restrictions
            return false;
        }

        if (ov::shape_size(shape) != 1) {
            eltwise_const_input = std::make_shared<opset6::Transpose>(eltwise_const_input, transpose->input_value(1));
            if (auto const_node = ov::get_constant_from_source(eltwise_const_input)) {
                eltwise_const_input = const_node;
            }
        }

        auto new_transpose = transpose->clone_with_new_inputs({eltwise_data_input, transpose->input_value(1)});
        auto new_eltwise = eltwise->clone_with_new_inputs({new_transpose, eltwise_const_input});
        register_new_node(new_transpose);

        new_transpose->set_friendly_name(eltwise->get_friendly_name());
        copy_runtime_info({eltwise, transpose}, {new_transpose, new_eltwise});
        replace_node(transpose, new_eltwise);
        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(transpose_p, matcher_name);
    register_matcher(m, callback);
}

ov::pass::TransposeConvert::TransposeConvert() {
    MATCHER_SCOPE(TransposeConvert);

    auto transpose_label =
        pattern::wrap_type<opset6::Transpose>({pattern::any_input(), pattern::wrap_type<opset6::Constant>()},
                                              pattern::consumers_count(1));
    auto convert_label = pattern::wrap_type<opset6::Convert>({transpose_label});

    matcher_pass_callback matcher_pass_callback = [=](ngraph::pattern::Matcher& m) {
        const auto& pattern_to_output = m.get_pattern_value_map();
        auto transpose = pattern_to_output.at(transpose_label).get_node_shared_ptr();
        auto convert = pattern_to_output.at(convert_label).get_node_shared_ptr();

        auto new_convert = convert->clone_with_new_inputs({transpose->input_value(0)});
        auto new_transpose = transpose->clone_with_new_inputs({new_convert, transpose->input_value(1)});
        register_new_node(new_transpose);

        new_transpose->set_friendly_name(convert->get_friendly_name());
        copy_runtime_info({transpose, convert}, {new_convert, new_transpose});
        replace_node(convert, new_transpose);
        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(convert_label, matcher_name);
    register_matcher(m, matcher_pass_callback);
}

ov::pass::TransposeReduction::TransposeReduction() {
    MATCHER_SCOPE(TransposeReduction);

    auto transpose_label =
        pattern::wrap_type<opset6::Transpose>({pattern::any_input(), pattern::wrap_type<opset6::Constant>()},
                                              pattern::consumers_count(1));
    auto reduce_or_squeeze_label =
        pattern::wrap_type<op::util::ArithmeticReductionKeepDims, op::util::LogicalReductionKeepDims, opset6::Squeeze>(
            {transpose_label, pattern::wrap_type<opset6::Constant>()});

    ov::matcher_pass_callback matcher_pass_callback = [=](ngraph::pattern::Matcher& m) {
        const auto& pattern_to_output = m.get_pattern_value_map();

        auto transpose = pattern_to_output.at(transpose_label).get_node_shared_ptr();
        auto reduction = pattern_to_output.at(reduce_or_squeeze_label).get_node_shared_ptr();
        auto arithmetic_reduce = std::dynamic_pointer_cast<op::util::ArithmeticReductionKeepDims>(reduction);
        auto logical_reduce = std::dynamic_pointer_cast<op::util::LogicalReductionKeepDims>(reduction);
        auto squeeze = std::dynamic_pointer_cast<opset6::Squeeze>(reduction);
        if (!transpose || !(arithmetic_reduce || logical_reduce || squeeze))
            return false;

        bool keep_dims = false;  // squeeze always reduces number of output dimensions
        if (logical_reduce)
            keep_dims = logical_reduce->get_keep_dims();
        else if (arithmetic_reduce)
            keep_dims = arithmetic_reduce->get_keep_dims();

        auto transpose_order = std::dynamic_pointer_cast<opset6::Constant>(transpose->get_input_node_shared_ptr(1));
        auto reduction_axes = std::dynamic_pointer_cast<opset6::Constant>(reduction->get_input_node_shared_ptr(1));
        if (!transpose_order || !reduction_axes)
            return false;

        const auto& non_negative_axes = normalize_axes(reduction->get_friendly_name(),
                                                       reduction_axes->cast_vector<int64_t>(),
                                                       reduction->get_input_partial_shape(0).rank());
        reduction_axes = opset6::Constant::create(ngraph::element::i64, {non_negative_axes.size()}, non_negative_axes);

        ngraph::NodeVector new_ops;
        auto new_axes =
            ov::op::util::make_try_fold<opset6::Gather>(transpose_order,
                                                        reduction_axes,
                                                        opset6::Constant::create(ngraph::element::i64, {}, {0}));
        new_ops.push_back(new_axes);
        auto new_reduce = reduction->clone_with_new_inputs({transpose->input_value(0), new_axes});
        new_ops.push_back(new_reduce);

        auto updated_order = transpose_order;
        if (!keep_dims) {
            updated_order = get_reduced_order_constant(reduction_axes, transpose_order);
            new_ops.push_back(updated_order);
        }
        auto new_transpose = register_new_node<opset6::Transpose>(new_reduce, updated_order);
        new_ops.push_back(new_transpose);
        new_transpose->set_friendly_name(reduction->get_friendly_name());

        ngraph::copy_runtime_info({reduction, transpose}, new_ops);
        ngraph::replace_node(reduction, new_transpose);

        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(reduce_or_squeeze_label, matcher_name);
    register_matcher(m, matcher_pass_callback);
}

ov::pass::TransposeFQReduction::TransposeFQReduction() {
    MATCHER_SCOPE(TransposeFQReduction);

    auto transpose_label =
        pattern::wrap_type<opset6::Transpose>({pattern::any_input(), pattern::wrap_type<opset6::Constant>()});
    auto fq_label = pattern::wrap_type<opset6::FakeQuantize>({transpose_label,
                                                              pattern::any_input(pattern::has_static_rank()),
                                                              pattern::any_input(pattern::has_static_rank()),
                                                              pattern::any_input(pattern::has_static_rank()),
                                                              pattern::any_input(pattern::has_static_rank())});
    auto reduce_or_squeeze_label =
        pattern::wrap_type<op::util::ArithmeticReductionKeepDims, op::util::LogicalReductionKeepDims, opset6::Squeeze>(
            {fq_label, pattern::wrap_type<opset6::Constant>()});

    ov::matcher_pass_callback matcher_pass_callback = [=](ngraph::pattern::Matcher& m) {
        auto& pattern_to_output = m.get_pattern_value_map();

        auto transpose = pattern_to_output.at(transpose_label).get_node_shared_ptr();
        if (!transpose)
            return false;

        auto transpose_order = std::dynamic_pointer_cast<opset6::Constant>(transpose->get_input_node_shared_ptr(1));
        auto fq = pattern_to_output.at(fq_label).get_node_shared_ptr();
        if (!transpose_order || !fq)
            return false;

        ngraph::NodeVector new_ops;

        const auto& reverse_order_constant = get_reversed_order_constant(transpose_order);
        new_ops.push_back(reverse_order_constant);

        const auto& input_rank = fq->get_input_partial_shape(0).rank().get_length();
        ngraph::OutputVector fq_inputs = {transpose->input_value(0)};
        for (size_t i = 1; i < fq->inputs().size(); ++i) {
            auto input = fq->input_value(i);
            const auto& ranks_diff = input_rank - input.get_partial_shape().rank().get_length();
            NGRAPH_CHECK(ranks_diff >= 0);
            if (ranks_diff > 0) {
                std::vector<int64_t> axes(ranks_diff);
                std::iota(axes.begin(), axes.end(), 0);
                const auto& axes_const = opset6::Constant::create(element::i64, Shape{axes.size()}, axes);
                new_ops.push_back(axes_const);
                const auto& unsqueezed_input = op::util::make_try_fold<opset6::Unsqueeze>(input, axes_const);
                new_ops.push_back(unsqueezed_input);
                input = unsqueezed_input->output(0);
            }
            const auto& transposed_input = op::util::make_try_fold<opset6::Transpose>(input, reverse_order_constant);
            new_ops.push_back(transposed_input);
            fq_inputs.push_back(transposed_input);
        }
        auto new_fq = fq->clone_with_new_inputs(fq_inputs);
        new_ops.push_back(new_fq);

        auto new_transpose = register_new_node<opset6::Transpose>(new_fq, transpose_order);
        new_ops.push_back(new_transpose);
        new_transpose->set_friendly_name(fq->get_friendly_name());

        ngraph::copy_runtime_info({fq, transpose}, new_ops);
        ngraph::replace_node(fq, new_transpose);
        // The root node (reduction) left unchanged during current matcher pass.
        // We return false here for further MatcherPasses to be applicable for this node as a root node
        return false;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(reduce_or_squeeze_label, matcher_name);
    register_matcher(m, matcher_pass_callback);
}

ov::pass::TransposeFuse::TransposeFuse() {
    MATCHER_SCOPE(TransposeFuse);

    auto transpose_1 =
        pattern::wrap_type<opset7::Transpose>({pattern::any_input(), pattern::wrap_type<opset7::Constant>()},
                                              pattern::consumers_count(1));
    auto transpose_2 = pattern::wrap_type<opset7::Transpose>({transpose_1, pattern::wrap_type<opset7::Constant>()});

    ov::matcher_pass_callback matcher_pass_callback = [=](ngraph::pattern::Matcher& m) {
        const auto& pattern_to_output = m.get_pattern_value_map();

        auto transpose1 = pattern_to_output.at(transpose_1).get_node_shared_ptr();
        auto transpose2 = pattern_to_output.at(transpose_2).get_node_shared_ptr();
        auto input = transpose1->input_value(0);

        auto transpose1_order = std::dynamic_pointer_cast<opset7::Constant>(transpose1->get_input_node_shared_ptr(1));
        auto transpose2_order = std::dynamic_pointer_cast<opset7::Constant>(transpose2->get_input_node_shared_ptr(1));
        if (!transpose1_order || !transpose2_order)
            return false;

        auto order1 = transpose1_order->cast_vector<int64_t>();
        auto order2 = transpose2_order->cast_vector<int64_t>();
        if (order1.size() != order2.size())
            return false;

        bool is_ordered = true;
        for (size_t i = 0; i < order1.size(); i++) {
            order2[i] = order1[order2[i]];
            if (order2[i] != (int64_t)i)
                is_ordered = false;
        }

        auto transpose_order_type = transpose1_order->get_element_type();
        if (transpose_order_type != transpose2_order->get_element_type())
            transpose_order_type = element::i64;

        if (is_ordered) {
            return ngraph::replace_output_update_name(transpose2->output(0), input);
        } else {
            auto new_order = opset7::Constant::create(transpose_order_type, {order2.size()}, order2);
            auto new_transpose = register_new_node<opset7::Transpose>(input, new_order);

            new_transpose->set_friendly_name(m.get_match_root()->get_friendly_name());
            ngraph::copy_runtime_info({transpose1, transpose2}, new_transpose);
            ngraph::replace_node(m.get_match_root(), new_transpose);

            transpose_sinking::UpdateForwardSinkingAbility(new_transpose);
        }

        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(transpose_2, matcher_name);
    register_matcher(m, matcher_pass_callback);
}
