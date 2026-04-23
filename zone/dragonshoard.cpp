#include "dragonshoard.h"
#include "client.h"
#include "../common/rulesys.h"
// #include "../common/repositories/dragonhoard_items_repository.h"
// TODO: create repository

// Dragon's Hoard feature handler
// Universal implementation - patch-agnostic logic
// Opcodes (TOB): OP_DragonHoard1=0x5807 (window/list), OP_DragonHoard2=0x603D (deposit/retrieve)
// Item packet type: ItemPacketDragonHoard=0x77
// Feature unlock slot: entry[29] DragonHoardSlots=200

void DragonHoard::SendItemList(Client* client)
{
	if (!client) {
		return;
	}

	// TODO: query dragonhoard_items for this character and send each item
	// via client->SendItemPacket(slot_id, inst, ItemPacketDragonHoard)
}

void DragonHoard::HandleDeposit(Client* client, const EQApplicationPacket* app)
{
	if (!client || !app) {
		return;
	}

	(void)app;

	// TODO: read item from cursor, insert into dragonhoard_items, send update
}

void DragonHoard::HandleRetrieve(Client* client, const EQApplicationPacket* app)
{
	if (!client || !app) {
		return;
	}

	(void)app;

	// TODO: read slot from packet, remove from dragonhoard_items, place on cursor
}

void DragonHoard::SendItemUpdate(Client* client, uint32 slot_id, uint32 item_id, bool remove)
{
	if (!client) {
		return;
	}

	(void)slot_id;
	(void)item_id;
	(void)remove;

	// TODO: send single item add/remove delta update to DH window
}
