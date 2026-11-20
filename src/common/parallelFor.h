#ifndef KYTY_COMMON_PARALLELFOR_H_
#define KYTY_COMMON_PARALLELFOR_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace Common {

// Half the hardware threads, at least one.
inline size_t ParallelThreads() {
	return std::max(1u, std::thread::hardware_concurrency() / 2);
}

// Calls job(i) for every i below count on up to max_threads threads, the caller included.
template <typename Job>
void ParallelFor(size_t count, size_t max_threads, const Job& job) {
	std::atomic_size_t next = 0;
	const auto         work = [&] {
		for (auto i = next++; i < count; i = next++) {
			job(i);
		}
	};
	std::vector<std::jthread> helpers;
	for (auto i = std::min(count, std::max<size_t>(1, max_threads)); i > 1; i--) {
		helpers.emplace_back(work);
	}
	work();
}

// Calls job(i) for every i below count on up to ParallelThreads() threads, the caller included.
template <typename Job>
void ParallelFor(size_t count, const Job& job) {
	ParallelFor(count, ParallelThreads(), job);
}

} // namespace Common

#endif // KYTY_COMMON_PARALLELFOR_H_
