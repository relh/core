#ifndef MANGOS_SOVEREIGNTY_MGR_H
#define MANGOS_SOVEREIGNTY_MGR_H

#include "Common.h"

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

class GameObject;
class Player;
class WorldPacket;

class SovereigntyMgr
{
    public:
        static SovereigntyMgr& Instance();

        void Load();
        void Update(uint32 diff);
        bool IsEnabled() const { return m_enabled; }
        bool HandleGameObjectUse(GameObject const* object, Player* player);
        uint32 FillInitialWorldStates(WorldPacket& data, uint32 mapId, uint32 zoneId) const;
        uint32 GetOwnerGuildForBuildDistrict(std::string const& districtId) const;
        uint32 GetInfrastructureTierForBuildDistrict(std::string const& districtId) const;
        bool AreContestEnemies(Player const* left, Player const* right) const;
        bool AreContestAllies(Player const* left, Player const* right) const;
        bool ShouldLoadGameObject(uint32 guid) const;

    private:
        enum ClaimState : uint8
        {
            CLAIM_UNCLAIMED = 0,
            CLAIM_SECURE = 1,
            CLAIM_REINFORCED = 2,
            CLAIM_CONTESTED = 3,
        };

        struct Rules
        {
            uint32 topologyRevision = 0;
            uint32 claimRank = 1;
            uint32 claimParticipants = 5;
            uint32 claimChannelSeconds = 30;
            uint32 challengeChannelSeconds = 30;
            uint32 vulnerabilityMinutes = 120;
            uint32 reinforcementWarningHours = 24;
            uint32 contestMinutes = 60;
            uint32 captureChannelSeconds = 10;
            uint32 recaptureLockSeconds = 20;
            uint32 controlTickSeconds = 5;
            uint32 majorityPoints = 5;
            uint32 supermajorityPoints = 7;
            uint32 totalControlPoints = 10;
            uint32 victoryPoints = 900;
            uint32 supplyPerHour = 2;
            uint32 supplyCap = 336;
            uint32 outpostCost = 96;
            uint32 strongholdCost = 240;
            uint32 outpostDailyUpkeep = 12;
            uint32 strongholdDailyUpkeep = 36;
        };

        struct Site
        {
            std::string id;
            std::string name;
            std::string buildDistrictId;
            uint32 mapId = 0;
            uint32 zoneId = 0;
            uint32 worldStateBase = 0;
            uint32 hubGuid = 0;
            std::vector<std::pair<float, float>> polygon;
            std::array<uint32, 5> nodeGuids{{0, 0, 0, 0, 0}};
            ClaimState state = CLAIM_UNCLAIMED;
            uint32 revision = 0;
            uint32 ownerGuildId = 0;
            uint32 vulnerabilityMidpointUtc = 0;
            uint32 challengerGuildId = 0;
            uint64 contestAt = 0;
            uint64 contestEndsAt = 0;
            uint32 defenderScore = 0;
            uint32 challengerScore = 0;
            uint32 infrastructureTier = 1;
            uint32 treasury = 0;
            uint64 lastAccrualAt = 0;
            uint64 nextUpkeepAt = 0;
            std::array<uint32, 5> nodeOwnerGuildIds{{0, 0, 0, 0, 0}};
            std::array<uint64, 5> nodeLockedUntil{{0, 0, 0, 0, 0}};
        };

        struct ObjectRef
        {
            std::string siteId;
            int8 nodeIndex = -1;
        };

        struct PendingInteraction
        {
            std::string siteId;
            int8 nodeIndex = -1;
            uint32 guildId = 0;
            uint64 completesAt = 0;
            float startX = 0.0f;
            float startY = 0.0f;
        };

        struct StructureRef
        {
            std::string siteId;
            uint8 tierMin = 1;
            uint8 tierMax = 3;
            uint32 entry = 0;
        };

        Site* FindSite(std::string const& siteId);
        Site const* FindSite(std::string const& siteId) const;
        void HandleHub(Site& site, Player* player, uint64 now, bool completed = false);
        void HandleNode(Site& site, uint8 nodeIndex, Player* player, uint64 now, bool completed = false);
        bool BeginInteraction(Site const& site, int8 nodeIndex, Player* player, uint64 now);
        void UpdateInteractions(uint64 now);
        void Persist(Site const& site, Player const* actor, char const* eventType);
        void ResetNodes(Site& site);
        void SendSiteWorldStates(Site const& site, Player* player) const;
        void BroadcastSiteWorldStates(Site const& site) const;
        bool IsInVulnerabilityWindow(Site const& site, uint64 now) const;
        uint32 CountClaimParticipants(Player const* player) const;
        bool Contains(Site const& site, Player const* player, float buffer = 0.0f) const;
        void AccrueAndUpkeep(Site& site, uint64 now);
        void Resolve(Site& site, bool challengerWon, uint64 now, char const* eventType);
        void ReconcileStructures(Site const& site);

        bool m_enabled = false;
        Rules m_rules;
        uint32 m_updateTimer = 0;
        std::map<std::string, Site> m_sites;
        std::map<uint32, ObjectRef> m_objects;
        std::map<uint32, StructureRef> m_structures;
        std::map<uint32, PendingInteraction> m_pendingInteractions;
};

#define sSovereigntyMgr SovereigntyMgr::Instance()

#endif
