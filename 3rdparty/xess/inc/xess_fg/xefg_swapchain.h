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

#ifndef XEFG_SWAPCHAIN_H
#define XEFG_SWAPCHAIN_H

#ifdef _WIN32
    #ifdef XEFG_SWAPCHAIN_EXPORT_API
        #define XEFG_SWAPCHAIN_API __declspec(dllexport)
    #else
        #define XEFG_SWAPCHAIN_API
    #endif
#else
    #define XEFG_SWAPCHAIN_API __attribute__((visibility("default")))
#endif

#if !defined _MSC_VER || (_MSC_VER >= 1929)
#define XEFG_SWAPCHAIN_PRAGMA(x) _Pragma(#x)
#else
#define XEFG_SWAPCHAIN_PRAGMA(x) __pragma(x)
#endif
#define XEFG_SWAPCHAIN_PACK_B_X(x) XEFG_SWAPCHAIN_PRAGMA(pack(push, x))
#define XEFG_SWAPCHAIN_PACK_E() XEFG_SWAPCHAIN_PRAGMA(pack(pop))
#define XEFG_SWAPCHAIN_PACK_B() XEFG_SWAPCHAIN_PACK_B_X(8)

#define XEFG_SWAPCHAIN_CAT_IMPL(a,b) a##b
#define XEFG_SWAPCHAIN_CAT(a,b) XEFG_SWAPCHAIN_CAT_IMPL(a,b)
#define XEFG_SWAPCHAIN_STATIC_ASSERT(cond)\
    typedef int XEFG_SWAPCHAIN_CAT(XEFG_SWAPCHAIN_STATIC_ASSERT_IMPL, __COUNTER__)[(cond) ? 1 : -1]

#include <limits.h>
#include <stdint.h>

/**
 * @brief Use maximum supported number of interpolated frames.
 *
 * Use this constant during XeSS-FG swap chain initialization to set number
 * of interpolated frames to the maximum supported value.
 *
 * The actual value can be queried later using @ref xefgSwapChainGetProperties
 * or by retrieving back the initialization parameters.
 *
 * @see @ref xefgswapchain-d3d12
 */
#define XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES UINT32_MAX

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _xefg_swapchain_handle_t* xefg_swapchain_handle_t;

/**
* @brief Informs the library about the lifetime of the
*  resource being provided. The library will take appropriate action based
*  on this value to either make a copy of the resource or store a reference
*  to it.
*/
typedef enum _xefg_swapchain_resource_validity_t
{
    /** Resource is valid until the next present. */
    XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT = 0,
    /** Resource is only valid now. */
    XEFG_SWAPCHAIN_RV_ONLY_NOW,
    XEFG_SWAPCHAIN_RV_COUNT
} xefg_swapchain_resource_validity_t;

/**
 * @brief XeSS-FG Swap Chain initialization flags.
 */
typedef enum _xefg_swapchain_init_flags_t
{
    /** No flags are enabled. */
    XEFG_SWAPCHAIN_INIT_FLAG_NONE = 0,
    /** Use inverted (increased precision) depth encoding. */
    XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH = 1 << 0,
    /** Use external descriptor heap. */
    XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 1,
    /** Deprecated. Setting this flag has no effect. */
    XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV = 1 << 2,
    /** Use velocity in normalized device coordinates (NDC). */
    XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY = 1 << 3,
    /** Remove jitter from input velocity. */
    XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV = 1 << 4,
    /** UI texture rgb values are not premultiplied by its alpha value */
    XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA = 1 << 5
} xefg_swapchain_init_flags_t;

/**
 * @brief XeSS-FG Swap Chain resources type.
 */
typedef enum _xefg_swapchain_resource_type_t
{
    XEFG_SWAPCHAIN_RES_HUDLESS_COLOR = 0,
    XEFG_SWAPCHAIN_RES_DEPTH,
    XEFG_SWAPCHAIN_RES_MOTION_VECTOR,
    XEFG_SWAPCHAIN_RES_UI,
    XEFG_SWAPCHAIN_RES_BACKBUFFER,
    XEFG_SWAPCHAIN_RES_COUNT
} xefg_swapchain_resource_type_t;

XEFG_SWAPCHAIN_PACK_B()
/**
* @brief XeSS-FG Swap Chain version.
*
* XeSS-FG Swap Chain uses major.minor.patch version format and Numeric 90+ scheme for development stage builds.
*/
typedef struct _xefg_swapchain_version_t
{
    /** A major version increment indicates a new API and potentially a
        * break in functionality. */
    uint16_t major;
    /** A minor version increment indicates incremental changes such as
        * optional inputs or flags. This does not break existing functionality. */
    uint16_t minor;
    /** A patch version increment may include performance or quality tweaks or fixes for known issues.
        * There's no change in the interfaces.
        * Versions beyond 90 are used for development builds to change the interface for the next release.
        */
    uint16_t patch;
    /** Reserved for future use. */
    uint16_t reserved;
} xefg_swapchain_version_t;
XEFG_SWAPCHAIN_PACK_E()

XEFG_SWAPCHAIN_PACK_B()
/**
 * @brief 2D variable.
 */
typedef struct _xefg_swapchain_2d_t
{
    uint32_t x;
    uint32_t y;
} xefg_swapchain_2d_t;
XEFG_SWAPCHAIN_PACK_E()

XEFG_SWAPCHAIN_PACK_B()
/**
 * @brief Contains all constants associated with a frame.
 */
typedef struct _xefg_swapchain_frame_constant_data_t
{
    /** float4x4, row-major convention. */
    float viewMatrix[16];
    /** float4x4, row-major convention. */
    float projectionMatrix[16];
    /** Jitter X coordinate in the range [-0.5, 0.5]. */
    float jitterOffsetX;
    /** Jitter Y coordinate in the range [-0.5, 0.5]. */
    float jitterOffsetY;
    /** Scale for motion vectors for X coordinate. */
    float motionVectorScaleX;
    /** Scale for motion vectors for Y coordinate. */
    float motionVectorScaleY;
    /** Tells Interpolation library to reset history. */
    uint32_t resetHistory;
    /** Time that was required to render current frame in milliseconds. */
    float frameRenderTime;
} xefg_swapchain_frame_constant_data_t;
XEFG_SWAPCHAIN_PACK_E()

XEFG_SWAPCHAIN_PACK_B()
/**
* @brief Properties for internal XeSS-FG Swap Chain resources.
*/
typedef struct _xefg_swapchain_properties_t
{
    /** Required amount of descriptors for XeSS-FG Swap Chain. */
    uint32_t requiredDescriptorCount;
    /** The heap size required by XeSS-FG Swap Chain for temporary buffer storage. */
    uint64_t tempBufferHeapSize;
    /** The heap size required by XeSS-FG Swap Chain for temporary texture storage. */
    uint64_t tempTextureHeapSize;
    /** The size required by XeSS-FG Swap Chain in a constant buffer for temporary storage. */
    uint64_t constantBufferSize;
    /** Maximum number of supported interpolations. */
    uint32_t maxSupportedInterpolations;
} xefg_swapchain_properties_t;
XEFG_SWAPCHAIN_PACK_E()

/**
 * @brief XeSS-FG Swap Chain return codes.
 */
typedef enum _xefg_swapchain_result_t
{
    /** An old or outdated driver. */
    XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER = 2,
    /** Warning. Frame ID should be over 0. */
    XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES = 3,
    /** Warning. Data for interpolation came in wrong order. */
    XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH = 4,
    /** Warning. There is no present status for last present. */
    XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS = 5,
    /** Warning. Resource sizes for the interpolation doesn't match between previous and next frames.
        Interpolation skipped. */
    XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH = 6,
    /** Operation was successful. */
    XEFG_SWAPCHAIN_RESULT_SUCCESS = 0,
    /** Operation not supported on the GPU. */
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    /** An unsupported driver. */
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    /** Execute called without initialization. */
    XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED = -3,
    /** Invalid argument were provided to the API call. */
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT = -4,
    /** Not enough available GPU memory. */
    XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    /** Device function such as resource or descriptor creation. */
    XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE = -6,
    /** The function is not implemented. */
    XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    /** Invalid context. */
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT = -8,
    /** Operation not finished yet. */
    XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    /** Operation not supported in current configuration. */
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED = -10,
    /** The library cannot be loaded. */
    XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    /** Input resources does not meet requirements based on UI Mode. */
    XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES = -12,
    /** Output resources does not meet requirements. */
    XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES = -13,
    /** Input is insufficient to evaluate interpolation. */
    XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES = -14,
    /** Requirements regarding XeLL Latency Reduction are not met. */
    XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED = -15,
    /** XeLL library does not contains required functions. */
    XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING = -16,
    /** One of the Windows functions has failed. For details, see the logs or debug layer. */
    XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE = -17,
    /** DXGI call failed with invalid call error. */
    XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL = -18,
    /** XeFG SwapChain pointer is still in use during destroy. */
    XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE = -19,
    /** Invalid or missing descriptor heap.*/
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP = -20,
    /** Call to function done in invalid order. */
    XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER = -21,

    /** Unknown internal failure */
    XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN = -1000
} xefg_swapchain_result_t;

/**
 * @brief XeSS-FG Swap Chain logging level.
 */
typedef enum _xefg_swapchain_logging_level_t
{
    XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG = 0,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO = 1,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING = 2,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR = 3
} xefg_swapchain_logging_level_t;

/**
 * @brief XeSS-FG UI composition mode.
 *
 * XeSS-FG can either interpolate UI (default) or compose a UI texture as-is on top of the interpolated frames.
 * The application can provide the UI texture directly or rely on XeSS-FG to extract the texture from
 * other available resources.
 *
 * UI composition can be enabled or disabled by changing the composition state.
 * When UI composition is enabled, the UI mode determines which composition method is used.
 *
 * @see xefgSwapChainSetUiCompositionState for enabling or disabling UI composition.
 */
typedef enum _xefg_swapchain_ui_mode_t
{
    /** Determine UI composition mode automatically, based on provided inputs: hudless color and UI texture. */
    XEFG_SWAPCHAIN_UI_MODE_AUTO = 0,
    /** Interpolate back buffer, without any UI composition. */
    XEFG_SWAPCHAIN_UI_MODE_NONE = 1,
    /** Interpolate back buffer, refine UI using UI texture + alpha. */
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_UITEXTURE = 2,
    /** Interpolate hudless color, blend UI from UI texture + alpha. */
    XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE = 3,
    /** Interpolate hudless color, extract UI from backbuffer. */
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS = 4,
    /** Interpolate hudless color, blend UI from UI texture + alpha and refine it by extracting from back buffer. */
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS_UITEXTURE = 5
} xefg_swapchain_ui_mode_t;

XEFG_SWAPCHAIN_PACK_B()
/**
 * @brief Contains status data for the present.
 */
typedef struct _xefg_swapchain_present_status_t
{
    /** Number of frames sent to be presented during the last call. */
    uint32_t framesPresented;
    /** Result of the interpolation. */
    xefg_swapchain_result_t frameGenResult;
    /** Specifies if frame generation was enabled. */
    uint32_t isFrameGenEnabled;
} xefg_swapchain_present_status_t;
XEFG_SWAPCHAIN_PACK_E()

/**
 * A logging callback provided by the application. This callback can be called from other threads.
 * Message pointer is only valid inside the function and may be invalid right after the return call.
 * Message is a null-terminated utf-8 string. Pointer to @p userData must remain valid until the call
 * to @ref xefgSwapChainDestroy returns.
 */
typedef void (*xefg_swapchain_app_log_callback_t)(const char* message, xefg_swapchain_logging_level_t loggingLevel, void* userData);

/**
 * @addtogroup xefgswapchain XeSS-FG Swap Chain API exports
 * The XeSS-FG Swap Chain API is used by applications to generate frames
 * when the application does not choose to be responsible for the in-order presentation
 * of the generated frames. This utils API wraps the IDXGISwapChain and makes
 * calls on behalf of the application to the underlying XeSS-FG API.
 *
 * @{
 */

 /**
  * @brief Gets the XeSS-FG Swap Chain version. This is baked into the XeSS-FG Swap Chain SDK release.
  * @param[out] pVersion Returned XeSS-FG Swap Chain version.
  * @return XeSS-FG Swap Chain return status code.
  */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetVersion(xefg_swapchain_version_t* pVersion);

 /**
 * @brief Retrieves various XeSS-FG Swap Chain properties.
 *
 * @note When called before the proxy swap chain initialization, this function will only report
 *       `pProperties->maxSupportedInterpolations`, all other fields of `pProperties` will be set to zero.
 *
 * If you need to query the required heap sizes and the descriptor count before the proxy swap chain
 * initialization, use @ref xefgSwapChainD3D12GetProperties.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param[out] pProperties A pointer to the @ref xefg_swapchain_properties_t structure where the values should be returned.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetProperties(xefg_swapchain_handle_t hSwapChain, xefg_swapchain_properties_t* pProperties);

/**
 * @brief Informs the XeSS-FG swap chain library of the frame constants used to generate a frame
 * that will be presented.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param presentId The unique frame identifier.
 *
 * @param pConstants A pointer to the memory location where the values for the constants are located.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainTagFrameConstants(xefg_swapchain_handle_t hSwapChain,
    uint32_t presentId, const xefg_swapchain_frame_constant_data_t* pConstants);

/**
 * @brief Enables or disables the generation and presentation of interpolated frames by the XeSS-FG swap chain library.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param enable Non-zero to enable interpolation, zero to disable it.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetEnabled(xefg_swapchain_handle_t hSwapChain, uint32_t enable);

/**
 * @brief Informs the XeSS-FG swap chain library of the unique identifier of the next frame
 * to be presented. This allows the application to provide to the swap chain library frames data
 * out-of-order with regard to the presentation.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param presentId The unique identifier of the frame to be presented by the next IDXGISwapChain::Present call.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetPresentId(xefg_swapchain_handle_t hSwapChain, uint32_t presentId);

/**
 * @brief This function allows you to retrieve status information from the last present.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 *
 * @param[out] pPresentStatus A pointer to the @ref xefg_swapchain_present_status_t structure where the values should be returned.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetLastPresentStatus(xefg_swapchain_handle_t hSwapChain, xefg_swapchain_present_status_t* pPresentStatus);

/**
 * @brief Sets logging callback.
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 * @param loggingLevel Minimum logging level for logging callback.
 * @param loggingCallback Logging callback.
 * @param userData User data that will be passed unchanged to the callback when invoked. Can be set to NULL.
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetLoggingCallback(xefg_swapchain_handle_t hSwapChain,
    xefg_swapchain_logging_level_t loggingLevel, xefg_swapchain_app_log_callback_t loggingCallback, void* userData);

/**
* @brief Destroys a XeSS-FG Swap Chain context data. DXGI swap chain object will be released as soon as reference count reaches 0,
* so it's recommended to release all outstanding references before calling this function.
* @param hSwapChain: The XeSS-FG Swap Chain context handle.
* @return XeSS-FG Swap Chain return status code.
*/
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainDestroy(xefg_swapchain_handle_t hSwapChain);

/**
 * @brief Sets XeLL context for latency reduction.
 *
 * @param hSwapChain: The XeSS-FG Swap Chain context handle.
 * @param hXeLLContext: The Xe Low Latency context.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetLatencyReduction(xefg_swapchain_handle_t hSwapChain, void* hXeLLContext);

/**
 * @brief Sets scene change detection threshold.
 *
 * A higher threshold value increases the algorithm's sensitivity, causing it to more readily identify
 * scene changes and disable interpolation. A lower threshold value decreases sensitivity, making the algorithm
 * less likely to respond to minor changes. The threshold value range is from 0 to 1, where 0 represents the least
 * sensitivity to scene changes, and 1 represents the highest sensitivity.
 *
 * @param hSwapChain: The XeSS-FG Swap Chain context handle.
 * @param threshold: Scene change detection threshold value. Default value is 0.7.
 *
 * @return XeSS-FG Swap Chain return status code.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetSceneChangeThreshold(xefg_swapchain_handle_t hSwapChain, float threshold);

/**
 * @brief Returns current state of the pipeline build
 *
 * This function can only be called after @ref xefgSwapChainD3D12BuildPipelines but
 * before XeSS-FG swap chain initialization (@ref xefgSwapChainD3D12InitFromSwapChain
 * or @ref xefgSwapChainD3D12InitFromSwapChainDesc).
 *
 * @param hSwapChain The XeSS-FG Swap Chain context handle.
 * @return XEFG_SWAPCHAIN_RESULT_SUCCESS if pipelines are already built.
 *         XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS if pipeline build are in progress.
 *         XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER if the function is called out of order.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetPipelineBuildStatus(xefg_swapchain_handle_t hSwapChain);

/**
 * @brief Adjust the number of interpolated frames after initialization.
 *
 * By default, XeSS-FG swap chain will produce the maximum number of interpolated frames
 * specified during initialization. Use this function to change the number of interpolated frames
 * without reinitializing the swap chain.
 *
 * The number of interpolated frames must be between 1 and the maximum provided during swap chain
 * initialization, otherwise the function returns @ref XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT.
 *
 * **Performance Note:** Avoid frequent calls to this function, as changing the number of
 * interpolated frames may trigger shader compilation and reallocation of internal resources.
 *
 * **Error Handling:** If this function fails for any reason other than
 * @ref XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT, reinitialize the swap chain (this is rare).
 * Ignoring such errors may lead to unexpected behavior. On non-argument errors, XeSS-FG automatically
 * disables frame generation to keep the swap chain operational (e.g., it can still display error messages).
 *
 * **Thread-Safety:** This function is not thread-safe with regard to calls to
 * @ref xefgSwapChainD3D12InitFromSwapChain, @ref xefgSwapChainD3D12InitFromSwapChainDesc, and
 * @ref xefgSwapChainDestroy. Do not call @ref xefgSwapChainSetNumInterpolatedFrames if another
 * thread might be initializing or destroying the swap chain.
 *
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetNumInterpolatedFrames(
    xefg_swapchain_handle_t hSwapChain, uint32_t numInterpolatedFrames);

/**
 * @brief Controls whether UI composition is enabled or disabled.
 */
typedef enum
{
    /* UI mode set during initialization has no effect. This is the default state. */
    XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED,
    /* UI is composited on top of the interpolated frame according to the UI mode set during initialization. */
    XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED
} xefg_swapchain_ui_composition_state_t;

/**
 * @brief Enable or disable UI composition.
 *
 * If UI composition is disabled, UI mode will have no effect.
 *
 * Setting UI mode to `XEFG_SWAPCHAIN_UI_MODE_NONE` and enabling UI composition is the same
 * as disabling UI composition.
 *
 * @note Try no UI composition first (default), then switch to UI composition if you are not happy
 * with the no-composition output.
 */
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetUiCompositionState(
    xefg_swapchain_handle_t hSwapChain, xefg_swapchain_ui_composition_state_t state);

/** @}*/

/* Enum size checks. All enums must be 4 bytes. */
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_resource_validity_t) == 4);
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_result_t) == 4);
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_init_flags_t) == 4);
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_logging_level_t) == 4);
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_resource_type_t) == 4);
XEFG_SWAPCHAIN_STATIC_ASSERT(sizeof(xefg_swapchain_ui_mode_t) == 4);

#ifdef __cplusplus
}
#endif

#endif  /* XEFG_SWAPCHAIN_H */
