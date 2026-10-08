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
	LeastRecentlyUsedCache() = default;
	/// Copying is disabled because links belong to this cache's item storage.
	LeastRecentlyUsedCache(const LeastRecentlyUsedCache&)            = delete;
	LeastRecentlyUsedCache& operator=(const LeastRecentlyUsedCache&) = delete;

	/// Existing IDs now belong to the destination; the source is empty and reusable.
	/// Constructing the empty deques can allocate, so this move is not noexcept.
	LeastRecentlyUsedCache(LeastRecentlyUsedCache&& other): LeastRecentlyUsedCache() {
		Swap(other);
	}

	/// Invalidates the old destination IDs and destroys its stored objects. Source IDs
	/// transfer unchanged and the source becomes empty; self-move preserves all entries.
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

	/// The tick must not precede the newest live tick: traversal stops at the first
	/// entry above its cutoff. The returned ID belongs to this cache until Free or a move.
	[[nodiscard]] size_t Insert(Object object, Tick tick) {
		const auto id   = Build();
		auto&      item = m_items[id];
		item.object     = std::move(object);
		item.tick       = tick;
		Attach(item);
		return id;
	}

	/// Requires a live ID. Older/equal ticks are ignored; an advancing tick must not
	/// precede the newest live tick, since this entry is appended without sorting.
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

	/// Requires a live ID; freeing it twice is invalid. The object remains in storage
	/// until the slot is reused or its storage is released, but traversal skips it.
	void Free(size_t id) {
		auto& item = m_items[id];
		Detach(item);
		item.next = nullptr;
		item.prev = nullptr;
		m_free.push_back(id);
	}

	/// The cutoff is inclusive and assumes nondecreasing live ticks. The callback may
	/// free the current entry; it must not invalidate the next entry saved by this walk.
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

	/// A reused slot still contains its previous object and tick. Insert must replace
	/// both before linking the slot into the live list.
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

	/// Requires an item outside the live chain. Storage must remain at
	/// a stable address while the item is linked.
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

	/// Unlinks a live item without recycling its ID or clearing its own links; Touch
	/// relinks it, whereas Free clears the links before making the slot reusable.
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
