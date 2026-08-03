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
	[[nodiscard]] size_t Insert(Object object, Tick tick) {
		const auto id   = Build();
		auto&      item = m_items[id];
		item.object     = std::move(object);
		item.tick       = tick;
		Attach(item);
		return id;
	}

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

	void Free(size_t id) {
		auto& item = m_items[id];
		Detach(item);
		item.next = nullptr;
		item.prev = nullptr;
		m_free.push_back(id);
	}

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
