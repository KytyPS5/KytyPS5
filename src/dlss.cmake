# The proprietary NVIDIA SDK stays outside the source tree. No network fetch is
# performed implicitly; see docs/dlss.md for the tested SDK revision.
option(KYTY_ENABLE_DLSS "Enable NVIDIA DLSS Super Resolution (NGX Vulkan)" OFF)
set(KYTY_DLSS_SDK_ROOT "${KYTY_THIRD_PARTY_DIR}/DLSS" CACHE PATH "NVIDIA/DLSS SDK checkout")

add_library(kyty_dlss_sdk INTERFACE)
if(KYTY_ENABLE_DLSS)
	if(NOT EXISTS "${KYTY_DLSS_SDK_ROOT}/include/nvsdk_ngx_helpers_vk.h")
		message(FATAL_ERROR "DLSS SDK not found. Set KYTY_DLSS_SDK_ROOT to a NVIDIA/DLSS checkout; see docs/dlss.md")
	endif()
	if(WIN32 AND KYTY_CLANG_CL)
		set(ngx_dir "${KYTY_DLSS_SDK_ROOT}/lib/Windows_x86_64")
		# CMake's default runtime is /MD, even when flags do not contain /MD
		# explicitly (CMP0091). Match NVIDIA's CRT variant to the actual runtime.
		set(ngx_crt d)
		if((DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL") OR
		   CMAKE_CXX_FLAGS MATCHES "/MT" OR CMAKE_CXX_FLAGS_RELEASE MATCHES "/MT")
			set(ngx_crt s)
		endif()
		set(ngx_release "${ngx_dir}/x64/nvsdk_ngx_${ngx_crt}.lib")
		set(ngx_debug "${ngx_dir}/x64/nvsdk_ngx_${ngx_crt}_dbg.lib")
		set(KYTY_DLSS_RUNTIME "${ngx_dir}/rel/nvngx_dlss.dll")
	elseif(LINUX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
		set(ngx_dir "${KYTY_DLSS_SDK_ROOT}/lib/Linux_x86_64")
		set(ngx_release "${ngx_dir}/libnvsdk_ngx.a")
		set(ngx_debug "${ngx_release}")
		file(GLOB ngx_runtime "${ngx_dir}/rel/libnvidia-ngx-dlss.so.*")
		list(LENGTH ngx_runtime ngx_runtime_count)
		if(NOT ngx_runtime_count EQUAL 1)
			message(FATAL_ERROR "Expected one DLSS Super Resolution runtime in ${ngx_dir}/rel")
		endif()
		list(GET ngx_runtime 0 KYTY_DLSS_RUNTIME)
		target_link_libraries(kyty_dlss_sdk INTERFACE ${CMAKE_DL_LIBS})
	else()
		message(FATAL_ERROR "DLSS supports Windows x64 with clang-cl and Linux x86_64 builds. Use KYTY_ENABLE_DLSS=OFF on this platform.")
	endif()
	foreach(ngx_file IN ITEMS "${ngx_release}" "${ngx_debug}" "${KYTY_DLSS_RUNTIME}" "${KYTY_DLSS_SDK_ROOT}/LICENSE.txt")
		if(NOT EXISTS "${ngx_file}")
			message(FATAL_ERROR "Missing NVIDIA DLSS SDK file: ${ngx_file}")
		endif()
	endforeach()
	target_include_directories(kyty_dlss_sdk SYSTEM INTERFACE "${KYTY_DLSS_SDK_ROOT}/include")
	target_compile_definitions(kyty_dlss_sdk INTERFACE KYTY_HAS_DLSS=1)
	target_link_libraries(kyty_dlss_sdk INTERFACE "$<IF:$<CONFIG:Debug>,${ngx_debug},${ngx_release}>")
endif()

function(deploy_kyty_dlss target)
	if(KYTY_ENABLE_DLSS)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_DLSS_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_DLSS_SDK_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:${target}>/NVIDIA-DLSS-LICENSE.txt"
			VERBATIM)
	endif()
endfunction()
