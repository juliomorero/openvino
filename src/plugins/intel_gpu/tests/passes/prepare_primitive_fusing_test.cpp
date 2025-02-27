// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "test_utils.h"

#include "intel_gpu/runtime/engine.hpp"

#include "intel_gpu/graph/network.hpp"
#include "intel_gpu/graph/program.hpp"
#include "data_inst.h"
#include "eltwise_inst.h"
#include "reduce_inst.h"
#include "pass_manager.h"
#include "to_string_utils.h"

#include "program_wrapper.h"

#include <memory>

using namespace cldnn;
using namespace ::tests;

TEST(prepare_primitive_fusing, fuse_activation_to_fc_dyn) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 16, 32 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };

    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }));
    topology.add(activation("act", input_info("fc"), activation_func::relu));
    topology.add(reorder("reorder", input_info("act"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<activation>(*prog));
}

TEST(prepare_primitive_fusing, dont_fuse_incompatible_eltwise) {
    auto& engine = get_test_engine();
    auto in_layout = layout{ ov::PartialShape{-1, -1, 10}, data_types::f32, format::bfyx };
    auto const_layout = layout{ ov::PartialShape{1, 1, 1}, data_types::f32, format::bfyx };
    auto const_mem = engine.allocate_memory(const_layout);

    topology topology;
    topology.add(input_layout("input", in_layout));
    topology.add(data("const", const_mem));
    topology.add(eltwise("eltw_pre", { input_info("input"), input_info("const") }, eltwise_mode::sum));
    topology.add(reduce("reduce", input_info("eltw_pre"), reduce_mode::max, {2}, true));
    topology.add(eltwise("eltw", { input_info("input"), input_info("reduce") }, eltwise_mode::sum));
    topology.add(reorder("reorder", input_info("eltw"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_TRUE(has_node(*prog, "eltw"));
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_legal) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 16, 20 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape::dynamic(2), data_types::f32, format::bfyx };

    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(input_layout("extra_input", in_eltw_layout));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }, "", data_types::f32));
    topology.add(eltwise("eltw", { input_info("fc"), input_info("extra_input") }, eltwise_mode::sum));
    topology.add(reorder("reorder", input_info("eltw"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{32, 20}, data_types::u8, format::bfyx });
    auto extra_input_memory = engine.allocate_memory(layout{ ov::PartialShape{32, 16}, data_types::f32, format::bfyx });

    net.set_input_data("input", input_memory);
    net.set_input_data("extra_input", extra_input_memory);

    auto output = net.execute();
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_illegal) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 2, 10 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape::dynamic(2), data_types::f32, format::bfyx };

    set_values<uint8_t>(weights, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(input_layout("extra_input", in_eltw_layout));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }, "", data_types::f32));
    topology.add(eltwise("eltw", { input_info("fc"), input_info("extra_input")}, eltwise_mode::sum));
    topology.add(reorder("reorder", input_info("eltw"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{1, 10}, data_types::u8, format::bfyx });
    auto extra_input_memory = engine.allocate_memory(layout{ ov::PartialShape{2, 2}, data_types::f32, format::bfyx });
    set_values<uint8_t>(input_memory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    set_values<float>(extra_input_memory, {10, 20, 30, 40});

    net.set_input_data("input", input_memory);
    net.set_input_data("extra_input", extra_input_memory);

    auto output = net.execute();
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);

    ASSERT_EQ(out_mem->count(), 4);
    ASSERT_EQ(out_mem->size(), 4 * sizeof(float));

    mem_lock<float> lock(out_mem, net.get_stream());

    ASSERT_EQ(lock[0], 285 + 10);
    ASSERT_EQ(lock[1], 285 + 20);
    ASSERT_EQ(lock[2], 285 + 30);
    ASSERT_EQ(lock[3], 285 + 40);
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_illegal_const) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 2, 10 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape{2, 2}, data_types::f32, format::bfyx };

    set_values<uint8_t>(weights, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    auto extra_input_memory = engine.allocate_memory(in_eltw_layout);
    set_values<float>(extra_input_memory, {10, 20, 30, 40});

    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(data("extra_input", extra_input_memory));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }, "", data_types::f32));
    topology.add(eltwise("eltw", { input_info("fc"), input_info("extra_input") }, eltwise_mode::sum));
    topology.add(reorder("reorder", input_info("eltw"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{1, 10}, data_types::u8, format::bfyx });
    set_values<uint8_t>(input_memory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    net.set_input_data("input", input_memory);

    auto output = net.execute();
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);

    ASSERT_EQ(out_mem->count(), 4);
    ASSERT_EQ(out_mem->size(), 4 * sizeof(float));

    mem_lock<float> lock(out_mem, net.get_stream());

    ASSERT_EQ(lock[0], 285 + 10);
    ASSERT_EQ(lock[1], 285 + 20);
    ASSERT_EQ(lock[2], 285 + 30);
    ASSERT_EQ(lock[3], 285 + 40);
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_legal_scalar_const_broadcast) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 2, 10 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape{1}, data_types::f32, format::bfyx };

    set_values<uint8_t>(weights, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                                  9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
    auto extra_input_memory = engine.allocate_memory(in_eltw_layout);
    set_values<float>(extra_input_memory, {10});

    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(data("extra_input", extra_input_memory));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }, "", data_types::f32));
    topology.add(eltwise("eltw", { input_info("fc"), input_info("extra_input") }, eltwise_mode::sum));
    topology.add(reorder("reorder", input_info("eltw"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{1, 10}, data_types::u8, format::bfyx });
    set_values<uint8_t>(input_memory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    net.set_input_data("input", input_memory);

    auto output = net.execute();
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);

    ASSERT_EQ(out_mem->count(), 2);
    ASSERT_EQ(out_mem->size(), 2 * sizeof(float));

    mem_lock<float> lock(out_mem, net.get_stream());

    ASSERT_EQ(lock[0], 285 + 10);
    ASSERT_EQ(lock[1], 120 + 10);
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_illegal_1) {
    auto& engine = get_test_engine();
    auto weights = engine.allocate_memory({ ov::PartialShape{ 2, 10 }, data_types::u8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::u8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape::dynamic(2), data_types::f32, format::bfyx };

    set_values<uint8_t>(weights, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    // The topology below is intended to check the following tricky things:
    // 1. Cases where original eltw input is also optimized (act_e2 is fused into act_e1)
    // 1. There is another layers in fusion pattern (activations before & after eltwise)
    // 1. Changed inputs order of eltwise, i.e. fused fc node is the second input
    topology topology;
    topology.add(data("weights", weights));
    topology.add(input_layout("input", in_layout));
    topology.add(input_layout("extra_input", in_eltw_layout));
    topology.add(activation("act_e1", input_info("extra_input"), activation_func::relu));
    topology.add(activation("act_e2", input_info("act_e1"), activation_func::relu));
    topology.add(fully_connected("fc", input_info("input"), { "weights" }, "", data_types::f32));
    topology.add(activation("act_fc1", input_info("fc"), activation_func::relu));
    topology.add(eltwise("eltw", { input_info("act_e2"), input_info("act_fc1")}, eltwise_mode::sum));
    topology.add(activation("act_fc2", input_info("eltw"), activation_func::relu));
    topology.add(reorder("reorder", input_info("act_fc2"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{1, 10}, data_types::u8, format::bfyx });
    auto extra_input_memory = engine.allocate_memory(layout{ ov::PartialShape{2, 2}, data_types::f32, format::bfyx });
    set_values<uint8_t>(input_memory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    set_values<float>(extra_input_memory, {10, 20, 30, 40});

    net.set_input_data("input", input_memory);
    net.set_input_data("extra_input", extra_input_memory);

    auto output = net.execute();
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);

    ASSERT_EQ(out_mem->count(), 4);
    ASSERT_EQ(out_mem->size(), 4 * sizeof(float));

    mem_lock<float> lock(out_mem, net.get_stream());

    ASSERT_EQ(lock[0], 285 + 10);
    ASSERT_EQ(lock[1], 285 + 20);
    ASSERT_EQ(lock[2], 285 + 30);
    ASSERT_EQ(lock[3], 285 + 40);
}

TEST(prepare_primitive_fusing, fuse_eltwise_to_fc_dyn_illegal_2) {
    auto& engine = get_test_engine();
    auto weights0 = engine.allocate_memory({ ov::PartialShape{ 2, 10 }, data_types::i8, format::bfyx });
    auto weights1 = engine.allocate_memory({ ov::PartialShape{ 4, 2 }, data_types::i8, format::bfyx });
    auto in_layout = layout{ ov::PartialShape::dynamic(2), data_types::i8, format::bfyx };
    auto in_eltw_layout = layout{ ov::PartialShape::dynamic(2), data_types::f32, format::bfyx };

    set_values<uint8_t>(weights0, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    set_values<uint8_t>(weights1, {1, 1, 1, 1, 1, 1, 1, 1});


    // The topology below is intended to check the following tricky things:
    // 1. Cases where original eltw input is also optimized (act_e2 is fused into act_e1)
    // 1. There is another layers in fusion pattern (activations before & after eltwise)
    // 1. Also, the input (act_fc1) of the fused node of the eltw (i.e., fc2) is fused to other node (fc1)

    topology topology;
    topology.add(data("weights0", weights0));
    topology.add(data("weights1", weights1));
    topology.add(input_layout("input", in_layout));
    topology.add(fully_connected("fc1", input_info("input"), { "weights0" }, "", data_types::i8));
    topology.add(activation("act_fc1", input_info("fc1"), activation_func::relu));
    topology.add(fully_connected("fc2", input_info("act_fc1"), { "weights1" }, "", data_types::i8));
    topology.add(activation("act_fc2", input_info("fc2"), activation_func::relu));
    topology.add(input_layout("extra_input", in_eltw_layout));
    topology.add(activation("act_e1", input_info("extra_input"), activation_func::abs));
    topology.add(activation("act_e2", input_info("act_e1"), activation_func::relu));
    topology.add(eltwise("eltw", { input_info("act_fc2"), input_info("act_e2") }, eltwise_mode::sum));
    topology.add(activation("act_fc3", input_info("eltw"), activation_func::relu));
    topology.add(reorder("reorder", input_info("act_fc3"), format::bfyx, data_types::f32));

    ExecutionConfig config;
    config.set_property(ov::intel_gpu::optimize_data(true));
    config.set_property(ov::intel_gpu::allow_new_shape_infer(true));
    auto prog = program::build_program(engine, topology, config, false, true);

    layout_optimizer lo(true);

    program_wrapper::apply_opt_pass<prepare_primitive_fusing>(*prog, lo);

    ASSERT_NE(prog, nullptr);
    ASSERT_FALSE(has_node_with_type<eltwise>(*prog));

    cldnn::network net(prog, 0);

    auto input_memory = engine.allocate_memory(layout{ ov::PartialShape{1, 10}, data_types::i8, format::bfyx });
    auto extra_input_memory = engine.allocate_memory(layout{ ov::PartialShape{4, 4}, data_types::f32, format::bfyx });
    set_values<int8_t>(input_memory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -10});
    set_values<float>(extra_input_memory, {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4});

    net.set_input_data("input", input_memory);
    net.set_input_data("extra_input", extra_input_memory);

    auto output = net.execute();
    auto out_l = net.get_output_layout("reorder");
    auto out_mem = output.at("reorder").get_memory();

    ASSERT_NE(out_mem, nullptr);

    ASSERT_EQ(out_l.batch(), 4);
    ASSERT_EQ(out_l.feature(), 4);
    ASSERT_EQ(out_mem->count(), 16);
    ASSERT_EQ(out_mem->size(), 16 * sizeof(float));

    mem_lock<float> lock(out_mem, net.get_stream());

    ASSERT_EQ(lock[0], 91);
    ASSERT_EQ(lock[1], 92);
    ASSERT_EQ(lock[2], 93);
    ASSERT_EQ(lock[3], 94);
}
