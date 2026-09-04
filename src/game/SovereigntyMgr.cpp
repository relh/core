#include "SovereigntyMgr.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "GameObject.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <memory>

SovereigntyMgr& SovereigntyMgr::Instance()
{
    static SovereigntyMgr instance;
    return instance;
}

SovereigntyMgr::Site* SovereigntyMgr::FindSite(std::string const& siteId)
{
    auto const itr = m_sites.find(siteId);
    return itr == m_sites.end() ? nullptr : &itr->second;
}

SovereigntyMgr::Site const* SovereigntyMgr::FindSite(std::string const& siteId) const
{
    auto const itr = m_sites.find(siteId);
    return itr == m_sites.end() ? nullptr : &itr->second;
}

void SovereigntyMgr::Load()
{
    m_enabled = sConfig.GetBoolDefault("Coworld.SovereigntyEnabled", false);
    m_sites.clear();
    m_objects.clear();
    m_structures.clear();
    m_pendingInteractions.clear();
    if (!m_enabled)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "Coworld sovereignty sites: disabled.");
        return;
    }

    std::unique_ptr<QueryResult> rulesResult(WorldDatabase.Query(
        "SELECT `topology_revision`, `claim_rank`, `claim_participants`, "
        "`claim_channel_seconds`, `challenge_channel_seconds`, `vulnerability_minutes`, "
        "`reinforcement_warning_hours`, `contest_minutes`, `capture_channel_seconds`, "
        "`recapture_lock_seconds`, `control_tick_seconds`, `majority_points`, `supermajority_points`, "
        "`total_control_points`, `victory_points`, `supply_per_hour`, `supply_cap`, "
        "`outpost_cost`, `stronghold_cost`, `outpost_daily_upkeep`, `stronghold_daily_upkeep` "
        "FROM `coworld_sovereignty_rules` WHERE `singleton_id`=1"));
    if (!rulesResult)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "Coworld sovereignty enabled without staged rules; disabling feature.");
        m_enabled = false;
        return;
    }
    Field* ruleFields = rulesResult->Fetch();
    m_rules.topologyRevision = ruleFields[0].GetUInt32();
    m_rules.claimRank = ruleFields[1].GetUInt32();
    m_rules.claimParticipants = ruleFields[2].GetUInt32();
    m_rules.claimChannelSeconds = ruleFields[3].GetUInt32();
    m_rules.challengeChannelSeconds = ruleFields[4].GetUInt32();
    m_rules.vulnerabilityMinutes = ruleFields[5].GetUInt32();
    m_rules.reinforcementWarningHours = ruleFields[6].GetUInt32();
    m_rules.contestMinutes = ruleFields[7].GetUInt32();
    m_rules.captureChannelSeconds = ruleFields[8].GetUInt32();
    m_rules.recaptureLockSeconds = ruleFields[9].GetUInt32();
    m_rules.controlTickSeconds = ruleFields[10].GetUInt32();
    m_rules.majorityPoints = ruleFields[11].GetUInt32();
    m_rules.supermajorityPoints = ruleFields[12].GetUInt32();
    m_rules.totalControlPoints = ruleFields[13].GetUInt32();
    m_rules.victoryPoints = ruleFields[14].GetUInt32();
    m_rules.supplyPerHour = ruleFields[15].GetUInt32();
    m_rules.supplyCap = ruleFields[16].GetUInt32();
    m_rules.outpostCost = ruleFields[17].GetUInt32();
    m_rules.strongholdCost = ruleFields[18].GetUInt32();
    m_rules.outpostDailyUpkeep = ruleFields[19].GetUInt32();
    m_rules.strongholdDailyUpkeep = ruleFields[20].GetUInt32();

    std::unique_ptr<QueryResult> siteResult(WorldDatabase.Query(
        "SELECT s.`site_id`, s.`name`, s.`build_district_id`, s.`map_id`, s.`zone_id`, "
        "s.`world_state_base`, s.`hub_gameobject_guid` FROM `coworld_sovereignty_site` s "
        "ORDER BY s.`site_id`"));
    if (!siteResult)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "Coworld sovereignty enabled without staged sites; disabling feature.");
        m_enabled = false;
        return;
    }
    do
    {
        Field* fields = siteResult->Fetch();
        Site site;
        site.id = fields[0].GetString();
        site.name = fields[1].GetString();
        site.buildDistrictId = fields[2].GetString();
        site.mapId = fields[3].GetUInt32();
        site.zoneId = fields[4].GetUInt32();
        site.worldStateBase = fields[5].GetUInt32();
        site.hubGuid = fields[6].GetUInt32();
        m_objects[site.hubGuid] = ObjectRef{site.id, -1};
        m_sites.emplace(site.id, site);
    } while (siteResult->NextRow());

    std::unique_ptr<QueryResult> polygonResult(WorldDatabase.Query(
        "SELECT `site_id`, `x`, `y` FROM `coworld_sovereignty_polygon` "
        "WHERE `polygon_kind`='territory' ORDER BY `site_id`, `vertex_index`"));
    if (polygonResult)
    {
        do
        {
            Field* fields = polygonResult->Fetch();
            if (Site* site = FindSite(fields[0].GetString()))
                site->polygon.push_back(std::make_pair(fields[1].GetFloat(), fields[2].GetFloat()));
        } while (polygonResult->NextRow());
    }

    std::unique_ptr<QueryResult> nodeResult(WorldDatabase.Query(
        "SELECT `site_id`, `node_index`, `gameobject_guid` "
        "FROM `coworld_sovereignty_node` ORDER BY `site_id`, `node_index`"));
    if (nodeResult)
    {
        do
        {
            Field* fields = nodeResult->Fetch();
            std::string const siteId = fields[0].GetString();
            uint32 const nodeIndex = fields[1].GetUInt32();
            uint32 const guid = fields[2].GetUInt32();
            if (Site* site = FindSite(siteId))
            {
                if (nodeIndex < site->nodeGuids.size())
                {
                    site->nodeGuids[nodeIndex] = guid;
                    m_objects[guid] = ObjectRef{siteId, int8(nodeIndex)};
                }
            }
        } while (nodeResult->NextRow());
    }

    std::unique_ptr<QueryResult> structureResult(WorldDatabase.Query(
        "SELECT `site_id`, `gameobject_guid`, `gameobject_entry`, `tier_min`, `tier_max` "
        "FROM `coworld_sovereignty_structure` ORDER BY `site_id`, `slot_id`"));
    if (structureResult)
    {
        do
        {
            Field* fields = structureResult->Fetch();
            std::string const siteId = fields[0].GetString();
            if (FindSite(siteId))
                m_structures[fields[1].GetUInt32()] = StructureRef{
                    siteId, fields[3].GetUInt8(), fields[4].GetUInt8(), fields[2].GetUInt32()};
        } while (structureResult->NextRow());
    }

    std::unique_ptr<QueryResult> claimResult(CharacterDatabase.Query(
        "SELECT `site_id`, `revision`, `state`, COALESCE(`owner_guild_id`,0), "
        "COALESCE(`vulnerability_midpoint_utc`,0), COALESCE(`challenger_guild_id`,0), "
        "COALESCE(`contest_at`,0), COALESCE(`contest_ends_at`,0), "
        "`defender_score`, `challenger_score`, `infrastructure_tier`, `treasury`, "
        "COALESCE(`last_accrual_at`,0), COALESCE(`next_upkeep_at`,0), `topology_revision` "
        "FROM `coworld_territory_claim`"));
    if (claimResult)
    {
        do
        {
            Field* fields = claimResult->Fetch();
            Site* site = FindSite(fields[0].GetString());
            if (!site || fields[14].GetUInt32() != m_rules.topologyRevision)
                continue;
            site->revision = fields[1].GetUInt32();
            site->state = ClaimState(fields[2].GetUInt32());
            site->ownerGuildId = fields[3].GetUInt32();
            site->vulnerabilityMidpointUtc = fields[4].GetUInt32();
            site->challengerGuildId = fields[5].GetUInt32();
            site->contestAt = fields[6].GetUInt64();
            site->contestEndsAt = fields[7].GetUInt64();
            site->defenderScore = fields[8].GetUInt32();
            site->challengerScore = fields[9].GetUInt32();
            site->infrastructureTier = fields[10].GetUInt32();
            site->treasury = fields[11].GetUInt32();
            site->lastAccrualAt = fields[12].GetUInt64();
            site->nextUpkeepAt = fields[13].GetUInt64();
            if (site->ownerGuildId && !sGuildMgr.GetGuildById(site->ownerGuildId))
            {
                site->state = CLAIM_UNCLAIMED;
                site->ownerGuildId = 0;
                site->vulnerabilityMidpointUtc = 0;
                site->challengerGuildId = 0;
                site->contestAt = 0;
                site->contestEndsAt = 0;
                site->defenderScore = 0;
                site->challengerScore = 0;
                site->infrastructureTier = 1;
                site->treasury = 0;
                ResetNodes(*site);
                ++site->revision;
                Persist(*site, nullptr, "owner-guild-removed");
            }
            else if (site->challengerGuildId
                && !sGuildMgr.GetGuildById(site->challengerGuildId))
            {
                site->state = CLAIM_SECURE;
                site->challengerGuildId = 0;
                site->contestAt = 0;
                site->contestEndsAt = 0;
                site->defenderScore = 0;
                site->challengerScore = 0;
                ResetNodes(*site);
                ++site->revision;
                Persist(*site, nullptr, "challenger-guild-removed");
            }
        } while (claimResult->NextRow());
    }

    std::unique_ptr<QueryResult> nodeStateResult(CharacterDatabase.Query(
        "SELECT `site_id`, `node_index`, COALESCE(`owner_guild_id`,0), COALESCE(`locked_until`,0) "
        "FROM `coworld_territory_node_state`"));
    if (nodeStateResult)
    {
        do
        {
            Field* fields = nodeStateResult->Fetch();
            Site* site = FindSite(fields[0].GetString());
            uint32 const nodeIndex = fields[1].GetUInt32();
            if (site && nodeIndex < site->nodeOwnerGuildIds.size())
            {
                site->nodeOwnerGuildIds[nodeIndex] = fields[2].GetUInt32();
                site->nodeLockedUntil[nodeIndex] = fields[3].GetUInt64();
            }
        } while (nodeStateResult->NextRow());
    }
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "Coworld sovereignty sites: loaded %u sites.",
        uint32(m_sites.size()));
}

void SovereigntyMgr::Update(uint32 diff)
{
    if (!m_enabled)
        return;
    if (m_updateTimer > diff)
    {
        m_updateTimer -= diff;
        return;
    }
    m_updateTimer = std::max<uint32>(1, m_rules.controlTickSeconds) * 1000;
    uint64 const now = uint64(time(nullptr));
    UpdateInteractions(now);
    bool contestActive = false;
    for (auto& entry : m_sites)
    {
        Site& site = entry.second;
        AccrueAndUpkeep(site, now);
        if (site.state == CLAIM_REINFORCED && now >= site.contestAt)
        {
            site.state = CLAIM_CONTESTED;
            site.contestEndsAt = now + uint64(m_rules.contestMinutes) * 60;
            ++site.revision;
            Persist(site, nullptr, "contest-started");
            BroadcastSiteWorldStates(site);
        }
        if (site.state != CLAIM_CONTESTED)
            continue;
        contestActive = true;
        uint32 defenderNodes = 0;
        uint32 challengerNodes = 0;
        for (uint32 owner : site.nodeOwnerGuildIds)
        {
            defenderNodes += owner == site.ownerGuildId;
            challengerNodes += owner == site.challengerGuildId;
        }
        auto points = [this](uint32 nodes) -> uint32
        {
            if (nodes == 3) return m_rules.majorityPoints;
            if (nodes == 4) return m_rules.supermajorityPoints;
            if (nodes == 5) return m_rules.totalControlPoints;
            return 0;
        };
        site.defenderScore += points(defenderNodes);
        site.challengerScore += points(challengerNodes);
        ++site.revision;
        if (site.challengerScore >= m_rules.victoryPoints ||
            (now >= site.contestEndsAt && site.challengerScore > site.defenderScore))
            Resolve(site, true, now, "challenger-victory");
        else if (site.defenderScore >= m_rules.victoryPoints || now >= site.contestEndsAt)
            Resolve(site, false, now, "defender-victory");
        else
            Persist(site, nullptr, "control-tick");
        BroadcastSiteWorldStates(site);
    }
    if (contestActive)
    {
        for (auto const& sessionEntry : sWorld.GetAllSessions())
        {
            Player* player = sessionEntry.second->GetPlayer();
            if (player)
            {
                player->ForceValuesUpdateAtIndex(PLAYER_FLAGS);
                player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
            }
        }
    }
}

bool SovereigntyMgr::Contains(Site const& site, Player const* player, float buffer) const
{
    if (!player || player->GetMapId() != site.mapId || site.polygon.size() < 3)
        return false;
    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    bool inside = false;
    for (size_t i = 0, j = site.polygon.size() - 1; i < site.polygon.size(); j = i++)
    {
        float const xi = site.polygon[i].first, yi = site.polygon[i].second;
        float const xj = site.polygon[j].first, yj = site.polygon[j].second;
        if (((yi > y) != (yj > y)) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    if (inside || buffer <= 0.0f)
        return inside;
    float const bufferSquared = buffer * buffer;
    for (size_t i = 0, j = site.polygon.size() - 1; i < site.polygon.size(); j = i++)
    {
        float const ax = site.polygon[j].first, ay = site.polygon[j].second;
        float const bx = site.polygon[i].first, by = site.polygon[i].second;
        float const dx = bx - ax, dy = by - ay;
        float const denominator = dx * dx + dy * dy;
        float const t = denominator > 0.0f ?
            std::max(0.0f, std::min(1.0f, ((x - ax) * dx + (y - ay) * dy) / denominator)) : 0.0f;
        float const edgeX = ax + t * dx, edgeY = ay + t * dy;
        float const offsetX = x - edgeX, offsetY = y - edgeY;
        if (offsetX * offsetX + offsetY * offsetY <= bufferSquared)
            return true;
    }
    return false;
}

bool SovereigntyMgr::AreContestEnemies(Player const* left, Player const* right) const
{
    if (!m_enabled || !left || !right || left == right || !left->GetGuildId() || !right->GetGuildId())
        return false;
    for (auto const& entry : m_sites)
    {
        Site const& site = entry.second;
        if (site.state != CLAIM_CONTESTED || !Contains(site, left, 50.0f) || !Contains(site, right, 50.0f))
            continue;
        uint32 const leftGuild = left->GetGuildId();
        uint32 const rightGuild = right->GetGuildId();
        if (leftGuild != rightGuild &&
            (leftGuild == site.ownerGuildId || leftGuild == site.challengerGuildId) &&
            (rightGuild == site.ownerGuildId || rightGuild == site.challengerGuildId))
            return true;
    }
    return false;
}

bool SovereigntyMgr::AreContestAllies(Player const* left, Player const* right) const
{
    if (!m_enabled || !left || !right || left == right || !left->GetGuildId() ||
        left->GetGuildId() != right->GetGuildId())
        return false;
    for (auto const& entry : m_sites)
    {
        Site const& site = entry.second;
        if (site.state == CLAIM_CONTESTED && Contains(site, left, 50.0f) && Contains(site, right, 50.0f) &&
            (left->GetGuildId() == site.ownerGuildId || left->GetGuildId() == site.challengerGuildId))
            return true;
    }
    return false;
}

bool SovereigntyMgr::BeginInteraction(Site const& site, int8 nodeIndex, Player* player, uint64 now)
{
    uint32 const guildId = player->GetGuildId();
    uint32 seconds = 0;
    if (nodeIndex >= 0)
        seconds = m_rules.captureChannelSeconds;
    else if (site.state == CLAIM_UNCLAIMED)
        seconds = m_rules.claimChannelSeconds;
    else if (site.state == CLAIM_SECURE && guildId && guildId != site.ownerGuildId)
        seconds = m_rules.challengeChannelSeconds;
    if (!seconds)
        return false;
    PendingInteraction pending;
    pending.siteId = site.id;
    pending.nodeIndex = nodeIndex;
    pending.guildId = guildId;
    pending.completesAt = now + seconds;
    pending.startX = player->GetPositionX();
    pending.startY = player->GetPositionY();
    m_pendingInteractions[player->GetGUIDLow()] = pending;
    player->PSendSysMessage("Securing %s: remain alive and within 3 yards for %u seconds.",
        nodeIndex < 0 ? site.name.c_str() : "command node", seconds);
    return true;
}

void SovereigntyMgr::UpdateInteractions(uint64 now)
{
    for (auto itr = m_pendingInteractions.begin(); itr != m_pendingInteractions.end();)
    {
        Player* player = nullptr;
        for (auto const& sessionEntry : sWorld.GetAllSessions())
        {
            Player* candidate = sessionEntry.second->GetPlayer();
            if (candidate && candidate->GetGUIDLow() == itr->first)
            {
                player = candidate;
                break;
            }
        }
        PendingInteraction const pending = itr->second;
        Site* site = FindSite(pending.siteId);
        float const dx = player ? player->GetPositionX() - pending.startX : 0.0f;
        float const dy = player ? player->GetPositionY() - pending.startY : 0.0f;
        if (!player || !site || !player->IsAlive() || player->GetGuildId() != pending.guildId ||
            dx * dx + dy * dy > 9.0f || !Contains(*site, player))
        {
            if (player)
                player->PSendSysMessage("Sovereignty channel interrupted.");
            itr = m_pendingInteractions.erase(itr);
            continue;
        }
        if (now < pending.completesAt)
        {
            ++itr;
            continue;
        }
        itr = m_pendingInteractions.erase(itr);
        if (pending.nodeIndex < 0)
            HandleHub(*site, player, now, true);
        else
            HandleNode(*site, uint8(pending.nodeIndex), player, now, true);
    }
}

void SovereigntyMgr::AccrueAndUpkeep(Site& site, uint64 now)
{
    if (!site.ownerGuildId || !site.lastAccrualAt || now < site.lastAccrualAt)
        return;
    uint64 const hours = (now - site.lastAccrualAt) / 3600;
    uint64 const days = site.nextUpkeepAt && now >= site.nextUpkeepAt ?
        (now - site.nextUpkeepAt) / 86400 + 1 : 0;
    if (!hours && !days)
        return;
    if (hours)
    {
        if (site.state == CLAIM_SECURE)
            site.treasury = std::min<uint32>(m_rules.supplyCap,
                site.treasury + uint32(hours) * m_rules.supplyPerHour);
        site.lastAccrualAt += hours * 3600;
    }
    for (uint64 day = 0; day < days; ++day)
    {
        uint32 const cost = site.infrastructureTier == 3 ? m_rules.strongholdDailyUpkeep :
            (site.infrastructureTier == 2 ? m_rules.outpostDailyUpkeep : 0);
        if (site.treasury >= cost)
            site.treasury -= cost;
        else if (site.infrastructureTier > 1)
            --site.infrastructureTier;
    }
    if (days)
        site.nextUpkeepAt += days * 86400;
    ++site.revision;
    Persist(site, nullptr, "economy-updated");
    ReconcileStructures(site);
    BroadcastSiteWorldStates(site);
}

void SovereigntyMgr::Resolve(Site& site, bool challengerWon, uint64 now, char const* eventType)
{
    if (challengerWon)
    {
        site.ownerGuildId = site.challengerGuildId;
        site.infrastructureTier = std::max<uint32>(1, site.infrastructureTier - 1);
        site.treasury /= 2;
        time_t wallTime = time_t(now);
        tm const utc = *gmtime(&wallTime);
        site.vulnerabilityMidpointUtc = utc.tm_hour * 60 + utc.tm_min;
    }
    site.state = CLAIM_SECURE;
    site.challengerGuildId = 0;
    site.contestAt = 0;
    site.contestEndsAt = 0;
    site.defenderScore = 0;
    site.challengerScore = 0;
    site.lastAccrualAt = now;
    site.nextUpkeepAt = now + 86400;
    ResetNodes(site);
    Persist(site, nullptr, eventType);
    ReconcileStructures(site);
}

bool SovereigntyMgr::ShouldLoadGameObject(uint32 guid) const
{
    static uint32 const firstGuid = 965000;
    static uint32 const lastGuid = 977999;
    if (guid < firstGuid || guid > lastGuid)
        return true;
    if (!m_enabled)
        return false;
    if (m_objects.find(guid) != m_objects.end())
        return true;
    auto const itr = m_structures.find(guid);
    if (itr == m_structures.end())
        return false;
    Site const* site = FindSite(itr->second.siteId);
    return site && site->ownerGuildId && site->infrastructureTier >= itr->second.tierMin
        && site->infrastructureTier <= itr->second.tierMax;
}

void SovereigntyMgr::ReconcileStructures(Site const& site)
{
    Map* map = sMapMgr.FindMap(site.mapId);
    if (!map)
        return;
    for (auto const& entry : m_structures)
    {
        if (entry.second.siteId != site.id)
            continue;
        bool const active = site.ownerGuildId && site.infrastructureTier >= entry.second.tierMin
            && site.infrastructureTier <= entry.second.tierMax;
        ObjectGuid const guid(HIGHGUID_GAMEOBJECT, entry.second.entry, entry.first);
        GameObject* object = map->GetGameObject(guid);
        if (active && !object)
            map->LoadGameObjectSpawn(entry.first);
        else if (!active && object)
            object->AddObjectToRemoveList();
    }
}

uint32 SovereigntyMgr::CountClaimParticipants(Player const* player) const
{
    uint32 count = 0;
    for (auto itr = player->GetMap()->GetPlayers().getFirst(); itr; itr = itr->next())
    {
        Player const* nearby = itr->getSource();
        if (nearby && nearby->IsAlive() && nearby->GetGuildId() == player->GetGuildId()
            && player->IsWithinDistInMap(nearby, 40.0f))
            ++count;
    }
    return count;
}

bool SovereigntyMgr::IsInVulnerabilityWindow(Site const& site, uint64 now) const
{
    time_t wallTime = time_t(now);
    tm const utc = *gmtime(&wallTime);
    int const minute = utc.tm_hour * 60 + utc.tm_min;
    int distance = std::abs(minute - int(site.vulnerabilityMidpointUtc));
    distance = std::min(distance, 1440 - distance);
    return distance * 2 <= int(m_rules.vulnerabilityMinutes);
}

void SovereigntyMgr::ResetNodes(Site& site)
{
    site.nodeOwnerGuildIds.fill(0);
    site.nodeLockedUntil.fill(0);
}

void SovereigntyMgr::Persist(Site const& site, Player const* actor, char const* eventType)
{
    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute(
        "REPLACE INTO `coworld_territory_claim` (`site_id`,`topology_revision`,`revision`,`state`,"
        "`owner_guild_id`,`vulnerability_midpoint_utc`,`challenger_guild_id`,`contest_at`,"
        "`contest_ends_at`,`defender_score`,`challenger_score`,`infrastructure_tier`,"
        "`treasury`,`last_accrual_at`,`next_upkeep_at`) VALUES "
        "('%s',%u,%u,%u,NULLIF(%u,0),NULLIF(%u,0),NULLIF(%u,0),NULLIF(%llu,0),"
        "NULLIF(%llu,0),%u,%u,%u,%u,NULLIF(%llu,0),NULLIF(%llu,0))",
        site.id.c_str(), m_rules.topologyRevision, site.revision, uint32(site.state),
        site.ownerGuildId, site.vulnerabilityMidpointUtc, site.challengerGuildId,
        static_cast<unsigned long long>(site.contestAt),
        static_cast<unsigned long long>(site.contestEndsAt), site.defenderScore,
        site.challengerScore, site.infrastructureTier, site.treasury,
        static_cast<unsigned long long>(site.lastAccrualAt),
        static_cast<unsigned long long>(site.nextUpkeepAt));
    CharacterDatabase.PExecute(
        "DELETE FROM `coworld_territory_node_state` WHERE `site_id`='%s'",
        site.id.c_str());
    for (uint32 index = 0; index < site.nodeOwnerGuildIds.size(); ++index)
    {
        if (!site.nodeOwnerGuildIds[index])
            continue;
        CharacterDatabase.PExecute(
            "INSERT INTO `coworld_territory_node_state` (`site_id`,`node_index`,`owner_guild_id`,`locked_until`,`claim_revision`) "
            "VALUES ('%s',%u,%u,NULLIF(%llu,0),%u)", site.id.c_str(), index,
            site.nodeOwnerGuildIds[index],
            static_cast<unsigned long long>(site.nodeLockedUntil[index]), site.revision);
    }
    CharacterDatabase.PExecute(
        "INSERT INTO `coworld_territory_event` (`site_id`,`claim_revision`,`event_type`,"
        "`actor_character_guid`,`actor_guild_id`) VALUES ('%s',%u,'%s',NULLIF(%u,0),NULLIF(%u,0))",
        site.id.c_str(), site.revision, eventType,
        actor ? actor->GetGUIDLow() : 0, actor ? actor->GetGuildId() : 0);
    CharacterDatabase.CommitTransaction();
}

void SovereigntyMgr::HandleHub(Site& site, Player* player, uint64 now, bool completed)
{
    uint32 const guildId = player->GetGuildId();
    if (!guildId)
    {
        player->PSendSysMessage("%s is a guild sovereignty site; join a guild to use it.", site.name.c_str());
        return;
    }
    if (site.state == CLAIM_UNCLAIMED)
    {
        if (player->GetRank() > m_rules.claimRank)
        {
            player->PSendSysMessage("Only a guild leader or officer can claim %s.", site.name.c_str());
            return;
        }
        uint32 const participants = CountClaimParticipants(player);
        if (participants < m_rules.claimParticipants)
        {
            player->PSendSysMessage("Claiming %s requires %u living guild members within 40 yards (%u present).",
                site.name.c_str(), m_rules.claimParticipants, participants);
            return;
        }
        if (!completed && BeginInteraction(site, -1, player, now))
            return;
        time_t wallTime = time_t(now);
        tm const utc = *gmtime(&wallTime);
        site.state = CLAIM_SECURE;
        site.ownerGuildId = guildId;
        site.vulnerabilityMidpointUtc = utc.tm_hour * 60 + utc.tm_min;
        site.infrastructureTier = 1;
        site.treasury = 0;
        site.lastAccrualAt = now;
        site.nextUpkeepAt = now + 86400;
        ++site.revision;
        Persist(site, player, "claimed");
        ReconcileStructures(site);
        player->PSendSysMessage("Your guild claimed %s. Its daily vulnerability window is centered on %02u:%02u UTC.",
            site.name.c_str(), site.vulnerabilityMidpointUtc / 60, site.vulnerabilityMidpointUtc % 60);
        BroadcastSiteWorldStates(site);
        return;
    }
    if (site.state == CLAIM_SECURE && guildId == site.ownerGuildId)
    {
        AccrueAndUpkeep(site, now);
        uint32 const cost = site.infrastructureTier == 1 ? m_rules.outpostCost :
            (site.infrastructureTier == 2 ? m_rules.strongholdCost : 0);
        if (cost && site.treasury >= cost && player->GetRank() <= m_rules.claimRank)
        {
            site.treasury -= cost;
            ++site.infrastructureTier;
            ++site.revision;
            Persist(site, player, "infrastructure-upgraded");
            ReconcileStructures(site);
            BroadcastSiteWorldStates(site);
            player->PSendSysMessage("%s upgraded to infrastructure tier %u.",
                site.name.c_str(), site.infrastructureTier);
            return;
        }
        if (site.infrastructureTier >= 2)
        {
            uint32 const serviceSupply = site.infrastructureTier == 3 ? 1 : 2;
            if (site.treasury < serviceSupply)
            {
                player->PSendSysMessage("%s's workshop needs %u treasury supply to repair equipment.",
                    site.name.c_str(), serviceSupply);
                return;
            }
            player->DurabilityRepairAll(false, 0.0f);
            site.treasury -= serviceSupply;
            ++site.revision;
            Persist(site, player, "workshop-repair");
            BroadcastSiteWorldStates(site);
            player->PSendSysMessage("%s's tier %u workshop repaired your equipment for %u treasury supply.",
                site.name.c_str(), site.infrastructureTier, serviceSupply);
            return;
        }
    }
    if (site.state == CLAIM_SECURE && guildId != site.ownerGuildId)
    {
        if (player->GetRank() > m_rules.claimRank)
        {
            player->PSendSysMessage("Only a guild leader or officer can challenge %s.", site.name.c_str());
            return;
        }
        uint32 const participants = CountClaimParticipants(player);
        if (participants < m_rules.claimParticipants)
        {
            player->PSendSysMessage("Challenging %s requires %u living guild members within 40 yards (%u present).",
                site.name.c_str(), m_rules.claimParticipants, participants);
            return;
        }
        if (!IsInVulnerabilityWindow(site, now))
        {
            player->PSendSysMessage("%s is outside its daily vulnerability window centered on %02u:%02u UTC.",
                site.name.c_str(), site.vulnerabilityMidpointUtc / 60, site.vulnerabilityMidpointUtc % 60);
            return;
        }
        if (!completed && BeginInteraction(site, -1, player, now))
            return;
        site.state = CLAIM_REINFORCED;
        site.challengerGuildId = guildId;
        site.contestAt = now + uint64(m_rules.reinforcementWarningHours) * 3600;
        site.defenderScore = 0;
        site.challengerScore = 0;
        ResetNodes(site);
        ++site.revision;
        Persist(site, player, "reinforced");
        player->PSendSysMessage("Your guild reinforced %s. Its five command nodes activate in %u hours.",
            site.name.c_str(), m_rules.reinforcementWarningHours);
        BroadcastSiteWorldStates(site);
        return;
    }
    player->PSendSysMessage("%s: state %u, owner guild %u, challenger guild %u, score %u-%u, tier %u, treasury %u/%u.",
        site.name.c_str(), uint32(site.state), site.ownerGuildId, site.challengerGuildId,
        site.defenderScore, site.challengerScore, site.infrastructureTier, site.treasury,
        m_rules.supplyCap);
    SendSiteWorldStates(site, player);
}

void SovereigntyMgr::HandleNode(Site& site, uint8 nodeIndex, Player* player, uint64 now, bool completed)
{
    if (site.state == CLAIM_REINFORCED && now >= site.contestAt)
    {
        site.state = CLAIM_CONTESTED;
        site.contestEndsAt = now + uint64(m_rules.contestMinutes) * 60;
        ++site.revision;
        Persist(site, player, "contest-started");
    }
    if (site.state != CLAIM_CONTESTED)
    {
        player->PSendSysMessage("This command node is dormant until %s reaches its final contest.", site.name.c_str());
        return;
    }
    uint32 const guildId = player->GetGuildId();
    if (guildId != site.ownerGuildId && guildId != site.challengerGuildId)
    {
        player->PSendSysMessage("Only the defender and challenger guilds can capture this command node.");
        return;
    }
    if (site.nodeOwnerGuildIds[nodeIndex] == guildId)
    {
        player->PSendSysMessage("Your guild already controls this command node.");
        return;
    }
    if (site.nodeLockedUntil[nodeIndex] > now)
    {
        player->PSendSysMessage("This command node is temporarily locked after its last capture.");
        return;
    }
    if (!completed && BeginInteraction(site, int8(nodeIndex), player, now))
        return;
    site.nodeOwnerGuildIds[nodeIndex] = guildId;
    site.nodeLockedUntil[nodeIndex] = now + m_rules.recaptureLockSeconds;
    ++site.revision;
    Persist(site, player, "node-captured");
    player->PSendSysMessage("%s command node captured. Control progress %u-%u.",
        site.name.c_str(), site.defenderScore, site.challengerScore);
    BroadcastSiteWorldStates(site);
}

bool SovereigntyMgr::HandleGameObjectUse(GameObject const* object, Player* player)
{
    if (!m_enabled || !object || !object->GetDBTableGUIDLow())
        return false;
    auto const objectItr = m_objects.find(object->GetDBTableGUIDLow());
    if (objectItr == m_objects.end())
        return false;
    Site* site = FindSite(objectItr->second.siteId);
    if (!site)
        return true;
    if (!player->IsAlive())
    {
        player->PSendSysMessage("You must be alive to interact with a sovereignty site.");
        return true;
    }
    uint64 const now = uint64(time(nullptr));
    if (objectItr->second.nodeIndex < 0)
        HandleHub(*site, player, now);
    else
        HandleNode(*site, uint8(objectItr->second.nodeIndex), player, now);
    return true;
}

void SovereigntyMgr::SendSiteWorldStates(Site const& site, Player* player) const
{
    player->SendUpdateWorldState(site.worldStateBase, uint32(site.state));
    player->SendUpdateWorldState(site.worldStateBase + 1, site.ownerGuildId);
    player->SendUpdateWorldState(site.worldStateBase + 2, site.challengerGuildId);
    player->SendUpdateWorldState(site.worldStateBase + 3, site.defenderScore);
    player->SendUpdateWorldState(site.worldStateBase + 4, site.challengerScore);
    player->SendUpdateWorldState(site.worldStateBase + 5,
        uint32(site.state == CLAIM_CONTESTED ? site.contestEndsAt : site.contestAt));
    player->SendUpdateWorldState(site.worldStateBase + 6, site.vulnerabilityMidpointUtc);
    for (uint32 index = 0; index < site.nodeOwnerGuildIds.size(); ++index)
        player->SendUpdateWorldState(site.worldStateBase + 7 + index, site.nodeOwnerGuildIds[index]);
    player->SendUpdateWorldState(site.worldStateBase + 12,
        site.ownerGuildId ? site.infrastructureTier : 0);
    player->SendUpdateWorldState(site.worldStateBase + 13, site.treasury);
    player->SendUpdateWorldState(site.worldStateBase + 14, m_rules.supplyCap);
    player->SendUpdateWorldState(site.worldStateBase + 15, m_rules.topologyRevision);
}

void SovereigntyMgr::BroadcastSiteWorldStates(Site const& site) const
{
    auto const sessions = sWorld.GetAllSessions();
    for (auto const& entry : sessions)
    {
        Player* player = entry.second->GetPlayer();
        if (player)
            SendSiteWorldStates(site, player);
    }
}

uint32 SovereigntyMgr::FillInitialWorldStates(WorldPacket& data, uint32 /*mapId*/, uint32 /*zoneId*/) const
{
    if (!m_enabled)
        return 0;
    uint32 count = 0;
    for (auto const& entry : m_sites)
    {
        Site const& site = entry.second;
        data << uint32(site.worldStateBase) << uint32(site.state);
        data << uint32(site.worldStateBase + 1) << site.ownerGuildId;
        data << uint32(site.worldStateBase + 2) << site.challengerGuildId;
        data << uint32(site.worldStateBase + 3) << site.defenderScore;
        data << uint32(site.worldStateBase + 4) << site.challengerScore;
        data << uint32(site.worldStateBase + 5) <<
            uint32(site.state == CLAIM_CONTESTED ? site.contestEndsAt : site.contestAt);
        data << uint32(site.worldStateBase + 6) << site.vulnerabilityMidpointUtc;
        for (uint32 index = 0; index < site.nodeOwnerGuildIds.size(); ++index)
            data << uint32(site.worldStateBase + 7 + index) << site.nodeOwnerGuildIds[index];
        data << uint32(site.worldStateBase + 12) <<
            uint32(site.ownerGuildId ? site.infrastructureTier : 0);
        data << uint32(site.worldStateBase + 13) << site.treasury;
        data << uint32(site.worldStateBase + 14) << m_rules.supplyCap;
        data << uint32(site.worldStateBase + 15) << m_rules.topologyRevision;
        count += 16;
    }
    return count;
}

uint32 SovereigntyMgr::GetOwnerGuildForBuildDistrict(std::string const& districtId) const
{
    if (!m_enabled)
        return 0;
    for (auto const& entry : m_sites)
        if (entry.second.buildDistrictId == districtId)
            return entry.second.ownerGuildId;
    return 0;
}

uint32 SovereigntyMgr::GetInfrastructureTierForBuildDistrict(std::string const& districtId) const
{
    if (!m_enabled)
        return 0;
    for (auto const& entry : m_sites)
        if (entry.second.buildDistrictId == districtId && entry.second.ownerGuildId)
            return entry.second.infrastructureTier;
    return 0;
}
