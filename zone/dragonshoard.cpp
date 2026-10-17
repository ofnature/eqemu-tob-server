#include "zone/dragonshoard.h"

#include "common/eqemu_logsys.h"
#include "common/misc_functions.h"
#include "zone/client.h"
#include "zone/string_ids.h"
#include "zone/zonedb.h"

#include <algorithm>
#include <iterator>
#include <list>

namespace DragonHoard {

namespace {

// The leading u32 of every OP_DragonHoard packet body. Values are defined by the client.
enum class Action : uint32 {
	WindowOpen   = 0,  // c->s: window opened
	SetSlotCount = 2,  // c->s: request, sent at zone-in; s->c: slot capacity
	Retrieve     = 3,  // c->s: request; s->c: remove the row and release the pending-op counter
	Deposit      = 4,  // c->s: deposit the cursor item
	Enable       = 8,  // s->c: enable the window
	Ack          = 10, // s->c: release the pending-op counter, nothing else
};

#pragma pack(push, 1)

struct SlotCount_Struct {
	Action action;
	uint32 slot_count;
};

struct Enable_Struct {
	Action action;
	uint8  flag;
};

struct Ack_Struct {
	Action action;
};

struct DepositRequest_Struct {
	Action action;
	uint64 serial_number;
};

struct Retrieve_Struct {
	Action action;
	uint64 serial_number;
	uint32 quantity; // always 1 from the client; the serial number identifies the row
};

#pragma pack(pop)

template <typename T>
void Send(Client* client, const T& body)
{
	auto* outapp = new EQApplicationPacket(
		OP_DragonHoard, reinterpret_cast<const unsigned char*>(&body), sizeof(T));

	client->FastQueuePacket(&outapp);
}

// The client counts each deposit and retrieve request as a pending operation and locks its
// inventory until the count is released, so every handled request ends with this, as on live.
void SendAck(Client* client)
{
	Send(client, Ack_Struct{Action::Ack});
}

bool IsEnabled(Client* client)
{
	return client->ClientVersion() >= EQ::versions::ClientVersion::TOB &&
		client->GetHoardCache().SlotLimit() > 0;
}

void SendSlotCount(Client* client)
{
	Send(client, SlotCount_Struct{Action::SetSlotCount, client->GetHoardCache().SlotLimit()});
}

void SendItemRow(Client* client, const EQ::ItemInstance* inst)
{
	// The client keys hoard rows by item serial number. The slot only has to pass the inventory
	// checks in SendItemPacket.
	client->SendItemPacket(EQ::invslot::slotCursor, inst, ItemPacketType::ItemPacketDragonHoard);
}

// Every row, then Enable. Live sends this on each window open and after each deposit; the client
// skips rows it already lists.
void SendItemList(Client* client)
{
	client->GetHoardCache().ForEach(database, client->CharacterID(), [client](const EQ::ItemInstance* inst) {
		SendItemRow(client, inst);
	});

	Send(client, Enable_Struct{Action::Enable, 0});
}

void HandleDeposit(Client* client, const EQApplicationPacket* app)
{
	VERIFY_PACKET_LENGTH(OP_DragonHoard, app, DepositRequest_Struct);

	const auto* request = reinterpret_cast<const DepositRequest_Struct*>(app->pBuffer);

	EQ::ItemInstance* cursor_item = client->GetInv().GetItem(EQ::invslot::slotCursor);
	if (!cursor_item || cursor_item->GetSerialNumber() != static_cast<int32>(request->serial_number)) {
		SendAck(client);
		return;
	}

	// The client refuses these before sending. They are enforced here as well: only the item row is
	// stored, which would lose augments and container contents, and a No Rent item must not outlive
	// its timer. Lore items are allowed, as on live; retrieve refuses a duplicate.
	const bool is_no_rent = cursor_item->GetItem()->NoRent == 0;
	if (cursor_item->IsAugmented() || cursor_item->IsNoneEmptyContainer() || is_no_rent) {
		client->Message(Chat::Red, "That item cannot be stored in your Dragon's Hoard.");
		SendAck(client);
		return;
	}

	HoardCache& cache = client->GetHoardCache();
	if (cache.Count(database, client->CharacterID()) >= cache.SlotLimit()) {
		client->Message(Chat::Red, "Your Dragon's Hoard is full.");
		SendAck(client);
		return;
	}

	// Both writes run and are checked before anything changes in memory, so a failure rolls back
	// with the item still on the cursor: the cursor saved without its first item, then the row.
	database.TransactionBegin();

	auto       remaining_begin = std::next(client->GetInv().cursor_cbegin());
	auto       remaining_end   = client->GetInv().cursor_cend();
	const bool cursor_saved    = database.SaveCursor(client->CharacterID(), remaining_begin, remaining_end);

	const EQ::ItemInstance* hoard_item = !cursor_saved ? nullptr : cache.Deposit(
		database, client->CharacterID(), std::unique_ptr<EQ::ItemInstance>(cursor_item->Clone()));

	if (!hoard_item) {
		database.TransactionRollback();
		client->Message(Chat::Red, "Your Dragon's Hoard could not store that item.");
		SendAck(client);
		return;
	}

	client->DeleteItemInInventory(EQ::invslot::slotCursor, 0, true, false);

	// DeleteItemInInventory soft-deletes an evolving item's record. The item still exists, so the
	// record stays active, as it does for an item in the inventory.
	hoard_item->TransferOwnership(database, client->CharacterID());

	if (!database.TransactionCommit().Success()) {
		LogError("Dragon's Hoard deposit of character [{}] was not committed", client->CharacterID());
	}

	// Same order as live: the new row, the Ack, then the full list.
	SendItemRow(client, hoard_item);
	SendAck(client);
	SendItemList(client);
}

void HandleRetrieve(Client* client, const EQApplicationPacket* app)
{
	VERIFY_PACKET_LENGTH(OP_DragonHoard, app, Retrieve_Struct);

	const auto* request       = reinterpret_cast<const Retrieve_Struct*>(app->pBuffer);
	const auto  serial_number = static_cast<int32>(request->serial_number);

	HoardCache&       cache  = client->GetHoardCache();
	EQ::ItemInstance* stored = cache.Find(database, client->CharacterID(), serial_number);
	if (!stored) {
		SendAck(client);
		return;
	}

	if (client->GetInv().GetItem(EQ::invslot::slotCursor)) {
		client->Message(Chat::Red, "You must have an empty cursor to retrieve an item.");
		SendAck(client);
		return;
	}

	if (client->CheckLoreConflict(stored->GetItem())) {
		client->MessageString(Chat::Red, DUP_LORE);
		SendAck(client);
		return;
	}

	// Both writes run and are checked before anything changes in memory, so a failure rolls back
	// with the item still in the hoard: the (empty) cursor saved holding the item, then the row
	// delete. PushItemOnCursor then saves the same cursor again.
	database.TransactionBegin();

	const std::list<EQ::ItemInstance*> cursor_after{stored};
	auto                               cursor_begin = cursor_after.cbegin();
	auto                               cursor_end   = cursor_after.cend();
	const bool cursor_saved = database.SaveCursor(client->CharacterID(), cursor_begin, cursor_end);

	const auto item = cursor_saved ? cache.Retrieve(database, client->CharacterID(), serial_number) : nullptr;
	if (!item) {
		database.TransactionRollback();
		client->Message(Chat::Red, "Your Dragon's Hoard could not release that item.");
		SendAck(client);
		return;
	}

	client->PushItemOnCursor(*item, true);

	if (!database.TransactionCommit().Success()) {
		LogError("Dragon's Hoard retrieve of character [{}] was not committed", client->CharacterID());
	}

	// The client removes the row only when the quantity covers the count it lists.
	const auto quantity = static_cast<uint32>(std::max<int16>(item->GetCharges(), 1));

	Send(client, Retrieve_Struct{Action::Retrieve, request->serial_number, quantity});
	SendAck(client);
}

} // namespace

void HandlePacket(Client* client, const EQApplicationPacket* app)
{
	if (!IsEnabled(client) || app->size < sizeof(Action)) {
		return;
	}

	// A request is handled only at its exact size; anything else is dropped. Window open and the
	// slot-count request are the action alone.
	switch (static_cast<Action>(app->ReadUInt32(0))) {
		case Action::SetSlotCount:
			if (app->size == sizeof(Action)) {
				SendSlotCount(client);
			}
			break;
		case Action::WindowOpen:
			if (app->size == sizeof(Action)) {
				SendItemList(client);
			}
			break;
		case Action::Deposit:
			HandleDeposit(client, app);
			break;
		case Action::Retrieve:
			HandleRetrieve(client, app);
			break;
		default:
			break;
	}
}

} // namespace DragonHoard
