#include "MarketStallMgr.h"

#include "AuctionHouse/AuctionHouseMgr.h"
#include "Config/Config.h"
#include "Creature.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellDefines.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include "Server/Packets/Trade.h"
#include "Policies/SingletonImp.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

INSTANTIATE_SINGLETON_1(MarketStallMgr);

namespace
{
    uint16 const MARKET_STALL_EVENT = 153;
    uint32 const MARKET_STALL_GOSSIP_SENDER = 0x4d53544c;
    uint32 const ACTION_CLAIM = 1;
    uint32 const ACTION_RENEW = 2;
    uint32 const ACTION_RELEASE = 3;
    uint32 const ACTION_GOODS = 4;
    uint32 const ACTION_MANAGE_OFFERS = 5;
    uint32 const ACTION_BROWSE_OFFERS = 6;
    uint32 const ACTION_MANAGE_CRAFT = 7;
    uint32 const ACTION_BROWSE_CRAFT = 8;
    uint32 const ACTION_CLAIM_CATEGORY = 100;
    uint32 const ACTION_MANAGE_PAGE = 1000;
    uint32 const ACTION_BROWSE_PAGE = 2000;
    uint32 const ACTION_MANAGE_CRAFT_PAGE = 3000;
    uint32 const ACTION_BROWSE_CRAFT_PAGE = 4000;
    uint32 const ACTION_ADD_SPELL = 100000;
    uint32 const ACTION_REMOVE_OFFER = 200000;
    uint32 const ACTION_SELECT_OFFER = 300000;
    uint32 const ACTION_ADD_CRAFT = 400000;
    uint32 const ACTION_SELECT_CRAFT = 500000;
    uint32 const LEASE_SECONDS = 24 * HOUR;
    uint32 const RENEW_WINDOW_SECONDS = 6 * HOUR;
    uint32 const CLAIM_COOLDOWN_SECONDS = 6 * HOUR;
    uint32 const CRAFT_REQUEST_COOLDOWN_SECONDS = 10;
    uint32 const OFFER_VALIDATION_SECONDS = 5 * MINUTE;
    uint32 const LEASE_FEE_COPPER = 25 * SILVER;
    uint32 const GOODS_LIMIT = 12;
    uint32 const SERVICE_LIMIT = 8;
    uint32 const OFFER_PAGE_SIZE = 20;
    uint32 const MIN_OWNER_LEVEL = 10;
    uint32 const MAX_PRICE = 2000000000;
    uint32 const MUTATION_INTERVAL_MS = 500;
    uint8 const SERVICE_ENCHANT = 0;
    uint8 const SERVICE_CRAFT_ADVERT = 1;
    uint8 const CATEGORY_COUNT = 6;

    std::string MoneyText(uint32 copper)
    {
        std::ostringstream text;
        text << copper / 10000 << "g " << (copper / 100) % 100 << "s " << copper % 100 << "c";
        return text.str();
    }

    std::string DurationText(uint64 seconds)
    {
        std::ostringstream text;
        uint64 const days = seconds / DAY;
        uint64 const hours = (seconds % DAY) / HOUR;
        uint64 const minutes = (seconds % HOUR) / MINUTE;
        if (days)
            text << days << "d ";
        if (hours || days)
            text << hours << "h ";
        text << (minutes ? minutes : 1) << "m";
        return text.str();
    }

    bool ParsePrice(char const* code, uint32& price)
    {
        if (!code || !*code)
            return false;
        errno = 0;
        char* end = nullptr;
        unsigned long value = std::strtoul(code, &end, 10);
        if (errno || !end || *end || value < 1 || value > MAX_PRICE)
            return false;
        price = uint32(value);
        return true;
    }
}

MarketStallMgr::MarketStallMgr()
    : m_enabled(false), m_nextOrderNonce(1), m_nextOfferValidationAt(0)
{
}

bool MarketStallMgr::IsStallGuid(uint32 stallGuid) const
{
    return m_pads.find(stallGuid) != m_pads.end();
}

uint32 MarketStallMgr::GetStallGuid(ObjectGuid auctioneerGuid) const
{
    if (!auctioneerGuid.IsCreature())
        return 0;
    std::map<uint32, uint32>::const_iterator stall = m_stallByClerk.find(auctioneerGuid.GetCounter());
    return stall == m_stallByClerk.end() ? 0 : stall->second;
}

MarketStallClaim const* MarketStallMgr::FindClaim(uint32 stallGuid) const
{
    ClaimMap::const_iterator itr = m_claims.find(stallGuid);
    return itr == m_claims.end() ? nullptr : &itr->second;
}

MarketStallClaim const* MarketStallMgr::FindAccountClaim(uint32 accountId) const
{
    for (ClaimMap::const_iterator itr = m_claims.begin(); itr != m_claims.end(); ++itr)
        if (itr->second.ownerAccountId == accountId)
            return &itr->second;
    return nullptr;
}

MarketStallOffer const* MarketStallMgr::FindOffer(uint32 stallGuid, uint32 offerId) const
{
    OfferMap::const_iterator list = m_offers.find(stallGuid);
    if (list == m_offers.end())
        return nullptr;
    for (std::vector<MarketStallOffer>::const_iterator itr = list->second.begin(); itr != list->second.end(); ++itr)
        if (itr->offerId == offerId)
            return &*itr;
    return nullptr;
}

MarketStallOrder* MarketStallMgr::FindOrder(uint32 playerGuid)
{
    std::map<uint32, uint32>::const_iterator owner = m_orderByPlayer.find(playerGuid);
    if (owner == m_orderByPlayer.end())
        return nullptr;
    std::map<uint32, MarketStallOrder>::iterator order = m_orders.find(owner->second);
    return order == m_orders.end() ? nullptr : &order->second;
}

MarketStallOrder const* MarketStallMgr::FindOrder(uint32 playerGuid) const
{
    std::map<uint32, uint32>::const_iterator owner = m_orderByPlayer.find(playerGuid);
    if (owner == m_orderByPlayer.end())
        return nullptr;
    std::map<uint32, MarketStallOrder>::const_iterator order = m_orders.find(owner->second);
    return order == m_orders.end() ? nullptr : &order->second;
}

bool MarketStallMgr::IsClaimActive(MarketStallClaim const* claim) const
{
    return claim && !claim->closing && claim->leaseExpiresAt > uint64(sWorld.GetGameTime());
}

bool MarketStallMgr::IsPlayerNearPad(Player const* player, uint32 stallGuid) const
{
    PadMap::const_iterator pad = m_pads.find(stallGuid);
    return pad != m_pads.end() && player->GetMapId() == pad->second.mapId &&
           player->GetDistance(pad->second.x, pad->second.y, pad->second.z) <= pad->second.radius;
}

bool MarketStallMgr::IsOfferSpell(uint32 spellId) const
{
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    if (!spell)
        return false;
    bool permanent = false;
    for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
    {
        if (spell->Effect[effect] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
            return false;
        if (spell->Effect[effect] == SPELL_EFFECT_ENCHANT_ITEM)
            permanent = true;
    }
    return permanent;
}

bool MarketStallMgr::IsCraftSpell(uint32 spellId) const
{
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    if (!spell)
        return false;
    bool createsItem = false;
    for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        if (spell->Effect[effect] == SPELL_EFFECT_CREATE_ITEM)
            createsItem = true;
    if (!createsItem)
        return false;

    SkillLineAbilityMapBounds bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
    for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        switch (itr->second->skillId)
        {
            case SKILL_ALCHEMY:
            case SKILL_BLACKSMITHING:
            case SKILL_ENGINEERING:
            case SKILL_LEATHERWORKING:
            case SKILL_TAILORING:
            case SKILL_COOKING:
            case SKILL_FIRST_AID:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool MarketStallMgr::HasActiveStock(uint32 stallGuid) const
{
    if (sAuctionMgr.CountActiveMarketStallAuctions(stallGuid) > 0)
        return true;
    OfferMap::const_iterator offers = m_offers.find(stallGuid);
    if (offers == m_offers.end())
        return false;
    for (std::vector<MarketStallOffer>::const_iterator offer = offers->second.begin();
         offer != offers->second.end(); ++offer)
        if (IsOfferCurrent(*offer))
            return true;
    return false;
}

bool MarketStallMgr::HasPendingLeaseMutation(uint32 accountId) const
{
    return m_pendingLeaseAccounts.find(accountId) != m_pendingLeaseAccounts.end();
}

bool MarketStallMgr::IsOfferCurrent(MarketStallOffer const& offer) const
{
    bool const validSpell = offer.serviceKind == SERVICE_CRAFT_ADVERT ?
        IsCraftSpell(offer.spellId) : IsOfferSpell(offer.spellId);
    if (!validSpell)
        return false;
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer.sellerGuid));
    if (seller)
        return seller->HasSpell(offer.spellId);
    std::unique_ptr<QueryResult> known = CharacterDatabase.PQuery(
        "SELECT 1 FROM `character_spell` WHERE `guid`='%u' AND `spell`='%u' AND `active`='1' LIMIT 1",
        offer.sellerGuid, offer.spellId);
    return known != nullptr;
}

bool MarketStallMgr::GetClaimCooldown(uint32 accountId, uint64& eligibleAt) const
{
    std::unique_ptr<QueryResult> cooldown = CharacterDatabase.PQuery(
        "SELECT COALESCE(MAX(`eligible_at`),0) FROM `market_stall_claim_cooldown` "
        "WHERE `owner_account_id`='%u'",
        accountId);
    if (!cooldown)
        return false;
    eligibleAt = cooldown->Fetch()[0].GetUInt64();
    return true;
}

char const* MarketStallMgr::CategoryName(uint8 category) const
{
    static char const* names[CATEGORY_COUNT] = {
        "General Goods", "Equipment", "Consumables", "Materials", "Enchanting", "Crafting Services"
    };
    return category < CATEGORY_COUNT ? names[category] : names[0];
}

std::string MarketStallMgr::StallName(MarketStallClaim const& claim) const
{
    std::string ownerName = "Unknown";
    sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, claim.ownerGuid), ownerName);
    char label[160];
    snprintf(label, sizeof(label), "%s's %s Stall", ownerName.c_str(), CategoryName(claim.category));
    return label;
}

bool MarketStallMgr::ReloadClaims()
{
    ClaimMap loaded;
    std::unique_ptr<QueryResult> result = CharacterDatabase.Query(
        "SELECT `stall_guid`, `owner_account_id`, `owner_guid`, `category`, `claimed_at`, "
        "`lease_expires_at`, `state` FROM `market_stall`");
    if (!result)
    {
        std::unique_ptr<QueryResult> count = CharacterDatabase.Query("SELECT COUNT(*) FROM `market_stall`");
        if (!count || count->Fetch()[0].GetUInt32() != 0)
            return false;
        m_claims.swap(loaded);
        return true;
    }
    do
    {
        Field* fields = result->Fetch();
        MarketStallClaim claim;
        claim.stallGuid = fields[0].GetUInt32();
        claim.ownerAccountId = fields[1].GetUInt32();
        claim.ownerGuid = fields[2].GetUInt32();
        claim.category = fields[3].GetUInt8();
        claim.claimedAt = fields[4].GetUInt64();
        claim.leaseExpiresAt = fields[5].GetUInt64();
        claim.closing = fields[6].GetCppString() == "closing";
        loaded[claim.stallGuid] = claim;
    }
    while (result->NextRow());
    m_claims.swap(loaded);
    return true;
}

bool MarketStallMgr::ReloadOffers()
{
    OfferMap loaded;
    std::unique_ptr<QueryResult> result = CharacterDatabase.Query(
        "SELECT `stall_guid`, `offer_id`, `service_kind`, `seller_guid`, `spell_id`, `price_copper` "
        "FROM `market_stall_service` ORDER BY `stall_guid`, `offer_id`");
    if (!result)
    {
        std::unique_ptr<QueryResult> count = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `market_stall_service`");
        if (!count || count->Fetch()[0].GetUInt32() != 0)
            return false;
        m_offers.swap(loaded);
        return true;
    }
    do
    {
        Field* fields = result->Fetch();
        MarketStallOffer offer;
        offer.stallGuid = fields[0].GetUInt32();
        offer.offerId = fields[1].GetUInt32();
        offer.serviceKind = fields[2].GetCppString() == "craft_advert" ? SERVICE_CRAFT_ADVERT : SERVICE_ENCHANT;
        offer.sellerGuid = fields[3].GetUInt32();
        offer.spellId = fields[4].GetUInt32();
        offer.priceCopper = fields[5].GetUInt32();
        loaded[offer.stallGuid].push_back(offer);
    }
    while (result->NextRow());
    m_offers.swap(loaded);
    return true;
}

void MarketStallMgr::PruneInvalidOffers()
{
    if (m_offers.empty())
        return;
    std::set<std::pair<uint32, uint32> > persistedSpells;
    std::unique_ptr<QueryResult> known = CharacterDatabase.Query(
        "SELECT `service`.`seller_guid`, `service`.`spell_id`, IF(`known`.`spell` IS NULL,0,1) "
        "FROM `market_stall_service` AS `service` LEFT JOIN `character_spell` AS `known` "
        "ON `known`.`guid`=`service`.`seller_guid` AND `known`.`spell`=`service`.`spell_id` "
        "AND `known`.`active`='1'");
    if (!known)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] offer-prune deferred reason=spell-readback-failed");
        return;
    }
    do
    {
        Field* fields = known->Fetch();
        if (fields[2].GetBool())
            persistedSpells.insert(std::make_pair(fields[0].GetUInt32(), fields[1].GetUInt32()));
    }
    while (known->NextRow());

    std::vector<std::pair<uint32, uint32> > invalid;
    for (OfferMap::const_iterator list = m_offers.begin(); list != m_offers.end(); ++list)
        for (std::vector<MarketStallOffer>::const_iterator offer = list->second.begin();
             offer != list->second.end(); ++offer)
        {
            bool const validSpell = offer->serviceKind == SERVICE_CRAFT_ADVERT ?
                IsCraftSpell(offer->spellId) : IsOfferSpell(offer->spellId);
            Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer->sellerGuid));
            bool const sellerKnows = seller ? seller->HasSpell(offer->spellId) :
                persistedSpells.count(std::make_pair(offer->sellerGuid, offer->spellId)) != 0;
            if (!IsClaimActive(FindClaim(offer->stallGuid)) || !validSpell || !sellerKnows)
                invalid.push_back(std::make_pair(offer->stallGuid, offer->offerId));
        }
    if (invalid.empty())
        return;

    std::vector<uint32> changedStalls;
    for (std::vector<std::pair<uint32, uint32> >::const_iterator itr = invalid.begin();
         itr != invalid.end(); ++itr)
    {
        uint32 const stallGuid = itr->first;
        uint32 const offerId = itr->second;
        if (!CharacterDatabase.DirectPExecute(
            "DELETE FROM `market_stall_service` WHERE `stall_guid`='%u' AND `offer_id`='%u'",
            stallGuid, offerId))
            continue;
        CancelOrders(stallGuid, offerId, "offer-invalid");
        changedStalls.push_back(stallGuid);
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                 "[MarketStall] offer-prune stall=%u offer=%u reason=stale-or-invalid",
                 stallGuid, offerId);
    }
    if (!ReloadOffers())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] offer-prune refresh failed; retaining prior live catalogue");
        return;
    }
    if (m_enabled && sGameEventMgr.IsActiveEvent(MARKET_STALL_EVENT))
        for (std::vector<uint32>::const_iterator stall = changedStalls.begin();
             stall != changedStalls.end(); ++stall)
            SyncAssets(*stall);
}

bool MarketStallMgr::AllowMutation(Player* player)
{
    if (HasPendingLeaseMutation(player->GetSession()->GetAccountId()))
    {
        player->SendSysMessage("Your previous market-stall change is still being saved.");
        return false;
    }
    uint32 const now = WorldTimer::getMSTime();
    uint32& last = m_lastMutationAt[player->GetGUIDLow()];
    if (last && WorldTimer::getMSTimeDiff(last, now) < MUTATION_INTERVAL_MS)
    {
        player->SendSysMessage("Please wait before changing this market stall again.");
        return false;
    }
    last = now;
    return true;
}

bool MarketStallMgr::QueueLeaseMutationBarrier(uint32 playerGuid, uint32 accountId)
{
    SqlQueryHolder* holder = new SqlQueryHolder(playerGuid);
    holder->SetSize(1);
    if (!holder->SetQuery(0, "SELECT 1"))
    {
        delete holder;
        return false;
    }
    m_pendingLeaseAccounts.insert(accountId);
    m_pendingLeasePlayers[playerGuid] = accountId;
    if (!CharacterDatabase.DelayQueryHolderUnsafe(
            this, &MarketStallMgr::OnLeaseMutationSettled, holder))
    {
        delete holder;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] lease mutation barrier could not be queued for account=%u", accountId);
        return false;
    }
    return true;
}

void MarketStallMgr::OnLeaseMutationSettled(
    std::unique_ptr<QueryResult>, SqlQueryHolder* holder)
{
    uint32 const playerGuid = holder ? holder->GetSerialId() : 0;
    std::map<uint32, uint32>::iterator pending = m_pendingLeasePlayers.find(playerGuid);
    if (pending != m_pendingLeasePlayers.end())
    {
        m_pendingLeaseAccounts.erase(pending->second);
        m_pendingLeasePlayers.erase(pending);
    }
    if (holder)
    {
        holder->DeleteAllResults();
        delete holder;
    }
}

void MarketStallMgr::SetAssetVisible(MarketStallAsset& asset, bool visible)
{
    if (asset.visible == visible)
        return;
    GameObjectData const* data = sObjectMgr.GetGOData(asset.assetGuid);
    if (!data)
        return;
    if (visible)
    {
        sObjectMgr.AddGameobjectToGrid(asset.assetGuid, data);
        GameObject::SpawnInMaps(asset.assetGuid, data);
    }
    else
    {
        sObjectMgr.RemoveGameobjectFromGrid(asset.assetGuid, data);
        GameObject::AddToRemoveListInMaps(asset.assetGuid, data);
    }
    asset.visible = visible;
}

void MarketStallMgr::SyncAssets(uint32 stallGuid)
{
    if (!m_enabled)
        return;
    bool const claimed = IsClaimActive(FindClaim(stallGuid));
    bool const goods = claimed && sAuctionMgr.CountActiveMarketStallAuctions(stallGuid) > 0;
    bool enchant = false;
    bool craft = false;
    OfferMap::const_iterator offers = m_offers.find(stallGuid);
    if (claimed && offers != m_offers.end())
        for (std::vector<MarketStallOffer>::const_iterator offer = offers->second.begin(); offer != offers->second.end(); ++offer)
            if (offer->serviceKind == SERVICE_CRAFT_ADVERT)
                craft = true;
            else
                enchant = true;

    for (AssetMap::iterator itr = m_assets.begin(); itr != m_assets.end(); ++itr)
    {
        MarketStallAsset& asset = itr->second;
        if (asset.stallGuid != stallGuid)
            continue;
        bool visible = asset.role == "banner" || asset.role == "counter";
        if (asset.role == "claimed")
            visible = claimed;
        else if (asset.role == "goods")
            visible = goods;
        else if (asset.role == "enchant")
            visible = enchant;
        else if (asset.role == "craft")
            visible = craft;
        SetAssetVisible(asset, visible);
    }
}

void MarketStallMgr::SyncAllAssets()
{
    for (PadMap::const_iterator pad = m_pads.begin(); pad != m_pads.end(); ++pad)
        SyncAssets(pad->first);
}

void MarketStallMgr::OnAuctionChanged(uint32 stallGuid)
{
    if (stallGuid)
        SyncAssets(stallGuid);
}

void MarketStallMgr::Load()
{
    m_enabled = sConfig.GetBoolDefault("MarketStall.Enable", false);
    m_pads.clear();
    m_stallByClerk.clear();
    m_assets.clear();
    std::unique_ptr<QueryResult> result = WorldDatabase.Query(
        "SELECT `stall_guid`, `clerk_guid`, `map`, `position_x`, `position_y`, `position_z`, "
        "`interaction_radius` FROM `market_stall_pad`");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            MarketStallPad pad;
            pad.stallGuid = fields[0].GetUInt32();
            pad.clerkGuid = fields[1].GetUInt32();
            pad.mapId = fields[2].GetUInt32();
            pad.x = fields[3].GetFloat();
            pad.y = fields[4].GetFloat();
            pad.z = fields[5].GetFloat();
            pad.radius = fields[6].GetFloat();
            m_pads[pad.stallGuid] = pad;
            m_stallByClerk[pad.clerkGuid] = pad.stallGuid;
        }
        while (result->NextRow());
    }
    result = WorldDatabase.Query(
        "SELECT `asset_guid`, `stall_guid`, `role` FROM `market_stall_asset`");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            MarketStallAsset asset;
            asset.assetGuid = fields[0].GetUInt32();
            asset.stallGuid = fields[1].GetUInt32();
            asset.role = fields[2].GetCppString();
            asset.visible = false;
            m_assets[asset.assetGuid] = asset;
        }
        while (result->NextRow());
    }
    if (!ReloadClaims() || !ReloadOffers())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] load failed while reading persistent claims or offers");
        return;
    }
    PruneInvalidOffers();

    uint64 const now = uint64(sWorld.GetGameTime());
    m_nextOfferValidationAt = now + OFFER_VALIDATION_SECONDS;
    std::vector<std::pair<uint32, bool> > close;
    for (ClaimMap::const_iterator itr = m_claims.begin(); itr != m_claims.end(); ++itr)
    {
        ObjectGuid owner(HIGHGUID_PLAYER, itr->second.ownerGuid);
        if (!IsStallGuid(itr->first) || !sObjectMgr.GetPlayerAccountIdByGUID(owner))
            close.push_back(std::make_pair(itr->first, false));
        else if (itr->second.leaseExpiresAt <= now)
            close.push_back(std::make_pair(itr->first, true));
    }
    for (std::vector<std::pair<uint32, bool> >::const_iterator itr = close.begin(); itr != close.end(); ++itr)
        CloseClaim(itr->first, itr->second ? "lease-expired" : "startup-reconciliation", itr->second);

    sAuctionMgr.ExpireOrphanMarketStallAuctions();
    if (m_enabled)
    {
        if (!sGameEventMgr.IsActiveEvent(MARKET_STALL_EVENT))
            sGameEventMgr.StartEvent(MARKET_STALL_EVENT, false);
        for (AssetMap::iterator asset = m_assets.begin(); asset != m_assets.end(); ++asset)
            asset->second.visible = true;
        SyncAllAssets();
    }
    else if (sGameEventMgr.IsActiveEvent(MARKET_STALL_EVENT))
        sGameEventMgr.StopEvent(MARKET_STALL_EVENT, false);
    uint32 visibleBaseAssets = 0;
    uint32 visibleConditionalAssets = 0;
    for (AssetMap::const_iterator asset = m_assets.begin(); asset != m_assets.end(); ++asset)
    {
        if (!asset->second.visible)
            continue;
        if (asset->second.role == "banner" || asset->second.role == "counter")
            ++visibleBaseAssets;
        else
            ++visibleConditionalAssets;
    }
    uint32 const visibleClerks = m_enabled && sGameEventMgr.IsActiveEvent(MARKET_STALL_EVENT) ?
        uint32(m_pads.size()) : 0;
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[MarketStall] runtime reason=%s clerks=%u base_assets=%u conditional_assets=%u",
             "load", visibleClerks, visibleBaseAssets, visibleConditionalAssets);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] loaded %u pads, %u assets, %u claims, enabled=%u",
             uint32(m_pads.size()), uint32(m_assets.size()), uint32(m_claims.size()), uint32(m_enabled));
}

bool MarketStallMgr::CloseClaim(uint32 stallGuid, char const* reason, bool cooldown)
{
    ClaimMap::iterator claim = m_claims.find(stallGuid);
    if (claim == m_claims.end())
        return true;
    bool const wasClosing = claim->second.closing;
    uint32 const ownerAccountId = claim->second.ownerAccountId;
    uint32 const ownerGuid = claim->second.ownerGuid;
    uint64 const cooldownFrom = std::string(reason) == "lease-expired" ?
        claim->second.leaseExpiresAt : uint64(sWorld.GetGameTime());

    if (!CharacterDatabase.BeginTransaction())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] close-failed stall=%u owner=%u reason=transaction-start",
                 stallGuid, ownerGuid);
        return false;
    }
    CharacterDatabase.PExecute(
        "UPDATE `market_stall` SET `state`='closing' WHERE `stall_guid`='%u'", stallGuid);
    if (cooldown)
        CharacterDatabase.PExecute(
            "INSERT INTO `market_stall_claim_cooldown` (`owner_account_id`,`eligible_at`,`reason`) "
            "VALUES ('%u','" UI64FMTD "','%s') ON DUPLICATE KEY UPDATE "
            "`eligible_at`=GREATEST(`eligible_at`,VALUES(`eligible_at`)),`reason`=VALUES(`reason`)",
            ownerAccountId, cooldownFrom + CLAIM_COOLDOWN_SECONDS, reason);
    CharacterDatabase.PExecute(
        "DELETE FROM `market_stall_service` WHERE `stall_guid`='%u'", stallGuid);
    CharacterDatabase.CommitTransactionDirect();
    if (!ReloadClaims() || !ReloadOffers())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] close-failed stall=%u owner=%u reason=transaction-refresh",
                 stallGuid, ownerGuid);
        return false;
    }
    claim = m_claims.find(stallGuid);
    if (claim != m_claims.end() && !claim->second.closing)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] close-failed stall=%u owner=%u reason=transaction-readback",
                 stallGuid, ownerGuid);
        return false;
    }
    if (m_offers.find(stallGuid) != m_offers.end())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] close-failed stall=%u owner=%u reason=service-readback",
                 stallGuid, ownerGuid);
        return false;
    }
    if (cooldown)
    {
        uint64 cooldownUntil = 0;
        if (!GetClaimCooldown(ownerAccountId, cooldownUntil) ||
            cooldownUntil < cooldownFrom + CLAIM_COOLDOWN_SECONDS)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[MarketStall] close-failed stall=%u owner=%u reason=cooldown-readback",
                     stallGuid, ownerGuid);
            return false;
        }
    }
    if (!wasClosing)
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] closing stall=%u owner=%u reason=%s",
                 stallGuid, ownerGuid, reason);
    CancelOrders(stallGuid, 0, "lease-closing");
    SyncAssets(stallGuid);
    sAuctionMgr.ExpireMarketStallAuctions(stallGuid);
    return true;
}

void MarketStallMgr::Update()
{
    uint64 const now = uint64(sWorld.GetGameTime());
    if (now >= m_nextOfferValidationAt)
    {
        PruneInvalidOffers();
        m_nextOfferValidationAt = now + OFFER_VALIDATION_SECONDS;
    }
    uint32 const nowMs = WorldTimer::getMSTime();
    for (std::map<uint32, uint32>::iterator itr = m_lastMutationAt.begin(); itr != m_lastMutationAt.end();)
    {
        if (WorldTimer::getMSTimeDiff(itr->second, nowMs) >= MINUTE * IN_MILLISECONDS)
            m_lastMutationAt.erase(itr++);
        else
            ++itr;
    }
    for (std::map<uint32, uint64>::iterator itr = m_lastCraftRequestAt.begin();
         itr != m_lastCraftRequestAt.end();)
    {
        if (itr->second + MINUTE <= now)
            m_lastCraftRequestAt.erase(itr++);
        else
            ++itr;
    }
    std::vector<uint32> closing;
    for (ClaimMap::const_iterator itr = m_claims.begin(); itr != m_claims.end(); ++itr)
        if (itr->second.closing || itr->second.leaseExpiresAt <= now)
            closing.push_back(itr->first);
    for (std::vector<uint32>::const_iterator itr = closing.begin(); itr != closing.end(); ++itr)
    {
        bool const readyToFinalize = CloseClaim(*itr, "lease-expired", true);
        if (readyToFinalize && !sAuctionMgr.CountMarketStallAuctions(*itr))
        {
            if (CharacterDatabase.DirectPExecute(
                    "DELETE FROM `market_stall` WHERE `stall_guid`='%u' AND `state`='closing'", *itr))
            {
                if (ReloadClaims() && !FindClaim(*itr))
                {
                    SyncAssets(*itr);
                    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                             "[MarketStall] released stall=%u after closing", *itr);
                }
            }
        }
    }
    SyncAllAssets();
}

bool MarketStallMgr::CanOpenAuction(Player* player, uint32 stallGuid) const
{
    if (!stallGuid)
        return true;
    MarketStallClaim const* claim = FindClaim(stallGuid);
    return m_enabled && !HasPendingLeaseMutation(player->GetSession()->GetAccountId()) &&
        IsClaimActive(claim) && IsPlayerNearPad(player, stallGuid);
}

bool MarketStallMgr::ValidateAuctionListing(Player* player, uint32 stallGuid, uint32 bid,
                                             uint32 buyout, uint32 durationMinutes) const
{
    if (!stallGuid)
        return true;
    MarketStallClaim const* claim = FindClaim(stallGuid);
    bool const allowed = m_enabled && !HasPendingLeaseMutation(player->GetSession()->GetAccountId()) &&
        IsClaimActive(claim) &&
        claim->ownerAccountId == player->GetSession()->GetAccountId() &&
        bid > 0 && bid == buyout && durationMinutes == 24 * 60 &&
        sAuctionMgr.CountMarketStallAuctions(stallGuid) < GOODS_LIMIT;
    if (!allowed)
        player->SendSysMessage("That market-stall listing is no longer valid.");
    return allowed;
}

bool MarketStallMgr::CanAccessAuction(AuctionEntry const* auction, uint32 stallGuid) const
{
    if (!auction || auction->marketStallGuid != stallGuid)
        return false;
    return !stallGuid || (m_enabled && IsClaimActive(FindClaim(stallGuid)));
}

bool MarketStallMgr::IsAuctionVisible(AuctionEntry const* auction, uint32 stallGuid) const
{
    return CanAccessAuction(auction, stallGuid);
}

void MarketStallMgr::ShowMainMenu(Player* player, Creature* creature)
{
    player->PlayerTalkClass->ClearMenus();
    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    uint32 const stallGuid = GetStallGuid(creature->GetObjectGuid());
    MarketStallClaim const* claim = FindClaim(stallGuid);
    if (claim && !IsClaimActive(claim) && !claim->closing)
    {
        if (!CloseClaim(stallGuid, "lease-expired", true))
        {
            player->GetSession()->SendNotification(
                "This market stall is temporarily unavailable while its lease is reconciled.");
            player->PlayerTalkClass->SendGossipMenu(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
            return;
        }
        claim = FindClaim(stallGuid);
    }
    if (!claim)
    {
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Claim this stall - 25 silver / 24 hours", MARKET_STALL_GOSSIP_SENDER,
                         ACTION_CLAIM, "Level 10 required. One stall per account. Choose a fixed category next.");
        player->GetSession()->SendNotification("Vacant market stall: 12 goods, 8 enchants, and 8 craft adverts.");
    }
    else if (claim->closing)
        player->GetSession()->SendNotification("This market stall is closing while its goods are returned.");
    else if (claim->ownerAccountId == player->GetSession()->GetAccountId())
    {
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Manage goods", MARKET_STALL_GOSSIP_SENDER, ACTION_GOODS, "");
        menu.AddMenuItem(GOSSIP_ICON_TRAINER, "Manage enchanting offers", MARKET_STALL_GOSSIP_SENDER, ACTION_MANAGE_OFFERS, "");
        menu.AddMenuItem(GOSSIP_ICON_TRAINER, "Manage crafting adverts", MARKET_STALL_GOSSIP_SENDER, ACTION_MANAGE_CRAFT, "");
        uint64 const now = uint64(sWorld.GetGameTime());
        if (claim->leaseExpiresAt - now <= RENEW_WINDOW_SECONDS)
        {
            if (HasActiveStock(stallGuid))
                menu.AddMenuItem(GOSSIP_ICON_CHAT, "Renew for 24 hours - 25 silver", MARKET_STALL_GOSSIP_SENDER,
                                 ACTION_RENEW, "Extend this lease by one day?");
            else
                player->GetSession()->SendNotification(
                    "Add a live good, enchant, or craft advert before expiry to renew.");
        }
        menu.AddMenuItem(GOSSIP_ICON_CHAT, "Release stall", MARKET_STALL_GOSSIP_SENDER, ACTION_RELEASE, "Release requires no active goods.");
        player->GetSession()->SendNotification("%s is active. Lease remaining: %s.",
            StallName(*claim).c_str(), DurationText(claim->leaseExpiresAt - now).c_str());
    }
    else
    {
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Browse goods", MARKET_STALL_GOSSIP_SENDER, ACTION_GOODS, "");
        menu.AddMenuItem(GOSSIP_ICON_TRAINER, "Browse enchanting offers", MARKET_STALL_GOSSIP_SENDER, ACTION_BROWSE_OFFERS, "");
        menu.AddMenuItem(GOSSIP_ICON_TRAINER, "Browse crafting adverts", MARKET_STALL_GOSSIP_SENDER, ACTION_BROWSE_CRAFT, "");
        uint64 const now = uint64(sWorld.GetGameTime());
        player->GetSession()->SendNotification("%s. Lease remaining: %s.",
            StallName(*claim).c_str(), DurationText(claim->leaseExpiresAt - now).c_str());
    }
    player->PlayerTalkClass->SendGossipMenu(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
}

void MarketStallMgr::ShowCategoryMenu(Player* player, Creature* creature)
{
    player->PlayerTalkClass->ClearMenus();
    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    for (uint8 category = 0; category < CATEGORY_COUNT; ++category)
    {
        std::string label = std::string("Claim as ") + CategoryName(category);
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, label, MARKET_STALL_GOSSIP_SENDER,
                         ACTION_CLAIM_CATEGORY + category, "Pay 25 silver for this 24-hour lease?");
    }
    player->PlayerTalkClass->SendGossipMenu(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
}

void MarketStallMgr::ShowOfferMenu(Player* player, Creature* creature, bool manage, bool craft, uint32 page)
{
    player->PlayerTalkClass->ClearMenus();
    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    uint32 const stallGuid = GetStallGuid(creature->GetObjectGuid());
    uint8 const serviceKind = craft ? SERVICE_CRAFT_ADVERT : SERVICE_ENCHANT;
    if (manage)
    {
        OfferMap::const_iterator existing = m_offers.find(stallGuid);
        if (existing != m_offers.end())
            for (std::vector<MarketStallOffer>::const_iterator offer = existing->second.begin(); offer != existing->second.end(); ++offer)
            {
                if (offer->serviceKind != serviceKind)
                    continue;
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(offer->spellId);
                std::string label = std::string("Remove ") + (spell ? spell->SpellName[0] : "unknown service") +
                                    " - " + MoneyText(offer->priceCopper);
                menu.AddMenuItem(GOSSIP_ICON_TRAINER, label, MARKET_STALL_GOSSIP_SENDER,
                                 ACTION_REMOVE_OFFER + offer->offerId, "Remove this offer?");
            }

        std::vector<uint32> spells;
        PlayerSpellMap const& known = player->GetSpellMap();
        for (PlayerSpellMap::const_iterator itr = known.begin(); itr != known.end(); ++itr)
            if (player->HasSpell(itr->first) && (craft ? IsCraftSpell(itr->first) : IsOfferSpell(itr->first)))
                spells.push_back(itr->first);
        std::sort(spells.begin(), spells.end());
        uint32 const first = page * OFFER_PAGE_SIZE;
        uint32 const last = std::min<uint32>(uint32(spells.size()), first + OFFER_PAGE_SIZE);
        for (uint32 index = first; index < last; ++index)
        {
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(spells[index]);
            std::string label = std::string("Advertise ") + (spell ? spell->SpellName[0] : "service");
            menu.AddMenuItem(GOSSIP_ICON_TRAINER, label, MARKET_STALL_GOSSIP_SENDER,
                             (craft ? ACTION_ADD_CRAFT : ACTION_ADD_SPELL) + spells[index],
                             craft ? "Enter the asking price in copper; fulfillment is an ordinary trade."
                                   : "Enter the exact attended-enchant price in copper.", true);
        }
        if (last < spells.size())
            menu.AddMenuItem(GOSSIP_ICON_CHAT, "Next page", MARKET_STALL_GOSSIP_SENDER,
                             (craft ? ACTION_MANAGE_CRAFT_PAGE : ACTION_MANAGE_PAGE) + page + 1, "");
    }
    else
    {
        std::vector<MarketStallOffer const*> filtered;
        OfferMap::const_iterator list = m_offers.find(stallGuid);
        if (list != m_offers.end())
        {
            for (std::vector<MarketStallOffer>::const_iterator offer = list->second.begin(); offer != list->second.end(); ++offer)
                if (offer->serviceKind == serviceKind)
                    filtered.push_back(&*offer);
            uint32 const first = page * OFFER_PAGE_SIZE;
            uint32 const last = std::min<uint32>(uint32(filtered.size()), first + OFFER_PAGE_SIZE);
            for (uint32 index = first; index < last; ++index)
            {
                MarketStallOffer const& offer = *filtered[index];
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(offer.spellId);
                std::string label;
                if (craft)
                {
                    char advert[256];
                    std::string const spellName = spell ? spell->SpellName[0] : "Craft";
                    snprintf(advert, sizeof(advert), "%s - asking %s - arrange an ordinary trade",
                             spellName.c_str(), MoneyText(offer.priceCopper).c_str());
                    label = advert;
                }
                else
                    label = std::string(spell ? spell->SpellName[0] : "Enchant") + " - " + MoneyText(offer.priceCopper);
                label += IsSellerAvailable(offer, player) ? " - available" : " - unavailable";
                menu.AddMenuItem(GOSSIP_ICON_TRAINER, label, MARKET_STALL_GOSSIP_SENDER,
                                 (craft ? ACTION_SELECT_CRAFT : ACTION_SELECT_OFFER) + offer.offerId,
                                 craft ? "Notify this crafter? Payment and materials remain an ordinary trade."
                                       : "Open an attended enchant trade?");
            }
            if (last < filtered.size())
                menu.AddMenuItem(GOSSIP_ICON_CHAT, "Next page", MARKET_STALL_GOSSIP_SENDER,
                                 (craft ? ACTION_BROWSE_CRAFT_PAGE : ACTION_BROWSE_PAGE) + page + 1, "");
        }
    }
    player->PlayerTalkClass->SendGossipMenu(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
}

bool MarketStallMgr::HandleGossipHello(Player* player, Creature* creature)
{
    if (!creature || !GetStallGuid(creature->GetObjectGuid()))
        return false;
    if (!m_enabled)
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }
    ShowMainMenu(player, creature);
    return true;
}

void MarketStallMgr::Claim(Player* player, uint32 stallGuid, uint8 category)
{
    uint64 const now = uint64(sWorld.GetGameTime());
    uint32 const accountId = player->GetSession()->GetAccountId();
    if (category >= CATEGORY_COUNT)
    {
        player->SendSysMessage("That market-stall category is invalid.");
        return;
    }
    if (player->GetLevel() < MIN_OWNER_LEVEL)
    {
        player->SendSysMessage("Reach level 10 before claiming a market stall.");
        return;
    }
    if (FindClaim(stallGuid))
    {
        player->SendSysMessage("Another player already holds this market stall; you were not charged.");
        return;
    }
    MarketStallClaim const* accountClaim = FindAccountClaim(accountId);
    if (accountClaim)
    {
        if (accountClaim->closing)
            player->SendSysMessage("Your previous market stall is still returning its goods.");
        else
            player->GetSession()->SendNotification("Your account already holds %s.",
                StallName(*accountClaim).c_str());
        return;
    }
    uint64 cooldownUntil = 0;
    if (!GetClaimCooldown(accountId, cooldownUntil))
    {
        player->SendSysMessage("The market could not check your claim cooldown; you were not charged.");
        return;
    }
    if (cooldownUntil > now)
    {
        player->GetSession()->SendNotification("You can claim another stall in %s.",
            DurationText(cooldownUntil - now).c_str());
        return;
    }
    if (player->GetMoney() < LEASE_FEE_COPPER)
    {
        player->SendSysMessage("You need 25 silver to claim this market stall.");
        return;
    }
    if (!CharacterDatabase.BeginTransaction(player->GetGUIDLow()))
    {
        player->SendSysMessage("The market could not start that claim; you were not charged.");
        return;
    }
    player->ModifyMoney(-int32(LEASE_FEE_COPPER));
    CharacterDatabase.PExecute(
        "INSERT INTO `market_stall` (`stall_guid`,`owner_account_id`,`owner_guid`,`category`,`claimed_at`,"
        "`lease_expires_at`,`last_active_at`,`state`) VALUES ('%u','%u','%u','%u','" UI64FMTD "','" UI64FMTD "',"
        "'" UI64FMTD "','active')",
        stallGuid, accountId, player->GetGUIDLow(), uint32(category), now, now + LEASE_SECONDS, now);
    CharacterDatabase.PExecute(
        "DELETE FROM `market_stall_claim_cooldown` WHERE `owner_account_id`='%u'", accountId);
    player->SaveGoldToDB();
    if (!CharacterDatabase.CommitTransaction())
    {
        CharacterDatabase.RollbackTransaction();
        player->ModifyMoney(int32(LEASE_FEE_COPPER));
        player->SendSysMessage("The market could not queue that claim; you were not charged.");
        return;
    }
    QueueLeaseMutationBarrier(player->GetGUIDLow(), accountId);
    MarketStallClaim newClaim;
    newClaim.stallGuid = stallGuid;
    newClaim.ownerAccountId = accountId;
    newClaim.ownerGuid = player->GetGUIDLow();
    newClaim.category = category;
    newClaim.claimedAt = now;
    newClaim.leaseExpiresAt = now + LEASE_SECONDS;
    newClaim.closing = false;
    m_claims[stallGuid] = newClaim;
    MarketStallClaim const* claim = FindClaim(stallGuid);
    SyncAssets(stallGuid);
    player->GetSession()->SendNotification("%s claimed for 24 hours. Lease remaining: %s.",
        StallName(*claim).c_str(), DurationText(LEASE_SECONDS).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] claim stall=%u account=%u owner=%u",
             stallGuid, claim->ownerAccountId, claim->ownerGuid);
}

void MarketStallMgr::Renew(Player* player, uint32 stallGuid)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    if (!IsClaimActive(claim) || claim->ownerAccountId != player->GetSession()->GetAccountId())
    {
        player->SendSysMessage("You no longer own an active lease for this market stall.");
        return;
    }
    uint64 const now = uint64(sWorld.GetGameTime());
    if (claim->leaseExpiresAt - now > RENEW_WINDOW_SECONDS)
    {
        player->GetSession()->SendNotification("Renewal opens in %s.",
            DurationText(claim->leaseExpiresAt - now - RENEW_WINDOW_SECONDS).c_str());
        return;
    }
    if (!HasActiveStock(stallGuid))
    {
        player->SendSysMessage("Renewal requires a live good, enchant, or craft advert.");
        return;
    }
    if (player->GetMoney() < LEASE_FEE_COPPER)
    {
        player->SendSysMessage("You need 25 silver to renew this market stall.");
        return;
    }
    uint64 const renewedUntil = claim->leaseExpiresAt + LEASE_SECONDS;
    uint64 const priorExpiry = claim->leaseExpiresAt;
    if (!CharacterDatabase.BeginTransaction(player->GetGUIDLow()))
    {
        player->SendSysMessage("The market could not start that renewal; you were not charged.");
        return;
    }
    player->ModifyMoney(-int32(LEASE_FEE_COPPER));
    CharacterDatabase.PExecute(
        "UPDATE `market_stall` SET `lease_expires_at`='" UI64FMTD "', `last_active_at`='" UI64FMTD "' "
        "WHERE `stall_guid`='%u' AND `owner_account_id`='%u' AND `state`='active' "
        "AND `lease_expires_at`='" UI64FMTD "'",
        renewedUntil, now, stallGuid, player->GetSession()->GetAccountId(), priorExpiry);
    player->SaveGoldToDB();
    if (!CharacterDatabase.CommitTransaction())
    {
        CharacterDatabase.RollbackTransaction();
        player->ModifyMoney(int32(LEASE_FEE_COPPER));
        player->SendSysMessage("The market could not queue that renewal; you were not charged.");
        return;
    }
    QueueLeaseMutationBarrier(player->GetGUIDLow(), player->GetSession()->GetAccountId());
    m_claims[stallGuid].leaseExpiresAt = renewedUntil;
    player->GetSession()->SendNotification("Market stall renewed. Lease remaining: %s.",
        DurationText(renewedUntil - now).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] renew stall=%u account=%u", stallGuid, player->GetSession()->GetAccountId());
}

void MarketStallMgr::Release(Player* player, uint32 stallGuid)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    if (!claim || claim->ownerAccountId != player->GetSession()->GetAccountId())
        return;
    if (sAuctionMgr.CountMarketStallAuctions(stallGuid))
    {
        player->SendSysMessage("Cancel or sell every stall good before releasing this stall.");
        return;
    }
    uint32 const accountId = claim->ownerAccountId;
    if (!CharacterDatabase.BeginTransaction())
    {
        player->SendSysMessage("The market could not start that release; the lease remains active.");
        return;
    }
    CharacterDatabase.PExecute(
        "DELETE FROM `market_stall_service` WHERE `stall_guid`='%u'", stallGuid);
    CharacterDatabase.PExecute(
        "DELETE FROM `market_stall` WHERE `stall_guid`='%u' AND `owner_account_id`='%u'",
        stallGuid, accountId);
    CharacterDatabase.PExecute(
        "INSERT INTO `market_stall_claim_cooldown` (`owner_account_id`,`eligible_at`,`reason`) "
        "VALUES ('%u','" UI64FMTD "','owner-release') ON DUPLICATE KEY UPDATE "
        "`eligible_at`=VALUES(`eligible_at`),`reason`=VALUES(`reason`)",
        accountId, uint64(sWorld.GetGameTime()) + CLAIM_COOLDOWN_SECONDS);
    CharacterDatabase.CommitTransactionDirect();
    if (!ReloadClaims() || !ReloadOffers())
    {
        player->SendSysMessage("The release was saved, but the live market could not refresh it yet.");
        return;
    }
    if (FindClaim(stallGuid))
    {
        player->SendSysMessage("The market could not complete that release; the lease remains active.");
        return;
    }
    CancelOrders(stallGuid, 0, "owner-release");
    SyncAssets(stallGuid);
    player->SendSysMessage("Market stall released. You can claim another in 6 hours.");
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] release stall=%u account=%u", stallGuid, accountId);
}

void MarketStallMgr::AddOffer(Player* player, uint32 stallGuid, uint32 spellId, uint8 serviceKind, char const* code)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    uint32 price = 0;
    bool const validSpell = serviceKind == SERVICE_CRAFT_ADVERT ? IsCraftSpell(spellId) : IsOfferSpell(spellId);
    if (!IsClaimActive(claim) || claim->ownerAccountId != player->GetSession()->GetAccountId() ||
        !player->HasSpell(spellId) || !validSpell || !ParsePrice(code, price))
    {
        player->SendSysMessage("That service advert or copper price is invalid.");
        return;
    }
    OfferMap::const_iterator list = m_offers.find(stallGuid);
    uint32 offerId = 0;
    if (list != m_offers.end())
        for (std::vector<MarketStallOffer>::const_iterator itr = list->second.begin(); itr != list->second.end(); ++itr)
            if (itr->serviceKind == serviceKind && itr->sellerGuid == player->GetGUIDLow() && itr->spellId == spellId)
                offerId = itr->offerId;
    uint32 kindCount = 0;
    if (list != m_offers.end())
        for (std::vector<MarketStallOffer>::const_iterator itr = list->second.begin(); itr != list->second.end(); ++itr)
            if (itr->serviceKind == serviceKind)
                ++kindCount;
    if (!offerId && kindCount >= SERVICE_LIMIT)
    {
        player->SendSysMessage("This stall already has eight adverts of that service type.");
        return;
    }
    if (!offerId)
    {
        offerId = 1;
        if (list != m_offers.end())
            for (std::vector<MarketStallOffer>::const_iterator itr = list->second.begin(); itr != list->second.end(); ++itr)
                offerId = std::max(offerId, itr->offerId + 1);
    }
    bool const cancelExistingOrder = offerId && serviceKind == SERVICE_ENCHANT;
    if (!CharacterDatabase.DirectPExecute(
        "INSERT INTO `market_stall_service` (`stall_guid`,`offer_id`,`service_kind`,`seller_guid`,`spell_id`,`price_copper`,`created_at`) "
        "VALUES ('%u','%u','%s','%u','%u','%u','" UI64FMTD "') "
        "ON DUPLICATE KEY UPDATE `price_copper`=VALUES(`price_copper`),`created_at`=VALUES(`created_at`)",
        stallGuid, offerId, serviceKind == SERVICE_CRAFT_ADVERT ? "craft_advert" : "enchant",
        player->GetGUIDLow(), spellId, price, uint64(sWorld.GetGameTime())))
    {
        player->SendSysMessage("The market could not save that advert.");
        return;
    }
    if (!ReloadOffers())
    {
        player->SendSysMessage("The advert was saved, but the live market could not refresh it yet.");
        return;
    }
    MarketStallOffer const* saved = FindOffer(stallGuid, offerId);
    if (!saved || saved->sellerGuid != player->GetGUIDLow() || saved->spellId != spellId ||
        saved->serviceKind != serviceKind || saved->priceCopper != price)
    {
        player->SendSysMessage("The market could not confirm that advert.");
        return;
    }
    if (cancelExistingOrder)
        CancelOrders(stallGuid, offerId, "offer-price-changed");
    SyncAssets(stallGuid);
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    player->GetSession()->SendNotification("Advertised %s for %s.",
        spell ? spell->SpellName[0] : "service", MoneyText(price).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] offer-add stall=%u offer=%u seller=%u spell=%u price=%u",
             stallGuid, offerId, player->GetGUIDLow(), spellId, price);
}

void MarketStallMgr::RemoveOffer(Player* player, uint32 stallGuid, uint32 offerId)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    MarketStallOffer const* offer = FindOffer(stallGuid, offerId);
    if (!claim || claim->ownerAccountId != player->GetSession()->GetAccountId() || !offer)
        return;
    uint32 const spellId = offer->spellId;
    if (!CharacterDatabase.DirectPExecute(
            "DELETE FROM `market_stall_service` WHERE `stall_guid`='%u' AND `offer_id`='%u'", stallGuid, offerId))
    {
        player->SendSysMessage("The market could not remove that advert.");
        return;
    }
    CancelOrders(stallGuid, offerId, "offer-removed");
    if (!ReloadOffers())
    {
        player->SendSysMessage("The advert removal was saved, but the live market could not refresh it yet.");
        return;
    }
    if (FindOffer(stallGuid, offerId))
    {
        player->SendSysMessage("The market could not confirm that advert removal.");
        return;
    }
    SyncAssets(stallGuid);
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    player->GetSession()->SendNotification("Removed %s from this market stall.",
        spell ? spell->SpellName[0] : "service");
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[MarketStall] offer-remove stall=%u offer=%u actor=%u",
             stallGuid, offerId, player->GetGUIDLow());
}

bool MarketStallMgr::IsSellerAvailable(MarketStallOffer const& offer, Player const* buyer) const
{
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer.sellerGuid));
    return seller && seller != buyer && seller->IsAlive() && !seller->GetTradeData() && !seller->IsTaxiFlying() &&
           !seller->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) &&
           !seller->GetSession()->IsLogingOut() &&
           seller->GetMapId() == buyer->GetMapId() && seller->GetDistance3dToCenter(buyer) <= TRADE_DISTANCE &&
           IsPlayerNearPad(seller, offer.stallGuid) && seller->HasSpell(offer.spellId);
}

void MarketStallMgr::OpenServiceOrder(Player* buyer, uint32 stallGuid, uint32 offerId)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    MarketStallOffer const* offer = FindOffer(stallGuid, offerId);
    if (!m_enabled || !IsClaimActive(claim) || !offer || offer->serviceKind != SERVICE_ENCHANT ||
        !IsSellerAvailable(*offer, buyer) || buyer->GetTradeData() ||
        !buyer->IsAlive() || buyer->IsTaxiFlying() ||
        buyer->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
        buyer->GetSession()->IsLogingOut())
    {
        buyer->SendSysMessage("That enchanter is not currently available.");
        return;
    }
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer->sellerGuid));
    if (!seller || (!sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_TRADE) && seller->GetTeam() != buyer->GetTeam()))
    {
        buyer->SendSysMessage("The ordinary trade rules do not allow this service.");
        return;
    }
    if (buyer->GetMoney() < offer->priceCopper)
    {
        buyer->GetSession()->SendNotification("You need %s for this enchant.",
            MoneyText(offer->priceCopper).c_str());
        return;
    }
    if (buyer->GetSession()->HasTrialRestrictions() || seller->GetSession()->HasTrialRestrictions() ||
        (!sWorld.getConfig(CONFIG_BOOL_GM_ALLOW_TRADES) &&
         (buyer->GetSession()->GetSecurity() > SEC_PLAYER || seller->GetSession()->GetSecurity() > SEC_PLAYER)))
    {
        buyer->SendSysMessage("The ordinary account trade rules do not allow this service.");
        return;
    }

    buyer->m_trade = new TradeData(buyer, seller);
    seller->m_trade = new TradeData(seller, buyer);
    buyer->m_trade->SetScamPreventionDelay(200);
    seller->m_trade->SetScamPreventionDelay(200);

    MarketStallOrder order;
    order.nonce = m_nextOrderNonce++;
    order.stallGuid = stallGuid;
    order.offerId = offerId;
    order.buyerGuid = buyer->GetGUIDLow();
    order.sellerGuid = seller->GetGUIDLow();
    order.spellId = offer->spellId;
    order.priceCopper = offer->priceCopper;
    m_orders[order.buyerGuid] = order;
    m_orderByPlayer[order.buyerGuid] = order.buyerGuid;
    m_orderByPlayer[order.sellerGuid] = order.buyerGuid;

    auto packet = std::make_unique<WorldPackets::Trade::TradeStatus>();
    packet->status = TRADE_STATUS_BEGIN_TRADE;
    packet->playerGuid = buyer->GetObjectGuid();
    seller->GetSession()->SendPacket(std::move(packet));
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(order.spellId);
    std::string const spellName = spell ? spell->SpellName[0] : "Enchant";
    std::string const price = MoneyText(order.priceCopper);
    buyer->GetSession()->SendNotification(
        "Trade opened for %s at %s. Place the target in the non-traded slot.",
        spellName.c_str(), price.c_str());
    seller->GetSession()->SendNotification(
        "%s requested %s for %s through your market stall.",
        buyer->GetName(), spellName.c_str(), price.c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[MarketStall] service-open nonce=" UI64FMTD " stall=%u offer=%u buyer=%u seller=%u spell=%u price=%u",
             order.nonce, stallGuid, offerId, order.buyerGuid, order.sellerGuid, order.spellId, order.priceCopper);
}

void MarketStallMgr::NotifyCraftAdvert(Player* buyer, uint32 stallGuid, uint32 offerId)
{
    MarketStallClaim const* claim = FindClaim(stallGuid);
    MarketStallOffer const* offer = FindOffer(stallGuid, offerId);
    if (!m_enabled || !IsClaimActive(claim) || !offer || offer->serviceKind != SERVICE_CRAFT_ADVERT ||
        !IsSellerAvailable(*offer, buyer))
    {
        buyer->SendSysMessage("That crafter is not currently available near this stall.");
        return;
    }
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer->sellerGuid));
    if (!seller || (!sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_TRADE) && seller->GetTeam() != buyer->GetTeam()))
    {
        buyer->SendSysMessage("The ordinary trade rules do not allow this craft request.");
        return;
    }
    uint64 const now = uint64(sWorld.GetGameTime());
    std::map<uint32, uint64>::const_iterator lastRequest = m_lastCraftRequestAt.find(buyer->GetGUIDLow());
    if (lastRequest != m_lastCraftRequestAt.end() &&
        lastRequest->second + CRAFT_REQUEST_COOLDOWN_SECONDS > now)
    {
        buyer->SendSysMessage("Please wait before sending another craft request.");
        return;
    }
    m_lastCraftRequestAt[buyer->GetGUIDLow()] = now;
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(offer->spellId);
    std::string const spellName = spell ? spell->SpellName[0] : "Craft";
    std::string const price = MoneyText(offer->priceCopper);
    seller->GetSession()->SendNotification(
        "Craft request from %s: %s (asking %s). Arrange an ordinary trade.",
        buyer->GetName(), spellName.c_str(), price.c_str());
    buyer->GetSession()->SendNotification(
        "Craft request sent to %s. Arrange payment and materials through ordinary trade.", seller->GetName());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[MarketStall] craft-request stall=%u offer=%u buyer=%u seller=%u spell=%u asking=%u",
             stallGuid, offerId, buyer->GetGUIDLow(), seller->GetGUIDLow(), offer->spellId, offer->priceCopper);
}

bool MarketStallMgr::HandleGossipSelect(Player* player, Creature* creature, uint32 sender,
                                         uint32 action, char const* code)
{
    if (!creature || !GetStallGuid(creature->GetObjectGuid()))
        return false;
    if (!m_enabled)
        return true;
    if (sender != MARKET_STALL_GOSSIP_SENDER)
        return false;
    bool const mutating = action == ACTION_RENEW || action == ACTION_RELEASE ||
        (action >= ACTION_CLAIM_CATEGORY && action < ACTION_CLAIM_CATEGORY + CATEGORY_COUNT) ||
        (action >= ACTION_ADD_SPELL && action < ACTION_SELECT_OFFER) ||
        (action >= ACTION_ADD_CRAFT && action < ACTION_SELECT_CRAFT) || action >= ACTION_SELECT_CRAFT;
    if (mutating && !AllowMutation(player))
        return true;
    uint32 const stallGuid = GetStallGuid(creature->GetObjectGuid());
    if (action == ACTION_CLAIM)
    {
        ShowCategoryMenu(player, creature);
        return true;
    }
    else if (action >= ACTION_CLAIM_CATEGORY && action < ACTION_CLAIM_CATEGORY + CATEGORY_COUNT)
        Claim(player, stallGuid, uint8(action - ACTION_CLAIM_CATEGORY));
    else if (action == ACTION_RENEW)
        Renew(player, stallGuid);
    else if (action == ACTION_RELEASE)
        Release(player, stallGuid);
    else if (action == ACTION_GOODS)
    {
        if (CanOpenAuction(player, stallGuid))
            player->GetSession()->SendAuctionHello(creature);
        return true;
    }
    else if (action == ACTION_MANAGE_OFFERS)
    {
        ShowOfferMenu(player, creature, true, false, 0);
        return true;
    }
    else if (action == ACTION_BROWSE_OFFERS)
    {
        ShowOfferMenu(player, creature, false, false, 0);
        return true;
    }
    else if (action == ACTION_MANAGE_CRAFT)
    {
        ShowOfferMenu(player, creature, true, true, 0);
        return true;
    }
    else if (action == ACTION_BROWSE_CRAFT)
    {
        ShowOfferMenu(player, creature, false, true, 0);
        return true;
    }
    else if (action >= ACTION_SELECT_CRAFT)
    {
        NotifyCraftAdvert(player, stallGuid, action - ACTION_SELECT_CRAFT);
        return true;
    }
    else if (action >= ACTION_ADD_CRAFT)
        AddOffer(player, stallGuid, action - ACTION_ADD_CRAFT, SERVICE_CRAFT_ADVERT, code);
    else if (action >= ACTION_ADD_SPELL && action < ACTION_REMOVE_OFFER)
        AddOffer(player, stallGuid, action - ACTION_ADD_SPELL, SERVICE_ENCHANT, code);
    else if (action >= ACTION_REMOVE_OFFER && action < ACTION_SELECT_OFFER)
        RemoveOffer(player, stallGuid, action - ACTION_REMOVE_OFFER);
    else if (action >= ACTION_SELECT_OFFER && action < ACTION_ADD_CRAFT)
    {
        OpenServiceOrder(player, stallGuid, action - ACTION_SELECT_OFFER);
        return true;
    }
    else if (action >= ACTION_BROWSE_CRAFT_PAGE && action < ACTION_ADD_SPELL)
    {
        ShowOfferMenu(player, creature, false, true, action - ACTION_BROWSE_CRAFT_PAGE);
        return true;
    }
    else if (action >= ACTION_MANAGE_CRAFT_PAGE && action < ACTION_BROWSE_CRAFT_PAGE)
    {
        ShowOfferMenu(player, creature, true, true, action - ACTION_MANAGE_CRAFT_PAGE);
        return true;
    }
    else if (action >= ACTION_BROWSE_PAGE && action < ACTION_MANAGE_CRAFT_PAGE)
    {
        ShowOfferMenu(player, creature, false, false, action - ACTION_BROWSE_PAGE);
        return true;
    }
    else if (action >= ACTION_MANAGE_PAGE && action < ACTION_BROWSE_PAGE)
    {
        ShowOfferMenu(player, creature, true, false, action - ACTION_MANAGE_PAGE);
        return true;
    }
    ShowMainMenu(player, creature);
    return true;
}

bool MarketStallMgr::ValidateOrder(MarketStallOrder const& order, bool requireTarget) const
{
    MarketStallClaim const* claim = FindClaim(order.stallGuid);
    MarketStallOffer const* offer = FindOffer(order.stallGuid, order.offerId);
    Player* buyer = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order.buyerGuid));
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order.sellerGuid));
    if (!m_enabled || !IsClaimActive(claim) || !offer || offer->serviceKind != SERVICE_ENCHANT ||
        offer->sellerGuid != order.sellerGuid ||
        offer->spellId != order.spellId || offer->priceCopper != order.priceCopper || !buyer || !seller ||
        !buyer->GetTradeData() || !seller->GetTradeData() || !seller->HasSpell(order.spellId) ||
        !buyer->IsAlive() || !seller->IsAlive() || buyer->GetDistance3dToCenter(seller) > TRADE_DISTANCE ||
        !IsPlayerNearPad(seller, order.stallGuid) || buyer->GetTradeData()->GetMoney() != order.priceCopper ||
        seller->GetTradeData()->GetMoney() != 0)
        return false;
    for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        if (buyer->GetTradeData()->GetItem(TradeSlots(slot)) || seller->GetTradeData()->GetItem(TradeSlots(slot)))
            return false;
    Item* target = buyer->GetTradeData()->GetItem(TRADE_SLOT_NONTRADED);
    if (requireTarget && (!target || !target->IsFitToSpellRequirements(sSpellMgr.GetSpellEntry(order.spellId)) ||
                          seller->GetTradeData()->GetSpell() != order.spellId))
        return false;
    return true;
}

void MarketStallMgr::EraseOrder(uint32 buyerGuid, char const* reason, bool completed)
{
    std::map<uint32, MarketStallOrder>::iterator itr = m_orders.find(buyerGuid);
    if (itr == m_orders.end())
        return;
    MarketStallOrder order = itr->second;
    m_orderByPlayer.erase(order.buyerGuid);
    m_orderByPlayer.erase(order.sellerGuid);
    m_orders.erase(itr);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[MarketStall] service-%s nonce=" UI64FMTD " stall=%u offer=%u buyer=%u seller=%u reason=%s",
             completed ? "complete" : "cancel", order.nonce, order.stallGuid, order.offerId,
             order.buyerGuid, order.sellerGuid, reason);
}

void MarketStallMgr::CancelOrders(uint32 stallGuid, uint32 offerId, char const* reason)
{
    std::vector<uint32> buyers;
    for (std::map<uint32, MarketStallOrder>::const_iterator itr = m_orders.begin(); itr != m_orders.end(); ++itr)
        if (itr->second.stallGuid == stallGuid && (!offerId || itr->second.offerId == offerId))
            buyers.push_back(itr->first);
    for (std::vector<uint32>::const_iterator itr = buyers.begin(); itr != buyers.end(); ++itr)
    {
        Player* buyer = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, *itr));
        EraseOrder(*itr, reason, false);
        if (buyer && buyer->GetTradeData())
            buyer->TradeCancel(true);
    }
}

void MarketStallMgr::RejectOrder(Player* player, char const* reason)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order)
        return;
    uint32 const buyerGuid = order->buyerGuid;
    EraseOrder(buyerGuid, reason, false);
    player->TradeCancel(true);
}

bool MarketStallMgr::ValidateServiceAccept(Player* player)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order)
        return true;
    if (ValidateOrder(*order, true))
        return true;
    RejectOrder(player, "accept-validation");
    return false;
}

bool MarketStallMgr::HandleTradeMoneyMutation(Player* player, uint32 money)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order)
        return false;
    uint32 const expected = player->GetGUIDLow() == order->buyerGuid ? order->priceCopper : 0;
    if (money == expected)
        return false;
    RejectOrder(player, "money-mutation");
    return true;
}

bool MarketStallMgr::HandleTradeItemMutation(Player* player, uint8 tradeSlot)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order)
        return false;
    if (player->GetGUIDLow() == order->buyerGuid && tradeSlot == TRADE_SLOT_NONTRADED)
        return false;
    RejectOrder(player, "item-mutation");
    return true;
}

void MarketStallMgr::OnTradeItemChanged(Player* player, uint8 tradeSlot)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order || tradeSlot != TRADE_SLOT_NONTRADED)
        return;
    if (!ValidateOrder(*order, false))
    {
        RejectOrder(player, "target-validation");
        return;
    }
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order->sellerGuid));
    Player* buyer = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order->buyerGuid));
    Item* target = buyer && buyer->GetTradeData() ? buyer->GetTradeData()->GetItem(TRADE_SLOT_NONTRADED) : nullptr;
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(order->spellId);
    if (!seller || !seller->GetTradeData() || !target || !spell || !target->IsFitToSpellRequirements(spell))
    {
        RejectOrder(player, "incompatible-target");
        return;
    }
    seller->GetTradeData()->SetSpell(order->spellId);
}

bool MarketStallMgr::HandleTradeItemClear(Player* player, uint8 tradeSlot)
{
    if (!FindOrder(player->GetGUIDLow()))
        return false;
    RejectOrder(player, tradeSlot == TRADE_SLOT_NONTRADED ? "target-removed" : "item-cleared");
    return true;
}

void MarketStallMgr::OnTradeWindowOpened(Player* player)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (!order)
        return;
    Player* buyer = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order->buyerGuid));
    if (!buyer || !buyer->GetTradeData())
    {
        RejectOrder(player, "open-validation");
        return;
    }
    buyer->GetTradeData()->SetMoney(order->priceCopper);
    buyer->GetSession()->SendUpdateTrade(false);
    buyer->GetTradeData()->SetLastModificationTime(sWorld.GetGameTime());
    Player* seller = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, order->sellerGuid));
    if (seller && seller->GetTradeData())
        seller->GetTradeData()->SetLastModificationTime(sWorld.GetGameTime());
    if (!ValidateOrder(*order, false))
        RejectOrder(player, "open-validation");
}

void MarketStallMgr::OnTradeCanceled(Player* player, char const* reason)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (order)
        EraseOrder(order->buyerGuid, reason, false);
}

void MarketStallMgr::OnTradeCompleted(Player* player)
{
    MarketStallOrder const* order = FindOrder(player->GetGUIDLow());
    if (order)
        EraseOrder(order->buyerGuid, "accepted", true);
}

void MarketStallMgr::CloseClaimsForCharacter(uint32 characterGuid, char const* reason)
{
    std::vector<uint32> close;
    for (ClaimMap::const_iterator itr = m_claims.begin(); itr != m_claims.end(); ++itr)
        if (itr->second.ownerGuid == characterGuid)
            close.push_back(itr->first);
    for (std::vector<uint32>::const_iterator itr = close.begin(); itr != close.end(); ++itr)
        CloseClaim(*itr, reason, false);
    CharacterDatabase.DirectPExecute("DELETE FROM `market_stall_service` WHERE `seller_guid`='%u'", characterGuid);
    if (!ReloadOffers())
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[MarketStall] character cleanup could not refresh the live offer catalogue");
}
