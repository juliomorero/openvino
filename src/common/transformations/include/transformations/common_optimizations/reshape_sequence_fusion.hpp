// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <openvino/pass/graph_rewrite.hpp>
#include <transformations_visibility.hpp>

namespace ov {
namespace pass {

class TRANSFORMATIONS_API ReshapeSequenceFusion;

}  // namespace pass
}  // namespace ov

/**
 * @ingroup ie_transformation_common_api
 * @brief ReshapeSequenceFusion fuses sequence of Reshape operation into single Reshape or eliminates full redundant
 * sequence
 */

class ov::pass::ReshapeSequenceFusion : public ov::pass::MatcherPass {
public:
    OPENVINO_RTTI("ReshapeSequenceFusion", "0");
    ReshapeSequenceFusion(bool use_shape_for_elimination = true);
};

namespace ngraph {
namespace pass {
using ov::pass::ReshapeSequenceFusion;
}  // namespace pass
}  // namespace ngraph
