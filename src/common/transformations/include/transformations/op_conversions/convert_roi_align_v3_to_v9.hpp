// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <openvino/pass/graph_rewrite.hpp>
#include <transformations_visibility.hpp>

namespace ov {
namespace pass {

class TRANSFORMATIONS_API ConvertROIAlign3To9;

}  // namespace pass
}  // namespace ov

/**
 * @ingroup ie_transformation_common_api
 * @brief ConvertROIAlign3To9 converts v3::ROIAlign into v9::ROIAlign.
 */
class ov::pass::ConvertROIAlign3To9 : public ov::pass::MatcherPass {
public:
    OPENVINO_RTTI("ConvertROIAlign3To9", "0");
    ConvertROIAlign3To9();
};

namespace ngraph {
namespace pass {
using ov::pass::ConvertROIAlign3To9;
}  // namespace pass
}  // namespace ngraph
