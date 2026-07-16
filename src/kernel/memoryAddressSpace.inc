// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN = 0x0000040000ull;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX = 0x07fffeffffull;
static constexpr uint64_t HOST_USER_MIN           = 0x1000000000ull;
static constexpr uint64_t HOST_USER_MAX           = 0xfbffffffffull;
#endif

class DirectMemoryBacking {
public:
	enum class FailureReason {
		None,
		BackingUnavailable,
		PhysicalRangeOutOfBounds,
		HostAddressNotGranularityAligned,
		SizeNotGranularityAligned,
		ReserveAlignedFailed,
		FixedMapViewFailed,
		PlaceholderReserveFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		ReservedContainerHasCommittedPages,
		ReservedContainerReleaseFailed,
		ReservedContainerRestoreFailed,
		ReservedContainerSplitNotGranularityAligned,
		WrongHostAddress,
		UnmapRollbackFailed,
	};

	explicit DirectMemoryBacking(uint64_t size): m_size(size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;

		m_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE,
		                              static_cast<DWORD>(m_size >> 32u),
		                              static_cast<DWORD>(m_size & 0xffffffffu), nullptr);
		if (m_handle == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: CreateFileMapping failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			return;
		}
		m_backing_base = static_cast<uint8_t*>(
		    MapViewOfFile(m_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, m_size));
		if (m_backing_base == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			CloseHandle(m_handle);
			m_handle = nullptr;
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
		if (m_fd < 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: memfd_create failed: %d\n",
			           errno);
			return;
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: ftruncate failed: %d\n", errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: writable alias failed: %d\n",
			           errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		m_backing_base = static_cast<uint8_t*>(backing);
#endif
	}

	~DirectMemoryBacking() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (m_backing_base != nullptr) {
			UnmapViewOfFile(m_backing_base);
		}
		if (m_handle != nullptr) {
			CloseHandle(m_handle);
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		if (m_backing_base != nullptr) {
			munmap(m_backing_base, m_size);
		}
		if (m_fd >= 0) {
			close(m_fd);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(DirectMemoryBacking);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::BackingUnavailable: return "backing-unavailable";
			case FailureReason::PhysicalRangeOutOfBounds: return "physical-range-out-of-bounds";
			case FailureReason::HostAddressNotGranularityAligned:
				return "host-address-not-64k-aligned";
			case FailureReason::SizeNotGranularityAligned: return "size-not-64k-aligned";
			case FailureReason::ReserveAlignedFailed: return "reserve-aligned-failed";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderReserveFailed: return "placeholder-reserve-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::ReservedContainerHasCommittedPages:
				return "reserved-container-has-committed-pages";
			case FailureReason::ReservedContainerReleaseFailed:
				return "reserved-container-release-failed";
			case FailureReason::ReservedContainerRestoreFailed:
				return "reserved-container-restore-failed";
			case FailureReason::ReservedContainerSplitNotGranularityAligned:
				return "reserved-container-split-not-64k-aligned";
			case FailureReason::WrongHostAddress: return "wrong-host-address";
			case FailureReason::UnmapRollbackFailed: return "unmap-rollback-failed";
			default: return "unknown";
		}
	}

	[[nodiscard]] bool IsAvailable() const {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return m_handle != nullptr && m_backing_base != nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		return m_fd >= 0 && m_backing_base != nullptr;
#else
		return false;
#endif
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (data == nullptr) {
			return false;
		}
		const auto* source = static_cast<const uint8_t*>(data);
		return TryTransferBacking(vaddr, size,
		                          [source](uint8_t* backing, uint64_t data_offset, size_t bytes) {
			                          std::memcpy(backing, source + data_offset, bytes);
		                          });
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr) {
			return false;
		}
		auto* destination = static_cast<uint8_t*>(data);
		return TryTransferBacking(vaddr, size,
		                          [destination](uint8_t* backing, uint64_t data_offset, size_t bytes) {
			                          std::memcpy(destination + data_offset, backing, bytes);
		                          });
	}

	uint64_t MapAligned(uint64_t search_addr, uint64_t size, uint64_t backing_offset,
	                    VirtualMemory::Mode mode, uint64_t alignment,
	                    FailureReason* failure_reason = nullptr) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return 0;
		}

		if (alignment == 0) {
			alignment = 0x4000;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		alignment = std::max<uint64_t>(alignment, m_granularity);
		return MapNewPlaceholderAligned(search_addr, size, backing_offset, mode, alignment,
		                                failure_reason);
#else
		auto reserved = VirtualMemory::ReserveAligned(search_addr, size, alignment);
		if (reserved == 0) {
			SetFailure(failure_reason, FailureReason::ReserveAlignedFailed);
			return 0;
		}
		if (!ReleaseReservedProbe(reserved, failure_reason)) {
			return 0;
		}
		if (MapFixed(reserved, size, backing_offset, mode, failure_reason)) {
			return reserved;
		}

		return 0;
#endif
	}

	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason = nullptr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (ShouldChunkMap(size) && IsGranularityAligned(vaddr) &&
		    IsGranularityAligned(backing_offset)) {
			return MapFixedChunked(vaddr, size, backing_offset, mode, failure_reason);
		}
#endif

		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}

		void* mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (mapped == nullptr && ReleaseExactHostAllocation(vaddr, size)) {
			mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
		}
		if (mapped == nullptr) {
			ReleasedReservedHostAllocation released {};
			if (ReleaseReservedHostAllocationForMap(vaddr, size, &released, failure_reason)) {
				mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
				if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
					if (mapped != nullptr) {
						UnmapView(mapped, size);
						mapped = nullptr;
					}
					RestoreReleasedReservedHostAllocation(released);
				} else if (!ReserveReleasedReservedHostRemainders(released)) {
					UnmapView(mapped, size);
					mapped = nullptr;
					RestoreReleasedReservedHostAllocation(released);
					SetFailure(failure_reason, FailureReason::ReservedContainerRestoreFailed);
				}
			}
		}
		if (mapped == nullptr) {
			std::vector<ReleasedReservedHostAllocation> released_ranges;
			if (ReleaseReservedHostAllocationsForMap(vaddr, size, &released_ranges,
			                                         failure_reason)) {
				mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
				if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
					if (mapped != nullptr) {
						UnmapView(mapped, size);
						mapped = nullptr;
					}
					RestoreReleasedReservedHostAllocations(released_ranges);
				} else if (!ReserveReleasedReservedHostRemainders(released_ranges)) {
					UnmapView(mapped, size);
					mapped = nullptr;
					RestoreReleasedReservedHostAllocations(released_ranges);
					SetFailure(failure_reason, FailureReason::ReservedContainerRestoreFailed);
				}
			}
		}
#endif
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			if (mapped != nullptr) {
				if (!UnmapView(mapped, size)) {
					SetFailure(failure_reason, FailureReason::UnmapRollbackFailed);
				}
			}
			if (mapped != nullptr) {
				SetFailure(failure_reason, FailureReason::WrongHostAddress);
			}
			return false;
		}

		{
			std::lock_guard lock(m_mutex);
			EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
	}

	bool MapExistingPlaceholderFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                                 VirtualMemory::Mode mode,
	                                 FailureReason*      failure_reason = nullptr) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		void* mapped = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			if (mapped != nullptr) {
				UnmapView(mapped, size);
				SetFailure(failure_reason, FailureReason::WrongHostAddress);
			}
			return false;
		}

		{
			std::lock_guard lock(m_mutex);
			EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#else
		return MapFixed(vaddr, size, backing_offset, mode, failure_reason);
#endif
	}

	bool Unmap(uint64_t vaddr, uint64_t size, bool preserve_placeholder = false,
	           bool* placeholder_preserved = nullptr) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = false;
		}

		if (!Contains(vaddr, size)) {
			return false;
		}

		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			MapEntry entry {};
			{
				std::lock_guard lock(m_mutex);
				if (!FindContainingUnlocked(current, 1, &entry)) {
					return false;
				}
			}

			const auto entry_end = entry.vaddr + entry.size;
			const auto part_size = std::min(end, entry_end) - current;
			const bool preserve_part_placeholder =
			    preserve_placeholder && current == vaddr && part_size == size;
			if (!UnmapOne(current, part_size, preserve_part_placeholder, placeholder_preserved)) {
				return false;
			}

			current += part_size;
		}

		return true;
	}

	bool UnmapOne(uint64_t vaddr, uint64_t size, bool preserve_placeholder = false,
	              bool* placeholder_preserved = nullptr) {
		MapEntry old {};
		{
			std::lock_guard lock(m_mutex);
			auto next = m_maps.upper_bound(vaddr);
			if (next != m_maps.begin()) {
				auto entry = std::prev(next);
				if (vaddr + size <= entry->second.vaddr + entry->second.size) {
					old = entry->second;
					m_maps.erase(entry);
				}
			}
		}

		if (old.size == 0) {
			return false;
		}

		const bool exact_unmap = (vaddr == old.vaddr && size == old.size);
		if (exact_unmap) {
			const bool unmapped =
			    preserve_placeholder
			        ? UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)
			        : UnmapView(reinterpret_cast<void*>(old.vaddr), old.size);
			if (!unmapped) {
				std::lock_guard lock(m_mutex);
				m_maps.emplace(old.vaddr, old);
				return false;
			}
			if (preserve_placeholder && placeholder_preserved != nullptr) {
				*placeholder_preserved = true;
			}
			return true;
		}

		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t old_end    = old.vaddr + old.size;
		const uint64_t right_size = old_end - right_addr;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)) {
			bool ok = true;
			if (left_size != 0 && left_size != old.size) {
				ok = SplitPlaceholder(old.vaddr, left_size) && ok;
			}
			if (right_size != 0) {
				ok = SplitPlaceholder(vaddr, size) && ok;
			}
			if (left_size != 0) {
				ok = MapExistingPlaceholderFixed(old.vaddr, left_size, old.backing_offset,
				                                 old.mode) &&
				     ok;
			}
			if (right_size != 0) {
				ok = MapExistingPlaceholderFixed(right_addr, right_size,
				                                 old.backing_offset + (right_addr - old.vaddr),
				                                 old.mode) &&
				     ok;
			}
			if (ok) {
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = true;
				}
				return true;
			}
		} else
#endif
		{
			if (!UnmapView(reinterpret_cast<void*>(old.vaddr), old.size)) {
				std::lock_guard lock(m_mutex);
				m_maps.emplace(old.vaddr, old);
				return false;
			}
		}

		bool ok = true;
		if (left_size != 0) {
			ok = MapFixed(old.vaddr, left_size, old.backing_offset, old.mode) && ok;
		}
		if (right_size != 0) {
			ok = MapFixed(right_addr, right_size, old.backing_offset + (right_addr - old.vaddr),
			              old.mode) &&
			     ok;
		}
		if (!ok) {
			if (left_size != 0) {
				UnmapOne(old.vaddr, left_size);
			}
			if (right_size != 0) {
				UnmapOne(right_addr, right_size);
			}
			MapFixed(old.vaddr, old.size, old.backing_offset, old.mode);
			return false;
		}

		return ok;
	}

	void UpdateProtection(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::lock_guard lock(m_mutex);
		for (auto& map_entry: m_maps) {
			auto& entry = map_entry.second;
			if (VirtualRangesOverlap(vaddr, size, entry.vaddr, entry.size)) {
				entry.mode = mode;
			}
		}
	}

	bool Contains(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		const auto      end     = vaddr + size;
		auto            current = vaddr;

		while (current < end) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) {
				return false;
			}
			const auto entry_end = entry.vaddr + entry.size;
			if (entry_end <= current) {
				return false;
			}
			current = std::min(end, entry_end);
		}

		return true;
	}

	bool SelfTest() {
		const uint64_t test_size = GetMappableTestSize();
		if (test_size == 0 || !RangeOk(0, test_size)) {
			LOGF_COLOR(Log::Color::Yellow, "\t direct-memory backing self-test skipped\n");
			return false;
		}

		const auto first = MapAligned(0, test_size, 0, VirtualMemory::Mode::ReadWrite, test_size);
		if (first == 0) {
			LOGF_COLOR(Log::Color::Yellow,
			           "\t direct-memory backing self-test skipped: first alias unavailable\n");
			return false;
		}

		const auto second = MapAligned(0, test_size, 0, VirtualMemory::Mode::ReadWrite, test_size);
		if (second == 0) {
			Unmap(first, test_size);
			LOGF_COLOR(Log::Color::Yellow,
			           "\t direct-memory backing self-test skipped: second alias unavailable\n");
			return false;
		}

		auto* second_ptr = reinterpret_cast<uint64_t*>(second);

		constexpr uint64_t test_value = 0x4b595459414c4941ull; // "KYTYALIA"
		EXIT_IF(!VirtualMemory::Protect(first, test_size, VirtualMemory::Mode::NoAccess));
		const bool ok = TryWriteBacking(first, &test_value, sizeof(test_value)) &&
		                *second_ptr == test_value;
		const uint64_t clear = 0;
		EXIT_IF(!TryWriteBacking(first, &clear, sizeof(clear)));

		Unmap(second, test_size);
		Unmap(first, test_size);

		LOGF_COLOR(ok ? Log::Color::Green : Log::Color::Red,
		           "\t direct-memory backing self-test: %s\n", ok ? "shared aliases ok" : "failed");
		return ok;
	}

private:
	template <typename Transfer>
	bool TryTransferBacking(uint64_t vaddr, uint64_t size, Transfer transfer) {
		if (!IsAvailable() || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) {
				return false;
			}
			const auto bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

		current = vaddr;
		while (current < end) {
			MapEntry entry {};
			EXIT_IF(!FindContainingUnlocked(current, 1, &entry));
			const auto bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto offset = entry.backing_offset + current - entry.vaddr;
			transfer(m_backing_base + offset, current - vaddr, static_cast<size_t>(bytes));
			current += bytes;
		}
		return true;
	}

	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
	};

	bool FindContainingUnlocked(uint64_t vaddr, uint64_t size, MapEntry* out) const {
		EXIT_IF(out == nullptr);

		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		auto next = m_maps.upper_bound(vaddr);
		if (next == m_maps.begin()) {
			return false;
		}
		const auto& entry = std::prev(next)->second;
		if (vaddr + size <= entry.vaddr + entry.size) {
			*out = entry;
			return true;
		}

		return false;
	}

	[[nodiscard]] bool RangeOk(uint64_t backing_offset, uint64_t size) const {
		return size != 0 && backing_offset < m_size && size <= m_size - backing_offset;
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	static bool ReleaseReservedProbe(uint64_t vaddr, FailureReason* failure_reason) {
		if (VirtualMemory::Free(vaddr)) {
			return true;
		}
		SetFailure(failure_reason, FailureReason::ReservedContainerReleaseFailed);
		return false;
	}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	[[nodiscard]] uint64_t    GetMappableTestSize() const { return m_granularity; }
	static constexpr uint64_t PageSize() { return 0x4000; }

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	[[nodiscard]] bool IsGranularityAligned(uint64_t value) const {
		return (value & (m_granularity - 1u)) == 0;
	}

	[[nodiscard]] uint64_t GetMapChunkSize() const {
		return std::max<uint64_t>(m_granularity, 0x200000ull);
	}

	[[nodiscard]] bool ShouldChunkMap(uint64_t size) const { return size > GetMapChunkSize(); }

	uint64_t MapNewPlaceholderAligned(uint64_t search_addr, uint64_t size,
	                                  uint64_t backing_offset, VirtualMemory::Mode mode,
	                                  uint64_t alignment, FailureReason* failure_reason) {
		MEM_ADDRESS_REQUIREMENTS requirements {};
		MEM_EXTENDED_PARAMETER   parameter {};
		const auto aligned_search =
		    (search_addr + alignment - 1u) & ~(alignment - 1u);
		requirements.LowestStartingAddress = reinterpret_cast<void*>(
		    search_addr != 0 ? aligned_search : HOST_SYSTEM_MANAGED_MIN);
		requirements.HighestEndingAddress =
		    reinterpret_cast<void*>(search_addr != 0 ? HOST_USER_MAX : HOST_SYSTEM_MANAGED_MAX);
		requirements.Alignment = alignment;
		parameter.Type          = MemExtendedParameterAddressRequirements;
		parameter.Pointer       = &requirements;

		auto reserve = [&]() {
			return VirtualAlloc2(GetCurrentProcess(), nullptr, size,
			                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &parameter, 1);
		};
		auto* placeholder = reserve();
		if (placeholder == nullptr && search_addr == 0) {
			requirements.LowestStartingAddress = reinterpret_cast<void*>(HOST_USER_MIN);
			requirements.HighestEndingAddress  = reinterpret_cast<void*>(HOST_USER_MAX);
			placeholder                        = reserve();
		}
		if (placeholder == nullptr) {
			SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
			return 0;
		}

		const auto            vaddr = reinterpret_cast<uint64_t>(placeholder);
		std::vector<MapEntry> entries;
		uint64_t              mapped = 0;
		while (mapped < size) {
			const auto current   = vaddr + mapped;
			const auto remaining = size - mapped;
			const auto part_size = std::min<uint64_t>(GetMapChunkSize(), remaining);
			const bool split     = part_size != remaining;
			if (split && !SplitPlaceholder(current, part_size)) {
				for (const auto& entry: entries) {
					UnmapView(reinterpret_cast<void*>(entry.vaddr), entry.size);
				}
				VirtualFree(reinterpret_cast<void*>(current), 0, MEM_RELEASE);
				SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
				return 0;
			}
			if (ReplacePlaceholderView(current, part_size, backing_offset + mapped, mode,
			                           failure_reason) == nullptr) {
				for (const auto& entry: entries) {
					UnmapView(reinterpret_cast<void*>(entry.vaddr), entry.size);
				}
				VirtualFree(reinterpret_cast<void*>(current), 0, MEM_RELEASE);
				if (split) {
					VirtualFree(reinterpret_cast<void*>(current + part_size), 0, MEM_RELEASE);
				}
				return 0;
			}
			entries.push_back(MapEntry {current, part_size, backing_offset + mapped, mode});
			mapped += part_size;
		}

		{
			std::lock_guard lock(m_mutex);
			for (const auto& entry: entries) {
				EXIT_IF(!m_maps.emplace(entry.vaddr, entry).second);
			}
		}
		SetFailure(failure_reason, FailureReason::None);
		return vaddr;
	}

	bool MapFixedChunked(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                     VirtualMemory::Mode mode, FailureReason* failure_reason) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}
		if (!IsGranularityAligned(vaddr) || !IsGranularityAligned(backing_offset)) {
			SetFailure(failure_reason, FailureReason::HostAddressNotGranularityAligned);
			return false;
		}

		const auto chunk_size = GetMapChunkSize();
		uint64_t   mapped     = 0;
		while (mapped < size) {
			const auto part_size = std::min<uint64_t>(chunk_size, size - mapped);
			if (!MapFixed(vaddr + mapped, part_size, backing_offset + mapped, mode,
			              failure_reason)) {
				uint64_t rollback = 0;
				while (rollback < mapped) {
					const auto rollback_size = std::min<uint64_t>(chunk_size, mapped - rollback);
					UnmapOne(vaddr + rollback, rollback_size);
					rollback += rollback_size;
				}
				return false;
			}
			mapped += part_size;
		}

		SetFailure(failure_reason, FailureReason::None);
		return true;
	}

	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	static bool ApplyProtection(void* ptr, uint64_t size, VirtualMemory::Mode mode) {
		DWORD old_protect = 0;
		return VirtualProtect(ptr, size, GetProtect(mode), &old_protect) != 0;
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return true;
		}
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	static bool ReleaseExactHostAllocation(uint64_t vaddr, uint64_t size) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(vaddr), &info, sizeof(info)) == 0) {
			return false;
		}

		if ((info.State != MEM_RESERVE && info.State != MEM_COMMIT) ||
		    reinterpret_cast<uint64_t>(info.BaseAddress) != vaddr ||
		    reinterpret_cast<uint64_t>(info.AllocationBase) != vaddr || info.RegionSize != size) {
			return false;
		}

		return VirtualFree(reinterpret_cast<void*>(vaddr), 0, MEM_RELEASE) != 0;
	}

	struct ReleasedReservedHostAllocation {
		uint64_t base      = 0;
		uint64_t size      = 0;
		uint64_t map_start = 0;
		uint64_t map_size  = 0;
	};

	static bool ReserveHostRange(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return true;
		}

		auto* ptr = VirtualAlloc(reinterpret_cast<void*>(vaddr), size, MEM_RESERVE, PAGE_NOACCESS);
		return ptr != nullptr && reinterpret_cast<uint64_t>(ptr) == vaddr;
	}

	static bool
	RestoreReleasedReservedHostAllocation(const ReleasedReservedHostAllocation& released) {
		return ReserveHostRange(released.base, released.size);
	}

	static bool RestoreReleasedReservedHostAllocations(
	    const std::vector<ReleasedReservedHostAllocation>& released_ranges) {
		bool ok = true;
		for (const auto& released: released_ranges) {
			ok = RestoreReleasedReservedHostAllocation(released) && ok;
		}
		return ok;
	}

	void* ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                             VirtualMemory::Mode mode, FailureReason* failure_reason) {
		const DWORD map_protect =
		    mode == VirtualMemory::Mode::NoAccess ? PAGE_READWRITE : GetProtect(mode);
		void* ptr =
		    MapViewOfFile3(m_handle, GetCurrentProcess(), reinterpret_cast<void*>(vaddr),
		                   backing_offset, size, MEM_REPLACE_PLACEHOLDER, map_protect, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile(ptr);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return nullptr;
		}

		if (!ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile(ptr);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool
	ReserveReleasedReservedHostRemainders(const ReleasedReservedHostAllocation& released) {
		const auto map_end      = released.map_start + released.map_size;
		const auto released_end = released.base + released.size;
		const auto left_size    = released.map_start - released.base;
		const auto right_size   = released_end - map_end;

		if (!ReserveHostRange(released.base, left_size)) {
			return false;
		}
		if (!ReserveHostRange(map_end, right_size)) {
			if (left_size != 0) {
				VirtualFree(reinterpret_cast<void*>(released.base), 0, MEM_RELEASE);
			}
			return false;
		}
		return true;
	}

	static bool ReserveReleasedReservedHostRemainders(
	    const std::vector<ReleasedReservedHostAllocation>& released_ranges) {
		bool ok = true;
		for (const auto& released: released_ranges) {
			ok = ReserveReleasedReservedHostRemainders(released) && ok;
		}
		return ok;
	}

	bool QueryReservedHostAllocation(uint64_t vaddr, ReleasedReservedHostAllocation* out,
	                                 FailureReason* failure_reason) const {
		EXIT_IF(out == nullptr);

		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(vaddr), &info, sizeof(info)) == 0) {
			SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
			return false;
		}

		const auto allocation_base = reinterpret_cast<uint64_t>(info.AllocationBase);
		const auto query_base      = reinterpret_cast<uint64_t>(info.BaseAddress);
		if (info.State != MEM_RESERVE || query_base > vaddr || allocation_base > vaddr) {
			SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
			return false;
		}

		uint64_t allocation_end = query_base + info.RegionSize;
		uint64_t scan           = allocation_end;
		while (true) {
			MEMORY_BASIC_INFORMATION next {};
			if (VirtualQuery(reinterpret_cast<const void*>(scan), &next, sizeof(next)) == 0 ||
			    reinterpret_cast<uint64_t>(next.AllocationBase) != allocation_base ||
			    next.State == MEM_FREE) {
				break;
			}
			if (next.State != MEM_RESERVE) {
				SetFailure(failure_reason, FailureReason::ReservedContainerHasCommittedPages);
				return false;
			}
			allocation_end = reinterpret_cast<uint64_t>(next.BaseAddress) + next.RegionSize;
			scan           = allocation_end;
		}

		scan = allocation_base;
		while (scan < allocation_end) {
			MEMORY_BASIC_INFORMATION part {};
			if (VirtualQuery(reinterpret_cast<const void*>(scan), &part, sizeof(part)) == 0 ||
			    reinterpret_cast<uint64_t>(part.AllocationBase) != allocation_base ||
			    part.State != MEM_RESERVE) {
				SetFailure(failure_reason, FailureReason::ReservedContainerHasCommittedPages);
				return false;
			}
			scan = reinterpret_cast<uint64_t>(part.BaseAddress) + part.RegionSize;
		}

		out->base      = allocation_base;
		out->size      = allocation_end - allocation_base;
		out->map_start = 0;
		out->map_size  = 0;
		return true;
	}

	bool ReleaseReservedHostAllocationForMap(uint64_t vaddr, uint64_t size,
	                                         ReleasedReservedHostAllocation* out,
	                                         FailureReason*                  failure_reason) const {
		if (!QueryReservedHostAllocation(vaddr, out, failure_reason)) {
			return false;
		}

		const auto allocation_end = out->base + out->size;
		if (vaddr < out->base || vaddr + size > allocation_end) {
			SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
			return false;
		}
		if (out->base == vaddr && out->size == size) {
			return false;
		}
		if (!IsGranularityAligned(size)) {
			SetFailure(failure_reason, FailureReason::ReservedContainerSplitNotGranularityAligned);
			return false;
		}
		if (VirtualFree(reinterpret_cast<void*>(out->base), 0, MEM_RELEASE) == 0) {
			SetFailure(failure_reason, FailureReason::ReservedContainerReleaseFailed);
			return false;
		}

		out->map_start = vaddr;
		out->map_size  = size;
		return true;
	}

	bool ReleaseReservedHostAllocationsForMap(uint64_t vaddr, uint64_t size,
	                                          std::vector<ReleasedReservedHostAllocation>* out,
	                                          FailureReason* failure_reason) const {
		EXIT_IF(out == nullptr);

		out->clear();
		if (size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			SetFailure(failure_reason, FailureReason::ReservedContainerSplitNotGranularityAligned);
			return false;
		}

		const auto target_end = vaddr + size;
		auto       current    = vaddr;
		while (current < target_end) {
			ReleasedReservedHostAllocation released {};
			if (!QueryReservedHostAllocation(current, &released, failure_reason)) {
				out->clear();
				return false;
			}

			const auto map_start = std::max(vaddr, released.base);
			const auto map_end   = std::min(target_end, released.base + released.size);
			if (map_start >= map_end || !IsPageAligned(map_start) ||
			    !IsPageAligned(map_end - map_start)) {
				SetFailure(failure_reason,
				           FailureReason::ReservedContainerSplitNotGranularityAligned);
				out->clear();
				return false;
			}

			released.map_start = map_start;
			released.map_size  = map_end - map_start;
			out->push_back(released);
			current = map_end;
		}

		for (size_t i = 0; i < out->size(); i++) {
			const auto& released = (*out)[i];
			if (VirtualFree(reinterpret_cast<void*>(released.base), 0, MEM_RELEASE) == 0) {
				for (size_t restore_index = 0; restore_index < i; restore_index++) {
					RestoreReleasedReservedHostAllocation((*out)[restore_index]);
				}
				SetFailure(failure_reason, FailureReason::ReservedContainerReleaseFailed);
				out->clear();
				return false;
			}
		}

		return !out->empty();
	}

	void* MapPlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                         VirtualMemory::Mode mode, FailureReason* failure_reason) {
		void* placeholder =
		    VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
		if (placeholder == nullptr || reinterpret_cast<uint64_t>(placeholder) != vaddr) {
			if (placeholder != nullptr) {
				VirtualFree(placeholder, 0, MEM_RELEASE);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderReserveFailed);
			return nullptr;
		}

		auto* ptr = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		if (ptr == nullptr) {
			VirtualFree(reinterpret_cast<void*>(vaddr), 0, MEM_RELEASE);
			return nullptr;
		}
		return ptr;
	}

	void* MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason) {
		constexpr DWORD access = FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_EXECUTE;
		void* ptr = MapViewOfFileEx(m_handle, access, static_cast<DWORD>(backing_offset >> 32u),
		                            static_cast<DWORD>(backing_offset & 0xffffffffu), size,
		                            reinterpret_cast<void*>(vaddr));
		if (ptr == nullptr) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			ptr = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
			if (ptr != nullptr) {
				return ptr;
			}
			return MapPlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		}

		if (!ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile(ptr);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool UnmapView(void* vaddr, uint64_t /*size*/) { return UnmapViewOfFile(vaddr) != 0; }

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE   m_handle      = nullptr;
	uint64_t m_granularity = 0x10000;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	[[nodiscard]] static uint64_t GetMappableTestSize() { return 0x4000; }

	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	void* MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason) {
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode),
		                 MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool UnmapView(void* vaddr, uint64_t size) { return munmap(vaddr, size) == 0; }

	static bool UnmapViewPreservePlaceholder(void* /*vaddr*/, uint64_t /*size*/) { return false; }

	int m_fd = -1;
#endif

	uint8_t*              m_backing_base = nullptr;
	uint64_t              m_size         = 0;
	std::mutex            m_mutex;
	std::map<uint64_t, MapEntry> m_maps;
};

class PlaceholderAddressSpace {
public:
	PlaceholderAddressSpace() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
	}

	KYTY_CLASS_NO_COPY(PlaceholderAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }

	uint64_t ReserveAligned(uint64_t search_addr, uint64_t size, uint64_t alignment) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (size == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment != 0 ? alignment : 0x4000, m_granularity);

		MEM_ADDRESS_REQUIREMENTS req {};
		MEM_EXTENDED_PARAMETER   param {};
		req.LowestStartingAddress = reinterpret_cast<void*>(
		    AlignUp(search_addr != 0 ? search_addr : HOST_SYSTEM_MANAGED_MIN, alignment));
		req.HighestEndingAddress =
		    reinterpret_cast<void*>(search_addr != 0 ? HOST_USER_MAX : HOST_SYSTEM_MANAGED_MAX);
		req.Alignment = alignment;
		param.Type    = MemExtendedParameterAddressRequirements;
		param.Pointer = &req;

		auto* ptr = VirtualAlloc2(GetCurrentProcess(), nullptr, size,
		                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
		if (ptr == nullptr && search_addr == 0) {
			req.LowestStartingAddress = reinterpret_cast<void*>(HOST_USER_MIN);
			req.HighestEndingAddress  = reinterpret_cast<void*>(HOST_USER_MAX);
			ptr                       = VirtualAlloc2(GetCurrentProcess(), nullptr, size,
			                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
		}
		if (ptr == nullptr) {
			return 0;
		}

		const auto addr = reinterpret_cast<uint64_t>(ptr);
		AddFree(addr, size);
		return addr;
#else
		auto addr = VirtualMemory::ReserveAligned(search_addr, size, alignment);
		return addr;
#endif
	}

	bool ReserveFixed(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		{
			std::lock_guard lock(m_mutex);
			if (FindContaining(vaddr, size) != m_free.end()) {
				return true;
			}
		}
		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			return false;
		}
		AddFree(vaddr, size);
		return true;
#else
		return VirtualMemory::ReserveFixed(vaddr, size);
#endif
	}

	bool Consume(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		std::lock_guard lock(m_mutex);
		auto            it = FindContaining(vaddr, size);
		if (it == m_free.end()) {
			return false;
		}
		if (!SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		it = m_free.find(vaddr);
		if (it == m_free.end() || it->second != size) {
			return false;
		}
		m_free.erase(it);
		return true;
#else
		return false;
#endif
	}

	bool ReleaseFree(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		std::lock_guard lock(m_mutex);
		auto            it = FindContaining(vaddr, size);
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		it = m_free.find(vaddr);
		if (it == m_free.end() || it->second != size) {
			return false;
		}
		m_free.erase(it);
		if (VirtualFree(reinterpret_cast<void*>(vaddr), 0, MEM_RELEASE) != 0) {
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#else
		return VirtualMemory::FreeRange(vaddr, size);
#endif
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!Consume(vaddr, size)) {
			return false;
		}

		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
		                          GetProtect(mode), nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			AddFree(vaddr, size);
			return false;
		}
		return true;
#else
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		if (VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
			return false;
		}
		AddFree(vaddr, size);
		return true;
#else
		return false;
#endif
	}

	bool SelfTest() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto addr = ReserveAligned(0, m_granularity, m_granularity);
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Yellow,
			           "\t placeholder address-space self-test skipped: reserve unavailable\n");
			return false;
		}
		if (!Commit(addr, m_granularity, VirtualMemory::Mode::ReadWrite)) {
			ReleaseFree(addr, m_granularity);
			LOGF_COLOR(Log::Color::Red, "\t placeholder address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, m_granularity)) {
			VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE);
			LOGF_COLOR(Log::Color::Red,
			           "\t placeholder address-space self-test: release-committed failed\n");
			return false;
		}

		const bool ok = ReleaseFree(addr, m_granularity);
		LOGF_COLOR(ok ? Log::Color::Green : Log::Color::Red,
		           "\t placeholder address-space self-test: %s\n",
		           ok ? "ok" : "release-free failed");
		return ok;
#else
		LOGF_COLOR(Log::Color::Yellow, "\t placeholder address-space self-test skipped\n");
		return false;
#endif
	}

	void AddFree(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		std::lock_guard lock(m_mutex);
		AddFreeUnlocked(vaddr, size);
#endif
	}

private:
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		return (value + alignment - 1u) & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	[[nodiscard]] bool IsGranularityAligned(uint64_t value) const {
		return (value & (m_granularity - 1u)) == 0;
	}

	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it  = std::prev(next);
		auto end = it->first + it->second;
		return (vaddr >= it->first && vaddr + size <= end ? it : m_free.end());
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base == vaddr && end == vaddr + size) {
			return true;
		}

		if (base != vaddr) {
			const auto left_size = vaddr - base;
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				return false;
			}
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				return false;
			}
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0) {
			return;
		}
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
			if (VirtualFree(reinterpret_cast<void*>(base), end - base,
			                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
		}
	}

	uint64_t                     m_granularity = 0x10000;
	std::mutex                   m_mutex;
	std::map<uint64_t, uint64_t> m_free;
#endif
};
