// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "arg_max_min_kernel_base.h"

namespace kernel_selector {
class ArgMaxMinKernelOpt : public ArgMaxMinKernelBase {
public:
    ArgMaxMinKernelOpt() : ArgMaxMinKernelBase("arg_max_min_opt") {}
    virtual ~ArgMaxMinKernelOpt() {}

    KernelsData GetKernelsData(const Params& params, const optional_params& options) const override;
    KernelsPriority GetKernelsPriority(const Params& params, const optional_params& options) const override;
    ParamsKey GetSupportedKey() const override;
    DeviceFeaturesKey get_required_device_features_key(const Params& params, const optional_params& /*options*/) const override;
};
}  // namespace kernel_selector
