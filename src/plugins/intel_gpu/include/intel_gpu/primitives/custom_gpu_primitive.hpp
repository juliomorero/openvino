// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once
#include "primitive.hpp"
#include "intel_gpu/runtime/memory.hpp"
#include <vector>
#include <string>

namespace cldnn {

/// @brief This primitive executes a custom kernel provided by the application
/// @details The application is required to provide all relevant details for executing the custom kernel
/// such as: sources, entry point, work sizes and parameter bindings.
struct custom_gpu_primitive : public primitive_base<custom_gpu_primitive> {
    CLDNN_DECLARE_PRIMITIVE(custom_gpu_primitive)

    /// @brief Custom primitive kernel argument type
    enum arg_type {
        arg_input,
        arg_output,
    };
    //
    /// @brief Custom primitive kernel argument index
    using arg_index = uint32_t;
    //
    /// @brief Custom primitive kernel argument description
    struct arg_desc {
        arg_type type;
        arg_index index;
    };

    /// @brief Constructs custom_gpu_primitive primitive
    /// @param id This primitive id.
    /// @param input Input primitive ids.
    /// @param kernels_code Source code for the kernel
    /// @param kernel_entry_point The name of the entry point function in the kernel
    /// @param kernel_arguments Argument bindings for the entry point function
    /// @param build_options Build options/flags used during the compilation of the custom kernel
    /// @param output_layout Output layout declared by the primitive
    /// @param gws Global work sizes
    /// @param lws Local work sizes
    custom_gpu_primitive(const primitive_id& id,
                         const std::vector<input_info>& inputs,
                         const std::vector<std::string>& kernels_code,
                         const std::string& kernel_entry_point,
                         const std::vector<arg_desc>& kernel_arguments,
                         const std::string& build_options,
                         const layout& output_layout,
                         const std::vector<size_t>& gws = {},
                         const std::vector<size_t>& lws = {})
        : primitive_base(id, inputs, {output_layout.data_padding}),
          kernel_entry_point(kernel_entry_point),
          kernel_arguments(kernel_arguments),
          build_options(build_options),
          output_layout(output_layout),
          gws(gws.size() ? gws : std::vector<size_t>{output_layout.count()}),
          lws(lws),
          kernels_code(kernels_code) {}

    /// @brief The name of the entry point function in the kernel
    const std::string kernel_entry_point;
    /// @brief Argument bindings for the entry point function
    const std::vector<arg_desc> kernel_arguments;
    /// @brief The kernel's build options
    const std::string build_options;
    /// @brief The output layout declared by the primitive
    const layout output_layout;
    /// @brief The global working sizes
    const std::vector<size_t> gws;
    /// @brief The local working sizes
    const std::vector<size_t> lws;
    /// @brief Source code for the kernel
    const primitive_id_arr kernels_code;
};
}  // namespace cldnn
