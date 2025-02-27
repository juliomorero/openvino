﻿// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "broadcast_kernel_base.h"
#include <vector>
#include "kernel_selector_utils.h"

namespace kernel_selector {
JitConstants BroadcastKernelBase::GetJitConstants(const broadcast_params& params) const {
    JitConstants jit = MakeBaseParamsJitConstants(params);

    jit.AddConstants({MakeJitConstant("BROADCAST_ORDER", params.input_order)});

    return jit;
}

BroadcastKernelBase::DispatchData BroadcastKernelBase::SetDefault(const broadcast_params& params) {
    const auto& output = params.outputs[0];

    DispatchData dispatchData;
    auto in_layout = params.inputs[0].GetLayout();
    auto out_layout = params.outputs[0].GetLayout();
    std::vector<std::vector<Tensor::DataChannelName>> dims_by_gws = {{ Tensor::DataChannelName::X },
                                                                     { Tensor::DataChannelName::Y, Tensor::DataChannelName::Z, Tensor::DataChannelName::W },
                                                                     { Tensor::DataChannelName::FEATURE, Tensor::DataChannelName::BATCH }};

    dispatchData.gws = { output.X().v, output.Y().v * output.Z().v * output.W().v, output.Batch().v * output.Feature().v };
    dispatchData.lws = GetOptimalLocalWorkGroupSizes(dispatchData.gws, params.engineInfo, in_layout, out_layout, dims_by_gws);

    return dispatchData;
}

static std::string GetInputBlockND(const broadcast_params& params) {
    const auto& input = params.inputs[0];
    auto input_dims = input.LogicalDims();
    std::reverse(input_dims.begin(), input_dims.end());
    const int rank = static_cast<int>(input_dims.size());
    std::vector<size_t> block_nd(rank + 1);
    std::vector<std::string> block_nd_s(rank + 1);
    block_nd[rank] = 1;
    block_nd_s[rank] = "1";
    for (int idx = (rank - 1); idx >= 0; idx--) {
        block_nd[idx] = input_dims[idx] * block_nd[idx + 1];
        block_nd_s[idx] = "(" + toCodeString(input.GetDims()[idx], rank - idx) + " * " + block_nd_s[idx + 1] + ")";
    }

    std::stringstream s;
    for (int i = 0; i < (rank + 1); i++) {
        if (i < rank) {
            s << (input.is_dynamic() ? block_nd_s[i] : std::to_string(block_nd[i])) << ",";
        } else {
            s << (input.is_dynamic() ? block_nd_s[i] : std::to_string(block_nd[i]));
        }
    }
    auto str_result = s.str();
    return str_result;
}

KernelsData BroadcastKernelBase::GetCommonKernelsData(const Params& params,
                                                      const optional_params& options) const {
    assert(params.GetType() == KernelType::BROADCAST);

    const auto& prim_params = static_cast<const broadcast_params&>(params);

    auto dispatchData = SetDefault(prim_params);
    KernelData k_data = KernelData::Default<broadcast_params>(params);

    k_data.update_dispatch_data_func = [](const Params& params, KernelData& kd) {
        const auto& prim_params = static_cast<const broadcast_params&>(params);
        auto dispatchData = SetDefault(prim_params);
        OPENVINO_ASSERT(kd.kernels.size() == 1, "[GPU] Invalid kernels size for update dispatch data func");
        kd.kernels[0].params.workGroups.global = dispatchData.gws;
        kd.kernels[0].params.workGroups.local = dispatchData.lws;
    };

    auto cldnn_jit = GetJitConstants(prim_params);
    cldnn_jit.AddConstant(MakeJitConstant("INPUT0_BLOCK_ND", GetInputBlockND(prim_params)));
    auto entry_point = GetEntryPoint(kernelName, prim_params.layerID, params, options);
    auto jit = CreateJit(kernelName, cldnn_jit, entry_point);

    auto& kernel = k_data.kernels[0];
    FillCLKernelData(kernel, dispatchData, params.engineInfo, kernelName, jit, entry_point,
                     EXE_MODE_DEFAULT,
                     false,
                     false,
                     1,
                     0,
                     1,
                     prim_params.inputs[0].is_dynamic() || prim_params.outputs[0].is_dynamic());

    return {k_data};
}
}  // namespace kernel_selector
