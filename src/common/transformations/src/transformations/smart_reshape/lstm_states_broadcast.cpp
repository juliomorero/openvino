// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/smart_reshape/lstm_states_broadcast.hpp"

#include <memory>

#include "dimension_tracker.hpp"
#include "itt.hpp"
#include "openvino/op/util/sub_graph_base.hpp"
#include "openvino/opsets/opset9.hpp"
#include "openvino/pass/manager.hpp"
#include "transformations/utils/utils.hpp"

using namespace std;
using namespace ov::opset9;

ov::Input<ov::Node> get_outer_input_of_ti_by_parameter(const shared_ptr<Parameter>& parameter,
                                                       const shared_ptr<TensorIterator>& ti) {
    int64_t parameter_index = ti->get_body()->get_parameter_index(parameter);
    for (const auto& input_descriptor : ti->get_input_descriptions())
        if (input_descriptor->m_body_parameter_index == parameter_index)
            return ti->input(input_descriptor->m_input_index);
    OPENVINO_UNREACHABLE("LSTMStatesBroadcast failed to get outer input of TI by its inner Parameter. TI ",
                         ti,
                         " Parameter ",
                         parameter);
}

shared_ptr<ov::Node> deduce_outer_source_of_batch_for_inner_lstm_cell(const shared_ptr<TensorIterator>& ti,
                                                                      const shared_ptr<LSTMCell>& lstm_cell) {
    const auto& body = ti->get_body();  // body is not nullptr -- we checked earlier

    map<Parameter*, ov::PartialShape> original_shapes;
    size_t label = 1;

    // mark all input dimensions with labels and making them dynamic, keeping original shapes
    for (auto& parameter : body->get_parameters()) {
        auto pshape = parameter->get_partial_shape();
        original_shapes[parameter.get()] = pshape;
        if (pshape.rank().is_dynamic())
            continue;
        for (ov::Dimension& n : pshape) {
            n = ov::Dimension::dynamic();
            ov::DimensionTracker::set_label(n, label++);
        }
        parameter->set_partial_shape(pshape);
    }

    // propagate labels through TI body
    body->validate_nodes_and_infer_types();
    // if lstm first input has undefined rank or if tracked label is zero -- we failed to track batch dimension
    // returning body to initial state
    if (lstm_cell->get_input_partial_shape(0).rank().is_dynamic() ||
        ov::DimensionTracker::get_label(lstm_cell->get_input_partial_shape(0)[0]) == 0) {
        for (auto& item : original_shapes)
            item.first->set_partial_shape(item.second);
        body->validate_nodes_and_infer_types();
        return nullptr;
    }

    // batch label was tracked -- finding parameter that delivered it
    shared_ptr<Parameter> batch_delivering_parameter;
    size_t index_of_batch_dim = 0;

    size_t batch_label = ov::DimensionTracker::get_label(lstm_cell->get_input_partial_shape(0)[0]);
    for (auto& parameter : body->get_parameters()) {
        auto pshape = parameter->get_partial_shape();
        if (pshape.rank().is_dynamic())
            continue;
        for (size_t i = 0; i < pshape.size(); ++i) {
            if (ov::DimensionTracker::get_label(pshape[i]) == batch_label) {
                batch_delivering_parameter = parameter;
                index_of_batch_dim = i;
                break;
            }
        }
        if (index_of_batch_dim != 0 && batch_delivering_parameter != nullptr)
            break;
    }
    for (auto& item : original_shapes)
        item.first->set_partial_shape(item.second);
    body->validate_nodes_and_infer_types();

    if (batch_delivering_parameter == nullptr)
        return nullptr;

    const auto& batched_source = get_outer_input_of_ti_by_parameter(batch_delivering_parameter, ti);
    const auto& batched_shape = make_shared<ShapeOf>(batched_source.get_source_output());
    const auto& batch = make_shared<Gather>(batched_shape,
                                            Constant::create(ov::element::i64, ov::Shape{1}, {index_of_batch_dim}),
                                            Constant::create(ov::element::i64, ov::Shape{}, {0}));
    return batch;
}

bool broadcast_state_by_batch(ov::Input<ov::Node> input, const shared_ptr<ov::Node>& batch_delivering_node) {
    auto constant_state = dynamic_pointer_cast<Constant>(input.get_source_output().get_node_shared_ptr());
    if (constant_state == nullptr)
        return false;
    const auto& constant_shape = constant_state->get_shape();
    if (constant_shape[0] != 1)
        // we only expect to broadcast LSTM states prepared for batch 1 -- no tiling of batch > 1 will be done
        return false;

    const auto& constant_copy = constant_state->copy_with_new_inputs({});
    const auto& broadcast_by_batch = make_shared<Broadcast>(
        constant_copy,
        make_shared<Concat>(ngraph::NodeVector{batch_delivering_node,
                                               ov::op::util::make_try_fold<Gather>(
                                                   ov::op::util::make_try_fold<ShapeOf>(constant_copy),
                                                   Constant::create(ov::element::i64, ov::Shape{1}, {1}),
                                                   Constant::create(ov::element::i64, ov::Shape{}, {0}))},
                            0));
    input.replace_source_output(broadcast_by_batch->output(0));
    return true;
}

bool relax_batch_for_initial_states_of_lstm_in_ti(const shared_ptr<TensorIterator>& ti,
                                                  const shared_ptr<LSTMCell>& lstm_cell) {
    bool rewritten = false;
    auto batch_delivering_node = deduce_outer_source_of_batch_for_inner_lstm_cell(ti, lstm_cell);
    if (batch_delivering_node == nullptr)
        return rewritten;
    if (auto init_hidden_state = dynamic_pointer_cast<Parameter>(lstm_cell->get_input_node_shared_ptr(1))) {
        auto outer_init_hidden_state_input = get_outer_input_of_ti_by_parameter(init_hidden_state, ti);
        rewritten |= broadcast_state_by_batch(outer_init_hidden_state_input, batch_delivering_node);
    }
    if (auto init_cell_state = dynamic_pointer_cast<Parameter>(lstm_cell->get_input_node_shared_ptr(2))) {
        auto outer_init_cell_state_input = get_outer_input_of_ti_by_parameter(init_cell_state, ti);
        rewritten |= broadcast_state_by_batch(outer_init_cell_state_input, batch_delivering_node);
    }
    return rewritten;
}

bool relax_batch_for_initial_states_of_lstm(const shared_ptr<LSTMCell>& lstm_cell) {
    bool rewritten = false;
    const auto& batched_shape = make_shared<ShapeOf>(lstm_cell->get_input_source_output(0));
    const auto& batch_delivering_node = make_shared<Gather>(batched_shape,
                                                            Constant::create(ov::element::i64, ov::Shape{1}, {0}),
                                                            Constant::create(ov::element::i64, ov::Shape{}, {0}));
    rewritten |= broadcast_state_by_batch(lstm_cell->input(1), batch_delivering_node);
    rewritten |= broadcast_state_by_batch(lstm_cell->input(2), batch_delivering_node);
    return rewritten;
}

bool ov::pass::LSTMStatesBroadcast::run_on_model(const shared_ptr<ov::Model>& f) {
    RUN_ON_FUNCTION_SCOPE(LSTMStatesBroadcast);
    bool rewritten = false;
    for (auto& node : f->get_ordered_ops()) {
        // Recursively apply transformation for sub-graph based operations
        if (const auto& sub_graph_node = dynamic_pointer_cast<ov::op::util::SubGraphOp>(node))
            if (const auto& sub_graph = sub_graph_node->get_function())
                rewritten |= run_on_model(sub_graph);

        // Case without TI (LSTMCell and Constant are in the same ov::Model)
        if (const auto& lstm_cell = dynamic_pointer_cast<LSTMCell>(node))
            rewritten |= relax_batch_for_initial_states_of_lstm(lstm_cell);

        // Case with TI (LSTMCell and Constant are in different ov::Model objects)
        if (auto ti = dynamic_pointer_cast<TensorIterator>(node)) {
            auto body = ti->get_body();
            if (body == nullptr)
                continue;
            for (const auto& body_node : body->get_ordered_ops())
                if (const auto& lstm_cell = dynamic_pointer_cast<LSTMCell>(body_node))
                    rewritten |= relax_batch_for_initial_states_of_lstm_in_ti(ti, lstm_cell);
        }
    }
    return rewritten;
}
