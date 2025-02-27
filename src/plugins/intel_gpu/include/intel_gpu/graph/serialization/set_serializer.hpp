// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <set>
#include <type_traits>
#include "buffer.hpp"
#include "helpers.hpp"

namespace cldnn {
template <typename BufferType, typename T>
class Serializer<BufferType, std::set<T>, typename std::enable_if<std::is_base_of<OutputBuffer<BufferType>, BufferType>::value>::type> {
public:
    static void save(BufferType& buffer, const std::set<T>& set) {
        buffer << set.size();
        for (const auto& el : set) {
            buffer << el;
        }
    }
};

template <typename BufferType, typename T>
class Serializer<BufferType, std::set<T>, typename std::enable_if<std::is_base_of<InputBuffer<BufferType>, BufferType>::value>::type> {
public:
    static void load(BufferType& buffer, std::set<T>& set) {
        typename std::set<T>::size_type set_size = 0UL;
        buffer >> set_size;

        for (long unsigned int i = 0; i < set_size; i++) {
            T el;
            buffer >> el;
            set.insert(el);
        }
    }
};
}  // namespace cldnn
