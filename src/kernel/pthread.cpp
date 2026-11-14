static bool NativeCondDeadlineFromAbs(KernelClockid clock_id, const KernelTimespec* abstime,
                                      std::chrono::steady_clock::time_point* out) {
	if (abstime == nullptr || out == nullptr) {
		return false;
	}

	if (abstime->tv_sec < 0 || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) {
		return false;
	}

	const auto now_steady = std::chrono::steady_clock::now();

	if (clock_id == KERNEL_CLOCK_REALTIME || clock_id == KERNEL_CLOCK_REALTIME_PRECISE ||
	    clock_id == KERNEL_CLOCK_REALTIME_FAST || clock_id == KERNEL_CLOCK_SECOND) {
		const auto abs_tp = std::chrono::time_point<std::chrono::system_clock>(
		    std::chrono::seconds(abstime->tv_sec) + std::chrono::nanoseconds(abstime->tv_nsec));
		const auto now_sys = std::chrono::system_clock::now();
		const auto delta   = abs_tp - now_sys;
		*out               = now_steady +
		                     std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta);
		return true;
	}

	KernelTimespec now {};
	if (!NativeClockGettime(clock_id, &now)) {
		return false;
	}

	const auto delta = std::chrono::seconds(abstime->tv_sec - now.tv_sec) +
	                   std::chrono::nanoseconds(abstime->tv_nsec - now.tv_nsec);
	*out = now_steady + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta);
	return true;
}
