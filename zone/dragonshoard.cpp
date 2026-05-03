#include "dragonshoard.h"
#include "client.h"
#include "../common/global_define.h"
#include "../common/item_instance.h"
#include "../common/rulesys.h"
#include "../common/strings.h"
#if __has_include("../common/repositories/dragonhoard_items_repository.h")
#include "../common/repositories/dragonhoard_items_repository.h"
#else
// TODO: create repository.
#endif

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

	// [DH_SEND_ITEM_LIST]
	const uint32 account_id = client->AccountID();
	if (!account_id) {
		return;
	}

	auto results = database.QueryDatabase(
		fmt::format(
			"SELECT slot_id, item_id, stack_count FROM dragonhoard_items "
			"WHERE account_id = {} ORDER BY slot_id",
			account_id
		)
	);

	if (!results.Success()) {
		LogError(
			"DragonHoard::SendItemList failed for account_id {}: {}",
			account_id,
			results.ErrorMessage()
		);
		return;
	}

	for (auto row = results.begin(); row != results.end(); ++row) {
		const uint32 slot_id = Strings::ToUnsignedInt(row[0]);
		const uint32 item_id = Strings::ToUnsignedInt(row[1]);
		const uint32 stack_count = Strings::ToUnsignedInt(row[2]);

		const EQ::ItemData* item_data = database.GetItem(item_id);
		if (!item_data) {
			LogError(
				"DragonHoard::SendItemList item_id {} not found for account_id {}",
				item_id,
				account_id
			);
			continue;
		}

		EQ::ItemInstance* inst = database.CreateItem(item_data, stack_count);
		if (!inst) {
			continue;
		}

		client->SendItemPacket(slot_id, inst, ItemPacketType::ItemPacketDragonHoard);
		safe_delete(inst);
	}

	LogDebug("DragonHoard::SendItemList sent items to account_id {}", account_id);
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
