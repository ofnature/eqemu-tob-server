#include "zone/tradeskilldepot.h"

#include "common/eq_constants.h"
#include "common/item_instance.h"
#include "common/rulesys.h"
#include "zone/client.h"
#include "zone/zonedb.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <list>
#include <memory>
#include <utility>
#include <vector>

// Personal Tradeskill Depot handler (The Outer Brood client).
// Opcode: OP_TradeskillDepot=0x2D36. See tradeskilldepot.h for the full wire format
// (59-byte stream header + embedded {serial, qty} removal map + per-action payload)
// recovered from client sub_140216ED0 / sub_1401E06A0 / sub_140203B10.
// Claim ids (OP_PlayerProfile): 2018125 (0x1ECB4D) / 2018260 (0x1ECBD4). Client window: qword_140DB52F0.
// Client slot count at player+0x2470, enabled flag at player+0x2474 (DH uses +0x246C / +0x2468).
// The piles live in the account's DepotCache (Client::GetDepotCache): reads come from memory,
// every change is written first and checked, and item moves run in one transaction.

namespace {

constexpr uint32 kHeaderSize      = 59;
constexpr uint32 kRouteByteOffset = 13; // 0 = depot window, 1 = tradeskill window; echoed
constexpr uint32 kMapCountOffset  = 55; // u32 count of {u32 serial, u32 qty} entries at 59

// Request payloads after the header (client requests carry no removal map).
constexpr uint32 kDepositFixedSize = sizeof(uint8) + sizeof(uint32); // [u8 flag][u32 count], then u64 serials
constexpr uint32 kWithdrawSize     = sizeof(uint32) + sizeof(uint32); // [u32 item_id][u32 qty]

constexpr uint32 kActionOpenState = 0;
constexpr uint32 kActionListSync  = 3;
constexpr uint32 kActionCapacity  = 4;
constexpr uint32 kActionDeposit   = 5;
constexpr uint32 kActionWithdraw  = 6;
constexpr uint32 kActionEnable    = 8;

using RemovalMap = std::vector<std::pair<uint32, uint32>>; // {item_id, qty removed}

template <typename T>
T ReadAt(const EQApplicationPacket* app, uint32 offset)
{
	T value;
	memcpy(&value, app->pBuffer + offset, sizeof(T));
	return value;
}

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

// Bare action packet; used to acknowledge deposits/withdrawals (see the pending-op counter
// note in tradeskilldepot.h).
void SendAck(Client* client, uint32 action, uint8 route_byte)
{
	auto* outapp = MakeDepotPacket(action, route_byte);
	client->FastQueuePacket(&outapp);
}

void SendCapacity(Client* client, uint8 route_byte)
{
	auto* capacity = MakeDepotPacket(kActionCapacity, route_byte, sizeof(uint32));
	*reinterpret_cast<uint32*>(capacity->pBuffer + kHeaderSize) = client->GetDepotCache().SlotLimit();
	client->FastQueuePacket(&capacity);
}

// action 8 sets the client's enabled flag (player+0x2474; header fail byte at offset 46
// must be 0), action 4 sets the max slot count (u32 after the empty map, i.e. offset 59,
// written to player+0x2470).
void SendUnlock(Client* client)
{
	auto* enable = MakeDepotPacket(kActionEnable, 0);
	client->FastQueuePacket(&enable);
	SendCapacity(client, 0);
}

// Depot rows on the wire: the client shows StackCount (u32), which the TOB serializer reads
// from MerchantCount for ItemPacketTradeskillDepot (piles exceed both int16 charges and the
// item's StackSize). The client keys rows by item->ID; the serial number (the row id) only
// becomes the row's GUID, used to match an incoming packet to an existing row.
void SendDepotItem(Client* client, int16 slot, const EQ::ItemInstance* pile)
{
	client->SendItemPacket(slot, pile, ItemPacketType::ItemPacketTradeskillDepot);
}

void SendItemList(Client* client)
{
	client->GetDepotCache().ForEach(database, client->AccountID(), [client](int16 slot, const EQ::ItemInstance* pile) {
		SendDepotItem(client, slot, pile);
	});
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

// Moves one inventory stack into its depot pile. Both writes run and are checked before
// anything changes in memory: the inventory slot's removal (the cursor's after-state for the
// cursor), then the pile row. On failure the transaction rolls back with inventory and depot
// untouched.
void DepositStack(Client* client, uint8 route_byte, uint32 serial)
{
	const int16 inv_slot = FindInventorySlotBySerial(client, serial);
	if (inv_slot == EQ::invslot::SLOT_INVALID) {
		return;
	}

	EQ::ItemInstance* inst = client->GetInv().GetItem(inv_slot);
	if (const char* reason = DepositIneligibleReason(inst)) {
		client->Message(Chat::Red, "The Personal Tradeskill Depot cannot hold %s.", reason);
		return;
	}

	const EQ::ItemData* item     = inst->GetItem();
	const uint32        quantity = inst->GetCharges() > 0 ? static_cast<uint32>(inst->GetCharges()) : 1U;

	auto&        cache      = client->GetDepotCache();
	const uint32 account_id = client->AccountID();
	const uint32 stored     = cache.Quantity(database, account_id, item->ID);

	if (!stored && cache.Count(database, account_id) >= cache.SlotLimit()) {
		client->Message(Chat::Red, "Your Personal Tradeskill Depot is full.");
		return;
	}

	if (stored + quantity > TradeskillDepot::MAX_PILE_QUANTITY) {
		client->Message(Chat::Red, "Your Personal Tradeskill Depot cannot hold any more of that item.");
		return;
	}

	database.TransactionBegin();

	bool inventory_saved;
	if (inv_slot == EQ::invslot::slotCursor) {
		auto remaining_begin = std::next(client->GetInv().cursor_cbegin());
		auto remaining_end   = client->GetInv().cursor_cend();
		inventory_saved = database.SaveCursor(client->CharacterID(), remaining_begin, remaining_end);
	} else {
		inventory_saved = database.SaveInventory(client->CharacterID(), nullptr, inv_slot);
	}

	const EQ::ItemInstance* pile = inventory_saved ? cache.Add(database, account_id, item, quantity) : nullptr;
	if (!pile) {
		database.TransactionRollback();
		client->Message(Chat::Red, "That item could not be stored in your Personal Tradeskill Depot.");
		return;
	}

	client->DeleteItemInInventory(inv_slot, 0, true, false);

	if (!database.TransactionCommit().Success()) {
		LogError("Tradeskill Depot deposit commit failed for character [{}] item [{}]", client->CharacterID(), item->ID);
	}

	// A pile that grew is replaced in the window: remove the row (action 3, removal qty >= shown
	// qty), then send it again with the new total.
	if (stored) {
		RemovalMap removals{{item->ID, stored}};
		auto* remove_row = MakeDepotPacket(kActionListSync, route_byte, 0, &removals);
		client->FastQueuePacket(&remove_row);
	}

	SendDepotItem(client, cache.SlotOf(database, account_id, item->ID), pile);
}

// Deposit request (client sub_1401CE920): header + [u8 flag=1][u32 count][u64 item serial
// x count]. Each serial is an inventory stack the client already eligibility-checked and
// capacity-checked; the whole stack is deposited (there is no partial-deposit UI).
void HandleDeposit(Client* client, const EQApplicationPacket* app, uint8 route_byte)
{
	if (app->size < kHeaderSize + kDepositFixedSize) {
		return;
	}

	const uint32 count = ReadAt<uint32>(app, kHeaderSize + sizeof(uint8));
	const uint64 expected_size = static_cast<uint64>(kHeaderSize) + kDepositFixedSize + static_cast<uint64>(count) * sizeof(uint64);
	if (count == 0 || app->size != expected_size) {
		return;
	}

	for (uint32 i = 0; i < count; ++i) {
		const auto serial = ReadAt<uint64>(app, kHeaderSize + kDepositFixedSize + i * sizeof(uint64));
		DepositStack(client, route_byte, static_cast<uint32>(serial));
	}

	SendAck(client, kActionDeposit, route_byte);
}

// Withdraw request (client sub_1401D08A0): header + [u32 item_id][u32 qty]. qty comes from
// the quantity dialog / ctrl-click (1) / shift-click (one inventory stack); the client
// pre-clamps to min(pile, StackSize) and so does the server. Both writes run and are checked
// before anything changes in memory: the cursor's after-state, then the pile row. The
// action 6 response carries the {item_id, removed qty} map (the client removes the row when
// removed >= shown, else subtracts) and decrements the pending-op counter.
void HandleWithdraw(Client* client, const EQApplicationPacket* app, uint8 route_byte)
{
	if (app->size != kHeaderSize + kWithdrawSize) {
		return;
	}

	const auto item_id       = ReadAt<uint32>(app, kHeaderSize);
	const auto requested_qty = ReadAt<uint32>(app, kHeaderSize + sizeof(uint32));

	auto&               cache      = client->GetDepotCache();
	const uint32        account_id = client->AccountID();
	const uint32        stored     = item_id ? cache.Quantity(database, account_id, item_id) : 0;
	const EQ::ItemData* item       = stored ? database.GetItem(item_id) : nullptr;

	if (!requested_qty || !item) {
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	if (client->GetInv().GetItem(EQ::invslot::slotCursor)) {
		client->Message(Chat::Red, "You must have an empty cursor to withdraw an item.");
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	const uint32 per_stack    = item->StackSize > 1 ? static_cast<uint32>(item->StackSize) : 1U;
	const uint32 withdraw_qty = std::min({requested_qty, stored, per_stack});

	const std::unique_ptr<EQ::ItemInstance> withdrawn(database.CreateItem(item, static_cast<int16>(withdraw_qty)));
	if (!withdrawn) {
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	database.TransactionBegin();

	const std::list<EQ::ItemInstance*> cursor_after{withdrawn.get()};
	auto cursor_begin = cursor_after.begin();
	auto cursor_end   = cursor_after.end();
	const bool cursor_saved = database.SaveCursor(client->CharacterID(), cursor_begin, cursor_end);

	if (!cursor_saved || !cache.Remove(database, account_id, item_id, withdraw_qty)) {
		database.TransactionRollback();
		client->Message(Chat::Red, "That item could not be withdrawn from your Personal Tradeskill Depot.");
		SendAck(client, kActionWithdraw, route_byte);
		return;
	}

	client->PushItemOnCursor(*withdrawn, true);

	if (!database.TransactionCommit().Success()) {
		LogError("Tradeskill Depot withdraw commit failed for character [{}] item [{}]", client->CharacterID(), item_id);
	}

	RemovalMap removals{{item_id, withdraw_qty}};
	auto* confirm = MakeDepotPacket(kActionWithdraw, route_byte, 0, &removals);
	client->FastQueuePacket(&confirm);
}

} // namespace

bool TradeskillDepot::IsEnabled(Client* client)
{
	return client
		&& RuleI(Features, TradeskillDepot) > 0
		&& client->ClientVersion() >= EQ::versions::ClientVersion::TOB;
}

void TradeskillDepot::HandlePacket(Client* client, const EQApplicationPacket* app)
{
	// Every request carries the full header, and client requests never carry a removal map.
	if (!IsEnabled(client) || app->size < kHeaderSize || ReadAt<uint32>(app, kMapCountOffset) != 0) {
		return;
	}

	const auto  action     = ReadAt<uint32>(app, 0);
	const uint8 route_byte = app->pBuffer[kRouteByteOffset];

	switch (action) {
		case kActionOpenState:
			// Depot-window state query (sent while the client thinks the depot is disabled).
			// Answer with enable + capacity and the stored items, as live does on open. For the
			// tradeskill-window variant (route=1) do nothing — its reply semantics are unmapped and
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
		case kActionCapacity:
			SendCapacity(client, route_byte);
			break;
		case kActionDeposit:
			HandleDeposit(client, app, route_byte);
			break;
		case kActionWithdraw:
			HandleWithdraw(client, app, route_byte);
			break;
		case kActionEnable:
			SendUnlock(client);
			break;
		default:
			break;
	}
}
