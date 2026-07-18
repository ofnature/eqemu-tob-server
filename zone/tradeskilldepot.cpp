#include "tradeskilldepot.h"
#include "client.h"
#include "../common/global_define.h"
#include "../common/item_instance.h"
#include "../common/rulesys.h"
#include "../common/strings.h"
#include <cstring>

// Personal Tradeskill Depot handler (The Outer Brood client).
// Opcode: OP_TradeskillDepot=0x2D36 (window/unlock/action codes, stream-serialized).
// FeatureUnlock ids: 2018125 (0x1ECB4D) / 2018260 (0x1ECBD4). Client window: qword_140DB52F0.
// Client slot count at player+0x2470, enabled flag at player+0x2474 (DH uses +0x246C / +0x2468).

bool TradeskillDepot::IsEnabled(Client* client)
{
	return client
		&& RuleB(Features, TradeskillDepotEnabled)
		&& client->ClientVersion() >= EQ::versions::ClientVersion::TOB;
}

// Mirrors DragonHoard::SendFeatureUnlock — the OP_FeatureUnlock wire format is shared; only
// the feature ids differ. The client's feature-array writer (sub_1402DC7E0) appends whatever
// ids we send, so the depot can send its own packet independently of Dragon's Hoard.
// Wire format: [u8 header=0][u32 feature_count][ {u32 feature_id, u32 count>=1} x feature_count ]
void TradeskillDepot::SendFeatureUnlock(Client* client)
{
	if (!IsEnabled(client)) {
		return;
	}

	static const uint32 feature_ids[] = {
		2018125, // 0x1ECB4D — Tradeskill Depot
		2018260, // 0x1ECBD4 — Tradeskill Depot (secondary)
	};
	const uint32 num_features = static_cast<uint32>(sizeof(feature_ids) / sizeof(feature_ids[0]));
	const uint32 active_count = 1;

	const uint32 packet_size = 1 + 4 + (num_features * 8);
	auto* outapp = new EQApplicationPacket(OP_FeatureUnlock, packet_size);
	memset(outapp->pBuffer, 0, packet_size);

	uint8* buf = outapp->pBuffer;
	buf[0] = 0;
	*reinterpret_cast<uint32*>(buf + 1) = num_features;
	uint32 off = 5;
	for (uint32 i = 0; i < num_features; ++i) {
		*reinterpret_cast<uint32*>(buf + off)     = feature_ids[i];
		*reinterpret_cast<uint32*>(buf + off + 4) = active_count;
		off += 8;
	}

	client->FastQueuePacket(&outapp);

	LogDebug("TradeskillDepot::SendFeatureUnlock sent {} feature(s) to {}", num_features, client->GetName());
}

// TODO: the OP_TradeskillDepot packets are stream-serialized (client sub_140216ED0 /
// sub_1401E06A0). action=4 sets the slot count (writes player+0x2470); the enable action and
// the exact byte layout still need reverse-engineering before SendUnlock can be implemented.
void TradeskillDepot::SendUnlock(Client* client)
{
	if (!IsEnabled(client)) {
		return;
	}
	// TODO: send enable + slot-count (OP_TradeskillDepot, stream layout TBD).
}

// TODO: query tradeskill_depot_items for the account and send each via SendItemPacket with the
// depot ItemPacketType (analog of DH's ItemPacketDragonHoard=0x77; type still to be identified).
void TradeskillDepot::SendItemList(Client* client)
{
	if (!IsEnabled(client)) {
		return;
	}
	// TODO
}

// TODO: parse the stream deposit packet; enforce eligibility (tradeskill-usable, stack size > 1,
// not temporary/attunable/container/no-storage/no-zone/no-trade/heirloom); store with quantity.
void TradeskillDepot::HandleDeposit(Client* client, const EQApplicationPacket* app)
{
	if (!app || !IsEnabled(client)) {
		return;
	}
	LogDebug("TradeskillDepot::HandleDeposit received size={} (handler not yet implemented)", app->size);
}

// TODO: parse the stream retrieve packet; honor withdraw quantity (ctrl-click single / shift-click
// whole stack); update or delete the row and send the client confirmation.
void TradeskillDepot::HandleRetrieve(Client* client, const EQApplicationPacket* app)
{
	if (!app || !IsEnabled(client)) {
		return;
	}
	LogDebug("TradeskillDepot::HandleRetrieve received size={} (handler not yet implemented)", app->size);
}
