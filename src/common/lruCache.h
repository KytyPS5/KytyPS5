#ifndef EMULATOR_SRC_COMMON_LRUCACHE_H_
#define EMULATOR_SRC_COMMON_LRUCACHE_H_

#include <cstddef>
#include <deque>
#include <type_traits>
#include <utility>

namespace Common {

template <typename Object, typename Tick>
class LeastRecentlyUsedCache {
	struct Item {
		Object object {};
		Tick   tick {};
		Item*  next = nullptr;
		Item*  prev = nullptr;
	};

public:
	/// Construct an empty cache with no linked entries.
	LeastRecentlyUsedCache() = default;
	/// Copying is disabled because links belong to this cache's item storage.
	LeastRecentlyUsedCache(const LeastRecentlyUsedCache&) = delete;
	/// Copy assignment cannot share the intrusive links of another cache.
	LeastRecentlyUsedCache& operator=(const LeastRecentlyUsedCache&) = delete;

	/// Transfer storage and links together, leaving the source empty and reusable.
	LeastRecentlyUsedCache(LeastRecentlyUsedCache&& other): LeastRecentlyUsedCache() {
		Swap(other);
	}

	/// Replace the contents by moving ownership; self-move preserves the cache.
	LeastRecentlyUsedCache& operator=(LeastRecentlyUsedCache&& other) noexcept {
		if (this != &other) {
			m_items.clear();
			m_free.clear();
			m_first = nullptr;
			m_last  = nullptr;
			Swap(other);
		}
		return *this;
	}

	/// Insert an object at the newest end, reusing a free ID when available.
	[[nodiscard]] size_t Insert(Object object, Tick tick) {
		const auto id   = Build();
		auto&      item = m_items[id];
		item.object     = std::move(object);
		item.tick       = tick;
		Attach(item);
		return id;
	}

	/// Advance an entry timestamp and move it to the newest end when needed.
	void Touch(size_t id, Tick tick) {
		auto& item = m_items[id];
		if (item.tick >= tick) {
			return;
		}
		item.tick = tick;
		if (&item != m_last) {
			Detach(item);
			Attach(item);
		}
	}

	/// Unlink a live entry and make its ID available for reuse.
	void Free(size_t id) {
		auto& item = m_items[id];
		Detach(item);
		item.next = nullptr;
		item.prev = nullptr;
		m_free.push_back(id);
	}

	/// Visit entries up to the cutoff, stopping when a bool callback returns true.
	template <typename Function>
	void ForEachItemBelow(Tick tick, Function&& function) {
		constexpr bool ReturnsBool = std::is_same_v<std::invoke_result_t<Function, Object>, bool>;
		for (auto* item = m_first; item != nullptr;) {
			if (item->tick > tick) {
				return;
			}
			auto* next = item->next;
			if constexpr (ReturnsBool) {
				if (function(item->object)) {
					return;
				}
			} else {
				function(item->object);
			}
			item = next;
		}
	}

private:
	/// Exchange storage, free IDs and intrusive endpoints as a single ownership unit.
	void Swap(LeastRecentlyUsedCache& other) noexcept {
		m_items.swap(other.m_items);
		m_free.swap(other.m_free);
		std::swap(m_first, other.m_first);
		std::swap(m_last, other.m_last);
	}

	/// Obtain an unused slot from the free list or append a new one.
	[[nodiscard]] size_t Build() {
		if (m_free.empty()) {
			const auto id = m_items.size();
			m_items.emplace_back();
			return id;
		}
		const auto id = m_free.front();
		m_free.pop_front();
		return id;
	}

	/// Append an unlinked item to the newest end of the list.
	void Attach(Item& item) {
		if (m_first == nullptr) {
			m_first = &item;
		}
		if (m_last == nullptr) {
			m_last = &item;
			return;
		}
		item.prev    = m_last;
		m_last->next = &item;
		item.next    = nullptr;
		m_last       = &item;
	}

	/// Remove an item from the live chain and update its neighbors and endpoints.
	void Detach(Item& item) {
		if (item.prev != nullptr) {
			item.prev->next = item.next;
		}
		if (item.next != nullptr) {
			item.next->prev = item.prev;
		}
		if (m_first == &item) {
			m_first = item.next;
		}
		if (m_last == &item) {
			m_last = item.prev;
		}
	}

	std::deque<Item>   m_items;
	std::deque<size_t> m_free;
	Item*              m_first = nullptr;
	Item*              m_last  = nullptr;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_LRUCACHE_H_
