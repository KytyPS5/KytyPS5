# The installer is copied outside the application before it runs. Keep it free of
# Qt and application DLL dependencies so it can replace those files on Windows.
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_executable(kyty_updater main.cpp UpdateInstaller.cpp)

if(MSVC)
	target_compile_options(kyty_updater PRIVATE /EHsc)
else()
	target_compile_options(kyty_updater PRIVATE -fexceptions)
endif()
if(MINGW)
	target_link_options(kyty_updater PRIVATE -static -municode)
endif()
