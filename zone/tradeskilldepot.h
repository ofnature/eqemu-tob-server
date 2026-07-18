#pragma once

#include "../common/types.h"

class Client;
class EQApplicationPacket;

// Personal Tradeskill Depot (The Outer Brood). Sibling of Dragon's Hoard — same
// FeatureUnlock plumbing, its own opcode (OP_TradeskillDepot = 0x2D36), feature ids
// (2018125 / 2018260), and DB table (tradeskill_depot_items).
//
// Differs from Dragon's Hoard: the depot holds STACKABLE tradeskill mats, and its
// OP_TradeskillDepot packets are stream-serialized (client handler sub_140216ED0 reads
// them through a deserializer), so the deposit/retrieve/enable packet layouts still
// need reverse-engineering before those handlers can be filled in.

namespace TradeskillDepot {

	// True only when the feature rule is on AND the client is TOB or later.
	bool IsEnabled(Client* client);

	// OP_FeatureUnlock (0x5B9B): add the depot feature ids to the client feature array.
	// This is the gate the deposit path checks; sent at zone-in. (Fully implemented.)
	void SendFeatureUnlock(Client* client);

	// Enable flag + slot count (OP_TradeskillDepot action codes). TODO: stream layout.
	void SendUnlock(Client* client);

	// Zone-in: populate the depot window with the account's stored items. TODO.
	void SendItemList(Client* client);

	// Client deposits an item into the depot. TODO: parse stream packet.
	void HandleDeposit(Client* client, const EQApplicationPacket* app);

	// Client withdraws an item (honoring quantity for stacks). TODO: parse stream packet.
	void HandleRetrieve(Client* client, const EQApplicationPacket* app);

	// Max depot slots (retail is purchasable; placeholder until confirmed).
	static constexpr int MAX_SLOTS = 500;

} // namespace TradeskillDepot
