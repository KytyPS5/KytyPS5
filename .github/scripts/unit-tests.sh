#!/usr/bin/env bash
# The unit tests that CI builds and runs, in one list.
#
#   unit-tests.sh targets [extra...]   the CMake targets: each name with _tests
#   unit-tests.sh filter [extra...]    the ctest -R filter: ^(name|name|...)$
#   unit-tests.sh check-windows FILE   fails when the Windows lines of the workflow FILE do
#                                      not name exactly these tests (Windows runs cmd, which
#                                      cannot call this script, so it keeps its own copy)
#   unit-tests.sh check-registered DIR [extra...]
#                                      fails when a test of the list is not registered in the
#                                      build DIR, or a registered test is in none of the lists
#
# An extra name is a test that only some platforms build (memory_tracker, page_manager,
# macos_sse4a).
set -euo pipefail

names=(
	archive_file
	audio_out2_port
	audio_out_timing
	avplayer_file
	bench_script
	bit_array
	buffer_chunk
	cache_collection
	cache_folder
	ces
	controller_settings
	copy_workers
	depth_snapshot_plan
	device_suitability
	event_queue_lifetime
	fence_retirement
	file_lock
	frame_pacer
	gamepad_quit
	gpu_write_ticks
	guest_memory_window
	guest_write_log
	host_input
	host_lowering
	http_uri_parse
	image_page_table
	image_readback
	overlay_queue
	const_ram
	ime_dialog
	lru_cache
	metadata_fill
	ngs2_sampler
	pad_haptics
	pad_input
	pending_labels
	perf_counters
	pipeline_use
	player_seats
	png_enc
	program_list
	queue_commits
	readback_plan
	resource_materialization
	resource_tracking
	save_data_dialog
	save_data_memory
	scalar_provenance
	shader_cfg
	shader_vertex_metadata
	soft_float64
	submit_plan
	sync_on_address
	trophy_system
	vdecsw
	video_out_resolution
	virtual_memory_allocation
	write_heat
)

# Registered ctest names that run a binary of the list or the emulator (no target of their own).
ctest_only=(
	emulator_present_mode_cli
	emulator_present_mode_invalid
	emulator_user_name_cli
	emulator_user_name_too_long
	emulator_video_out_resolution_cli
	emulator_video_out_resolution_invalid
	guest_red_zone_patcher
)

# Tests that need a GPU. CI builds shader_recompiler_compute_tests but its runners have no GPU to
# run them: run them locally (ctest -R shader_recompiler_compute and these names).
# kernel_file_system needs one too: guest memory needs the GPU resources of a Vulkan device.
local_only=(
	buffer_cache_dirty_gc
	buffer_cache_guest_backed
	buffer_cache_ranges
	color_render_target_1d
	command_scheduler_timeline
	compute_meta_clear_classification
	gpu_command_lane
	gpu_tiler
	graphics_draw_offsets
	graphics_pipeline_rasterization
	host_image_allocation
	image_transfer_layout
	kernel_file_system
	pm4_context_state
	shader_recompiler_alignbyte
	shader_recompiler_compute
	shader_recompiler_tessellation
	stream_buffer_ring
	texture_cache_depth_readback
	texture_cache_htile_clear
	texture_cache_image_overlap
	texture_cache_image_save
	texture_cache_image_views
	texture_cache_layered_image
	texture_cache_storage_sampled
)

# Tests that only some platforms build, given as extras by the jobs that build them.
platform_only=(
	macos_sse4a
	memory_tracker
	page_manager
)

mode="${1:-}"
shift || true

case "$mode" in
	targets)
		all=("${names[@]}" "$@")
		printf '%s_tests\n' "${all[@]}" | paste -sd ' ' -
		;;
	filter)
		all=("${names[@]}" "${ctest_only[@]}" "$@")
		printf '^(%s)$\n' "$(printf '%s\n' "${all[@]}" | paste -sd '|' -)"
		;;
	check-windows)
		workflow="$1"
		expected="$(printf '%s\n' "${names[@]}" | sort)"
		expected_filter="$(printf '%s\n' "${names[@]}" "${ctest_only[@]}" | sort)"
		filter="$(grep -o 'ctest --test-dir _Build/windows .*-R "^([^)]*)' "$workflow" |
			sed 's/.*"^(//; s/)$//' | tr '|' '\n' | sort)"
		targets="$(sed -n '/cmake --build _Build\/windows/,/--parallel/p' "$workflow" |
			grep -o '[a-z0-9_]*_tests' | sed 's/_tests$//' |
			grep -vx 'shader_recompiler_compute' | sort)"
		status=0
		if [ "$filter" != "$expected_filter" ]; then
			echo "Windows ctest filter differs from .github/scripts/unit-tests.sh:" >&2
			diff <(echo "$expected_filter") <(echo "$filter") >&2 || true
			status=1
		fi
		if [ "$targets" != "$expected" ]; then
			echo "Windows build targets differ from .github/scripts/unit-tests.sh:" >&2
			diff <(echo "$expected") <(echo "$targets") >&2 || true
			status=1
		fi
		exit "$status"
		;;
	check-registered)
		dir="$1"
		shift
		registered="$(ctest --test-dir "$dir" -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort)"
		listed="$(printf '%s\n' "${names[@]}" "${ctest_only[@]}" "$@" | sort)"
		known="$(printf '%s\n' "${names[@]}" "${ctest_only[@]}" "${local_only[@]}" \
			"${platform_only[@]}" | sort)"
		status=0
		missing="$(comm -23 <(echo "$listed") <(echo "$registered"))"
		if [ -n "$missing" ]; then
			echo "Listed tests that are not registered in $dir:" >&2
			echo "$missing" >&2
			status=1
		fi
		unlisted="$(comm -13 <(echo "$known") <(echo "$registered"))"
		if [ -n "$unlisted" ]; then
			echo "Registered tests in none of the lists of $0:" >&2
			echo "$unlisted" >&2
			status=1
		fi
		exit "$status"
		;;
	*)
		echo "usage: $0 targets|filter [extra...] | check-windows FILE |" \
			"check-registered DIR [extra...]" >&2
		exit 2
		;;
esac
