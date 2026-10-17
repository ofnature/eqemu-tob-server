#include "tradeskilldepot.h"
#include "client.h"
#include "../common/global_define.h"
#include "../common/item_instance.h"
#include "../common/rulesys.h"
#include "../common/strings.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

// Personal Tradeskill Depot handler (The Outer Brood client).
// Opcode: OP_TradeskillDepot=0x2D36. See tradeskilldepot.h for the full wire format
// (59-byte stream header + embedded {serial, qty} removal map + per-action payload)
// recovered from client sub_140216ED0 / sub_1401E06A0 / sub_140203B10.
// Claim ids (OP_PlayerProfile): 2018125 (0x1ECB4D) / 2018260 (0x1ECBD4). Client window: qword_140DB52F0.
// Client slot count at player+0x2470, enabled flag at player+0x2474 (DH uses +0x246C / +0x2468).

namespace {

constexpr uint32 kHeaderSize      = 59;
constexpr uint32 kRouteByteOffset = 13; // 0 = depot window, 1 = tradeskill window; echoed
constexpr uint32 kMapCountOffset  = 55; // u32 count of {u32 serial, u32 qty} entries at 59

constexpr uint32 kActionOpenState = 0;
constexpr uint32 kActionListSync  = 3;
constexpr uint32 kActionCapacity  = 4;
constexpr uint32 kActionDeposit   = 5;
constexpr uint32 kActionWithdraw  = 6;
constexpr uint32 kActionEnable    = 8;

using RemovalMap = std::vector<std::pair<uint32, uint32>>; // {row serial, qty removed}

// Build an OP_TradeskillDepot packet: 59-byte header (+ removal map entries), then
// extra_payload_size zeroed bytes after the map for the caller to fill in.
EQApplicationPacket* MakeDepotPacket(uint32 action, uint8 route_byte, uint32 extra_payload_size = 0,
	const RemovalMap* removals = nullptr)
{
	const uint32 map_entries = removals ? static_cast<uint32>(removals->size()) : 0;
	const uint32 size = kHeaderSize + map_entries * 8 + extra_payload_size;
	auto* outapp = new EQApplicationPacket(OP_TradeskillDepot, size);
	memset(outapp->pBuffer, 0, size);
	*reinterpret_cast<uint32*>(outapp->pBuffer) = action;
	outapp->pBuffer[kRouteByteOffset] = route_byte;
	*reinterpret_cast<uint32*>(outapp->pBuffer + kMapCountOffset) = map_entries;
	uint32 off = kHeaderSize;
	for (uint32 i = 0; i < map_entries; ++i) {
		*reinterpret_cast<uint32*>(outapp->pBuffer + off)     = (*removals)[i].first;
		*reinterpret_cast<uint32*>(outapp->pBuffer + off + 4) = (*removals)[i].second;
		off += 8;
	}
	return outapp;
}

// Bare action packet; used to acknowledge deposits/withdrawals. The client increments a
// pending-op counter on every deposit/withdraw send and freezes depot input until an
// action 5/6/12 response decrements it, so EVERY such request must be answered.
void SendAck(Client* client, uint32 action, uint8 route_byte)
{
	auto* outapp = MakeDepotPacket(action, route_byte);
	client->FastQueuePacket(&outapp);
}

// Depot rows on the wire: the client shows StackCount (u32) which the TOB serializer
// reads from MerchantCount for ItemPacketTradeskillDepot (depot piles exceed both int16
// charges and the item's StackSize). The client keys/identifies rows by item->ID (struct
// +56), which the item body always carries, and that is what it echoes on withdraw. The
// stamped serial (slot_id + 1, never 0) only becomes the row's +24 GUID, used by the
// receiver to match an incoming packet to an existing row; it plays no part in withdraw.
void SendDepotItem(Client* client, uint32 slot_id, const EQ::ItemData* item_data, uint32 quantity)
{
	const int16 charges = static_cast<int16>(std::min<uint32>(quantity,
		item_data->StackSize > 1 ? static_cast<uint32>(item_data->StackSize) : 1U));
	EQ::ItemInstance* inst = database.CreateItem(item_data, charges);
	if (!inst) {
		return;
	}
	inst->SetSerialNumber(static_cast<int32>(slot_id + 1));
	inst->SetMerchantCount(static_cast<int32>(quantity));
	client->SendItemPacket(static_cast<int16>(slot_id), inst, ItemPacketType::ItemPacketTradeskillDepot);
	safe_delete(inst);
}

// Server-side mirror of the client's eligibility gate (sub_1401CE7A0): stackable
// tradeskill mats only. Returns nullptr when depositable, else a short reason.
const char* DepositIneligibleReason(const EQ::ItemInstance* inst)
{
	const EQ::ItemData* item = inst->GetItem();
	if (!item) {
		return "unknown items";
	}
	if (item->ItemClass != EQ::item::ItemClassCommon) {
		return "containers";
	}
	if (!item->Tradeskills) {
		return "non-tradeskill items";
	}
	if (item->StackSize <= 1) {
		return "non-stackable items";
	}
	if (item->NoRent == 0) {
		return "temporary items";
	}
	if (item->Attuneable || inst->IsAttuned()) {
		return "attunable items";
	}
	if (item->NoDrop == 0) {
		return "no-trade items";
	}
	if (item->NoTransfer) {
		return "heirloom items";
	}
	return nullptr;
}

// The deposit request identifies inventory items by the serial the server stamped on
// their item packets. Scan cursor, general slots and bag slots for a match.
int16 FindInventorySlotBySerial(Client* client, uint32 serial)
{
	const auto& inv = client->GetInv();
	const EQ::ItemInstance* inst = inv.GetItem(EQ::invslot::slotCursor);
	if (inst && static_cast<uint32>(inst->GetSerialNumber()) == serial) {
		return EQ::invslot::slotCursor;
	}
	for (int16 i = EQ::invslot::GENERAL_BEGIN; i <= EQ::invslot::GENERAL_END; ++i) {
		inst = inv.GetItem(i);
		if (inst && static_cast<uint32>(inst->GetSerialNumber()) == serial) {
			return i;
		}
	}
	for (int16 i = EQ::invbag::GENERAL_BAGS_BEGIN; i <= EQ::invbag::GENERAL_BAGS_END; ++i) {
		inst = inv.GetItem(i);
		if (inst && static_cast<uint32>(inst->GetSerialNumber()) == serial) {
			return i;
		}
	}
	return EQ::invslot::SLOT_INVALID;
}

} // namespace

bool TradeskillDepot::IsEnabled(Client* client)
{
	return client
		&& RuleB(Features, TradeskillDepotEnabled)
		&& client->ClientVersion() >= EQ::versions::ClientVersion::TOB;
}

// action 8 sets the client's enabled flag (player+0x2474; header fail byte at offset 46
// must be 0), action 4 sets the max slot count (u32 after the empty map, i.e. offset 59,
// written to player+0x2470).
void TradeskillDepot::SendUnlock(Client* client)
{
	if (!IsEnabled(client)) {
		return;
	}

	auto* enable = MakeDepotPacket(kActionEnable, 0);

	auto* capacity = MakeDepotPacket(kActionCapacity, 0, 4);
	*reinterpret_cast<uint32*>(capacity->pBuffer + kHeaderSize) = static_cast<uint32>(MAX_SLOTS);

	client->FastQueuePacket(&enable);
	client->FastQueuePacket(&capacity);

	LogDebug("TradeskillDepot::SendUnlock sent to {}", client->GetName());
}

void TradeskillDepot::SendItemList(Client* client)
{
	if (!IsEnabled(client)) {
		return;
	}

	const uint32 account_id = client->AccountID();
	if (!account_id) {
		return;
	}

	auto results = database.QueryDatabase(
		fmt::format(
			"SELECT slot_id, item_id, quantity FROM tradeskill_depot_items "
			"WHERE account_id = {} ORDER BY slot_id",
			account_id
		)
	);

	if (!results.Success()) {
		LogError(
			"TradeskillDepot::SendItemList failed for account_id {}: {}",
			account_id,
			results.ErrorMessage()
		);
		return;
	}

	for (auto row = results.begin(); row != results.end(); ++row) {
		const uint32 slot_id  = Strings::ToUnsignedInt(row[0]);
		const uint32 item_id  = Strings::ToUnsignedInt(row[1]);
		const uint32 quantity = Strings::ToUnsignedInt(row[2]);

		const EQ::ItemData* item_data = database.GetItem(item_id);
		if (!item_data) {
			LogError(
				"TradeskillDepot::SendItemList item_id {} not found for account_id {}",
				item_id,
				account_id
			);
			continue;
		}

		SendDepotItem(client, slot_id, item_data, quantity);
	}

	LogDebug("TradeskillDepot::SendItemList sent items to account_id {}", account_id);
}

void TradeskillDepot::HandlePacket(Client* client, const EQApplicationPacket* app)
{
	if (!app || app->size < 4 || !IsEnabled(client)) {
		return;
	}

	const uint32 action = *reinterpret_cast<uint32*>(app->pBuffer);
	const uint8 route_byte = app->size > kRouteByteOffset ? app->pBuffer[kRouteByteOffset] : 0;

	LogDebug("TradeskillDepot: action={} route={} size={} from {}", action, route_byte, app->size, client->GetName());

	switch (action) {
		case kActionOpenState:
			// Depot-window state query (sent while the client thinks the depot is disabled).
			// Answer with enable + capacity and the stored items, as live does on open. For the
			// tradeskill-window variant (route=1) just log — its reply semantics are unmapped and
			// an action-0 reply with a nonzero payload byte makes the client re-request in a loop.
			if (route_byte == 0) {
				SendUnlock(client);
				SendItemList(client);
			}
			break;
		case kActionListSync:
			// Window refresh request (the depot window clears its list first).
			SendItemList(client);
			break;
		case kActionCapacity: {
			auto* capacity = MakeDepotPacket(kActionCapacity, route_byte, 4);
			*reinterpret_cast<uint32*>(capacity->pBuffer + kHeaderSize) = static_cast<uint32>(MAX_SLOTS);
			client->FastQueuePacket(&capacity);
			break;
		}
		case kActionDeposit:
			HandleDeposit(client, app);
			break;
		case kActionWithdraw:
			HandleRetrieve(client, app);
			break;
		case kActionEnable:
			SendUnlock(client);
			break;
		default:
			LogDebug("TradeskillDepot: unhandled action {} size {} from {}", action, app->size, client->GetName());
			break;
	}
}

// Deposit request (client sub_1401CE920): header + [u8 flag=1][u32 count][u64 item serial
// x count]. Each serial is an inventory stack the client already eligibility-checked and
// capacity-checked; the whole stack is deposited (there is no partial-deposit UI). Piles
// of the same item merge into one depot row.
void TradeskillDepot::HandleDeposit(Client* client, const EQApplicationPacket* app)
{
	if (!app || !IsEnabled(client)) {
		return;
	}

	const uint8 route_byte = app->size > kRouteByteOffset ? app->pBuffer[kRouteByteOffset] : 0;

	if (app->size < kHeaderSize + 5) {
		LogError("TradeskillDepot::HandleDeposit packet too small: {}", app->size);
		SendAck(client, kActionDeposit, route_byte);
		return;
	}

	const uint32 count = *reinterpret_cast<uint32*>(app->pBuffer + kHeaderSize + 1);
	if (count == 0 || app->size < kHeaderSize + 5 + count * 8) {
		LogError("TradeskillDepot::HandleDeposit malformed packet: size={} count={}", app->size, count);
		SendAck(client, kActionDeposit, route_byte);
		return;
	}

	const uint32 account_id = client->AccountID();

	for (uint32 i = 0; i < count; ++i) {
		uint64 serial = 0;
		memcpy(&serial, app->pBuffer + kHeaderSize + 5 + i * 8, sizeof(uint64));

		const int16 inv_slot = FindInventorySlotBySerial(client, static_cast<uint32>(serial));
		if (inv_slot == EQ::invslot::SLOT_INVALID) {
			LogError("TradeskillDepot::HandleDeposit serial {} not found in inventory of {}", serial, client->GetName());
			continue;
		}

		const EQ::ItemInstance* inst = client->GetInv().GetItem(inv_slot);
		const EQ::ItemData* item_data = inst->GetItem();

		if (const char* reason = DepositIneligibleReason(inst)) {
			client->Message(Chat::Red, "The Personal Tradeskill Depot cannot hold %s.", reason);
			continue;
		}

		const uint32 item_id = item_data->ID;
		const uint32 deposit_qty = inst->GetCharges() > 0 ? static_cast<uint32>(inst->GetCharges()) : 1U;

		// Merge into an existing row of the same item, else claim the first free slot.
		auto existing = database.QueryDatabase(
			fmt::format(
				"SELECT slot_id, quantity FROM tradeskill_depot_items "
				"WHERE account_id = {} AND item_id = {}",
				account_id, item_id
			)
		);

		if (existing.Success() && existing.RowCount() > 0) {
			auto row = existing.begin();
			const uint32 slot_id = Strings::ToUnsignedInt(row[0]);
			const uint32 old_qty = Strings::ToUnsignedInt(row[1]);
			const uint32 new_qty = old_qty + deposit_qty;

			auto update = database.QueryDatabase(
				fmt::format(
					"UPDATE tradeskill_depot_items SET quantity = {} "
					"WHERE account_id = {} AND slot_id = {}",
					new_qty, account_id, slot_id
				)
			);
			if (!update.Success()) {
				LogError("TradeskillDepot::HandleDeposit DB update failed for account_id {}: {}", account_id, update.ErrorMessage());
				continue;
			}

			client->DeleteItemInInventory(inv_slot, 0, true);

			// Replace the window row: remove it (action 3, removal qty >= shown qty),
			// then re-add with the merged total. The depot keys rows by item_id (client
			// item struct +56), so the removal map must use item_id, not the slot serial.
			RemovalMap removals{{item_id, old_qty}};
			auto* remove_row = MakeDepotPacket(kActionListSync, route_byte, 0, &removals);
			client->FastQueuePacket(&remove_row);
			SendDepotItem(client, slot_id, item_data, new_qty);

			LogDebug("TradeskillDepot::HandleDeposit account_id {} merged {} x item_id {} into slot {} (now {})",
				account_id, deposit_qty, item_id, slot_id, new_qty);
			continue;
		}

		// First free slot (rows are deleted on full withdraw, so reuse gaps).
		uint32 slot_id = 0;
		auto slots = database.QueryDatabase(
			fmt::format(
				"SELECT slot_id FROM tradeskill_depot_items WHERE account_id = {} ORDER BY slot_id",
				account_id
			)
		);
		if (slots.Success()) {
			for (auto row = slots.begin(); row != slots.end(); ++row) {
				if (Strings::ToUnsignedInt(row[0]) != slot_id) {
					break;
				}
				++slot_id;
			}
		}

		if (slot_id >= static_cast<uint32>(MAX_SLOTS)) {
			client->Message(Chat::Red, "Your Personal Tradeskill Depot is full.");
			continue;
		}

		auto insert = database.QueryDatabase(
			fmt::format(
				"INSERT INTO tradeskill_depot_items (account_id, slot_id, item_id, item_name, quantity) "
				"VALUES ({}, {}, {}, '{}', {})",
				account_id,
				slot_id,
				item_id,
				Strings::Escape(std::string(item_data->Name)),
				deposit_qty
			)
		);
		if (!insert.Success()) {
			LogError("TradeskillDepot::HandleDeposit DB insert failed for account_id {}: {}", account_id, insert.ErrorMessage());
			continue;
		}

		client->DeleteItemInInventory(inv_slot, 0, true);
		SendDepotItem(client, slot_id, item_data, deposit_qty);

		LogDebug("TradeskillDepot::HandleDeposit account_id {} deposited {} x item_id {} into slot {}",
			account_id, deposit_qty, item_id, slot_id);
	}

	// Clear the client's pending-op counter (frozen depot UI otherwise).
	SendAck(client, kActionDeposit, route_byte);
}

// Withdraw request (client sub_1401D08A0): header + [u32 item_id][u32 qty]. The depot keys
// rows by item_id (client item struct +56 = item->ID), NOT by the slot serial we stamp, so
// the first dword is the item id of the selected row. qty comes from the quantity dialog /
// ctrl-click (1) / shift-click (one inventory stack); the client pre-clamps to
// min(row qty, StackSize) but we clamp again server-side. The action 6 response carries the
// {item_id, removed qty} map (client removes the row when removed >= shown, else subtracts)
// and decrements the pending-op counter.
void TradeskillDepot::HandleRetrieve(Client* client, const EQApplicationPacket* app)
{
	if (!app || !IsEnabled(client)) {
		return;
	}

	const uint8 route_byte = app->size > kRouteByteOffset ? app->pBuffer[kRouteByteOffset] : 0;

	if (app->size < kHeaderSize + 8) {
		LogError("TradeskillDepot::HandleRetrieve packet too small: {}", app->size);
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	const uint32 item_id       = *reinterpret_cast<uint32*>(app->pBuffer + kHeaderSize);
	const uint32 requested_qty = *reinterpret_cast<uint32*>(app->pBuffer + kHeaderSize + 4);

	if (item_id == 0 || requested_qty == 0) {
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	const uint32 account_id = client->AccountID();

	auto result = database.QueryDatabase(
		fmt::format(
			"SELECT slot_id, quantity FROM tradeskill_depot_items "
			"WHERE account_id = {} AND item_id = {}",
			account_id, item_id
		)
	);

	if (!result.Success() || result.RowCount() == 0) {
		LogError("TradeskillDepot::HandleRetrieve item_id {} not found for account_id {}", item_id, account_id);
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	auto row = result.begin();
	const uint32 slot_id   = Strings::ToUnsignedInt(row[0]);
	const uint32 depot_qty = Strings::ToUnsignedInt(row[1]);

	const EQ::ItemData* item_data = database.GetItem(item_id);
	if (!item_data) {
		LogError("TradeskillDepot::HandleRetrieve item_id {} not found", item_id);
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	if (client->GetInv().GetItem(EQ::invslot::slotCursor)) {
		client->Message(Chat::Red, "You must have an empty cursor to withdraw an item.");
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	// One withdraw yields at most one player-holdable stack.
	const uint32 max_stack = item_data->StackSize > 1 ? static_cast<uint32>(item_data->StackSize) : 1U;
	const uint32 withdraw_qty = std::min({requested_qty, depot_qty, max_stack});

	const uint32 remaining = depot_qty - withdraw_qty;
	auto update = database.QueryDatabase(
		remaining == 0
			? fmt::format(
				"DELETE FROM tradeskill_depot_items WHERE account_id = {} AND slot_id = {}",
				account_id, slot_id)
			: fmt::format(
				"UPDATE tradeskill_depot_items SET quantity = {} WHERE account_id = {} AND slot_id = {}",
				remaining, account_id, slot_id)
	);
	if (!update.Success()) {
		LogError("TradeskillDepot::HandleRetrieve DB update failed: {}", update.ErrorMessage());
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	EQ::ItemInstance* inst = database.CreateItem(item_data, static_cast<int16>(withdraw_qty));
	if (inst) {
		client->PushItemOnCursor(*inst, true);
		safe_delete(inst);
	}

	// action 6 response: removal map + pending-op ack in one packet. The client subtracts
	// withdraw_qty from the row (keyed by item_id), removing it when the pile is exhausted.
	RemovalMap removals{{item_id, withdraw_qty}};
	auto* confirm = MakeDepotPacket(kActionWithdraw, route_byte, 0, &removals);
	client->FastQueuePacket(&confirm);

	LogDebug("TradeskillDepot::HandleRetrieve account_id {} withdrew {} x item_id {} from slot {} ({} left)",
		account_id, withdraw_qty, item_id, slot_id, remaining);
}
