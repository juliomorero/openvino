// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "primitive.hpp"
#include <vector>

namespace cldnn {

/// @brief Select mode for the @ref reduce layer
enum class reduce_mode : uint16_t {
    /// @brief Reduce max
    max,
    /// @brief Reduce min
    min,
    /// @brief Reduce mean
    mean,
    /// @brief Reduce prod
    prod,
    /// @brief Reduce sum
    sum,
    /// @brief Reduce and
    logical_and,
    /// @brief Reduce or
    logical_or,
    /// @brief Reduce  sum_square
    sum_square,
    /// @brief Reduce l1
    l1,
    /// @brief Reduce l2
    l2,
    /// @brief Reduce log_sum
    log_sum,
    /// @brief Reduce sum_exp
    log_sum_exp
};

/// @brief Applies the specific reduction function along provided axes (second input) of the input tensor (first input).
/// @details
struct reduce : public primitive_base<reduce> {
    CLDNN_DECLARE_PRIMITIVE(reduce)

    /// @brief Constructs reduce primitive
    /// @param id This primitive id
    /// @param input Input primitive id
    /// @param keep_dims The axes which reduced
    reduce(const primitive_id& id,
           const input_info& input,
           const reduce_mode mode,
           const std::vector<int64_t> axes,
           const bool keep_dims,
           const padding& output_padding = padding())
        : primitive_base(id, {input}, {output_padding}), mode(mode), axes(axes), keep_dims(keep_dims) {}

    /// @brief Reduce operation type
    reduce_mode mode;
    /// @brief List of axes to reduce
    std::vector<int64_t> axes;
    /// @brief Keep the reduced dimension or not, 1 mean keep reduced dimension
    bool keep_dims;
};
}  // namespace cldnn
