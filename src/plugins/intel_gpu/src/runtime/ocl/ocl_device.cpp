﻿// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ocl_device.hpp"
#include "ocl_common.hpp"
#include "intel_gpu/runtime/debug_configuration.hpp"

#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <cassert>
#include <time.h>
#include <limits>
#include <chrono>
#include <fstream>
#include <iostream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <cstring>
#else
#include <unistd.h>
#include <limits.h>
#include <link.h>
#include <dlfcn.h>
#endif


namespace cldnn {
namespace ocl {

namespace {
int driver_dev_id() {
    const std::vector<int> unused_ids = {
        0x4905, 0x4906, 0x4907, 0x4908
    };
    std::vector<int> result;

#ifdef _WIN32
    {
        HDEVINFO device_info_set = SetupDiGetClassDevsA(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
        if (device_info_set == INVALID_HANDLE_VALUE)
            return 0;

        SP_DEVINFO_DATA devinfo_data;
        std::memset(&devinfo_data, 0, sizeof(devinfo_data));
        devinfo_data.cbSize = sizeof(devinfo_data);

        for (DWORD dev_idx = 0; SetupDiEnumDeviceInfo(device_info_set, dev_idx, &devinfo_data); dev_idx++) {
            const size_t kBufSize = 512;
            char buf[kBufSize];
            if (!SetupDiGetDeviceInstanceIdA(device_info_set, &devinfo_data, buf, kBufSize, NULL)) {
                continue;
            }

            char* vendor_pos = std::strstr(buf, "VEN_");
            if (vendor_pos != NULL && std::stoi(vendor_pos + 4, NULL, 16) == 0x8086) {
                char* device_pos = strstr(vendor_pos, "DEV_");
                if (device_pos != NULL) {
                    result.push_back(std::stoi(device_pos + 4, NULL, 16));
                }
            }
        }

        if (device_info_set) {
            SetupDiDestroyDeviceInfoList(device_info_set);
        }
    }
#elif defined(__linux__)
    {
        std::string dev_base{ "/sys/devices/pci0000:00/0000:00:02.0/" };
        std::ifstream ifs(dev_base + "vendor");
        if (ifs.good()) {
            int ven_id;
            ifs >> std::hex >> ven_id;
            ifs.close();
            if (ven_id == 0x8086) {
                ifs.open(dev_base + "device");
                if (ifs.good()) {
                    int res = 0;
                    ifs >> std::hex >> res;
                    result.push_back(res);
                }
            }
        }
    }
#endif

    auto id_itr = result.begin();
    while (id_itr != result.end()) {
        if (std::find(unused_ids.begin(), unused_ids.end(), *id_itr) != unused_ids.end())
            id_itr = result.erase(id_itr);
        else
            id_itr++;
    }

    if (result.empty())
        return 0;
    else
        return result.back();
}

device_type get_device_type(const cl::Device& device) {
    auto unified_mem = device.getInfo<CL_DEVICE_HOST_UNIFIED_MEMORY>();

    return unified_mem ? device_type::integrated_gpu : device_type::discrete_gpu;
}

gfx_version parse_version(cl_uint ver) {
    uint16_t major = ver >> 16;
    uint8_t minor = (ver >> 8) & 0xFF;
    uint8_t revision = ver & 0xFF;

    return {major, minor, revision};
}

bool get_imad_support(const cl::Device& device) {
    std::string dev_name = device.getInfo<CL_DEVICE_NAME>();

    if (dev_name.find("Gen12") != std::string::npos ||
        dev_name.find("Xe") != std::string::npos)
        return true;

    if (get_device_type(device) == device_type::integrated_gpu) {
        const std::vector<int> imad_ids = {
            0x9A40, 0x9A49, 0x9A59, 0x9AD9,
            0x9A60, 0x9A68, 0x9A70, 0x9A78,
            0x9A7F, 0x9AF8, 0x9AC0, 0x9AC9
        };
        int dev_id = driver_dev_id();
        if (dev_id == 0)
            return false;

        if (std::find(imad_ids.begin(), imad_ids.end(), dev_id) != imad_ids.end())
            return true;
    } else {
        return true;
    }

    return false;
}

device_info init_device_info(const cl::Device& device) {
    device_info info = {};
    info.vendor_id = static_cast<uint32_t>(device.getInfo<CL_DEVICE_VENDOR_ID>());
    info.dev_name = device.getInfo<CL_DEVICE_NAME>();
    info.driver_version = device.getInfo<CL_DRIVER_VERSION>();
    info.dev_type = get_device_type(device);

    info.execution_units_count = device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();

    info.gpu_frequency = static_cast<uint32_t>(device.getInfo<CL_DEVICE_MAX_CLOCK_FREQUENCY>());

    info.max_work_group_size = static_cast<uint64_t>(device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>());

    info.max_local_mem_size = static_cast<uint64_t>(device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>());
    info.max_global_mem_size = static_cast<uint64_t>(device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>());
    info.max_alloc_mem_size = static_cast<uint64_t>(device.getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>());

    info.supports_image = static_cast<uint8_t>(device.getInfo<CL_DEVICE_IMAGE_SUPPORT>());
    info.max_image2d_width = static_cast<uint64_t>(device.getInfo<CL_DEVICE_IMAGE2D_MAX_WIDTH>());
    info.max_image2d_height = static_cast<uint64_t>(device.getInfo<CL_DEVICE_IMAGE2D_MAX_HEIGHT>());

    // Check for supported features.
    auto extensions = device.getInfo<CL_DEVICE_EXTENSIONS>();
    extensions.push_back(' ');  // Add trailing space to ease searching (search with keyword with trailing space).

    info.supports_intel_planar_yuv = extensions.find("cl_intel_planar_yuv ") != std::string::npos;
    info.supports_fp16 = extensions.find("cl_khr_fp16 ") != std::string::npos;
    info.supports_fp64 = extensions.find("cl_khr_fp64 ") != std::string::npos;
    info.supports_fp16_denorms = info.supports_fp16 && (device.getInfo<CL_DEVICE_HALF_FP_CONFIG>() & CL_FP_DENORM) != 0;

    info.supports_khr_subgroups = extensions.find("cl_khr_subgroups ") != std::string::npos;
    info.supports_intel_subgroups = extensions.find("cl_intel_subgroups ") != std::string::npos;
    info.supports_intel_subgroups_short = extensions.find("cl_intel_subgroups_short ") != std::string::npos;
    info.supports_intel_subgroups_char = extensions.find("cl_intel_subgroups_char ") != std::string::npos;
    info.supports_intel_required_subgroup_size = extensions.find("cl_intel_required_subgroup_size ") != std::string::npos;

    info.supports_imad = get_imad_support(device);
    info.supports_immad = false;

    info.supports_usm = extensions.find("cl_intel_unified_shared_memory ") != std::string::npos ||
                        extensions.find("cl_intel_unified_shared_memory_preview ") != std::string::npos;

    info.supports_local_block_io = extensions.find("cl_intel_subgroup_local_block_io ") != std::string::npos;

    info.supports_queue_families = extensions.find("cl_intel_command_queue_families ") != std::string::npos;

    if (info.supports_intel_required_subgroup_size) {
        info.supported_simd_sizes = device.getInfo<CL_DEVICE_SUB_GROUP_SIZES_INTEL>();
    } else {
        // Set these values as reasonable default for most of the supported platforms
        info.supported_simd_sizes = {8, 16, 32};
    }

    bool device_uuid_supported = extensions.find("cl_khr_device_uuid ") != std::string::npos;
    if (device_uuid_supported) {
        static_assert(CL_UUID_SIZE_KHR == device_uuid::max_uuid_size, "");
        info.uuid.val = device.getInfo<CL_DEVICE_UUID_KHR>();
    } else {
        std::fill_n(std::begin(info.uuid.val), device_uuid::max_uuid_size, 0);
    }

    bool device_attr_supported = extensions.find("cl_intel_device_attribute_query ") != std::string::npos;
    if (device_attr_supported) {
        info.gfx_ver = parse_version(device.getInfo<CL_DEVICE_IP_VERSION_INTEL>());
        info.device_id = device.getInfo<CL_DEVICE_ID_INTEL>();
        info.num_slices = device.getInfo<CL_DEVICE_NUM_SLICES_INTEL>();
        info.num_sub_slices_per_slice = device.getInfo<CL_DEVICE_NUM_SUB_SLICES_PER_SLICE_INTEL>();
        info.num_eus_per_sub_slice = device.getInfo<CL_DEVICE_NUM_EUS_PER_SUB_SLICE_INTEL>();
        info.num_threads_per_eu = device.getInfo<CL_DEVICE_NUM_THREADS_PER_EU_INTEL>();
        auto features = device.getInfo<CL_DEVICE_FEATURE_CAPABILITIES_INTEL>();

        info.supports_imad = info.supports_imad || (features & CL_DEVICE_FEATURE_FLAG_DP4A_INTEL);
        info.supports_immad = info.supports_immad || (features & CL_DEVICE_FEATURE_FLAG_DPAS_INTEL);
        GPU_DEBUG_GET_INSTANCE(debug_config);
        GPU_DEBUG_IF(debug_config->disable_onednn)
            info.supports_immad = false;
    } else {
        info.gfx_ver = {0, 0, 0};
        info.device_id = driver_dev_id();
        info.num_slices = 0;
        info.num_sub_slices_per_slice = 0;
        info.num_eus_per_sub_slice = 0;
        info.num_threads_per_eu = 0;
    }

    info.num_ccs = 1;
    if (info.supports_queue_families) {
        cl_uint num_queues = 0;

        std::vector<cl_queue_family_properties_intel> qfprops = device.getInfo<CL_DEVICE_QUEUE_FAMILY_PROPERTIES_INTEL>();
        for (cl_uint q = 0; q < qfprops.size(); q++) {
            if (qfprops[q].capabilities == CL_QUEUE_DEFAULT_CAPABILITIES_INTEL && qfprops[q].count > num_queues) {
                num_queues = qfprops[q].count;
            }
        }
        info.num_ccs = std::max<uint32_t>(num_queues, info.num_ccs);
    }

    return info;
}

bool does_device_support(int32_t param, const cl::Device& device) {
    cl_device_unified_shared_memory_capabilities_intel capabilities;
    auto err = clGetDeviceInfo(device.get(), param, sizeof(cl_device_unified_shared_memory_capabilities_intel), &capabilities, NULL);
    if (err) throw std::runtime_error("[CLDNN ERROR]. clGetDeviceInfo error " + std::to_string(err));
    return !((capabilities & CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL) == 0u);
}

memory_capabilities init_memory_caps(const cl::Device& device, const device_info& info) {
    std::vector<allocation_type> memory_caps;
    if (info.supports_usm) {
        if (does_device_support(CL_DEVICE_HOST_MEM_CAPABILITIES_INTEL, device)) {
            memory_caps.push_back(allocation_type::usm_host);
        }
        if (does_device_support(CL_DEVICE_SINGLE_DEVICE_SHARED_MEM_CAPABILITIES_INTEL, device)) {
            memory_caps.push_back(allocation_type::usm_shared);
        }
        if (does_device_support(CL_DEVICE_DEVICE_MEM_CAPABILITIES_INTEL, device)) {
            memory_caps.push_back(allocation_type::usm_device);
        }
    }

    return memory_capabilities(memory_caps);
}

}  // namespace


ocl_device::ocl_device(const cl::Device dev, const cl::Context& ctx, const cl_platform_id platform)
: _context(ctx)
, _device(dev)
, _platform(platform)
, _info(init_device_info(dev))
, _mem_caps(init_memory_caps(dev, _info)) { }

bool ocl_device::is_same(const device::ptr other) {
    auto casted = downcast<ocl_device>(other.get());
    if (!casted)
        return false;

    return _device == casted->get_device() && _platform == casted->get_platform();
}

}  // namespace ocl
}  // namespace cldnn
