// Copyright (C) 2019-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ocl_stream.hpp"
#include "ocl_event.hpp"
#include "ocl_user_event.hpp"
#include "ocl_command_queues_builder.hpp"
#include "intel_gpu/runtime/debug_configuration.hpp"
#include "ocl_kernel.hpp"
#include "ocl_common.hpp"

#include <cassert>
#include <iomanip>
#include <ios>

#include <fstream>
#include <thread>
#include <string>
#include <vector>
#include <memory>

// NOTE: Due to buggy scope transition of warnings we need to disable warning in place of use/instantation
//       of some types (even though we already disabled them in scope of definition of these types).
//       Moreover this warning is pretty much now only for annoyance: it is generated due to lack
//       of proper support for mangling of custom GCC attributes into type name (usually when used
//       with templates, even from standard library).
#if defined __GNUC__ && __GNUC__ >= 6
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

#ifdef ENABLE_ONEDNN_FOR_GPU
#include <oneapi/dnnl/dnnl_ocl.hpp>
#endif

namespace cldnn {
namespace ocl {

namespace {
inline cl::NDRange toNDRange(const std::vector<size_t>& v) {
    switch (v.size()) {
        case 1:
            return cl::NDRange(v[0]);
        case 2:
            return cl::NDRange(v[0], v[1]);
        case 3:
            return cl::NDRange(v[0], v[1], v[2]);
        default:
            return cl::NullRange;
    }
}

cl_int set_kernel_arg(ocl_kernel_type& kernel, uint32_t idx, cldnn::memory::cptr mem) {
    if (!mem)
        return CL_INVALID_ARG_VALUE;

    if (mem->get_layout().format.is_image_2d()) {
        auto buf = std::dynamic_pointer_cast<const ocl::gpu_image2d>(mem)->get_buffer();
        GPU_DEBUG_TRACE_DETAIL << "kernel: " << kernel.get() << " set arg (image) " << idx << " mem: " << buf.get() << " size: " << mem->size() << std::endl;
        return kernel.setArg(idx, buf);
    } else if (memory_capabilities::is_usm_type(mem->get_allocation_type())) {
        auto buf = std::dynamic_pointer_cast<const ocl::gpu_usm>(mem)->get_buffer();
        GPU_DEBUG_TRACE_DETAIL << "kernel: " << kernel.get() << " set arg (usm) " << idx << " mem: " << buf.get() << " size: " << mem->size() << std::endl;
        return kernel.setArgUsm(idx, buf);
    } else {
        auto buf = std::dynamic_pointer_cast<const ocl::gpu_buffer>(mem)->get_buffer();
        GPU_DEBUG_TRACE_DETAIL << "kernel: " << kernel.get() << " set arg (buffer) " << idx << " mem: " << buf.get() << " size: " << mem->size() << std::endl;
        return kernel.setArg(idx, buf);
    }

    return CL_INVALID_ARG_VALUE;
}

void set_arguments_impl(ocl_kernel_type& kernel,
                        const arguments_desc& args,
                        const kernel_arguments_data& data) {
    using args_t = argument_desc::Types;
    using scalar_t = scalar_desc::Types;
    for (uint32_t i = 0; i < static_cast<uint32_t>(args.size()); i++) {
        cl_int status = CL_INVALID_ARG_VALUE;
        switch (args[i].t) {
            case args_t::INPUT:
                if (args[i].index < data.inputs.size() && data.inputs[args[i].index]) {
                    status = set_kernel_arg(kernel, i, data.inputs[args[i].index]);
                }
                break;
            case args_t::INPUT_OF_FUSED_PRIMITIVE:
                if (args[i].index < data.fused_op_inputs.size() && data.fused_op_inputs[args[i].index]) {
                    status = set_kernel_arg(kernel, i, data.fused_op_inputs[args[i].index]);
                }
                break;
            case args_t::INTERNAL_BUFFER:
                if (args[i].index < data.intermediates.size() && data.intermediates[args[i].index]) {
                    status = set_kernel_arg(kernel, i, data.intermediates[args[i].index]);
                }
                break;
            case args_t::OUTPUT:
                if (args[i].index < data.outputs.size() && data.outputs[args[i].index]) {
                    status = set_kernel_arg(kernel, i, data.outputs[args[i].index]);
                }
                break;
            case args_t::WEIGHTS:
                status = set_kernel_arg(kernel, i, data.weights);
                break;
            case args_t::BIAS:
                status = set_kernel_arg(kernel, i, data.bias);
                break;
            case args_t::WEIGHTS_ZERO_POINTS:
                status = set_kernel_arg(kernel, i, data.weights_zero_points);
                break;
            case args_t::ACTIVATIONS_ZERO_POINTS:
                status = set_kernel_arg(kernel, i, data.activations_zero_points);
                break;
            case args_t::COMPENSATION:
                status = set_kernel_arg(kernel, i, data.compensation);
                break;
            case args_t::SCALE_TABLE:
                status = set_kernel_arg(kernel, i, data.scale_table);
                break;
            case args_t::SLOPE:
                status = set_kernel_arg(kernel, i, data.slope);
                break;
            case args_t::SCALAR:
                if (data.scalars && args[i].index < data.scalars->size()) {
                    const auto& scalar = (*data.scalars)[args[i].index];
                    switch (scalar.t) {
                        case scalar_t::UINT8:
                            status = kernel.setArg(i, scalar.v.u8);
                            break;
                        case scalar_t::UINT16:
                            status = kernel.setArg(i, scalar.v.u16);
                            break;
                        case scalar_t::UINT32:
                            status = kernel.setArg(i, scalar.v.u32);
                            break;
                        case scalar_t::UINT64:
                            status = kernel.setArg(i, scalar.v.u64);
                            break;
                        case scalar_t::INT8:
                            status = kernel.setArg(i, scalar.v.s8);
                            break;
                        case scalar_t::INT16:
                            status = kernel.setArg(i, scalar.v.s16);
                            break;
                        case scalar_t::INT32:
                            status = kernel.setArg(i, scalar.v.s32);
                            break;
                        case scalar_t::INT64:
                            status = kernel.setArg(i, scalar.v.s64);
                            break;
                        case scalar_t::FLOAT32:
                            status = kernel.setArg(i, scalar.v.f32);
                            break;
                        case scalar_t::FLOAT64:
                            status = kernel.setArg(i, scalar.v.f64);
                            break;
                        default:
                            break;
                    }
                }
                break;
            case args_t::RECURRENT:
                status = set_kernel_arg(kernel, i, data.recurrent);
                break;
            case args_t::HIDDEN:
                status = set_kernel_arg(kernel, i, data.hidden);
                break;
            case args_t::CELL:
                status = set_kernel_arg(kernel, i, data.cell);
                break;
            case args_t::SHAPE_INFO:
                status = set_kernel_arg(kernel, i, data.shape_info);
                break;
            default:
                break;
        }

        if (status != CL_SUCCESS) {
            throw std::runtime_error("Error set arg " + std::to_string(i) + ", error code: " + std::to_string(status) + "\n");
        }
    }
}

sync_methods get_expected_sync_method(const ExecutionConfig& config) {
    auto profiling = config.get_property(ov::enable_profiling);
    auto queue_type = config.get_property(ov::intel_gpu::queue_type);
    return profiling ? sync_methods::events : queue_type == QueueTypes::out_of_order ? sync_methods::barriers
                                                                                     : sync_methods::none;
}

}  // namespace

ocl_stream::ocl_stream(const ocl_engine &engine, const ExecutionConfig& config)
    : stream(config.get_property(ov::intel_gpu::queue_type))
    , _engine(engine)
    , sync_method(get_expected_sync_method(config)) {
    auto context = engine.get_cl_context();
    auto device = engine.get_cl_device();
    ocl::command_queues_builder queue_builder;
    queue_builder.set_profiling(config.get_property(ov::enable_profiling));
    queue_builder.set_out_of_order(queue_type == QueueTypes::out_of_order);

    if (sync_method == sync_methods::none && queue_type == QueueTypes::out_of_order) {
        throw std::runtime_error("[CLDNN] Unexpected sync method (none) is specified for out_of_order queue");
    }

    bool priorty_extensions = engine.extension_supported("cl_khr_priority_hints") && engine.extension_supported("cl_khr_create_command_queue");
    queue_builder.set_priority_mode(config.get_property(ov::intel_gpu::hint::queue_priority), priorty_extensions);

    bool throttle_extensions = engine.extension_supported("cl_khr_throttle_hints") && engine.extension_supported("cl_khr_create_command_queue");
    queue_builder.set_throttle_mode(config.get_property(ov::intel_gpu::hint::queue_throttle), throttle_extensions);

    bool queue_families_extension = engine.get_device_info().supports_queue_families;
    queue_builder.set_supports_queue_families(queue_families_extension);

    _command_queue = queue_builder.build(context, device);
}

ocl_stream::ocl_stream(const ocl_engine &engine, const ExecutionConfig& config, void *handle)
    : stream(ocl_stream::detect_queue_type(handle))
    , _engine(engine)
    , sync_method(get_expected_sync_method(config)) {
    auto casted_handle = static_cast<cl_command_queue>(handle);
    _command_queue = ocl_queue_type(casted_handle, true);
}

#ifdef ENABLE_ONEDNN_FOR_GPU
dnnl::stream& ocl_stream::get_onednn_stream() {
    OPENVINO_ASSERT(queue_type == QueueTypes::in_order, "[GPU] Can't create onednn stream handle as onednn doesn't support out-of-order queue");
    OPENVINO_ASSERT(_engine.get_device_info().vendor_id == INTEL_VENDOR_ID, "[GPU] Can't create onednn stream handle as for non-Intel devices");
    if (!_onednn_stream) {
        _onednn_stream = std::make_shared<dnnl::stream>(dnnl::ocl_interop::make_stream(_engine.get_onednn_engine(), _command_queue.get()));
    }

    return *_onednn_stream;
}
#endif

QueueTypes ocl_stream::detect_queue_type(void *queue_handle) {
    cl_command_queue queue = static_cast<cl_command_queue>(queue_handle);
    cl_command_queue_properties properties;
    auto status = clGetCommandQueueInfo(queue, CL_QUEUE_PROPERTIES, sizeof(cl_command_queue_properties), &properties, nullptr);
    if (status != CL_SUCCESS) {
        throw std::runtime_error("Can't get queue properties for user handle\n");
    }

    return (properties & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) ? QueueTypes::out_of_order : QueueTypes::in_order;
}

void ocl_stream::set_arguments(kernel& kernel, const kernel_arguments_desc& args_desc, const kernel_arguments_data& args) {
    static std::mutex m;
    std::lock_guard<std::mutex> guard(m);

    auto& ocl_kernel = downcast<ocl::ocl_kernel>(kernel);

    auto& kern = ocl_kernel.get_handle();

    try {
        GPU_DEBUG_TRACE_DETAIL << "Set arguments for primitive: " << args_desc.layerID << " (" << kern.get() << ")\n";
        set_arguments_impl(kern, args_desc.arguments, args);
    } catch (cl::Error const& err) {
        throw ocl_error(err);
    }
}

event::ptr ocl_stream::enqueue_kernel(kernel& kernel,
                                      const kernel_arguments_desc& args_desc,
                                      const kernel_arguments_data& /* args */,
                                      std::vector<event::ptr> const& deps,
                                      bool is_output) {
    auto& ocl_kernel = downcast<ocl::ocl_kernel>(kernel);

    auto& kern = ocl_kernel.get_handle();
    auto global = toNDRange(args_desc.workGroups.global);
    auto local = toNDRange(args_desc.workGroups.local);
    std::vector<cl::Event> dep_events;
    std::vector<cl::Event>* dep_events_ptr = nullptr;
    if (sync_method == sync_methods::events) {
        for (auto& dep : deps) {
            if (auto ocl_base_ev = std::dynamic_pointer_cast<ocl_base_event>(dep)) {
                if (ocl_base_ev->get().get() != nullptr)
                    dep_events.push_back(ocl_base_ev->get());
            }
        }
        dep_events_ptr = &dep_events;
    } else if (sync_method == sync_methods::barriers) {
        sync_events(deps, is_output);
    }

    cl::Event ret_ev;

    bool set_output_event = sync_method == sync_methods::events || is_output;

    try {
        _command_queue.enqueueNDRangeKernel(kern, cl::NullRange, global, local, dep_events_ptr, set_output_event ? &ret_ev : nullptr);
    } catch (cl::Error const& err) {
        throw ocl_error(err);
    }

    return std::make_shared<ocl_event>(ret_ev, ++_queue_counter);
}

void ocl_stream::enqueue_barrier() {
    _command_queue.enqueueBarrierWithWaitList(nullptr, nullptr);
}

event::ptr ocl_stream::enqueue_marker(std::vector<event::ptr> const& deps, bool is_output) {
    if (deps.empty())
        return std::make_shared<ocl_user_event>(_engine.get_cl_context(), true);

    if (sync_method == sync_methods::events) {
        cl::Event ret_ev;
        std::vector<cl::Event> dep_events;
        for (auto& dep : deps) {
            if (auto ocl_base_ev = dynamic_cast<ocl_base_event*>(dep.get()))
                if (ocl_base_ev->get().get() != nullptr)
                    dep_events.push_back(ocl_base_ev->get());
        }

        try {
            if (dep_events.empty()) {
                return create_user_event(true);
            }
            _command_queue.enqueueMarkerWithWaitList(&dep_events, &ret_ev);
        } catch (cl::Error const& err) {
            throw ocl_error(err);
        }

        return std::make_shared<ocl_event>(ret_ev, ++_queue_counter);
    } else if (sync_method == sync_methods::barriers) {
        sync_events(deps, is_output);
        return std::make_shared<ocl_event>(_last_barrier_ev, _last_barrier);
    } else {
        return std::make_shared<ocl_user_event>(_engine.get_cl_context(), true);
    }
}

event::ptr ocl_stream::group_events(std::vector<event::ptr> const& deps) {
    return std::make_shared<ocl_events>(deps);
}

event::ptr ocl_stream::create_user_event(bool set) {
    return std::make_shared<ocl_user_event>(_engine.get_cl_context(), set);
}

event::ptr ocl_stream::create_base_event() {
    cl::Event ret_ev;
    return std::make_shared<ocl_event>(ret_ev, ++_queue_counter);
}

void ocl_stream::flush() const { get_cl_queue().flush(); }
void ocl_stream::finish() const { get_cl_queue().finish(); }

void ocl_stream::wait_for_events(const std::vector<event::ptr>& events) {
    if (events.empty())
        return;

    bool needs_barrier = false;
    std::vector<cl::Event> clevents;
    for (auto& ev : events) {
        if (auto ocl_base_ev = downcast<ocl_base_event>(ev.get())) {
            if (ocl_base_ev->get().get() != nullptr) {
                clevents.push_back(ocl_base_ev->get());
            } else {
                needs_barrier = true;
            }
        }
    }

    if (needs_barrier) {
        try {
            cl::Event barrier_ev;
            _command_queue.enqueueBarrierWithWaitList(nullptr, &barrier_ev);
            clevents.push_back(barrier_ev);
        } catch (cl::Error const& err) {
            throw ocl_error(err);
        }
    }

    try {
        cl::WaitForEvents(clevents);
    } catch (cl::Error const& err) {
        throw ocl_error(err);
    }
}

void ocl_stream::sync_events(std::vector<event::ptr> const& deps, bool is_output) {
    bool needs_barrier = false;
    for (auto& dep : deps) {
        auto* ocl_base_ev = downcast<ocl_base_event>(dep.get());
        if (ocl_base_ev->get_queue_stamp() > _last_barrier) {
            needs_barrier = true;
        }
    }

    if (needs_barrier) {
        try {
            if (is_output)
                _command_queue.enqueueBarrierWithWaitList(nullptr, &_last_barrier_ev);
            else
                _command_queue.enqueueBarrierWithWaitList(nullptr, nullptr);
        } catch (cl::Error const& err) {
            throw ocl_error(err);
        }

        _last_barrier = ++_queue_counter;
    }
}

}  // namespace ocl
}  // namespace cldnn
