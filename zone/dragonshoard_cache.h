#pragma once

#include "common/types.h"

#include <memory>
#include <vector>

class SharedDatabase;

namespace EQ {
	class ItemInstance;
}

namespace DragonHoard {

// A character's Dragon's Hoard contents. Rows are read from dragonhoard_items on first use and
// served from memory afterwards; a mutation changes the cache only after its row is written.
class HoardCache {
public:
	HoardCache();
	~HoardCache();

	HoardCache(const HoardCache&) = delete;
	HoardCache& operator=(const HoardCache&) = delete;

	// Capacity, read from the rule when the character enters the zone and kept for the zone
	// session, so a rule reload does not change it under a connected client.
	uint32 SlotLimit() const { return m_slot_limit; }

	size_t Count(SharedDatabase& db, uint32 character_id);

	// Writes the row, then keeps inst. Returns the stored item, or nullptr if the write failed; the
	// cache is unchanged then.
	const EQ::ItemInstance* Deposit(SharedDatabase& db, uint32 character_id, std::unique_ptr<EQ::ItemInstance> inst);

	// The stored item with this serial number, or nullptr. The cache keeps ownership.
	EQ::ItemInstance* Find(SharedDatabase& db, uint32 character_id, int32 serial_number);

	// Deletes the row, then hands the item to the caller. Returns nullptr if no item has this serial
	// number or the delete failed; the cache is unchanged then.
	std::unique_ptr<EQ::ItemInstance> Retrieve(SharedDatabase& db, uint32 character_id, int32 serial_number);

	// Calls f(const EQ::ItemInstance*) for each stored item, oldest first.
	template <typename F>
	void ForEach(SharedDatabase& db, uint32 character_id, F&& f)
	{
		EnsureLoaded(db, character_id);

		for (const auto& entry : m_items) {
			f(entry.inst.get());
		}
	}

private:
	struct Entry {
		uint32                            row_id;
		std::unique_ptr<EQ::ItemInstance> inst;
	};

	void EnsureLoaded(SharedDatabase& db, uint32 character_id);
	std::vector<Entry>::iterator FindEntry(int32 serial_number);

	std::vector<Entry> m_items;
	const uint32       m_slot_limit;
	bool               m_loaded = false;
};

} // namespace DragonHoard
