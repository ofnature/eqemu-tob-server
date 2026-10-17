#include "zone/dragonshoard_cache.h"

#include "common/eqemu_logsys.h"
#include "common/item_instance.h"
#include "common/repositories/character_evolving_items_repository.h"
#include "common/repositories/dragonhoard_items_repository.h"
#include "common/rulesys.h"
#include "common/shareddb.h"

#include <fmt/format.h>

#include <algorithm>

namespace DragonHoard {

HoardCache::HoardCache()
	: m_slot_limit(static_cast<uint32>(std::max(RuleI(Features, DragonsHoard), 0)))
{
}

HoardCache::~HoardCache() = default;

void HoardCache::EnsureLoaded(SharedDatabase& db, uint32 character_id)
{
	if (m_loaded) {
		return;
	}

	m_loaded = true;

	const auto rows = DragonhoardItemsRepository::GetWhere(
		db, fmt::format("character_id = {} ORDER BY id", character_id));

	// An evolving item's record is found by character and item id, as the inventory load does.
	const auto evolving = CharacterEvolvingItemsRepository::GetWhere(
		db, fmt::format("character_id = {} AND deleted_at IS NULL", character_id));

	for (const auto& row : rows) {
		std::unique_ptr<EQ::ItemInstance> inst(db.CreateItem(
			row.item_id, row.charges, 0, 0, 0, 0, 0, 0, row.attuned != 0, row.custom_data,
			row.ornament_icon, row.ornament_idfile, static_cast<uint32>(row.ornament_hero_model)
		));

		if (!inst) {
			LogError(
				"Dragon's Hoard item [{}] in row [{}] of character [{}] could not be created",
				row.item_id, row.id, character_id
			);
			continue;
		}

		inst->SetColor(row.color);

		if (inst->IsEvolving()) {
			const auto record = std::find_if(evolving.begin(), evolving.end(), [&row](const auto& e) {
				return e.item_id == row.item_id;
			});

			if (record != evolving.end()) {
				inst->GetEvolvingDetails() = *record;
				inst->CalculateEvolveProgression();
			}
		}

		m_items.push_back({row.id, std::move(inst)});
	}
}

size_t HoardCache::Count(SharedDatabase& db, uint32 character_id)
{
	EnsureLoaded(db, character_id);

	return m_items.size();
}

const EQ::ItemInstance* HoardCache::Deposit(
	SharedDatabase& db, uint32 character_id, std::unique_ptr<EQ::ItemInstance> inst)
{
	EnsureLoaded(db, character_id);

	auto e = DragonhoardItemsRepository::NewEntity();
	e.character_id        = character_id;
	e.item_id             = inst->GetItem()->ID;
	e.item_name           = inst->GetItem()->Name;
	e.charges             = inst->GetCharges();
	e.color               = inst->GetColor();
	e.attuned             = inst->IsAttuned() ? 1 : 0;
	e.ornament_icon       = inst->GetOrnamentationIcon();
	e.ornament_idfile     = inst->GetOrnamentationIDFile();
	e.ornament_hero_model = static_cast<int32_t>(inst->GetOrnamentHeroModel());
	e.custom_data         = inst->GetCustomDataString();

	e = DragonhoardItemsRepository::InsertOne(db, e);
	if (!e.id) {
		LogError("Dragon's Hoard write failed for character [{}] item [{}]", character_id, inst->GetItem()->ID);
		return nullptr;
	}

	m_items.push_back({e.id, std::move(inst)});

	return m_items.back().inst.get();
}

std::vector<HoardCache::Entry>::iterator HoardCache::FindEntry(int32 serial_number)
{
	return std::find_if(m_items.begin(), m_items.end(), [serial_number](const Entry& entry) {
		return entry.inst->GetSerialNumber() == serial_number;
	});
}

EQ::ItemInstance* HoardCache::Find(SharedDatabase& db, uint32 character_id, int32 serial_number)
{
	EnsureLoaded(db, character_id);

	const auto it = FindEntry(serial_number);

	return it != m_items.end() ? it->inst.get() : nullptr;
}

std::unique_ptr<EQ::ItemInstance> HoardCache::Retrieve(SharedDatabase& db, uint32 character_id, int32 serial_number)
{
	EnsureLoaded(db, character_id);

	const auto it = FindEntry(serial_number);
	if (it == m_items.end()) {
		return nullptr;
	}

	if (DragonhoardItemsRepository::DeleteOne(db, it->row_id) == 0) {
		LogError("Dragon's Hoard delete failed for character [{}] row [{}]", character_id, it->row_id);
		return nullptr;
	}

	auto inst = std::move(it->inst);
	m_items.erase(it);

	return inst;
}

} // namespace DragonHoard
