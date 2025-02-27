// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "quantize_inst.h"
#include "primitive_base.hpp"
#include "impls/implementation_map.hpp"
#include "kernel_selector_helper.h"
#include "quantize/quantize_kernel_selector.h"
#include "quantize/quantize_kernel_ref.h"
#include "intel_gpu/runtime/error_handler.hpp"

using namespace cldnn;

namespace cldnn {
namespace ocl {

struct quantize_impl : typed_primitive_impl_ocl<quantize> {
    using parent = typed_primitive_impl_ocl<quantize>;
    using parent::parent;
    using kernel_selector_t = kernel_selector::quantize_kernel_selector;
    using kernel_params_t = std::pair<kernel_selector::quantize_params, kernel_selector::quantize_optional_params>;

    DECLARE_OBJECT_TYPE_SERIALIZATION

    std::unique_ptr<primitive_impl> clone() const override {
        return make_unique<quantize_impl>(*this);
    }

protected:
    kernel_arguments_data get_arguments(const typed_primitive_inst<quantize>& instance) const override {
        kernel_arguments_data args;

        for (size_t i = 0; i < instance.inputs_memory_count(); i++) {
            args.inputs.push_back(instance.input_memory_ptr(i));
        }
        if (instance.node->get_scale_shift_opt()) {
            if (instance.node->get_dependencies().size() == 9) {
                args.inputs.push_back(instance.dep_memory_ptr(5));
                args.inputs.push_back(instance.dep_memory_ptr(6));
                args.inputs.push_back(instance.dep_memory_ptr(7));
                args.inputs.push_back(instance.dep_memory_ptr(8));
            }
        }
        args.outputs = { instance.output_memory_ptr() };
        return args;
    }

public:
    static std::unique_ptr<primitive_impl> create(const quantize_node& arg, const kernel_impl_params& impl_param) {
        auto quantize_params = get_default_params<kernel_selector::quantize_params>(impl_param);
        auto quantize_optional_params =
            get_default_optional_params<kernel_selector::quantize_optional_params>(impl_param.get_program());

        quantize_params.levels = arg.get_levels();
        quantize_params.packed_binary_output = arg.get_packed_binary_output();
        quantize_params.scale_shift_opt = arg.get_scale_shift_opt();
        quantize_params.has_post_scale = arg.get_need_post_scale();
        quantize_params.has_post_shift = arg.get_need_post_shift();
        quantize_params.has_pre_shift = arg.get_need_pre_shift();
        quantize_params.has_clamp = arg.get_need_clamp();
        quantize_params.has_min_clamp = arg.get_need_min_clamp();
        quantize_params.has_max_clamp = arg.get_need_max_clamp();

        quantize_params.per_tensor_input_range = arg.get_per_tensor_input_range();
        quantize_params.per_tensor_input_scale = arg.get_per_tensor_input_scale();
        quantize_params.per_tensor_input_shift = arg.get_per_tensor_input_shift();
        quantize_params.per_tensor_output_range = arg.get_per_tensor_output_range();
        quantize_params.per_tensor_output_scale = arg.get_per_tensor_output_scale();
        quantize_params.per_tensor_output_shift = arg.get_per_tensor_output_shift();

        quantize_params.in_lo = arg.get_input_lo_val();
        quantize_params.in_hi = arg.get_input_hi_val();
        quantize_params.in_scale = arg.get_input_scale_val();
        quantize_params.in_shift = arg.get_input_shift_val();
        quantize_params.out_lo = arg.get_output_lo_val();
        quantize_params.out_hi = arg.get_output_hi_val();
        quantize_params.out_scale = arg.get_output_scale_val();
        quantize_params.out_shift = arg.get_output_shift_val();

        for (size_t i = 1; i < arg.inputs_count(); i++) {
            quantize_params.inputs.push_back(convert_data_tensor(impl_param.input_layouts[i]));
        }
        const auto& output_layout = impl_param.get_output_layout();
        quantize_params.outputs = { convert_data_tensor(output_layout) };

        auto& kernel_selector = kernel_selector::quantize_kernel_selector::Instance();
        auto best_kernel = kernel_selector.get_best_kernel(quantize_params, quantize_optional_params);

        return make_unique<quantize_impl>(best_kernel);
    }
};

namespace detail {

attach_quantize_impl::attach_quantize_impl() {
    implementation_map<quantize>::add(impl_types::ocl, quantize_impl::create, {
        std::make_tuple(data_types::f16, format::fs_b_yx_fsv32),
        std::make_tuple(data_types::f32, format::fs_b_yx_fsv32),
        std::make_tuple(data_types::i8, format::fs_b_yx_fsv32),
        std::make_tuple(data_types::u8, format::fs_b_yx_fsv32),

        std::make_tuple(data_types::f32, format::b_fs_yx_fsv16),
        std::make_tuple(data_types::f16, format::b_fs_yx_fsv16),
        std::make_tuple(data_types::i8, format::b_fs_yx_fsv16),
        std::make_tuple(data_types::u8, format::b_fs_yx_fsv16),

        std::make_tuple(data_types::f32, format::byxf),
        std::make_tuple(data_types::f16, format::byxf),
        std::make_tuple(data_types::u8, format::byxf),
        std::make_tuple(data_types::i8, format::byxf),

        std::make_tuple(data_types::f32, format::b_fs_yx_fsv4),
        std::make_tuple(data_types::f16, format::b_fs_yx_fsv4),
        std::make_tuple(data_types::u8, format::b_fs_yx_fsv4),
        std::make_tuple(data_types::i8, format::b_fs_yx_fsv4),

        std::make_tuple(data_types::f32, format::b_fs_yx_fsv32),
        std::make_tuple(data_types::f16, format::b_fs_yx_fsv32),
        std::make_tuple(data_types::u8, format::b_fs_yx_fsv32),
        std::make_tuple(data_types::i8, format::b_fs_yx_fsv32),

        std::make_tuple(data_types::f32, format::b_fs_zyx_fsv32),
        std::make_tuple(data_types::f16, format::b_fs_zyx_fsv32),
        std::make_tuple(data_types::u8, format::b_fs_zyx_fsv32),
        std::make_tuple(data_types::i8, format::b_fs_zyx_fsv32),

        std::make_tuple(data_types::f32, format::bs_fs_yx_bsv16_fsv16),
        std::make_tuple(data_types::f16, format::bs_fs_yx_bsv16_fsv16),
        std::make_tuple(data_types::u8, format::bs_fs_yx_bsv16_fsv16),
        std::make_tuple(data_types::i8, format::bs_fs_yx_bsv16_fsv16),

        std::make_tuple(data_types::f32, format::bs_fs_yx_bsv16_fsv32),
        std::make_tuple(data_types::f16, format::bs_fs_yx_bsv16_fsv32),
        std::make_tuple(data_types::u8, format::bs_fs_yx_bsv16_fsv32),
        std::make_tuple(data_types::i8, format::bs_fs_yx_bsv16_fsv32),

        std::make_tuple(data_types::f32, format::bs_fs_zyx_bsv16_fsv16),
        std::make_tuple(data_types::f16, format::bs_fs_zyx_bsv16_fsv16),
        std::make_tuple(data_types::u8, format::bs_fs_zyx_bsv16_fsv16),
        std::make_tuple(data_types::i8, format::bs_fs_zyx_bsv16_fsv16),

        std::make_tuple(data_types::f32, format::bs_fs_zyx_bsv16_fsv32),
        std::make_tuple(data_types::f16, format::bs_fs_zyx_bsv16_fsv32),
        std::make_tuple(data_types::u8, format::bs_fs_zyx_bsv16_fsv32),
        std::make_tuple(data_types::i8, format::bs_fs_zyx_bsv16_fsv32),

        std::make_tuple(data_types::f32, format::bfyx),
        std::make_tuple(data_types::f16, format::bfyx),
        std::make_tuple(data_types::i32, format::bfyx),
        std::make_tuple(data_types::u8, format::bfyx),
        std::make_tuple(data_types::i8, format::bfyx),

        std::make_tuple(data_types::f32, format::byxf),
        std::make_tuple(data_types::f16, format::byxf),
        std::make_tuple(data_types::i32, format::byxf),
        std::make_tuple(data_types::u8, format::byxf),
        std::make_tuple(data_types::i8, format::byxf),

        std::make_tuple(data_types::f32, format::yxfb),
        std::make_tuple(data_types::f16, format::yxfb),

        std::make_tuple(data_types::f32, format::bfzyx),
        std::make_tuple(data_types::f16, format::bfzyx),
        std::make_tuple(data_types::i32, format::bfzyx),
        std::make_tuple(data_types::u8, format::bfzyx),
        std::make_tuple(data_types::i8, format::bfzyx),

        std::make_tuple(data_types::f32, format::bfwzyx),
        std::make_tuple(data_types::f16, format::bfwzyx),
        std::make_tuple(data_types::i32, format::bfwzyx),
        std::make_tuple(data_types::u8, format::bfwzyx),
        std::make_tuple(data_types::i8, format::bfwzyx),

        std::make_tuple(data_types::f32, format::b_fs_zyx_fsv16),
        std::make_tuple(data_types::f16, format::b_fs_zyx_fsv16),
        std::make_tuple(data_types::u8, format::b_fs_zyx_fsv16),
        std::make_tuple(data_types::i8, format::b_fs_zyx_fsv16),

        std::make_tuple(data_types::f32, format::bs_fs_yx_bsv32_fsv32),
        std::make_tuple(data_types::f16, format::bs_fs_yx_bsv32_fsv32),
        std::make_tuple(data_types::i8, format::bs_fs_yx_bsv32_fsv32),
        std::make_tuple(data_types::u8, format::bs_fs_yx_bsv32_fsv32),

        std::make_tuple(data_types::f32, format::bs_fs_zyx_bsv32_fsv32),
        std::make_tuple(data_types::f16, format::bs_fs_zyx_bsv32_fsv32),
        std::make_tuple(data_types::i8, format::bs_fs_zyx_bsv32_fsv32),
        std::make_tuple(data_types::u8, format::bs_fs_zyx_bsv32_fsv32),

        std::make_tuple(data_types::f32, format::bs_fs_yx_bsv32_fsv16),
        std::make_tuple(data_types::f16, format::bs_fs_yx_bsv32_fsv16),
        std::make_tuple(data_types::i8, format::bs_fs_yx_bsv32_fsv16),
        std::make_tuple(data_types::u8, format::bs_fs_yx_bsv32_fsv16),

        std::make_tuple(data_types::f32, format::bs_fs_zyx_bsv32_fsv16),
        std::make_tuple(data_types::f16, format::bs_fs_zyx_bsv32_fsv16),
        std::make_tuple(data_types::i8, format::bs_fs_zyx_bsv32_fsv16),
        std::make_tuple(data_types::u8, format::bs_fs_zyx_bsv32_fsv16),
    });
}

}  // namespace detail
}  // namespace ocl
}  // namespace cldnn

BIND_BINARY_BUFFER_WITH_TYPE(cldnn::ocl::quantize_impl)
