#include "zone/tradeskilldepot_cache.h"

#include "common/eqemu_logsys.h"
#include "common/item_data.h"
#include "common/item_instance.h"
#include "common/repositories/tradeskill_depot_items_repository.h"
#include "common/rulesys.h"
#include "common/shareddb.h"

#include <fmt/format.h>

#include <algorithm>

namespace TradeskillDepot {

DepotCache::DepotCache()
	: m_slot_limit(static_cast<uint32>(std::max(RuleI(Features, TradeskillDepot), 0)))
{
}

DepotCache::~DepotCache() = default;

void DepotCache::SetQuantity(Pile& pile, uint32 quantity)
{
	const auto* item      = pile.inst->GetItem();
	const uint32 per_stack = item->StackSize > 1 ? static_cast<uint32>(item->StackSize) : 1U;

	pile.quantity = quantity;
	pile.inst->SetCharges(static_cast<int16>(std::min(quantity, per_stack)));
	pile.inst->SetMerchantCount(static_cast<int32>(quantity));
}

void DepotCache::EnsureLoaded(SharedDatabase& db, uint32 account_id)
{
	if (m_loaded) {
		return;
	}

	m_loaded = true;

	const auto rows = TradeskillDepotItemsRepository::GetWhere(db, fmt::format("account_id = {} ORDER BY id", account_id));

	for (const auto& row : rows) {
		std::unique_ptr<EQ::ItemInstance> inst(db.CreateItem(row.item_id));
		if (!inst) {
			LogError(
				"Tradeskill Depot item [{}] in row [{}] of account [{}] could not be created",
				row.item_id, row.id, account_id
			);
			continue;
		}

		inst->SetSerialNumber(static_cast<int32>(row.id));

		Pile pile{row.id, static_cast<int16>(m_piles.size()), 0, std::move(inst)};
		SetQuantity(pile, row.quantity);
		m_piles.push_back(std::move(pile));
	}
}

std::vector<DepotCache::Pile>::iterator DepotCache::FindPile(uint32 item_id)
{
	return std::find_if(m_piles.begin(), m_piles.end(), [item_id](const Pile& pile) {
		return pile.inst->GetID() == item_id;
	});
}

int16 DepotCache::FreeSlot() const
{
	int16 slot = 0;
	while (std::any_of(m_piles.begin(), m_piles.end(), [slot](const Pile& pile) { return pile.slot == slot; })) {
		++slot;
	}

	return slot;
}

size_t DepotCache::Count(SharedDatabase& db, uint32 account_id)
{
	EnsureLoaded(db, account_id);

	return m_piles.size();
}

uint32 DepotCache::Quantity(SharedDatabase& db, uint32 account_id, uint32 item_id)
{
	EnsureLoaded(db, account_id);

	const auto it = FindPile(item_id);

	return it != m_piles.end() ? it->quantity : 0;
}

int16 DepotCache::SlotOf(SharedDatabase& db, uint32 account_id, uint32 item_id)
{
	EnsureLoaded(db, account_id);

	const auto it = FindPile(item_id);

	return it != m_piles.end() ? it->slot : -1;
}

const EQ::ItemInstance* DepotCache::Add(SharedDatabase& db, uint32 account_id, const EQ::ItemData* item, uint32 quantity)
{
	EnsureLoaded(db, account_id);

	const auto it = FindPile(item->ID);
	if (it != m_piles.end()) {
		auto e = TradeskillDepotItemsRepository::NewEntity();
		e.id         = it->row_id;
		e.account_id = account_id;
		e.item_id    = item->ID;
		e.item_name  = item->Name;
		e.quantity   = it->quantity + quantity;

		if (TradeskillDepotItemsRepository::UpdateOne(db, e) == 0) {
			LogError("Tradeskill Depot update failed for account [{}] row [{}]", account_id, it->row_id);
			return nullptr;
		}

		SetQuantity(*it, e.quantity);

		return it->inst.get();
	}

	std::unique_ptr<EQ::ItemInstance> inst(db.CreateItem(item));
	if (!inst) {
		LogError("Tradeskill Depot item [{}] could not be created for account [{}]", item->ID, account_id);
		return nullptr;
	}

	auto e = TradeskillDepotItemsRepository::NewEntity();
	e.account_id = account_id;
	e.item_id    = item->ID;
	e.item_name  = item->Name;
	e.quantity   = quantity;

	e = TradeskillDepotItemsRepository::InsertOne(db, e);
	if (!e.id) {
		LogError("Tradeskill Depot write failed for account [{}] item [{}]", account_id, item->ID);
		return nullptr;
	}

	inst->SetSerialNumber(static_cast<int32>(e.id));

	Pile pile{e.id, FreeSlot(), 0, std::move(inst)};
	SetQuantity(pile, quantity);
	m_piles.push_back(std::move(pile));

	return m_piles.back().inst.get();
}

bool DepotCache::Remove(SharedDatabase& db, uint32 account_id, uint32 item_id, uint32 quantity)
{
	EnsureLoaded(db, account_id);

	const auto it = FindPile(item_id);
	if (it == m_piles.end() || it->quantity < quantity) {
		return false;
	}

	const uint32 remaining = it->quantity - quantity;
	if (remaining == 0) {
		if (TradeskillDepotItemsRepository::DeleteOne(db, it->row_id) == 0) {
			LogError("Tradeskill Depot delete failed for account [{}] row [{}]", account_id, it->row_id);
			return false;
		}

		m_piles.erase(it);

		return true;
	}

	auto e = TradeskillDepotItemsRepository::NewEntity();
	e.id         = it->row_id;
	e.account_id = account_id;
	e.item_id    = item_id;
	e.item_name  = it->inst->GetItem()->Name;
	e.quantity   = remaining;

	if (TradeskillDepotItemsRepository::UpdateOne(db, e) == 0) {
		LogError("Tradeskill Depot update failed for account [{}] row [{}]", account_id, it->row_id);
		return false;
	}

	SetQuantity(*it, remaining);

	return true;
}

} // namespace TradeskillDepot
