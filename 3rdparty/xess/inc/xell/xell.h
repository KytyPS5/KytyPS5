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
#include <stdint.h>

#if defined (_WIN32)
#if defined(XELL_EXPORT_API)
#define  XELL_EXPORT __declspec(dllexport)
#else
#define  XELL_EXPORT __declspec(dllimport)
#endif /* XELL_EXPORT_API */
#else /* !defined (_WIN32) */
#define XELL_EXPORT
#endif

#if !defined _MSC_VER || (_MSC_VER >= 1929)
#define XELL_PRAGMA(x) _Pragma(#x)
#else
#define XELL_PRAGMA(x) __pragma(x)
#endif
#define XELL_PACK_B_X(x) XELL_PRAGMA(pack(push, x))
#define XELL_PACK_E() XELL_PRAGMA(pack(pop))
#define XELL_PACK_B() XELL_PACK_B_X(8)

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief  XeLL return codes.
*/
typedef enum _xell_result_t
{
	/** XeLL operation was successful. */
	XELL_RESULT_SUCCESS = 0,
	/** XeLL not supported on the GPU. */
	XELL_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
	/** An unsupported driver. */
	XELL_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
	/** Execute called without initialization. */
	XELL_RESULT_ERROR_UNINITIALIZED = -3,
	/** Invalid argument. */
	XELL_RESULT_ERROR_INVALID_ARGUMENT = -4,
	/** Device function. */
	XELL_RESULT_ERROR_DEVICE = -6,
	/** The function is not implemented. */
	XELL_RESULT_ERROR_NOT_IMPLEMENTED = -7,
	/** Invalid context. */
	XELL_RESULT_ERROR_INVALID_CONTEXT = -8,
	/** Operation not supported in current configuration. */
	XELL_RESULT_ERROR_UNSUPPORTED = -10,
	/** Unknown internal failure. */
	XELL_RESULT_ERROR_UNKNOWN = -1000,
} xell_result_t;

/**
 * @brief XeLL markers.
 *
 * XeLL markers for game instrumentation.
 */
typedef enum _xell_latency_marker_type_t
{
	XELL_SIMULATION_START = 0,		// required
	XELL_SIMULATION_END = 1,		// required
	XELL_RENDERSUBMIT_START = 2,	// required
	XELL_RENDERSUBMIT_END = 3,		// required
	XELL_PRESENT_START = 4,			// required
	XELL_PRESENT_END = 5,			// required
	XELL_INPUT_SAMPLE = 6,

	XELL_MARKER_COUNT = 7
} xell_latency_marker_type_t;
XELL_PACK_B()

typedef struct _xell_sleep_params_t
{
	/** Minimum interval expressed in microseconds.
	* If != 0 it will enable fps capping to that value.
	*/
	uint32_t minimumIntervalUs;   //us
	/** Enables latency reduction feature. */
	uint32_t bLowLatencyMode : 1;
	/** Boost is not supported as of now. */
	uint32_t bLowLatencyBoost : 1;

	/** Reserved for future use. */
	uint32_t reserved : 30;
} xell_sleep_params_t;
XELL_PACK_E()

/**
 * @brief XeLL logging level
 */
typedef enum _xell_logging_level_t
{
	XELL_LOGGING_LEVEL_DEBUG = 0,
	XELL_LOGGING_LEVEL_INFO = 1,
	XELL_LOGGING_LEVEL_WARNING = 2,
	XELL_LOGGING_LEVEL_ERROR = 3
} xell_logging_level_t;

/**
 * A logging callback provided by the application. This callback can be called from other threads.
 * Message pointer are only valid inside function and may be invalid right after return call.
 * Message is a null-terminated utf-8 string
*/
typedef void (*xell_app_log_callback_t)(const char* message, xell_logging_level_t loggingLevel);

XELL_PACK_B()
/**
* @brief XeLL version.
*
* XeLL uses major.minor.patch version format and Numeric 90+ scheme for development stage builds.
*/
typedef struct _xell_version_t
{
	/** A major version increment indicates a new API and potentially a
	* break in functionality. 
	*/
	uint16_t major;
	/** A minor version increment indicates incremental changes such as
	* optional inputs or flags. This does not break existing functionality.
	*/
	uint16_t minor;
	/** A patch version increment may include performance or quality tweaks or fixes for known issues.
	* There's no change in the interfaces.
	* Versions beyond 90 used for development builds to change the interface for the next release.
	*/
	uint16_t patch;
	/** Reserved for future use. */
	uint16_t reserved;
} xell_version_t;
XELL_PACK_E()

typedef struct _xell_context_handle_t* xell_context_handle_t;

/**
 * @brief Destroy the XeLL context.
 * @param context: The XeLL context handle.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellDestroyContext(xell_context_handle_t context);

/**
 * @brief Setup how XeLL operate.
 * @param context: The XeLL context handle.
 * @param param: Initialization parameters.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellSetSleepMode(xell_context_handle_t context, const xell_sleep_params_t* param);

/**
 * @brief Get current XeLL parameters.
 * @param context: The XeLL context handle.
 * @param param: Returned parameters.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellGetSleepMode(xell_context_handle_t context, xell_sleep_params_t* param);

/**
 * @brief XeLL will sleep here for the simulation start.
 * @param context: The XeLL context handle.
 * @param frame_id: The incremental frame counter from the game.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellSleep(xell_context_handle_t context, uint32_t frame_id);

/**
 * @brief Pass markers to inform XeLL how long the game simulation, render and present time are.
 * @param context: The XeLL context handle.
 * @param frame_id: The incremental frame counter from the game.
 * @param marker: Marker type.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellAddMarkerData(xell_context_handle_t context, uint32_t frame_id, xell_latency_marker_type_t marker);

/**
 * @brief Gets the XeLL version. This is baked into the XeLL SDK release.
 * @param[out] pVersion Returned XeLL version.
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellGetVersion(xell_version_t* pVersion);

/**
 * @brief Sets logging callback
 *
 * @param hContext The XeLL context handle.
 * @param loggingLevel Minimum logging level for logging callback.
 * @param loggingCallback Logging callback
 * @return XeLL return status code.
 */
XELL_EXPORT xell_result_t xellSetLoggingCallback(xell_context_handle_t hContext, xell_logging_level_t loggingLevel, xell_app_log_callback_t loggingCallback);

XELL_PACK_B()
/**
 * @brief XeLL frame stats.
 *
 * XeLL frame timestamps.
 */
typedef struct _xell_frame_report_t
{
	uint32_t m_frame_id;					//frame id
	uint64_t m_sim_start_ts;				//ns
	uint64_t m_sim_end_ts;					//ns
	uint64_t m_render_submit_start_ts;		//ns
	uint64_t m_render_submit_end_ts;		//ns
	uint64_t m_present_start_ts;			//ns
	uint64_t m_present_end_ts;				//ns

	/** Reserved for future use. */
	uint64_t reserved1;
	uint64_t reserved2;
	uint64_t reserved3;
	uint64_t reserved4;
	uint64_t reserved5;
} xell_frame_report_t;
XELL_PACK_E()

/**
 * @brief Get frame stats for debugging purpose.
 * @param context: The XeLL context handle.
 * @param[out] outdata: Last 64 frames reports.
 * @return XeLL return status code.
*/
XELL_EXPORT xell_result_t xellGetFramesReports(xell_context_handle_t context, xell_frame_report_t* outdata);


// Enum size checks. All enums must be 4 bytes
typedef char sizecheck__LINE__[(sizeof(xell_result_t) == 4) ? 1 : -1];
typedef char sizecheck__LINE__[(sizeof(_xell_latency_marker_type_t) == 4) ? 1 : -1];

#ifdef __cplusplus
}
#endif
