/*******************************************************************************
 * Copyright (C) 2024 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you ("License"). Unless the License provides otherwise, you may
 * not use, modify, copy, publish, distribute, disclose or transmit this
 * software or the related documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no express
 * or implied warranties, other than those that are expressly stated in the
 * License.
 ******************************************************************************/
#pragma once
#include <d3d12.h>
#include "xell.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the XeLL DX12 context .
 * @param[in] device: DX12 device
 * @param[out] out_context: Returned XeLL context handle.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellD3D12CreateContext(ID3D12Device* device, xell_context_handle_t* out_context);

#ifdef __cplusplus
}
#endif
