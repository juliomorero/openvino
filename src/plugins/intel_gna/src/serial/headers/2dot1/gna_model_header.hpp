// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <cstdint>
#include "backend/dnn_types.hpp"

#pragma pack(push, 1)


namespace ov {
namespace intel_gna {
namespace header_2_dot_1 {

/**
 * @brief Header version 2.1
 */
struct ModelHeader {
    /**
     *@brief MagicNumber – GNAM in ascii table, equals to hex 0x474e414d
     */
    char gnam[4] = {};
    /**
     * @brief if header size is not equal to sizeof ModelHeader - some reserved data append in the end of header
     * usually it is an indicator of working with version of model different that is current export function produce
     */
    uint32_t headerSize = 0u;
    struct Version {
        /**
         * @details Version of format Major – unsigned int, ex: 0x0001
         * every change in the header or in the layers definition should be reflected in version change
         * for backward compatibility new parsers can read old versions of model with certain restrictions
         */
        uint16_t major = 2u;
        /**
         * @details Version of Format Minor – unsigned int,  corresponding to build revision for example
         * changes in minor version are not affected layout of model
         */
        uint32_t minor = 1u;
    } version;
    /**
     * @brief Memory required to be allocated using GNAAlloc()
     */
    uint64_t gnaMemSize = 0ull;
    /**
     * @brief Number of GNA Layers
     */
    uint64_t layersCount = 0ull;
    /**
     * @brief Grouping level
     */
    uint32_t nGroup = 0u;
    /**
     * Convolution related setting - they are affecting input transformation
     */
    uint32_t nRotateRows = 0u;
    uint32_t nRotateColumns = 0u;

    uint32_t nInputs = 0u;
    uint32_t nOutputs = 0u;

    /**
     * Reserved Data might be here
     */
};
#pragma pack(pop)

/*
 * In runtime endpoint mostly same as in serial version, except of descriptor field
 */
struct RuntimeEndPoint {
    /**
     * if scale factor is different then pased into infer , network might need to be requantized
     */
    float scaleFactor = 0;
    /**
     * Pointer descriptor
     */
    void* descriptor_ptr = nullptr;
    /**
     * Endpoint resolution in bytes.
     */
    uint32_t element_size = 0;
    /**
     * Number of elements
     */
    uint32_t elements_count = 0;
    /**
     * Offset in bytes of pointer descriptor
     */
    uint64_t descriptor_offset = 0ull;

    intel_dnn_orientation_t orientation = kDnnUnknownOrientation;

    RuntimeEndPoint() = default;
    RuntimeEndPoint(double scaleFactor,
                    void* descriptor_ptr,
                    uint32_t element_size,
                    uint32_t elements_count,
                    intel_dnn_orientation_t orientation)
        : scaleFactor(static_cast<float>(scaleFactor)),
          descriptor_ptr(descriptor_ptr),
          element_size(element_size),
          elements_count(elements_count),
          orientation(orientation) {}
};

}  // namespace header_2_dot_1
}  // namespace intel_gna
}  // namespace ov
