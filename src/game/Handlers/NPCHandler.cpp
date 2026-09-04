/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "GossipDef.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "Pet.h"
#include "Spell.h"
#include "Chat.h"
#include "CharacterDatabaseCache.h"

enum StableResultCode
{
    STABLE_ERR_MONEY        = 0x01,                         // "you don't have enough money"
    STABLE_ERR_STABLE       = 0x06,                         // currently used in most fail cases
    STABLE_SUCCESS_STABLE   = 0x08,                         // stable success
    STABLE_SUCCESS_UNSTABLE = 0x09,                         // unstable/swap success
    STABLE_SUCCESS_BUY_SLOT = 0x0A,                         // buy slot success
};

void WorldSession::HandleTabardVendorActivateOpcode(WorldPackets::Npc::TabardVendorActivate const& packet)
{
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_TABARDDESIGNER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTabardVendorActivateOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    SendTabardVendorActivate(packet.guid);
}

void WorldSession::SendTabardVendorActivate(ObjectGuid guid)
{
    auto tabardVendor = std::make_unique<WorldPackets::Npc::TabardVendorActivateResponse>();
    tabardVendor->tabardVendorNpcGuid = guid;
    SendPacket(std::move(tabardVendor));
}

void WorldSession::HandleBankerActivateOpcode(WorldPackets::Npc::BankerActivate const& packet)
{
    if (!CheckBanker(packet.guid))
        return;

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_FEIGN_DEATH))
        GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    SendShowBank(packet.guid);
}

void WorldSession::SendShowBank(ObjectGuid guid)
{
    GetPlayer()->m_currentBankerGuid = guid;

    auto packet = std::make_unique<WorldPackets::Npc::ShowBank>();
    packet->bankerGuid = guid;
    SendPacket(std::move(packet));
}

void WorldSession::HandleTrainerListOpcode(WorldPackets::Npc::TrainerList const& packet)
{
    SendTrainerList(packet.guid);
}

static void SendTrainerSpellHelper(WorldPacket& data, TrainerSpell const* tSpell, uint32 triggerSpell, TrainerSpellState state, float fDiscountMod, bool can_learn_primary_prof)
{
    SpellEntry const* triggerInfo = sSpellMgr.GetSpellEntry(triggerSpell);
    uint32 spellLevel = 0;
    if (tSpell->reqLevel)
        spellLevel = tSpell->reqLevel;
    else if (triggerInfo)
        spellLevel = triggerInfo->spellLevel;
    else
        return;

    bool primary_prof_first_rank = sSpellMgr.IsPrimaryProfessionFirstRankSpell(triggerSpell);

    SpellChainNode const* chain_node = sSpellMgr.GetSpellChainNode(triggerSpell);

    data << uint32(tSpell->spell);
    data << uint8(state == TRAINER_SPELL_GREEN_DISABLED ? TRAINER_SPELL_GREEN : state);
    data << uint32(tSpell->spellCost * fDiscountMod + 0.5f);

    data << uint32(primary_prof_first_rank && can_learn_primary_prof ? 1 : 0);
    // primary prof. learn confirmation dialog
    data << uint32(primary_prof_first_rank ? 1 : 0);    // must be equal prev. field to have learn button in enabled state
    data << uint8(spellLevel);
    data << uint32(tSpell->reqSkill);
    data << uint32(tSpell->reqSkillValue);
    // Nostalrius: le client veut spellreq1, spellreq2 avec spellreq2 != 0 seulement si spellreq1 != 0.
    if (chain_node)
    {
        if (chain_node->req)
        {
            data << uint32(chain_node->req);
            data << uint32(chain_node->prev);
        }
        else
        {
            data << uint32(chain_node->prev);
            data << uint32(0);
        }
    }
    else
        data << uint32(0) << uint32(0);
    data << uint32(0);
}

void WorldSession::SendTrainerList(ObjectGuid guid)
{
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_TRAINER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: SendTrainerList - %s not found or you can't interact with him.", guid.GetString().c_str());
        return;
    }

    // trainer list loaded at check;
    if (!unit->IsTrainerOf(_player, true))
        return;

    CreatureInfo const* ci = unit->GetCreatureInfo();
    if (!ci)
        return;

    TrainerSpellData const* cSpells = unit->GetTrainerSpells();
    TrainerSpellData const* tSpells = unit->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: SendTrainerList - Training spells not found for %s", guid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    uint32 maxcount = (cSpells ? cSpells->spellList.size() : 0) + (tSpells ? tSpells->spellList.size() : 0);
    uint32 trainer_type = cSpells && cSpells->trainerType ? cSpells->trainerType : (tSpells ? tSpells->trainerType : 0);

    std::string strTitle;
    if (TrainerGreetingLocale const* trainerGreeting = sObjectMgr.GetTrainerGreetingLocale(guid.GetEntry()))
    {
        int locale_idx = GetSessionDbLocaleIndex();

        if ((int32)trainerGreeting->Content.size() > locale_idx + 1 && !trainerGreeting->Content[locale_idx + 1].empty())
            strTitle = trainerGreeting->Content[locale_idx + 1];
        else
            strTitle = trainerGreeting->Content[0];
    }
    else
    {
        strTitle = GetMangosString(LANG_NPC_TAINER_HELLO);
    }

    WorldPacket data(SMSG_TRAINER_LIST, 8 + 4 + 4 + maxcount * 38 + strTitle.size() + 1);
    data << ObjectGuid(guid);
    data << uint32(trainer_type);

    size_t count_pos = data.wpos();
    data << uint32(maxcount);

    // reputation discount
    float fDiscountMod = _player->GetReputationPriceDiscount(unit);
    bool can_learn_primary_prof = GetPlayer()->GetFreePrimaryProfessionPoints() > 0;

    uint32 count = 0;

    if (cSpells)
    {
        for (const auto& itr : cSpells->spellList)
        {
            TrainerSpell const* tSpell = &itr.second;

            uint32 triggerSpell = sSpellMgr.GetSpellEntry(tSpell->spell)->EffectTriggerSpell[0];

            if (!_player->IsSpellFitByClassAndRace(triggerSpell))
                continue;

            TrainerSpellState state = _player->GetTrainerSpellState(tSpell);

            SendTrainerSpellHelper(data, tSpell, triggerSpell, state, fDiscountMod, can_learn_primary_prof);

            ++count;
        }
    }

    if (tSpells)
    {
        for (const auto& itr : tSpells->spellList)
        {
            TrainerSpell const* tSpell = &itr.second;

            uint32 triggerSpell = sSpellMgr.GetSpellEntry(tSpell->spell)->EffectTriggerSpell[0];

            if (!_player->IsSpellFitByClassAndRace(triggerSpell))
                continue;

            TrainerSpellState state = _player->GetTrainerSpellState(tSpell);

            SendTrainerSpellHelper(data, tSpell, triggerSpell, state, fDiscountMod, can_learn_primary_prof);

            ++count;
        }
    }

    data << strTitle;

    data.put<uint32>(count_pos, count);
    SendPacket(&data);
}

void WorldSession::SendTrainingSuccess(ObjectGuid guid, uint32 spellId)
{
    auto packet = std::make_unique<WorldPackets::Npc::TrainerBuySucceeded>();
    packet->trainerGuid = guid;
    packet->spellId = spellId; // should be same as in packet from client
    SendPacket(std::move(packet));
}

void WorldSession::SendTrainingFailure(ObjectGuid guid, uint32 serviceId, uint32 errorCode)
{
    auto packet = std::make_unique<WorldPackets::Npc::TrainerBuyFailed>();
    packet->trainerGuid = guid;
    packet->serviceId = serviceId;
    packet->errorCode = errorCode;
    SendPacket(std::move(packet));
}

void WorldSession::HandleTrainerBuySpellOpcode(WorldPackets::Npc::TrainerBuySpell const& packet)
{
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_TRAINER_BUY_SPELL Trainer: %s, learn spell id is: %u", packet.guid.GetString().c_str(), packet.spellId);

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_TRAINER);

    if (!unit || !unit->IsTrainerOf(_player, true) || !unit->IsWithinLOSInMap(_player))
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTrainerBuySpellOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    // Check if the spell is present in the trainer's spell list.
    TrainerSpellData const* cSpells = unit->GetTrainerSpells();
    TrainerSpellData const* tSpells = unit->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        return;
    }

    // Try to find the spell in npc_trainer.
    TrainerSpell const* trainer_spell = cSpells ? cSpells->Find(packet.spellId) : nullptr;

    // Not found, try find it in npc_trainer_template.
    if (!trainer_spell && tSpells)
        trainer_spell = tSpells->Find(packet.spellId);

    // Not found anywhere, cheating?
    if (!trainer_spell)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        return;
    }

    // Can't be learned, cheat? Or double learn with lags...
    if (_player->GetTrainerSpellState(trainer_spell) != TRAINER_SPELL_GREEN)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_NOT_ENOUGH_SKILL);
        return;
    }

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(trainer_spell->spell);

    // Apply reputation discount.
    uint32 nSpellCost = uint32(trainer_spell->spellCost * _player->GetReputationPriceDiscount(unit) + 0.5f);

    // Check money requirement.
    if (_player->GetMoney() < nSpellCost)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_NOT_ENOUGH_MONEY);
        return;
    }

    // All is good. Spell can be learned if we reach this point.
    _player->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    _player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    Spell* spell;
    if (proto->SpellVisual == 222)
        spell = new Spell(_player, proto, false);
    else
        spell = new Spell(unit, proto, false);

    SpellCastTargets targets;
    targets.setUnitTarget(_player);

    SpellCastResult cast_result = spell->prepare(std::move(targets));
    spell->update(1); // Update the spell right now. Prevents desynch => take twice the money if you click really fast.

    // Only charge player if cast of learning spell was successful.
    if (cast_result == SPELL_CAST_OK)
    {
        _player->ModifyMoney(-int32(nSpellCost));
        SendTrainingSuccess(packet.guid, packet.spellId);
    }
    else
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
}

void WorldSession::HandleGossipHelloOpcode(WorldPackets::Npc::GossipHello const& packet)
{
    Creature* pCreature = GetPlayer()->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_NONE);
    if (!pCreature)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipHelloOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
        pCreature->PauseOutOfCombatMovement();

    if (pCreature->IsSpiritGuide())
        pCreature->SendAreaSpiritHealerQueryOpcode(_player);

    if (!sScriptMgr.OnGossipHello(_player, pCreature))
    {
        _player->PrepareGossipMenu(pCreature, pCreature->GetDefaultGossipMenuId());
        _player->SendPreparedGossip(pCreature);
    }
}

void WorldSession::HandleGossipSelectOptionOpcode(WorldPackets::Npc::GossipSelectOption const& packet)
{
    bool const isCoded = _player->PlayerTalkClass->GossipOptionCoded(packet.gossipListId);
    if (isCoded && packet.code.empty())
        return;  // coded option requires a code from the client

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    uint32 sender = _player->PlayerTalkClass->GossipOptionSender(packet.gossipListId);
    uint32 action = _player->PlayerTalkClass->GossipOptionAction(packet.gossipListId);

    // Only forward a non-null code to scripts for coded gossip options.
    const char* code = (isCoded && !packet.code.empty()) ? packet.code.c_str() : nullptr;

    if (packet.guid.IsAnyTypeCreature())
    {
        Creature* pCreature = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_NONE);

        if (!pCreature)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipSelectOptionOpcode - %s not found or you can't interact with it.", packet.guid.GetString().c_str());
            return;
        }

        if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
            pCreature->PauseOutOfCombatMovement();

        if (!sScriptMgr.OnGossipSelect(_player, pCreature, sender, action, code))
            _player->OnGossipSelect(pCreature, packet.gossipListId);
    }
    else if (packet.guid.IsGameObject())
    {
        GameObject* pGo = GetPlayer()->GetGameObjectIfCanInteractWith(packet.guid);

        if (!pGo)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipSelectOptionOpcode - %s not found or you can't interact with it.", packet.guid.GetString().c_str());
            return;
        }

        if (!sScriptMgr.OnGossipSelect(_player, pGo, sender, action, code))
            _player->OnGossipSelect(pGo, packet.gossipListId);
    }
}

void WorldSession::HandleSpiritHealerActivateOpcode(WorldPackets::Npc::SpiritHealerActivate const& packet)
{
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_SPIRITHEALER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleSpiritHealerActivateOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendSpiritResurrect();
}

void WorldSession::SendSpiritResurrect()
{
    _player->ResurrectPlayer(0.5f, true);

    _player->DurabilityLossAll(0.25f, true);

    // get corpse nearest graveyard
    WorldSafeLocsEntry const* corpseGrave = nullptr;
    Corpse* corpse = _player->GetCorpse();
    if (corpse)
        corpseGrave = sObjectMgr.GetClosestGraveYard(
                          corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), corpse->GetMapId(), _player->GetTeam());

    // now can spawn bones
    _player->SpawnCorpseBones();

    // teleport to nearest from corpse graveyard, if different from nearest to player ghost
    if (corpseGrave)
    {
        WorldSafeLocsEntry const* ghostGrave = sObjectMgr.GetClosestGraveYard(
                _player->GetPositionX(), _player->GetPositionY(), _player->GetPositionZ(), _player->GetMapId(), _player->GetTeam());

        float orientation = _player->GetOrientation();

        // World of Warcraft Client Patch 1.8.0 (2005-10-11)
        // - All graveyards that needed adjustment were changed so that a
        //   character's spirit comes into the world facing toward the Spirit Healer.
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
        if (float facing = sObjectMgr.GetWorldSafeLocFacing(corpseGrave->ID))
            orientation = facing;
#endif

        if (corpseGrave != ghostGrave)
            _player->TeleportTo(corpseGrave->map_id, corpseGrave->x, corpseGrave->y, corpseGrave->z, orientation);
        // or update at original position
        else
        {
            _player->GetCamera().UpdateVisibilityForOwner();
            _player->UpdateObjectVisibility();
        }
    }
    // or update at original position
    else
    {
        _player->GetCamera().UpdateVisibilityForOwner();
        _player->UpdateObjectVisibility();
    }
}

void WorldSession::HandleBinderActivateOpcode(WorldPackets::Npc::BinderActivate const& packet)
{
    if (!GetPlayer()->IsInWorld() || !GetPlayer()->IsAlive())
        return;

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_INNKEEPER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleBinderActivateOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendBindPoint(unit);
}

void WorldSession::SendBindPoint(Creature* npc)
{
    // prevent set homebind to instances in any case
    if (GetPlayer()->GetMap()->Instanceable())
        return;

    // send spell for bind 3286 bind magic
    npc->CastSpell(_player, 3286, true);                    // Bind

    _player->PlayerTalkClass->CloseGossip();
}

void WorldSession::HandleListStabledPetsOpcode(WorldPackets::Npc::ListStabledPets const& packet)
{
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_STABLEMASTER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleListStabledPetsOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendStablePet(packet.npcGuid);
}

void WorldSession::SendStablePet(ObjectGuid guid)
{
    WorldPacket data(MSG_LIST_STABLED_PETS, 200);           // guess size
    data << guid;

    Pet* pet = _player->GetPet();

    size_t wpos = data.wpos();
    data << uint8(0);                                       // place holder for slot show number

    data << uint8(GetPlayer()->m_stableSlots);

    uint8 num = 0;                                          // counter for place holder

    // not let move dead pet in slot
    if (pet && pet->IsAlive() && pet->GetPetType() == HUNTER_PET)
    {
        data << uint32(pet->GetCharmInfo()->GetPetNumber());
        data << uint32(pet->GetEntry());
        data << uint32(pet->GetLevel());
        data << pet->GetName();                             // petname
        data << uint32(pet->GetLoyaltyLevel());             // loyalty
        data << uint8(0x01);                                // client slot 1 == current pet (0)
        ++num;
    }
    // Pet may be despawned if owner went far away from pet for example.
    else if (CharacterPetCache const* currentPetData = sCharacterDatabaseCache.GetCharacterPetByOwner(_player->GetGUIDLow()))
    {
        data << uint32(currentPetData->id);
        data << uint32(currentPetData->entry);
        data << uint32(currentPetData->level);
        data << currentPetData->name;                           // petname
        data << uint32(currentPetData->loyalty);                // loyalty
        data << uint8(0x01);                                    // client slot 1 == current pet (0)
        ++num;
    }
    CharPetMap const& pets = sCharacterDatabaseCache.GetCharPetsMap();
    CharPetMap::const_iterator myPets = pets.find(GetPlayer()->GetGUIDLow());
    if (myPets != pets.end())
        for (const auto it : myPets->second)
            if (it->slot >= PET_SAVE_FIRST_STABLE_SLOT && it->slot <= PET_SAVE_LAST_STABLE_SLOT)
            {
                data << uint32(it->id);                 // pet number
                data << uint32(it->entry);              // creature entry
                data << uint32(it->level);              // level
                data << it->name;                       // name
                data << uint32(it->loyalty);            // loyalty
                data << uint8(it->slot + 1);            // slot
                ++num;
            }

    data.put<uint8>(wpos, num);                             // set real data to placeholder
    SendPacket(&data);
}

void WorldSession::SendStableResult(uint8 res)
{
    auto packet = std::make_unique<WorldPackets::Npc::StableResult>();
    packet->result = res;
    SendPacket(std::move(packet));
}

bool WorldSession::CheckStableMaster(ObjectGuid guid)
{
    // spell case or GM
    if (guid == GetPlayer()->GetObjectGuid())
    {
        // command case will return only if player have real access to command
        if (!ChatHandler(GetPlayer()).FindCommand("stable"))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "%s attempt open stable in cheating way.", guid.GetString().c_str());
            return false;
        }
    }
    // stable master case
    else
    {
        if (!GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_STABLEMASTER))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "Stablemaster %s not found or you can't interact with him.", guid.GetString().c_str());
            return false;
        }
    }

    return true;
}

namespace
{
    bool IsStoredHunterPet(CharacterPetCache const* petData)
    {
        if (!petData || petData->petType != HUNTER_PET || !petData->entry)
            return false;
        CreatureInfo const* creatureInfo = sObjectMgr.GetCreatureTemplate(petData->entry);
        return creatureInfo && creatureInfo->IsTameable();
    }

    CharacterPetCache* GetStoredCurrentHunterPet(Player* player)
    {
        CharacterPetCache* petData =
            sCharacterDatabaseCache.GetCharacterPetByOwner(player->GetGUIDLow());
        return IsStoredHunterPet(petData) ? petData : nullptr;
    }

    uint32 FindFreeHunterStableSlot(Player* player)
    {
        bool usedSlots[PET_SAVE_LAST_STABLE_SLOT - PET_SAVE_FIRST_STABLE_SLOT + 1] = {false};
        CharPetMap const& pets = sCharacterDatabaseCache.GetCharPetsMap();
        CharPetMap::const_iterator ownerPets = pets.find(player->GetGUIDLow());
        if (ownerPets != pets.end())
        {
            for (CharacterPetCache const* petData : ownerPets->second)
            {
                if (petData->slot >= PET_SAVE_FIRST_STABLE_SLOT &&
                    petData->slot <= PET_SAVE_LAST_STABLE_SLOT)
                    usedSlots[petData->slot - PET_SAVE_FIRST_STABLE_SLOT] = true;
            }
        }

        uint32 slot = PET_SAVE_FIRST_STABLE_SLOT;
        while (slot <= PET_SAVE_LAST_STABLE_SLOT &&
               usedSlots[slot - PET_SAVE_FIRST_STABLE_SLOT])
            ++slot;
        return slot;
    }

    void SetStoredPetSlots(
        CharacterPetCache* firstPet,
        uint32 firstSlot,
        CharacterPetCache* secondPet = nullptr,
        uint32 secondSlot = PET_SAVE_NOT_IN_SLOT)
    {
        CharacterDatabase.BeginTransaction();

        static SqlStatementID updateFirstPetSlot;
        SqlStatement stmt = CharacterDatabase.CreateStatement(
            updateFirstPetSlot,
            "UPDATE `character_pet` SET `slot` = ? WHERE `owner_guid` = ? AND `id` = ?");
        stmt.PExecute(firstSlot, firstPet->ownerGuid, firstPet->id);

        if (secondPet)
        {
            static SqlStatementID updateSecondPetSlot;
            stmt = CharacterDatabase.CreateStatement(
                updateSecondPetSlot,
                "UPDATE `character_pet` SET `slot` = ? WHERE `owner_guid` = ? AND `id` = ?");
            stmt.PExecute(secondSlot, secondPet->ownerGuid, secondPet->id);
        }

        CharacterDatabase.CommitTransaction();
        firstPet->slot = firstSlot;
        if (secondPet)
            secondPet->slot = secondSlot;
    }
}

void WorldSession::HandleStablePet(WorldPackets::Npc::StablePet const& packet)
{
    if (!GetPlayer()->IsAlive())
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    Pet* pet = _player->GetPet();
    CharacterPetCache* storedPet = nullptr;
    if (pet)
    {
        if (pet->GetPetType() != HUNTER_PET)
        {
            SendStableResult(STABLE_ERR_STABLE);
            return;
        }
    }
    else
    {
        storedPet = GetStoredCurrentHunterPet(_player);
        if (!storedPet)
        {
            SendStableResult(STABLE_ERR_STABLE);
            return;
        }
    }

    uint32 freeSlot = FindFreeHunterStableSlot(_player);
    if (freeSlot > GetPlayer()->m_stableSlots)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    if (pet)
        pet->Unsummon(PetSaveMode(freeSlot), _player);
    else
        SetStoredPetSlots(storedPet, freeSlot);
    SendStableResult(STABLE_SUCCESS_STABLE);
}

void WorldSession::HandleUnstablePet(WorldPackets::Npc::UnstablePet const& packet)
{
    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (_player->GetPet())
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    CharacterPetCache* petData =
        sCharacterDatabaseCache.GetCharacterPetCacheByOwnerAndId(
            _player->GetGUIDLow(), packet.petNumber);
    if (!IsStoredHunterPet(petData) ||
        petData->slot < PET_SAVE_FIRST_STABLE_SLOT ||
        petData->slot > PET_SAVE_LAST_STABLE_SLOT)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    CharacterPetCache* currentPet = GetStoredCurrentHunterPet(_player);
    uint32 const stableSlot = petData->slot;
    uint32 const currentSlot = currentPet ? currentPet->slot : PET_SAVE_NOT_IN_SLOT;
    uint32 const newPetSlot =
        petData->currentHealth == 0 ? PET_SAVE_NOT_IN_SLOT : PET_SAVE_AS_CURRENT;

    if (currentPet)
        SetStoredPetSlots(currentPet, stableSlot, petData, newPetSlot);
    else
        SetStoredPetSlots(petData, newPetSlot);

    if (petData->currentHealth == 0)
    {
        SendStableResult(STABLE_SUCCESS_UNSTABLE);
        return;
    }

    Pet* newpet = new Pet(HUNTER_PET);
    if (!newpet->LoadPetFromDB(_player, petData->entry, packet.petNumber))
    {
        delete newpet;
        if (currentPet)
            SetStoredPetSlots(petData, stableSlot, currentPet, currentSlot);
        else
            SetStoredPetSlots(petData, stableSlot);
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    SendPetNameQuery(newpet->GetObjectGuid(), packet.petNumber);
    SendStableResult(STABLE_SUCCESS_UNSTABLE);
}

void WorldSession::HandleBuyStableSlot(WorldPackets::Npc::BuyStableSlot const& packet)
{
    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (GetPlayer()->m_stableSlots < MAX_PET_STABLES)
    {
        StableSlotPricesEntry const* SlotPrice = sStableSlotPricesStore.LookupEntry(GetPlayer()->m_stableSlots + 1);
        if (_player->GetMoney() >= SlotPrice->Price)
        {
            ++GetPlayer()->m_stableSlots;
            _player->ModifyMoney(-int32(SlotPrice->Price));
            SendStableResult(STABLE_SUCCESS_BUY_SLOT);
        }
        else
            SendStableResult(STABLE_ERR_MONEY);
    }
    else
        SendStableResult(STABLE_ERR_STABLE);
}

void WorldSession::HandleStableRevivePet(NullClientPacket const& /*packet*/)
{
}

void WorldSession::HandleStableSwapPet(WorldPackets::Npc::StableSwapPet const& packet)
{
    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    CharacterPetCache* swappedPet =
        sCharacterDatabaseCache.GetCharacterPetCacheByOwnerAndId(
            _player->GetGUIDLow(), packet.petNumber);
    if (!IsStoredHunterPet(swappedPet) ||
        swappedPet->slot < PET_SAVE_FIRST_STABLE_SLOT ||
        swappedPet->slot > PET_SAVE_LAST_STABLE_SLOT)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    uint32 const stableSlot = swappedPet->slot;
    CharacterPetCache* currentPetData = nullptr;
    uint32 currentPetSlot = PET_SAVE_NOT_IN_SLOT;
    Pet* pet = _player->GetPet();
    if (pet)
    {
        if (pet->GetPetType() != HUNTER_PET)
        {
            SendStableResult(STABLE_ERR_STABLE);
            return;
        }
        currentPetData = sCharacterDatabaseCache.GetCharacterPetCacheByOwnerAndId(
            _player->GetGUIDLow(), pet->GetCharmInfo()->GetPetNumber());
        if (currentPetData)
            currentPetSlot = currentPetData->slot;
        pet->Unsummon(PetSaveMode(stableSlot), _player);
    }
    else
    {
        currentPetData = GetStoredCurrentHunterPet(_player);
        if (!currentPetData)
        {
            SendStableResult(STABLE_ERR_STABLE);
            return;
        }
        currentPetSlot = currentPetData->slot;
        uint32 const newPetSlot =
            swappedPet->currentHealth == 0 ? PET_SAVE_NOT_IN_SLOT : PET_SAVE_AS_CURRENT;
        SetStoredPetSlots(currentPetData, stableSlot, swappedPet, newPetSlot);
    }

    if (swappedPet->currentHealth == 0)
    {
        SetStoredPetSlots(swappedPet, PET_SAVE_NOT_IN_SLOT);
        SendStableResult(STABLE_SUCCESS_UNSTABLE);
        return;
    }

    Pet* newpet = new Pet(HUNTER_PET);
    if (!newpet->LoadPetFromDB(_player, swappedPet->entry, packet.petNumber))
    {
        delete newpet;
        if (currentPetData)
            SetStoredPetSlots(swappedPet, stableSlot, currentPetData, currentPetSlot);
        else
            SetStoredPetSlots(swappedPet, stableSlot);
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    SendPetNameQuery(newpet->GetObjectGuid(), packet.petNumber);
    SendStableResult(STABLE_SUCCESS_UNSTABLE);
}

void WorldSession::HandleRepairItemOpcode(WorldPackets::Npc::RepairItem const& packet)
{
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_REPAIR);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleRepairItemOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    // reputation discount
    float discountMod = _player->GetReputationPriceDiscount(unit);

    if (packet.itemGuid)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "ITEM: %s repair of %s", packet.npcGuid.GetString().c_str(), packet.itemGuid.GetString().c_str());
        if (Item* item = _player->GetItemByGuid(packet.itemGuid))
            _player->DurabilityRepair(item->GetPos(), true, discountMod);
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "ITEM: %s repair all items", packet.npcGuid.GetString().c_str());
        _player->DurabilityRepairAll(true, discountMod);
    }
}
