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

#ifndef XEFG_SWAPCHAIN_D3D12_H
#define XEFG_SWAPCHAIN_D3D12_H

#ifdef __cplusplus
extern "C" {
#endif

#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_5.h>

#include "xefg_swapchain.h"

XEFG_SWAPCHAIN_PACK_B()
/**
* @brief Initialization parameters.
* Optional external allocations are only available for the internal XeSS-FG resources,
* the Swap Chain API resources will still be managed by the API.
*/
typedef struct _xefg_swapchain_d3d12_init_params_t
{
    /** Pointer to the application swap chain. */
    IDXGISwapChain* pApplicationSwapChain;
    /** Initialization flags. */
    uint32_t initFlags;
    /**
     * @brief XeSS-FG won't produce more than this number of interpolated frames.
     *
     * The value must be between one and the maximum supported number of interpolated frames or
     * @ref XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES.
     *
     * If the value is set to @ref XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES,
     * XeSS-FG will automatically pick the maximum supported number.
     *
     * @see
     * - xefgSwapChainGetProperties to query the maximum supported number of interpolated frames,
     * works before initialization.
     * - xefgSwapChainD3D12GetInitializationParameters to query the value that was applied
     * during initialization.
     * */
    uint32_t maxInterpolatedFrames;
    /** Specifies the node mask for internally created resources on
     * multi-adapter systems. */
    uint32_t creationNodeMask;
    /** Specifies the node visibility mask for internally created resources
     * on multi-adapter systems. */
    uint32_t visibleNodeMask;
    /** Optional externally allocated buffer storage for XeSS-FG. If NULL the
     * storage is allocated internally. If allocated, the heap type must be
     * D3D12_HEAP_TYPE_DEFAULT. This heap is not accessed by the CPU. */
    ID3D12Heap* pTempBufferHeap;
    /** Offset in the externally allocated heap for temporary buffer storage. */
    uint64_t bufferHeapOffset;
    /** Optional externally allocated texture storage for XeSS-FG. If NULL the
     * storage is allocated internally. If allocated, the heap type must be
     * D3D12_HEAP_TYPE_DEFAULT. This heap is not accessed by the CPU. */
    ID3D12Heap* pTempTextureHeap;
    /** Offset in the externally allocated heap for temporary texture storage. */
    uint64_t textureHeapOffset;
    /** Pointer to pipeline library. If not NULL, then it will be used for pipeline caching. */
    ID3D12PipelineLibrary* pPipelineLibrary;
    /** Determines UI composition mode when UI composition is enabled.
     * Use @ref XEFG_SWAPCHAIN_UI_MODE_AUTO to determine UI composition mode based on the provided inputs.
     * @see xefgSwapChainSetUiCompositionState
     */
    xefg_swapchain_ui_mode_t uiMode;
} xefg_swapchain_d3d12_init_params_t;
XEFG_SWAPCHAIN_PACK_E()

XEFG_SWAPCHAIN_PACK_B()
/**
 * @brief Contains all fields needed when providing a resource to
 * the library, describing its state, lifetime, range and so on.
 */
typedef struct _xefg_swapchain_d3d12_resource_data_t
{
    /** XeSS-FG type of the resource. */
    xefg_swapchain_resource_type_t type;
    /** Time period this resource is valid for. */
    xefg_swapchain_resource_validity_t validity;
    /** Base offset to the resource data. */
    xefg_swapchain_2d_t resourceBase;
    /** Valid extent of the resource. */
    xefg_swapchain_2d_t resourceSize;
    /** D3D12Resource object for this resource. */
    ID3D12Resource *pResource;
    /** Incoming state of the resource. */
    D3D12_RESOURCE_STATES incomingState;
} xefg_swapchain_d3d12_resource_data_t;
XEFG_SWAPCHAIN_PACK_E()

/**
 * @addtogroup xefgswapchain-d3d12 XeSS-FG Swap Chain D3D12 API exports
 * @{
 */

/**
 * @brief Creates a swap chain handle and checks necessary D3D12 features.
 *
 * @param pDevice A D3D12 device created by the user.
 *
 * @param[out] phSwapChain Pointer to a XeSS-FG swap chain context handle.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
 XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12CreateContext(ID3D12Device* pDevice, xefg_swapchain_handle_t* phSwapChain);

/**
 * @brief Initiates pipeline build process
 * This function can only be called between @ref xefgSwapChainD3D12CreateContext and
 * any initialization function
 * This call initiates build of DX12 pipelines and kernel compilation
 * This call can be blocking (if @p blocking set to true) or non-blocking.
 * In case of non-blocking call library will wait for pipeline build on call to
 * initialization function
 * If @p pPipelineLibrary passed to this call - same pipeline library must be passed
 * to initialization function
 * Pipeline build status can be retrieved using @ref xefgSwapChainGetPipelineBuildStatus
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 * @param pPipelineLibrary Optional pointer to pipeline library for pipeline caching.
 * @param blocking Wait for kernel compilation and pipeline creation to finish or not
 * @param initFlags Initialization flags. *Must* be identical to flags passed to initialization function
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12BuildPipelines(xefg_swapchain_handle_t hSwapChain,
    ID3D12PipelineLibrary* pPipelineLibrary, uint8_t blocking, uint32_t initFlags);

 /**
 * @brief Queries properties (including required heap sizes) given desired initialization parameters.
 *
 * This function can be called before or after the proxy swap chain initialization.
 *
 * Once the proxy swap chain is initialized, `properties` returned by this function will match
 * the properties returned by `xefgSwapChainGetProperties`.
 *
 * Use this function to determine heap sizes required for successful initialization of the proxy swap chain.
 * The heap sizes are valid for any number of interpolated between one and the maximum specified
 * via `initParams.maxInterpolatedFrames`.
 *
 * If you need to query heap sizes required for a resolution change, set `initParams` to `NULL`
 * and pass in the new `backBufferWidth` and `backBufferHeight`.
 *
 * If you don't want to specify the maximum number of interpolated frames, set `initParams.maxInterpolatedFrames`
 * to `XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES`.
 *
 * @param context
 * @param initParams - pass `NULL` to use the values provided during the proxy swap chain initialization
 * @param backBufferWidth - pass 0 to use the value provided during initialization.
 * @param backBufferHeight - pass 0 to use the value provided during initialization.
 * @param backBufferFormat - pass DXGI_FORMAT_UNKNOWN to use the value provided during initialization.
 * @param properties - output, cannot be `NULL`
 * @return XEFG_SWAPCHAIN_RESULT_SUCCESS if the operation was successful.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12GetProperties(
    xefg_swapchain_handle_t context,
    const xefg_swapchain_d3d12_init_params_t* initParams,
    uint32_t backBufferWidth,
    uint32_t backBufferHeight,
    DXGI_FORMAT backBufferFormat,
    xefg_swapchain_properties_t* properties);

/**
 * @brief Initializes the XeSS-FG Swap Chain library for the generation
 * and presentation of additional frames.
 * The application should call @ref xefgSwapChainD3D12GetSwapChainPtr to retrieve the actual IDXGISwapChain handle.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param pCmdQueue Command queue used by the application
 * to present frames. This queue must conform to all restrictions of any IDXGISwapChain.
 *
 * @param pInitParams XeSS-FG Swap Chain API initialization parameters.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChain(xefg_swapchain_handle_t hSwapChain,
    ID3D12CommandQueue* pCmdQueue, const xefg_swapchain_d3d12_init_params_t* pInitParams);

/**
 * @brief Initializes the XeSS-FG Swap Chain library for the generation
 * and presentation of additional frames.
 * The application should call @ref xefgSwapChainD3D12GetSwapChainPtr to retrieve the actual IDXGISwapChain handle.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param hWnd Window handle used to create the swap chain.
 *
 * @param pSwapChainDesc Swap chain description.
 *
 * @param pFullscreenDesc Fullscreen swap chain description. Can be set to NULL.
 *
 * @param pCmdQueue Command queue used by the application
 * to present frames. This queue must conform to all restrictions of any IDXGISwapChain.
 *
 * @param pDxgiFactory IDXGIFactory2 object to be used for the swap chain creation.
 *
 * @param pInitParams XeSS-FG Swap Chain API initialization parameters.
 * @p pApplicationSwapChain field will not be used and must be set to NULL, a swap chain will be created according to @p hWnd, @p pSwapChainDesc and @p pFullscreenDesc.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChainDesc(xefg_swapchain_handle_t hSwapChain,
    HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pSwapChainDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    ID3D12CommandQueue* pCmdQueue, IDXGIFactory2* pDxgiFactory, const xefg_swapchain_d3d12_init_params_t* pInitParams);

/**
 * @brief Gets a pointer to a DXGI swap chain. The application must use it for
 * all presentation in the application.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param riid Type of the interface to which the caller wants a pointer.
 *
 * @param[out] ppSwapChain Pointer to a IDXGISwapChain handle.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12GetSwapChainPtr(xefg_swapchain_handle_t hSwapChain, REFIID riid, void** ppSwapChain);

/**
 * @brief Informs the XeSS-FG swap chain library of the frame resources used to generate a frame
 * that will be presented.
 *
 * @attention This function can be used to provide information about the interpolation region within the
 * backbuffer by using @p XEFG_SWAPCHAIN_RES_BACKBUFFER resource type. In that case only information about region (size
 * and offset) is used from the @p pResData argument.
 *
 * @param hSwapChain The swap chain associated with the resource being tagged.
 *
 * @param pCmdList Optional command list utilized to manage the resource
 * lifetime. It is only required if the resource is not valid until the next
 * present call.
 *
 * @param presentId The unique frame identifier this resource is associated with.
 *
 * @param pResData The resource and the information needed to properly
 * manage and interpret it.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12TagFrameResource(xefg_swapchain_handle_t hSwapChain,
    ID3D12CommandList* pCmdList, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t* pResData);

/**
 * @brief Informs the XeSS-FG swap chain library of the descriptor heap to use during frame generation. This API
 * needs to be called before presenting a frame. The descriptor heap must be a D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param pDescriptorHeap a pointer to the ID3D12DescriptorHeap object used during interpolation programming.
 *
 * @param descriptorHeapOffsetInBytes The offset within @p pDescriptorHeap XeSS-FG will start using during interpolation
 * programming, in bytes. This value is number of descriptors to skip times GetDescriptorHandleIncrementSize().
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12SetDescriptorHeap(xefg_swapchain_handle_t hSwapChain,
    ID3D12DescriptorHeap* pDescriptorHeap, uint32_t descriptorHeapOffsetInBytes);


/**
 * @brief Updates external heap pointers during the next call to ResizeBuffers or ResizeBuffers1
 *
 * XeSS-FG won't reference the provided heap pointers until the application calls ResizeBuffers
 * or ResizeBuffers1. Once ResizeBuffers or ResizeBuffers1 return control back to the application,
 * XeSS-FG is guaranteed to be using the new heaps.
 *
 * If ResizeBuffers or ResizeBuffers1 fail, XeSS-FG will release the references to the provided
 * heaps.
 *
 * Use @ref xefgSwapChainD3D12GetProperties to get the required heap sizes.
 *
 * @param hSwapChain
 * @param tempBufferHeap
 * @param tempBufferHeapOffset - must be zero if @p tempBufferHeap is null
 * @param tempTextureHeap
 * @param tempTextureHeapOffset - must be zero if @p tempTextureHeap is null
 * @return
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12UpdateExternalHeapOnResize(
    xefg_swapchain_handle_t hSwapChain,
    ID3D12Heap* tempBufferHeap,
    uint64_t tempBufferHeapOffset,
    ID3D12Heap* tempTextureHeap,
    uint64_t tempTextureHeapOffset);

/**
 * @brief Retrieve initialization parameters if the initialization was successful.
 * 
 * This function is not thread-safe with regards to calls to @ref xefgSwapChainD3D12InitFromSwapChain,
 * @ref xefgSwapChainD3D12InitFromSwapChainDesc, and @ref xefgSwapChainDestroy. Do not call
 * @ref xefgSwapChainD3D12GetInitializationParameters if another thread might be doing initialization of or
 * destroying the same context handle.
 * 
 * @param hSwapChain XeSS-FG swap chain context handle.
 *
 * @param[out] pParams will contain parameters that were used to initialize the provided swap chain.
 *
 * @note
 *  - `pParams->pApplicationSwapChain` will be set to `NULL` to signify that the swap chain was re-created.
 *  - `pParams->maxInterpolatedFrames` will be set to the actual value that was used during initialization:
 *  the maximum suppored number if the user requested @ref XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES
 *  or the user-provided value otherwise.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12GetInitializationParameters(
    xefg_swapchain_handle_t hSwapChain, xefg_swapchain_d3d12_init_params_t* pParams);

/** @}*/

#ifdef __cplusplus
}
#endif

#endif  /* XEFG_SWAPCHAIN_D3D12_H */
