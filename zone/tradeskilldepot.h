#pragma once

#include "common/types.h"

class Client;
class EQApplicationPacket;

// Personal Tradeskill Depot (The Outer Brood). Sibling of Dragon's Hoard — unlocked by its
// claims in OP_PlayerProfile, its own opcode (OP_TradeskillDepot = 0x2D36), feature ids
// (2018125 / 2018260), item packet type (ItemPacketTradeskillDepot = 0x79) and DB
// table (tradeskill_depot_items, one pile per item per account).
//
// Unlike Dragon's Hoard the depot holds STACKABLE tradeskill mats and its packets are
// stream-serialized: every OP_TradeskillDepot packet (both directions) starts with a
// 59-byte header (client reader sub_1401E06A0 + sub_140203B10):
//   [ 0] u32 action
//   [ 4] u64 zero      [12] u8 zero
//   [13] u8  route     0 = depot window, 1 = tradeskill window (echo from request)
//   [14] u64 zero      [22] u64 zero    [30] u64 zero    [38] u64 zero
//   [46] u8  fail      action 8 requires 0 to set the enabled flag
//   [47] u64 zero
//   [55] u32 map_count [59] {u32 serial, u32 qty} x map_count  (row-removal deltas)
// Action-specific payload follows the (usually empty) map.
//
// Actions (client handler sub_140216ED0 / senders sub_1401CE920, sub_1401D08A0 ...):
//   0 = open/state  (s->c payload u8: clear+show window)
//   3 = list sync   (c->s: refresh request; s->c: apply removal map)
//   4 = capacity    (s->c payload u32 max slots at offset 59; writes player+0x2470)
//   5 = deposit     (c->s payload [u8=1][u32 count][u64 inventory item serial x count])
//   6 = withdraw    (c->s payload [u32 item_id][u32 qty]; s->c: removal map + op ack)
//                   NOTE: rows are keyed by item_id (client item struct +56 = item->ID),
//                   NOT by the row serial; the removal map is keyed by item_id too.
//   8 = enable      (s->c; sets player+0x2474, shows depot UI)
//  12 = plain ack   (s->c; only decrements the client's pending-op counter)
// The client increments a pending-op counter (dword_140EE2B2C) on every deposit/withdraw
// send and freezes depot input until a 5/6/12 response decrements it — every well-formed
// request must be answered. Malformed requests are dropped.

namespace TradeskillDepot {

	// True only when the feature rule is on AND the client is TOB or later.
	bool IsEnabled(Client* client);

	// OP_TradeskillDepot dispatcher — routes by leading action dword.
	void HandlePacket(Client* client, const EQApplicationPacket* app);

	// Client-side per-pile cap (deposit merge path checks row qty < 99999).
	constexpr uint32 MAX_PILE_QUANTITY = 99999;

} // namespace TradeskillDepot
