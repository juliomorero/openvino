// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "include/batch_headers/fetch_data.cl"

#if defined(CONVERT_FROM_NV12) || defined(CONVERT_FROM_I420)
#ifdef BUFFER_MEM
KERNEL(convert_color_ref)(const __global INPUT0_TYPE* input1,
#if INPUTS_COUNT > 1
                          const __global INPUT1_TYPE* input2,
#if INPUTS_COUNT == 3
                          const __global INPUT2_TYPE* input3,
#endif
#endif
                          __global OUTPUT_TYPE* output) {

    const uint b = get_global_id(0);
    const uint y = get_global_id(1);
    const uint x = get_global_id(2);

    float Y = input1[GET_DATA_INDEX(INPUT0, b, 0, y, x)];

#if INPUTS_COUNT == 3
    float U = input2[GET_DATA_INDEX(INPUT1, b, 0, y / 2, x / 2)];
    float V = input3[GET_DATA_INDEX(INPUT2, b, 0, y / 2, x / 2)];
#elif INPUTS_COUNT == 2
    float U = input2[GET_DATA_INDEX(INPUT1, b, 0, y / 2, x / 2)];
    float V = input2[GET_DATA_INDEX(INPUT1, b, 1, y / 2, x / 2)];
#else // Single plane
    uint input_uv_offset = INPUT0_SIZE_X * INPUT0_SIZE_Y / 3 * 2;
#ifdef CONVERT_FROM_NV12
    float U = input1[GET_DATA_INDEX(INPUT0, b, 0, y / 2, (x / 2) * 2) + input_uv_offset];
    float V = input1[GET_DATA_INDEX(INPUT0, b, 1, y / 2, (x / 2) * 2) + input_uv_offset];
#else
    float U = input1[GET_DATA_INDEX(INPUT0, b, 0, 0, x / 2 + (y / 2)*(INPUT0_Y_PITCH / 2)) + input_uv_offset];
    float V = input1[GET_DATA_INDEX(INPUT0, b, 0, 0, x / 2 + (y / 2)*(INPUT0_Y_PITCH / 2)) + 5 * input_uv_offset / 4];
#endif
#endif

    float Ycomponent = mad(Y, 1.164f, -18.624f);
    float Ucomponent = mad(U, 1.f, -128.f);
    float Vcomponent = mad(V, 1.f, -128.f);

    float R = clamp(mad(Vcomponent, 1.596f, Ycomponent), 0.f, 255.f);
    float G = clamp(mad(Vcomponent, -0.813f, mad(Ucomponent, -0.391f, Ycomponent)), 0.f, 255.f);
    float B = clamp(mad(Ucomponent, 2.018f, Ycomponent), 0.f, 255.f);

#if UINT8_UNIT_USED
    R = round(R);
    G = round(G);
    B = round(B);
#endif

#ifdef CONVERT_TO_RGB
    output[OUTPUT_GET_INDEX(b, 0, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(R), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 1, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(G), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 2, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(B), ACTIVATION_PARAMS);
#else // BGR
    output[OUTPUT_GET_INDEX(b, 0, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(B), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 1, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(G), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 2, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(R), ACTIVATION_PARAMS);
#endif
}
#endif


#ifdef SURFACE_MEM
KERNEL(convert_color_ref)(read_only image2d_t input1,
#if INPUTS_COUNT > 1
                          read_only image2d_t input2,
#if INPUTS_COUNT == 3
                          read_only image2d_t input3,
#endif
#endif
                          __global OUTPUT_TYPE* output) {

    const uint b = get_global_id(0);
    const uint y = get_global_id(1);
    const uint x = get_global_id(2);

    float4 Y = read_imagef(input1, (int2)(x, y));
    float Ycomponent = mad(Y.x, 296.82f, -18.624f);

#if INPUTS_COUNT == 3
    float4 U = read_imagef(input2, (int2)(x / 2, y / 2));
    float4 V = read_imagef(input3, (int2)(x / 2, y / 2));
    float Ucomponent = mad(U.x, 255.0f, -128.f);
    float Vcomponent = mad(V.x, 255.0f, -128.f);
#elif INPUTS_COUNT == 2
    float4 UV = read_imagef(input2, (int2)(x / 2, y / 2));
    float Ucomponent = mad(UV.x, 255.0f, -128.f);
    float Vcomponent = mad(UV.y, 255.0f, -128.f);
#else // Single plane
    uint input_y_offset = INPUT0_SIZE_Y / 3 * 2;
    float4 U = read_imagef(input1, (int2)((x / 2) * 2,     y / 2 + input_y_offset));
    float4 V = read_imagef(input1, (int2)((x / 2) * 2 + 1, y / 2 + input_y_offset));
    float Ucomponent = mad(U.x, 255.0f, -128.f);
    float Vcomponent = mad(V.x, 255.0f, -128.f);
#endif

    float R = clamp(mad(Vcomponent, 1.596f, Ycomponent), 0.f, 255.f);
    float G = clamp(mad(Vcomponent, -0.813f, mad(Ucomponent, -0.391f, Ycomponent)), 0.f, 255.f);
    float B = clamp(mad(Ucomponent, 2.018f, Ycomponent), 0.f, 255.f);

#if UINT8_UNIT_USED
    R = round(R);
    G = round(G);
    B = round(B);
#endif

#ifdef CONVERT_TO_RGB
    output[OUTPUT_GET_INDEX(b, 0, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(R), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 1, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(G), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 2, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(B), ACTIVATION_PARAMS);
#else // BGR
    output[OUTPUT_GET_INDEX(b, 0, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(B), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 1, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(G), ACTIVATION_PARAMS);
    output[OUTPUT_GET_INDEX(b, 2, y, x)] = ACTIVATION(TO_OUTPUT_TYPE(R), ACTIVATION_PARAMS);
#endif
}
#endif
#endif
