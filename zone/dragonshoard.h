#pragma once

#include "../common/types.h"

class Client;
class EQApplicationPacket;

// Dragon's Hoard feature handler
// Universal implementation - patch-agnostic logic
// Serialization is handled by the patch-specific serializer (tob.cpp etc.)
// DB table: dragonhoard_items (character_id, slot_id, item_id)

namespace DragonHoard {

	// Called on zone-in to populate the DH window with the character's stored items
	void SendItemList(Client* client);

	// Called when client deposits an item into Dragon's Hoard
	void HandleDeposit(Client* client, const EQApplicationPacket* app);

	// Called when client retrieves an item from Dragon's Hoard
	void HandleRetrieve(Client* client, const EQApplicationPacket* app);

	// Send a single item update to the DH window (add or remove)
	void SendItemUpdate(Client* client, uint32 slot_id, uint32 item_id, bool remove);

	// Max slots available in Dragon's Hoard
	static constexpr int MAX_SLOTS = 200;

} // namespace DragonHoard
