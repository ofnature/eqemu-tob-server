#pragma once

#include "common/types.h"

#include <memory>
#include <vector>

class SharedDatabase;

namespace EQ {
	class ItemInstance;
	struct ItemData;
}

namespace TradeskillDepot {

// An account's Tradeskill Depot: one pile per item. Rows are read from tradeskill_depot_items on first use and
// served from memory afterwards; a mutation changes the cache only after its row is written.
class DepotCache {
public:
	DepotCache();
	~DepotCache();

	DepotCache(const DepotCache&) = delete;
	DepotCache& operator=(const DepotCache&) = delete;

	// Number of piles allowed, read from the rule when the character enters the zone and kept for the zone
	// session, so a rule reload does not change it under a connected client.
	uint32 SlotLimit() const { return m_slot_limit; }

	size_t Count(SharedDatabase& db, uint32 account_id);

	// Quantity stored of this item, 0 if there is no pile of it.
	uint32 Quantity(SharedDatabase& db, uint32 account_id, uint32 item_id);

	// Adds quantity to the item's pile, creating the pile if there is none. Writes the row first; returns the
	// pile as the window shows it, or nullptr if the write failed. The cache is unchanged then.
	const EQ::ItemInstance* Add(SharedDatabase& db, uint32 account_id, const EQ::ItemData* item, uint32 quantity);

	// Takes quantity from the item's pile and deletes the row when it empties. Returns false if no pile holds that
	// much or the write failed; the cache is unchanged then.
	bool Remove(SharedDatabase& db, uint32 account_id, uint32 item_id, uint32 quantity);

	// Window slot of the item's pile, or -1. A pile keeps its slot for the zone session.
	int16 SlotOf(SharedDatabase& db, uint32 account_id, uint32 item_id);

	// Calls f(int16 slot, const EQ::ItemInstance*) for each pile.
	template <typename F>
	void ForEach(SharedDatabase& db, uint32 account_id, F&& f)
	{
		EnsureLoaded(db, account_id);

		for (const auto& pile : m_piles) {
			f(pile.slot, pile.inst.get());
		}
	}

private:
	// inst is what the window shows: serial number = row id, MerchantCount = the whole pile (the TOB item
	// serializer sends it as the stack count for depot rows), charges = what one inventory stack can hold.
	struct Pile {
		uint32                            row_id;
		int16                             slot;
		uint32                            quantity;
		std::unique_ptr<EQ::ItemInstance> inst;
	};

	void EnsureLoaded(SharedDatabase& db, uint32 account_id);
	std::vector<Pile>::iterator FindPile(uint32 item_id);
	int16 FreeSlot() const;
	static void SetQuantity(Pile& pile, uint32 quantity);

	std::vector<Pile> m_piles;
	const uint32      m_slot_limit;
	bool              m_loaded = false;
};

} // namespace TradeskillDepot
