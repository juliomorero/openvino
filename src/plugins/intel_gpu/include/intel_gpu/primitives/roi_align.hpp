// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once
#include "primitive.hpp"
#include <vector>

namespace cldnn {

/// @brief ROIAlign is a pooling layer used over feature maps of
/// non-uniform input sizes and outputs a feature map of a fixed size.
struct roi_align : public primitive_base<roi_align> {
    CLDNN_DECLARE_PRIMITIVE(roi_align)

    /// @brief Pooling mode for the @ref roi_align
    enum PoolingMode { max, avg };

    /// @brief Aligned mode for the @ref roi_align
    enum AlignedMode { asymmetric, half_pixel_for_nn, half_pixel };

    /// @brief Constructs roi_align primitive.
    /// @param id This primitive id.
    /// @param inputs Inputs data primitive ids.
    /// @param pooled_h Height of the ROI output feature map.
    /// @param pooled_w Width of the ROI output feature map.
    /// @param sampling_ratio Number of bins over height and width to use to calculate each output feature map element.
    /// @param spatial_scale multiplicative spatial scale factor to translate ROI coordinates
    /// from their input spatial scale to the scale used when pooling.
    /// @param pooling_mode Method to perform pooling to produce output feature map elements.
    /// @param aligned_mode Method to coordinates alignment.
    roi_align(const primitive_id& id,
              const std::vector<input_info>& inputs,
              int pooled_h,
              int pooled_w,
              int sampling_ratio,
              float spatial_scale,
              PoolingMode pooling_mode,
              AlignedMode aligned_mode,
              const padding& output_padding = padding())
        : primitive_base(id, inputs, {output_padding}),
          pooled_h{pooled_h},
          pooled_w{pooled_w},
          sampling_ratio{sampling_ratio},
          spatial_scale{spatial_scale},
          pooling_mode{pooling_mode},
          aligned_mode{aligned_mode} {}

    /// @brief Height of the ROI output feature map.
    int pooled_h;
    /// @brief Width of the ROI output feature map.
    int pooled_w;
    /// @brief Number of bins over height and width to use to calculate each output feature map element.
    int sampling_ratio;
    /// @brief multiplicative spatial scale factor to translate ROI coordinates
    /// from their input spatial scale to the scale used when pooling.
    float spatial_scale;
    /// @brief Method to perform pooling to produce output feature map elements.
    PoolingMode pooling_mode;
    /// @brief Method to coordinate alignment.
    AlignedMode aligned_mode;
};
}  // namespace cldnn
