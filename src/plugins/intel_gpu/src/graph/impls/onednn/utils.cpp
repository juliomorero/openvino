// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "utils.hpp"
#include "onednn_formats_map.hpp"
#include <oneapi/dnnl/dnnl_debug.h>
#include <numeric>
#include <oneapi/dnnl/dnnl_ocl.hpp>

#include "to_string_utils.h"

namespace cldnn {
namespace onednn {

namespace {
void pad_dims(dnnl::memory::dims& padded_dims, cldnn::format format) {
    auto block_sizes = format.block_sizes();
    for (auto& block : block_sizes) {
        auto rounded_dim = round_up_to(padded_dims[block.first], block.second);
        padded_dims[block.first] = rounded_dim;
    }
}

std::string convert_data_format_string(cldnn::format fmt) {
    switch (fmt) {
        case cldnn::format::b_fs_yx_fsv2: return "aBcd2b";
        case cldnn::format::b_fs_zyx_fsv2: return "aBcde2b";
        case cldnn::format::bs_fs_yx_bsv16_fsv2: return "ABcd16a2b";
        case cldnn::format::bs_fs_zyx_bsv16_fsv2: return "ABcde16a2b";
        case cldnn::format::bs_fs_yx_bsv16_fsv4: return "ABcd16a4b";
        case cldnn::format::bs_fs_zyx_bsv16_fsv4: return "ABcde16a4b";
        case cldnn::format::bs_fs_yx_bsv16_fsv32: return "ABcd16a32b";
        case cldnn::format::bs_fs_zyx_bsv16_fsv32: return "ABcde16a32b";
        default: throw std::invalid_argument("[clDNN] Unsupported conversion from cldnn to onednn layout string" + fmt_to_str(fmt));
    }
}

dnnl::memory::desc create_memory_desc_from_format_string(dnnl::memory::dims dims, dnnl::memory::data_type dt, std::string tag) {
    dnnl::memory::desc desc;
    dnnl_memory_desc_t* md = &desc.data;

    int ndims = static_cast<int>(dims.size());
    md->ndims = ndims;
    if (ndims > DNNL_MAX_NDIMS) throw std::invalid_argument("[clDNN] Unsupported ndims " + std::to_string(ndims));

    std::copy(&dims[0], &dims[0] + ndims, md->dims);
    md->data_type = static_cast<dnnl_data_type_t>(dt);
    md->format_kind = dnnl_blocked;

    // Parse dimensions and their block sizes starting from the innermost one.
    std::vector<std::pair<int, int>> dim_blocks;
    int pos = static_cast<int>(tag.size()) - 1;
    int ndims_from_tag = -1;
    while (pos >= 0) {
        int pos0 = pos;

        --pos;
        while (pos >= 0 && std::isdigit(tag[pos]))
            pos--;

        int dim_idx = std::tolower(tag[pos0]) - 'a';
        if (dim_idx >= ndims) throw std::invalid_argument("[clDNN] Unsupported tag for oneDNN " + tag);
        ndims_from_tag = std::max(dim_idx + 1, ndims_from_tag);
        int block_str_len = pos0 - pos - 1;
        int block = (block_str_len == 0)
                ? 1
                : std::stoi(tag.substr(pos + 1, block_str_len));
        dim_blocks.emplace_back(dim_idx, block);
    }
    if (ndims_from_tag != ndims) throw std::invalid_argument("[clDNN] Unsupported tag for oneDNN " + tag);

    auto &blk = md->format_desc.blocking;

    // Compute strides and fill inner block sizes/indices.
    dnnl_dim_t stride = 1;
    dnnl_dims_t full_inner_blks;
    std::fill(full_inner_blks, full_inner_blks + md->ndims, 1);
    for (auto &p : dim_blocks) {
        int dim_idx = p.first;
        int block = p.second;
        if (block == 1) {
            assert(blk.strides[dim_idx] == 0);
            blk.strides[dim_idx] = stride;

            dnnl_dim_t fib = full_inner_blks[dim_idx];
            dnnl_dim_t padded_dim = md->dims[dim_idx] == DNNL_RUNTIME_DIM_VAL
                    ? DNNL_RUNTIME_DIM_VAL
                    : (md->dims[dim_idx] + fib - 1) / fib * fib;
            md->padded_dims[dim_idx] = padded_dim;
            if (padded_dim == DNNL_RUNTIME_DIM_VAL)
                stride = DNNL_RUNTIME_DIM_VAL;
            else
                stride *= (padded_dim / fib);
        } else {
            full_inner_blks[dim_idx] *= block;
            blk.inner_blks[blk.inner_nblks] = block;
            blk.inner_idxs[blk.inner_nblks] = dim_idx;
            blk.inner_nblks++;
            stride *= block;
        }
    }

    // Inner block sizes/indices are stored from the outermost to the innermost
    // so need to reverse them.
    std::reverse(blk.inner_blks, blk.inner_blks + blk.inner_nblks);
    std::reverse(blk.inner_idxs, blk.inner_idxs + blk.inner_nblks);

    return desc;
}
}  // namespace

template <typename T>
cldnn::memory::ptr convert_zp_data_to_s32(const memory::ptr zp_memory) {
    auto engine = zp_memory->get_engine();
    auto& stream = engine->get_service_stream();

    auto zp_s32_layout = zp_memory->get_layout();
    zp_s32_layout.data_type = data_types::i32;
    auto zp_s32_memory = engine->allocate_memory(zp_s32_layout, false);

    mem_lock<T, mem_lock_type::read> zp_data(zp_memory, stream);
    mem_lock<int32_t, mem_lock_type::write> zp_s32_data(zp_s32_memory, stream);
    for (size_t i = 0; i < zp_data.size(); i++) {
        zp_s32_data.data()[i] = static_cast<int32_t>(zp_data.data()[i]);
    }

    return zp_s32_memory;
}

template cldnn::memory::ptr convert_zp_data_to_s32<int8_t>(const memory::ptr zp_memory);
template cldnn::memory::ptr convert_zp_data_to_s32<uint8_t>(const memory::ptr zp_memory);

cldnn::format default_fmt_for_dims(size_t dims, bool is_grouped) {
    switch (dims) {
    case 6: return is_grouped ? cldnn::format::goizyx : cldnn::format::bfwzyx;
    case 5: return is_grouped ? cldnn::format::goiyx : cldnn::format::bfzyx;
    default: return cldnn::format::bfyx;
    }
}

dnnl::memory::dims convert_tensor(cldnn::tensor t, size_t dims, bool is_grouped) {
    auto sizes = t.sizes(default_fmt_for_dims(dims, is_grouped));
    dnnl::memory::dims res(sizes.begin(), sizes.end());
    return res;
}

dnnl::memory::dims convert_gemm_tensor(cldnn::tensor t, size_t dims, bool batched_dims_can_be_removed) {
    auto sizes = t.sizes(default_fmt_for_dims(dims, false));
    dnnl::memory::dims res(sizes.begin(), sizes.end());
    if (dims > 3) {
        for (size_t i = 0; i < dims - 3; i++) {
            res[i + 1] *= res[i];
        }
        res.erase(res.begin(), res.begin() + dims - 3);
    }
    if (res.size() == 3 && batched_dims_can_be_removed) {
        res.erase(res.begin());
    }
    return res;
}

dnnl::memory::format_tag convert_gemm_data_format(dnnl::memory::dims dims) {
    if (dims.size() > 3)
        throw std::runtime_error("[clDNN] Unsupported dims size for onednn gemm: should be <= 3");
    return dims.size() == 3 ? dnnl::memory::format_tag::abc : dnnl::memory::format_tag::ab;
}


dnnl::memory::dims convert_spatials(cldnn::tensor t, size_t dims) {
    auto spatials = t.spatial;
    dnnl::memory::dims res(dims);
    for (size_t i = 0; i < dims; i++) {
        res[i] = spatials[dims - i - 1];
    }
    return res;
}

dnnl::memory::dims flatten_tensor(cldnn::tensor t) {
    return {static_cast<int64_t>(t.count())};
}

dnnl::memory::data_type convert_data_type(cldnn::data_types dt) {
    switch (dt) {
        case cldnn::data_types::f32: return dnnl::memory::data_type::f32;
        case cldnn::data_types::f16: return dnnl::memory::data_type::f16;
        case cldnn::data_types::i8: return dnnl::memory::data_type::s8;
        case cldnn::data_types::u8: return dnnl::memory::data_type::u8;
        case cldnn::data_types::i32: return dnnl::memory::data_type::s32;
        default: throw std::invalid_argument("[clDNN] Unsupported conversion from cldnn to onednn type");
    }
}

std::vector<std::pair<cldnn::format, dnnl::memory::format_tag>> format_map = {
        { cldnn::format::bfyx, dnnl::memory::format_tag::nchw },
        { cldnn::format::bfzyx, dnnl::memory::format_tag::ncdhw },
        { cldnn::format::byxf, dnnl::memory::format_tag::nhwc },
        { cldnn::format::bzyxf, dnnl::memory::format_tag::ndhwc },
        { cldnn::format::b_fs_yx_fsv2, dnnl::memory::format_tag::undef },
        { cldnn::format::b_fs_yx_fsv4, dnnl::memory::format_tag::aBcd4b },
        { cldnn::format::b_fs_yx_fsv16, dnnl::memory::format_tag::nChw16c },
        { cldnn::format::b_fs_yx_fsv32, dnnl::memory::format_tag::aBcd32b },
        { cldnn::format::b_fs_zyx_fsv4, dnnl::memory::format_tag::aBcde4b },
        { cldnn::format::b_fs_zyx_fsv16, dnnl::memory::format_tag::nCdhw16c },
        { cldnn::format::b_fs_zyx_fsv32, dnnl::memory::format_tag::aBcde32b },
        { cldnn::format::bs_fs_yx_bsv16_fsv16, dnnl::memory::format_tag::NChw16n16c },
        { cldnn::format::bs_fs_yx_bsv32_fsv32, dnnl::memory::format_tag::NChw32n32c },
        { cldnn::format::bs_fs_yx_bsv4_fsv4, dnnl::memory::format_tag::ABcd4a4b },
        { cldnn::format::bs_fs_yx_bsv8_fsv4, dnnl::memory::format_tag::ABcd8a4b },
        { cldnn::format::bs_fs_yx_bsv8_fsv2, dnnl::memory::format_tag::ABcd8a2b },
        { cldnn::format::bs_fs_yx_bsv4_fsv2, dnnl::memory::format_tag::ABcd4a2b },
        { cldnn::format::bs_fs_yx_bsv32_fsv16, dnnl::memory::format_tag::NChw32n16c },
        { cldnn::format::bs_fs_zyx_bsv32_fsv16, dnnl::memory::format_tag::NCdhw32n16c },
        { cldnn::format::bs_fs_zyx_bsv32_fsv32, dnnl::memory::format_tag::NCdhw32n32c },
        { cldnn::format::bs_fs_zyx_bsv16_fsv16, dnnl::memory::format_tag::NCdhw16n16c },
        { cldnn::format::bs_fs_zyx_bsv8_fsv4, dnnl::memory::format_tag::ABcde8a4b },
        { cldnn::format::bs_fs_zyx_bsv8_fsv2, dnnl::memory::format_tag::ABcde8a2b },
};

dnnl::memory::format_tag convert_data_format(cldnn::format fmt) {
    auto ret = std::find_if(format_map.begin(), format_map.end(),
            [fmt](std::pair<cldnn::format, dnnl::memory::format_tag> &e) {
                    return e.first == fmt; });
    if (ret == format_map.end())
        return dnnl::memory::format_tag::undef;

    return ret->second;
}

 cldnn::format convert_data_format(dnnl::memory::format_tag fmt) {
    auto ret = std::find_if(format_map.begin(), format_map.end(),
            [fmt](std::pair<cldnn::format, dnnl::memory::format_tag> &e) {
                    return e.second == fmt; });
    if (ret == format_map.end())
        throw std::invalid_argument("[clDNN] Unsupported onednn layout");

    return ret->first;
}

void combine_bf_with_first_spatial_dim(cldnn::layout& l) {
    auto pshape = l.get_shape();
    ov::Shape new_shape{1, 1};
    for (size_t i = 0; i < pshape.size(); ++i) {
        if (i < 2) {
            new_shape[0] *= pshape[i];
        } else {
            new_shape[1] *= pshape[i];
        }
    }
    l.set_partial_shape(new_shape);
}

int64_t get_f_offset(cldnn::layout&& l, dnnl::memory::desc&& desc) {
    int64_t offset = 0;
    auto f_padding = l.data_padding.lower_size().feature[0];
    if (f_padding != 0) {
        offset = f_padding;
        for (size_t i = 0; i < l.get_spatial_rank(); ++i) {
            offset *= l.spatial(i);
        }
    }

    switch (desc.data.data_type) {
        case dnnl_data_type_t::dnnl_s8:
        case dnnl_data_type_t::dnnl_u8:
            return offset;
        case dnnl_data_type_t::dnnl_f16:
        case dnnl_data_type_t::dnnl_bf16:
            return (offset * 2);
        case dnnl_data_type_t::dnnl_f32:
        case dnnl_data_type_t::dnnl_s32:
            return (offset * 4);
        default: throw std::runtime_error(std::string("Unsupported offset for dnnl_data_type_t ") + dnnl_dt2str(desc.data.data_type));
    }
}

dnnl::memory::desc layout_to_memory_desc(cldnn::layout l, dnnl::memory::format_tag target_fmt, bool flatten) {
    dnnl::memory::dims dims;
    dnnl::memory::dims padded_dims;
    dnnl::memory::dims padded_offset;
    if (target_fmt == dnnl::memory::format_tag::ab && flatten) {
        dims = flatten_tensor(l.get_tensor());
        dims.insert(dims.begin(), 1);
    } else if (target_fmt == dnnl::memory::format_tag::ab) {
        dims.push_back(l.batch());
        dims.push_back(l.get_tensor().count() / l.batch());
        padded_dims = dims;
    } else if (flatten) {
        dims = flatten_tensor(l.get_tensor());
    } else {
        auto rank = cldnn::format::dimension(l.format);
        dims = convert_tensor(l.get_tensor(), rank, cldnn::format::is_grouped(l.format));
    }
    padded_dims = dims;
    pad_dims(padded_dims, l.format);

    dnnl::memory::data_type dt = convert_data_type(l.data_type);
    dnnl::memory::format_tag fmt = target_fmt == dnnl::memory::format_tag::undef ? convert_data_format(l.format) : target_fmt;

    if (fmt == dnnl::memory::format_tag::undef) {
        return create_memory_desc_from_format_string(dims, dt, convert_data_format_string(l.format));
    } else {
        dnnl::memory::desc res(dims, dt, fmt);

        std::copy(padded_dims.begin(), padded_dims.end(), res.data.padded_dims);
        std::copy(padded_offset.begin(), padded_offset.end(), res.data.padded_offsets);

        return res;
    }
}

static void get_identical_order(std::vector<std::vector<size_t>>& orders, std::vector<size_t> order,
                            size_t first, size_t depth) {
    if (depth == 0)
        return;

    for (size_t idx = first; idx <= first + depth ; idx++) {
        std::swap(order[first], order[idx]);
        if (first != idx)
            orders.push_back(order);

        get_identical_order(orders, order, first+1, depth-1);
        std::swap(order[first], order[idx]);
    }
}

// Get candidate orders calculated by stride value of dnnl::memory::descriptor could be multiple
std::vector<std::vector<size_t>> get_candidate_orders(dnnl::memory::desc desc) {
    std::vector<std::vector<size_t>> orders;
    auto strides = desc.data.format_desc.blocking.strides;
    std::vector<size_t> order(desc.data.ndims);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
                [&strides] (size_t ind_l, size_t ind_r) {
                    return (strides[ind_l] > strides[ind_r]);
                });

    orders.push_back(order);

    // Orders of those axes which have a same stride in memory::desc can be changed.
    // If y and x axes have same, then it can be bfyx or bfxy.
    for (size_t idx = 0 ; idx+1 < order.size() ; idx++) {
        size_t depth = 0;
        for (size_t next = idx+1 ; next < order.size() ; next++) {
            if (strides[order[idx]] == strides[order[next]]) {
                depth++;
            } else {
                break;
            }
        }

        // mutiple axes can have a same stride value of mem descriptor
        get_identical_order(orders, order, idx, depth);
        idx += depth;
    }

    return orders;
}

static bool compare_orders(std::vector<std::vector<size_t>> a, std::vector<size_t> b) {
    for (size_t idx = 0 ; idx < a.size() ; idx++) {
        if (std::equal(a[idx].begin(), a[idx].end(), b.begin()))
            return true;
    }

    return false;
}

cldnn::format find_data_format(dnnl::memory::desc desc) {
    auto blk = desc.data.format_desc.blocking;
    auto order = get_candidate_orders(desc);

    for (int32_t fmt_idx = format::bfyx ; fmt_idx < format::oiyx ; fmt_idx++) {
        auto candidate_trait = format::traits(static_cast<format::type>(fmt_idx));
        if (desc.data.ndims == static_cast<int>(candidate_trait._order.size())
            && blk.inner_nblks == static_cast<int>(candidate_trait.block_sizes.size())
            && compare_orders(order, candidate_trait._order)) {
            bool is_match = true;
            for (size_t idx = 0 ; idx < candidate_trait.block_sizes.size() ; idx++) {
                if (blk.inner_blks[idx] != static_cast<int>(candidate_trait.block_sizes[idx].second)
                    || blk.inner_idxs[idx] != static_cast<int>(candidate_trait.block_sizes[idx].first)) {
                    is_match = false;
                    break;
                }
            }
            if (is_match)
                return static_cast<format::type>(fmt_idx);
        }
    }

    std::stringstream msg;
    msg << "Unsupported onednn dnnl::memory::desc find_data_format. "
        << "ndims: " << desc.data.ndims
        << ", inner_nblks: " << blk.inner_nblks
        << ", inner_blks: ";
    for (int i = 0; i < blk.inner_nblks; i++)
        msg << "(blk " << blk.inner_blks[i] << ", idx " << blk.inner_idxs[i] << ") ";

    throw std::runtime_error(msg.str());
}

cldnn::format find_format(dnnl::memory::desc desc, bool is_grouped) {
    auto blk = desc.data.format_desc.blocking;
    auto orders = get_candidate_orders(desc);

    format start_format = format::oiyx;
    if (is_grouped)
        start_format = format::goiyx;

    for (int32_t fmt_idx = start_format ; fmt_idx < format::format_num ; fmt_idx++) {
        auto candidate_trait = format::traits(static_cast<format::type>(fmt_idx));
        if (static_cast<size_t>(desc.data.ndims) == candidate_trait._order.size()
            && static_cast<size_t>(blk.inner_nblks) == candidate_trait.block_sizes.size()
            && compare_orders(orders, candidate_trait._order)) {
            // Compare all pairs of dimension number and block size to format_traits_map of all formats
            bool is_match = true;
            for (size_t idx = 0 ; idx < candidate_trait.block_sizes.size() ; idx++) {
                auto block_idx = static_cast<dnnl_dim_t>(candidate_trait.block_sizes[idx].first);
                auto block_size = static_cast<dnnl_dim_t>(candidate_trait.block_sizes[idx].second);
                if (is_grouped && candidate_trait.is_group_char(candidate_trait.internal_order[block_idx])) {
                    // inner_idx gets the index of group dimension in mem::desc when blocked axis is group
                    auto inner_idx = candidate_trait.order.find_first_of(candidate_trait.internal_order[block_idx]);
                    if (blk.inner_blks[idx] != block_size ||
                        blk.inner_idxs[idx] != static_cast<dnnl_dim_t>(inner_idx)) {
                        is_match = false;
                        break;
                    }
                } else if (is_grouped) {
                    // g,o,i from cldnn formats are matching to a,b,c of dnnl. But g is at the end of internal order.
                    if (blk.inner_blks[idx] != block_size ||
                        (blk.inner_idxs[idx] - static_cast<dnnl_dim_t>(candidate_trait.group_num)) != block_idx) {
                        is_match = false;
                        break;
                    }
                } else {
                    if (blk.inner_blks[idx] != block_size ||
                        blk.inner_idxs[idx] != block_idx) {
                        is_match = false;
                        break;
                    }
                }
            }

            if (is_match)
                return static_cast<format::type>(fmt_idx);
        }
    }

    std::stringstream msg;
    msg << "Unsupported " << (is_grouped ? "grouped" : "") << "onednn dnnl::memory::desc find_format. "
        << "ndims: " << desc.data.ndims
        << ", inner_nblks: " << blk.inner_nblks
        << ", inner_blks: ";
    for (int i = 0; i < blk.inner_nblks; i++)
        msg << "(blk " << blk.inner_blks[i] << ", idx " << blk.inner_idxs[i] << ") ";
    for (auto order : orders) {
        msg << ", strides_order : ";
        for (const auto& value : order)
            msg << value << " ";
    }
    msg << ", stride_value : ";
    auto strides = desc.data.format_desc.blocking.strides;
    for (size_t idx = 0; idx < orders[0].size() ; idx++) {
        msg << strides[idx] << " ";
    }

    throw std::runtime_error(msg.str());
}

// Currently, usage of alpha and beta between cldnn::pow and dnnl::eltwise::pow is different : d = pow(src, a) / d = a * pow(src, b)
dnnl::algorithm convert_activation_func(cldnn::activation_func func) {
    switch (func) {
        case cldnn::activation_func::relu: return dnnl::algorithm::eltwise_relu;
        case cldnn::activation_func::relu_negative_slope: return dnnl::algorithm::eltwise_relu;
        case cldnn::activation_func::gelu: return dnnl::algorithm::eltwise_gelu;
        case cldnn::activation_func::elu: return dnnl::algorithm::eltwise_elu;
        case cldnn::activation_func::mish: return dnnl::algorithm::eltwise_mish;
        case cldnn::activation_func::swish: return dnnl::algorithm::eltwise_swish;
        case cldnn::activation_func::hswish: return dnnl::algorithm::eltwise_hardswish;
        case cldnn::activation_func::abs: return dnnl::algorithm::eltwise_abs;
        case cldnn::activation_func::exp: return dnnl::algorithm::eltwise_exp;
        case cldnn::activation_func::logistic: return dnnl::algorithm::eltwise_logistic;
        case cldnn::activation_func::clamp: return dnnl::algorithm::eltwise_clip;
        case cldnn::activation_func::hyperbolic_tan: return dnnl::algorithm::eltwise_tanh;
        case cldnn::activation_func::pow: return dnnl::algorithm::eltwise_pow;
        case cldnn::activation_func::sqrt: return dnnl::algorithm::eltwise_sqrt;
        case cldnn::activation_func::hard_sigmoid: return dnnl::algorithm::eltwise_hardsigmoid;
        default: throw std::runtime_error("Unsupported activation func for onednn primitive " + std::to_string(static_cast<int>(func)));
    }
}

template <typename T>
bool is_per_tensor(cldnn::data_node& node, int32_t& zp_val) {
    auto ptr = node.get_attached_memory_ptr();
    auto engine = ptr->get_engine();
    auto& stream = engine->get_service_stream();
    auto num_elems = node.get_output_layout().count();
    mem_lock<T, mem_lock_type::read> old_data {ptr, stream};
    auto val = old_data[0];
    for (size_t i = 1; i < num_elems; i++) {
        if (val != old_data[i]) {
            zp_val = DNNL_RUNTIME_S32_VAL;
            return false;
        }
    }

    zp_val = val;
    return true;
}

template bool is_per_tensor<int8_t>(cldnn::data_node& node, int32_t& zp_val);
template bool is_per_tensor<uint8_t>(cldnn::data_node& node, int32_t& zp_val);
}  // namespace onednn
}  // namespace cldnn
