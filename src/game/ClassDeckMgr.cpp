#include "ClassDeckMgr.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/Spell.h"
#include "Spells/SpellEntry.h"
#include "Spells/SpellMgr.h"
#include "Util.h"
#include "Utilities/Random.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

#include <openssl/sha.h>

namespace
{
uint32 EarnedDraftCount(uint32 level)
{
    if (level < 4)
        return 0;
    return std::min<uint32>(29, level / 2 - 1);
}

uint32 EarnedLevelForDraft(uint32 draftIndex)
{
    return 2 * (draftIndex + 1);
}

bool IsPlayableClass(uint32 classId)
{
    return classId > 0 && classId < MAX_CLASSES &&
        (CLASSMASK_ALL_PLAYABLE & (1u << (classId - 1))) != 0;
}

constexpr uint32 MinimumCatalogServices = 29;

std::string Sha256Hex(std::string const& value)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(
        reinterpret_cast<unsigned char const*>(value.data()),
        value.size(),
        digest.data());
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        result << std::setw(2) << uint32(byte);
    return result.str();
}
}

ClassDeckMgr& ClassDeckMgr::Instance()
{
    static ClassDeckMgr instance;
    return instance;
}

void ClassDeckMgr::LoadCatalog()
{
    m_enabled = sConfig.GetBoolDefault("Coworld.ClassDeck.Enable", false);
    m_testSeedSet = sConfig.GetBoolDefault("Coworld.ClassDeck.TestSeedSet", false);
    std::string const testSeedText = sConfig.GetStringDefault(
        "Coworld.ClassDeck.TestSeed", "0");
    char* testSeedEnd = nullptr;
    unsigned long long const parsedTestSeed = std::strtoull(
        testSeedText.c_str(), &testSeedEnd, 10);
    if (!testSeedEnd || *testSeedEnd != '\0' ||
        parsedTestSeed > std::numeric_limits<uint32>::max())
        throw std::runtime_error("Coworld.ClassDeck.TestSeed must fit uint32");
    m_testSeed = uint32(parsedTestSeed);
    m_interruptAfterSelectOnce = sConfig.GetBoolDefault(
        "Coworld.ClassDeck.InterruptAfterSelectOnce", false);
    m_interruptConsumed = false;
    for (auto& classCatalog : m_catalog)
        classCatalog.clear();

    if (!m_enabled)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "Coworld class deck progression: disabled.");
        return;
    }

    std::array<std::map<uint32, TrainerSpell>, MAX_CLASSES> deduplicated;
    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* creature = entry.second.get();
        if (creature->trainer_type != TRAINER_TYPE_CLASS ||
            !IsPlayableClass(creature->trainer_class))
            continue;

        auto collect = [&](TrainerSpellData const* spells)
        {
            if (!spells)
                return;
            for (auto const& trainerEntry : spells->spellList)
            {
                TrainerSpell const& trainerSpell = trainerEntry.second;
                SpellEntry const* service = sSpellMgr.GetSpellEntry(trainerSpell.spell);
                if (!service)
                    continue;
                std::vector<uint32> const taught = TaughtSpells(trainerSpell.spell);
                if (taught.empty())
                    continue;
                SpellEntry const* firstTaught = sSpellMgr.GetSpellEntry(taught[0]);
                uint32 requiredLevel = trainerSpell.reqLevel
                    ? trainerSpell.reqLevel
                    : firstTaught->spellLevel;
                if (requiredLevel < 2)
                    continue;
                deduplicated[creature->trainer_class].emplace(
                    trainerSpell.spell, trainerSpell);
            }
        };
        collect(sObjectMgr.GetNpcTrainerSpells(creature->entry));
        collect(sObjectMgr.GetNpcTrainerTemplateSpells(creature->trainer_id));
    }

    std::ostringstream serialized;
    uint32 total = 0;
    for (uint32 classId = 1; classId < MAX_CLASSES; ++classId)
    {
        for (auto const& entry : deduplicated[classId])
        {
            TrainerSpell const& spell = entry.second;
            m_catalog[classId].push_back(spell);
            serialized << classId << ':' << spell.spell << ':' << spell.spellCost
                       << ':' << spell.reqSkill << ':' << spell.reqSkillValue
                       << ':' << spell.reqLevel;
            for (uint32 taughtSpell : TaughtSpells(spell.spell))
                serialized << ':' << taughtSpell;
            serialized << '\n';
            ++total;
        }
        if (IsPlayableClass(classId))
        {
            uint32 const serviceCount = uint32(m_catalog[classId].size());
            sLog.Out(
                LOG_BASIC,
                LOG_LVL_BASIC,
                "Coworld class deck catalog: class %u has %u services.",
                classId,
                serviceCount);
            if (serviceCount < MinimumCatalogServices)
                throw std::runtime_error(
                    "Coworld class deck catalog has fewer than 29 services for "
                    "playable class " + std::to_string(classId) + ": " +
                    std::to_string(serviceCount));
        }
    }
    m_catalogVersion = Sha256Hex(serialized.str());
    sLog.Out(
        LOG_BASIC,
        LOG_LVL_BASIC,
        "Coworld class deck progression: enabled with %u services, catalog %s.",
        total,
        m_catalogVersion.c_str());
}

TrainerSpell const* ClassDeckMgr::FindService(
    uint8 classId, uint32 serviceSpell) const
{
    if (classId >= m_catalog.size())
        return nullptr;
    auto const& catalog = m_catalog[classId];
    auto found = std::lower_bound(
        catalog.begin(), catalog.end(), serviceSpell,
        [](TrainerSpell const& candidate, uint32 spell)
        {
            return candidate.spell < spell;
        });
    return found != catalog.end() && found->spell == serviceSpell
        ? &*found
        : nullptr;
}

bool ClassDeckMgr::IsCoveredService(uint8 classId, uint32 serviceSpell) const
{
    return m_enabled && FindService(classId, serviceSpell) != nullptr;
}

std::vector<uint32> ClassDeckMgr::TaughtSpells(uint32 serviceSpell) const
{
    std::vector<uint32> taught;
    SpellEntry const* service = sSpellMgr.GetSpellEntry(serviceSpell);
    if (!service)
        return taught;
    for (uint32 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
    {
        if (service->Effect[effect] == SPELL_EFFECT_LEARN_SPELL &&
            service->EffectTriggerSpell[effect] != 0 &&
            sSpellMgr.GetSpellEntry(service->EffectTriggerSpell[effect]))
            taught.push_back(service->EffectTriggerSpell[effect]);
    }
    if (taught.empty() && service->EffectTriggerSpell[0] != 0 &&
        sSpellMgr.GetSpellEntry(service->EffectTriggerSpell[0]))
        taught.push_back(service->EffectTriggerSpell[0]);
    std::sort(taught.begin(), taught.end());
    taught.erase(std::unique(taught.begin(), taught.end()), taught.end());
    return taught;
}

std::vector<ClassDeckDraft> ClassDeckMgr::LoadDrafts(uint32 guid) const
{
    std::vector<ClassDeckDraft> drafts;
    std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
        "SELECT `draft_index`, `earned_level`, `catalog_version`, "
        "`offer1`, `offer2`, `offer3`, `selected_service`, `state` "
        "FROM `character_class_deck_draft` WHERE `guid`='%u' "
        "ORDER BY `draft_index`",
        guid);
    if (!result)
        return drafts;
    do
    {
        Field* fields = result->Fetch();
        ClassDeckDraft draft;
        draft.index = fields[0].GetUInt32();
        draft.earnedLevel = fields[1].GetUInt32();
        draft.catalogVersion = fields[2].GetCppString();
        draft.offers = {{
            fields[3].GetUInt32(), fields[4].GetUInt32(), fields[5].GetUInt32()}};
        draft.selectedService = fields[6].GetUInt32();
        draft.state = ClassDeckDraftState(fields[7].GetUInt8());
        drafts.push_back(draft);
    }
    while (result->NextRow());
    return drafts;
}

std::vector<TrainerSpell> ClassDeckMgr::EligibleServices(Player* player) const
{
    std::vector<TrainerSpell> eligible;
    if (player->GetClass() >= m_catalog.size())
        return eligible;
    for (TrainerSpell const& service : m_catalog[player->GetClass()])
        if (player->GetTrainerSpellState(&service) == TRAINER_SPELL_GREEN)
            eligible.push_back(service);
    return eligible;
}

std::array<uint32, 3> ClassDeckMgr::SampleOffers(
    Player const* player,
    uint32 draftIndex,
    std::vector<TrainerSpell> eligible) const
{
    uint32 seed = m_testSeedSet
        ? m_testSeed
        : randu32();
    seed ^= player->GetGUIDLow() * 0x9e3779b9u;
    seed ^= draftIndex * 0x85ebca6bu;
    std::mt19937 random(seed);
    std::shuffle(eligible.begin(), eligible.end(), random);
    std::array<uint32, 3> offers = {{0, 0, 0}};
    for (size_t index = 0; index < offers.size() && index < eligible.size(); ++index)
        offers[index] = eligible[index].spell;
    return offers;
}

bool ClassDeckMgr::EnsureNextDraft(
    Player* player, std::vector<ClassDeckDraft> const& drafts)
{
    uint32 earned = EarnedDraftCount(player->GetLevel());
    for (ClassDeckDraft const& draft : drafts)
        if (draft.state != CLASS_DECK_DRAFT_APPLIED)
            return false;
    uint32 nextIndex = drafts.empty() ? 1 : drafts.back().index + 1;
    if (nextIndex > earned)
        return false;
    std::vector<TrainerSpell> eligible = EligibleServices(player);
    if (eligible.empty())
        return false;
    std::array<uint32, 3> offers = SampleOffers(player, nextIndex, eligible);
    return CharacterDatabase.DirectPExecute(
        "INSERT INTO `character_class_deck_draft` "
        "(`guid`, `draft_index`, `earned_level`, `catalog_version`, "
        "`offer1`, `offer2`, `offer3`, `selected_service`, `state`) "
        "VALUES ('%u', '%u', '%u', '%s', '%u', '%u', '%u', '0', '0')",
        player->GetGUIDLow(),
        nextIndex,
        EarnedLevelForDraft(nextIndex),
        m_catalogVersion.c_str(),
        offers[0],
        offers[1],
        offers[2]);
}

bool ClassDeckMgr::BlockPending(Player* player, ClassDeckDraft const& draft)
{
    return CharacterDatabase.DirectPExecute(
        "UPDATE `character_class_deck_draft` SET `state`='3' "
        "WHERE `guid`='%u' AND `draft_index`='%u' AND `state`='0'",
        player->GetGUIDLow(), draft.index);
}

ClassDeckApplyResult ClassDeckMgr::ApplySelected(
    Player* player, ClassDeckDraft const& draft)
{
    if (!player->IsAlive() || player->IsInCombat() ||
        player->IsNonMeleeSpellCasted(false))
        return CLASS_DECK_APPLY_DEFERRED;
    TrainerSpell const* service = FindService(
        player->GetClass(), draft.selectedService);
    if (!service)
    {
        CharacterDatabase.DirectPExecute(
            "UPDATE `character_class_deck_draft` SET `state`='3' "
            "WHERE `guid`='%u' AND `draft_index`='%u' AND `state`='1'",
            player->GetGUIDLow(), draft.index);
        return CLASS_DECK_APPLY_BLOCKED;
    }

    std::vector<uint32> taught = TaughtSpells(service->spell);
    if (taught.empty())
    {
        CharacterDatabase.DirectPExecute(
            "UPDATE `character_class_deck_draft` SET `state`='3' "
            "WHERE `guid`='%u' AND `draft_index`='%u' AND `state`='1'",
            player->GetGUIDLow(), draft.index);
        return CLASS_DECK_APPLY_BLOCKED;
    }
    bool allKnown = true;
    std::vector<bool> knownBefore;
    for (uint32 spell : taught)
    {
        knownBefore.push_back(player->HasSpell(spell));
        if (!player->HasSpell(spell))
            allKnown = false;
    }
    if (!allKnown)
    {
        player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        SpellEntry const* serviceSpell = sSpellMgr.GetSpellEntry(service->spell);
        Spell* teachingSpell = new Spell(player, serviceSpell, false);
        SpellCastTargets targets;
        targets.setUnitTarget(player);
        SpellCastResult castResult = teachingSpell->prepare(std::move(targets));
        teachingSpell->update(1);
        if (castResult != SPELL_CAST_OK)
        {
            bool learnedAny = false;
            for (size_t index = 0; index < taught.size(); ++index)
                if (!knownBefore[index] && player->HasSpell(taught[index]))
                    learnedAny = true;
            if (learnedAny)
                return CLASS_DECK_APPLY_DEFERRED;
            CharacterDatabase.DirectPExecute(
                "UPDATE `character_class_deck_draft` "
                "SET `selected_service`='0', `state`='0' "
                "WHERE `guid`='%u' AND `draft_index`='%u' AND `state`='1'",
                player->GetGUIDLow(), draft.index);
            return CLASS_DECK_APPLY_RETRYABLE;
        }
        for (uint32 spell : taught)
        {
            if (!player->HasSpell(spell))
            {
                return CLASS_DECK_APPLY_DEFERRED;
            }
        }
    }
    return player->SaveClassDeckSpellsAndState(draft.index)
        ? CLASS_DECK_APPLY_APPLIED
        : CLASS_DECK_APPLY_DEFERRED;
}

void ClassDeckMgr::Reconcile(Player* player)
{
    if (!m_enabled || !player)
        return;
    std::vector<ClassDeckDraft> drafts = LoadDrafts(player->GetGUIDLow());
    for (ClassDeckDraft const& draft : drafts)
    {
        if (draft.state == CLASS_DECK_DRAFT_SELECTED)
        {
            ApplySelected(player, draft);
            break;
        }
    }
    drafts = LoadDrafts(player->GetGUIDLow());
    for (ClassDeckDraft const& draft : drafts)
    {
        if (draft.state == CLASS_DECK_DRAFT_PENDING)
        {
            bool valid = true;
            for (uint32 offer : draft.offers)
            {
                if (offer != 0 &&
                    (!FindService(player->GetClass(), offer) ||
                     TaughtSpells(offer).empty()))
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
                BlockPending(player, draft);
            break;
        }
    }
    drafts = LoadDrafts(player->GetGUIDLow());
    if (EnsureNextDraft(player, drafts))
        drafts = LoadDrafts(player->GetGUIDLow());
}

void ClassDeckMgr::OnLevelChanged(Player* player)
{
    if (!m_enabled || !player)
        return;
    Reconcile(player);
    WorldSession* session = player->GetSession();
    if (session && session->IsClassDeckCapable())
        SendState(session, false);
}

void ClassDeckMgr::SendState(WorldSession* session, bool reconcile)
{
    if (!m_enabled || !session || !session->IsClassDeckCapable() ||
        !session->GetPlayer())
        return;
    Player* player = session->GetPlayer();
    if (reconcile)
        Reconcile(player);
    std::vector<ClassDeckDraft> drafts = LoadDrafts(player->GetGUIDLow());
    ClassDeckDraft const* active = nullptr;
    uint32 applied = 0;
    for (ClassDeckDraft const& draft : drafts)
    {
        if (draft.state == CLASS_DECK_DRAFT_APPLIED)
            ++applied;
        else if (!active)
            active = &draft;
    }
    uint32 earned = EarnedDraftCount(player->GetLevel());
    uint32 credits = earned > applied ? earned - applied : 0;

    WorldPacket data(SMSG_COWORLD_CLASS_DECK_STATE, 256);
    data << uint8(1);
    data << uint8(player->GetLevel());
    data << uint8(std::min<uint32>(credits, 29));
    uint8 status = 0;
    if (active)
    {
        if (active->state == CLASS_DECK_DRAFT_PENDING)
            status = 1;
        else if (active->state == CLASS_DECK_DRAFT_SELECTED)
            status = 2;
        else if (active->state == CLASS_DECK_DRAFT_BLOCKED)
            status = 3;
    }
    else if (credits > 0)
        status = 4;
    data << status;
    data << uint32(active ? active->index : applied + 1);
    data << uint32(active ? active->earnedLevel : EarnedLevelForDraft(applied + 1));
    data << (active ? active->catalogVersion : m_catalogVersion);

    uint8 offerCount = 0;
    if (active && active->state == CLASS_DECK_DRAFT_PENDING)
        for (uint32 offer : active->offers)
            if (offer != 0)
                ++offerCount;
    data << offerCount;
    if (active && active->state == CLASS_DECK_DRAFT_PENDING)
    {
        for (uint32 offer : active->offers)
        {
            if (!offer)
                continue;
            data << offer;
            std::vector<uint32> taught = TaughtSpells(offer);
            data << uint8(std::min<size_t>(taught.size(), 255));
            for (uint32 spell : taught)
                data << spell;
        }
    }

    data << uint8(std::min<size_t>(drafts.size(), 29));
    for (size_t index = 0; index < drafts.size() && index < 29; ++index)
    {
        data << drafts[index].index;
        data << drafts[index].earnedLevel;
        uint8 storedOfferCount = 0;
        for (uint32 offer : drafts[index].offers)
            if (offer != 0)
                ++storedOfferCount;
        data << storedOfferCount;
        for (uint32 offer : drafts[index].offers)
            if (offer != 0)
                data << offer;
        data << drafts[index].selectedService;
        data << uint8(drafts[index].state);
    }
    session->SendPacket(&data);
}

void ClassDeckMgr::SendResult(
    WorldSession* session,
    uint32 draftIndex,
    uint32 serviceSpell,
    ClassDeckResultCode result) const
{
    WorldPacket data(SMSG_COWORLD_CLASS_DECK_RESULT, 10);
    data << uint8(1);
    data << draftIndex;
    data << serviceSpell;
    data << uint8(result);
    session->SendPacket(&data);
}

void ClassDeckMgr::Choose(
    WorldSession* session,
    uint8 protocolVersion,
    uint32 draftIndex,
    uint32 serviceSpell,
    std::string const& catalogVersion)
{
    if (!session || !session->GetPlayer())
        return;
    if (!m_enabled)
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_DISABLED);
        return;
    }
    if (!session->IsClassDeckCapable())
        return;
    if (protocolVersion != 1)
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_MALFORMED);
        SendState(session, false);
        return;
    }
    Player* player = session->GetPlayer();
    Reconcile(player);
    std::vector<ClassDeckDraft> drafts = LoadDrafts(player->GetGUIDLow());
    ClassDeckDraft const* pending = nullptr;
    for (ClassDeckDraft const& draft : drafts)
        if (draft.state != CLASS_DECK_DRAFT_APPLIED)
        {
            pending = &draft;
            break;
        }
    if (!pending || pending->state != CLASS_DECK_DRAFT_PENDING ||
        pending->index != draftIndex ||
        pending->catalogVersion != catalogVersion)
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_STALE);
        SendState(session, false);
        return;
    }
    if (std::find(pending->offers.begin(), pending->offers.end(), serviceSpell) ==
        pending->offers.end())
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_UNAVAILABLE);
        SendState(session, false);
        return;
    }
    TrainerSpell const* service = FindService(player->GetClass(), serviceSpell);
    std::vector<uint32> const taught = service
        ? TaughtSpells(service->spell)
        : std::vector<uint32>();
    bool const allTaughtKnown = !taught.empty() && std::all_of(
        taught.begin(), taught.end(),
        [player](uint32 spell) { return player->HasSpell(spell); });
    if (!service ||
        (player->GetTrainerSpellState(service) != TRAINER_SPELL_GREEN &&
         !allTaughtKnown))
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_UNAVAILABLE);
        SendState(session, false);
        return;
    }
    if (!player->IsAlive() || player->IsInCombat() ||
        player->IsNonMeleeSpellCasted(false))
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_BUSY);
        SendState(session, false);
        return;
    }

    bool selectedWritten = CharacterDatabase.DirectPExecute(
        "UPDATE `character_class_deck_draft` SET `selected_service`='%u', "
        "`state`='1' WHERE `guid`='%u' AND `draft_index`='%u' AND `state`='0'",
        serviceSpell, player->GetGUIDLow(), draftIndex);
    std::vector<ClassDeckDraft> selectedDrafts = LoadDrafts(player->GetGUIDLow());
    auto selectedRow = std::find_if(
        selectedDrafts.begin(), selectedDrafts.end(),
        [draftIndex](ClassDeckDraft const& candidate)
        {
            return candidate.index == draftIndex;
        });
    if (!selectedWritten || selectedRow == selectedDrafts.end() ||
        selectedRow->state != CLASS_DECK_DRAFT_SELECTED ||
        selectedRow->selectedService != serviceSpell)
    {
        SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_UNAVAILABLE);
        SendState(session);
        return;
    }
    if (m_interruptAfterSelectOnce && !m_interruptConsumed)
    {
        m_interruptConsumed = true;
        sLog.Out(
            LOG_BASIC,
            LOG_LVL_BASIC,
            "Coworld class deck test interruption after selecting draft %u for guid %u.",
            draftIndex,
            player->GetGUIDLow());
        SendState(session, false);
        session->KickPlayer();
        return;
    }

    ClassDeckDraft selected = *pending;
    selected.selectedService = serviceSpell;
    selected.state = CLASS_DECK_DRAFT_SELECTED;
    ClassDeckApplyResult const applied = ApplySelected(player, selected);
    if (applied != CLASS_DECK_APPLY_APPLIED)
    {
        ClassDeckResultCode result = CLASS_DECK_RESULT_DEFERRED;
        if (applied == CLASS_DECK_APPLY_RETRYABLE)
            result = CLASS_DECK_RESULT_BUSY;
        else if (applied == CLASS_DECK_APPLY_BLOCKED)
            result = CLASS_DECK_RESULT_BLOCKED;
        SendResult(session, draftIndex, serviceSpell, result);
        SendState(session, false);
        return;
    }
    SendResult(session, draftIndex, serviceSpell, CLASS_DECK_RESULT_APPLIED);
    Reconcile(player);
    SendState(session, false);
}

void WorldSession::HandleClassDeckHello(
    WorldPackets::ClassDeck::Hello const& packet)
{
    if (!sClassDeckMgr.IsEnabled() || packet.protocolVersion != 1)
        return;
    SetClassDeckCapable(true);
    sClassDeckMgr.SendState(this);
}

void WorldSession::HandleClassDeckChoose(
    WorldPackets::ClassDeck::Choose const& packet)
{
    sClassDeckMgr.Choose(
        this,
        packet.protocolVersion,
        packet.draftIndex,
        packet.serviceSpell,
        packet.catalogVersion);
}
