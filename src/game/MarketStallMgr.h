#ifndef _MARKET_STALL_MGR_H
#define _MARKET_STALL_MGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Policies/Singleton.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class AuctionEntry;
class Creature;
class Item;
class Player;
class QueryResult;
class SqlQueryHolder;

struct MarketStallPad
{
    uint32 stallGuid;
    uint32 clerkGuid;
    uint32 mapId;
    float x;
    float y;
    float z;
    float radius;
};

struct MarketStallClaim
{
    uint32 stallGuid;
    uint32 ownerAccountId;
    uint32 ownerGuid;
    uint8 category;
    uint64 claimedAt;
    uint64 leaseExpiresAt;
    bool closing;
};

struct MarketStallOffer
{
    uint32 stallGuid;
    uint32 offerId;
    uint8 serviceKind;
    uint32 sellerGuid;
    uint32 spellId;
    uint32 priceCopper;
};

struct MarketStallAsset
{
    uint32 assetGuid;
    uint32 stallGuid;
    std::string role;
    bool visible;
};

struct MarketStallOrder
{
    uint64 nonce;
    uint32 stallGuid;
    uint32 offerId;
    uint32 buyerGuid;
    uint32 sellerGuid;
    uint32 spellId;
    uint32 priceCopper;
};

class MarketStallMgr
{
public:
    MarketStallMgr();

    void Load();
    void Update();
    bool IsEnabled() const { return m_enabled; }
    bool IsStallGuid(uint32 stallGuid) const;
    uint32 GetStallGuid(ObjectGuid auctioneerGuid) const;
    void OnAuctionChanged(uint32 stallGuid);

    bool HandleGossipHello(Player* player, Creature* creature);
    bool HandleGossipSelect(Player* player, Creature* creature, uint32 sender,
                            uint32 action, char const* code);

    bool CanOpenAuction(Player* player, uint32 stallGuid) const;
    bool ValidateAuctionListing(Player* player, uint32 stallGuid, uint32 bid,
                                uint32 buyout, uint32 durationMinutes) const;
    bool CanAccessAuction(AuctionEntry const* auction, uint32 stallGuid) const;
    bool IsAuctionVisible(AuctionEntry const* auction, uint32 stallGuid) const;

    void CloseClaimsForCharacter(uint32 characterGuid, char const* reason);
    bool ValidateServiceAccept(Player* player);
    bool HandleTradeMoneyMutation(Player* player, uint32 money);
    bool HandleTradeItemMutation(Player* player, uint8 tradeSlot);
    void OnTradeItemChanged(Player* player, uint8 tradeSlot);
    bool HandleTradeItemClear(Player* player, uint8 tradeSlot);
    void OnTradeWindowOpened(Player* player);
    void OnTradeCanceled(Player* player, char const* reason);
    void OnTradeCompleted(Player* player);

private:
    typedef std::map<uint32, MarketStallPad> PadMap;
    typedef std::map<uint32, MarketStallClaim> ClaimMap;
    typedef std::map<uint32, std::vector<MarketStallOffer> > OfferMap;
    typedef std::map<uint32, MarketStallAsset> AssetMap;

    MarketStallClaim const* FindClaim(uint32 stallGuid) const;
    MarketStallClaim const* FindAccountClaim(uint32 accountId) const;
    MarketStallOffer const* FindOffer(uint32 stallGuid, uint32 offerId) const;
    MarketStallOrder* FindOrder(uint32 playerGuid);
    MarketStallOrder const* FindOrder(uint32 playerGuid) const;
    bool IsClaimActive(MarketStallClaim const* claim) const;
    bool IsPlayerNearPad(Player const* player, uint32 stallGuid) const;
    bool IsOfferSpell(uint32 spellId) const;
    bool IsCraftSpell(uint32 spellId) const;
    bool IsOfferCurrent(MarketStallOffer const& offer) const;
    bool HasActiveStock(uint32 stallGuid) const;
    bool HasPendingLeaseMutation(uint32 accountId) const;
    bool GetClaimCooldown(uint32 accountId, uint64& eligibleAt) const;
    char const* CategoryName(uint8 category) const;
    std::string StallName(MarketStallClaim const& claim) const;
    bool IsSellerAvailable(MarketStallOffer const& offer, Player const* buyer) const;
    bool ValidateOrder(MarketStallOrder const& order, bool requireTarget) const;
    void ShowMainMenu(Player* player, Creature* creature);
    void ShowCategoryMenu(Player* player, Creature* creature);
    void ShowOfferMenu(Player* player, Creature* creature, bool manage, bool craft, uint32 page);
    void Claim(Player* player, uint32 stallGuid, uint8 category);
    void Renew(Player* player, uint32 stallGuid);
    void Release(Player* player, uint32 stallGuid);
    void AddOffer(Player* player, uint32 stallGuid, uint32 spellId, uint8 serviceKind, char const* code);
    void RemoveOffer(Player* player, uint32 stallGuid, uint32 offerId);
    void OpenServiceOrder(Player* buyer, uint32 stallGuid, uint32 offerId);
    void NotifyCraftAdvert(Player* buyer, uint32 stallGuid, uint32 offerId);
    bool CloseClaim(uint32 stallGuid, char const* reason, bool cooldown);
    void CancelOrders(uint32 stallGuid, uint32 offerId, char const* reason);
    void EraseOrder(uint32 buyerGuid, char const* reason, bool completed);
    void RejectOrder(Player* player, char const* reason);
    bool ReloadClaims();
    bool ReloadOffers();
    void PruneInvalidOffers();
    bool QueueLeaseMutationBarrier(uint32 playerGuid, uint32 accountId);
    void OnLeaseMutationSettled(std::unique_ptr<QueryResult>, SqlQueryHolder* holder);
    void SyncAllAssets();
    void SyncAssets(uint32 stallGuid);
    void SetAssetVisible(MarketStallAsset& asset, bool visible);
    bool AllowMutation(Player* player);

    bool m_enabled;
    uint64 m_nextOrderNonce;
    uint64 m_nextOfferValidationAt;
    PadMap m_pads;
    ClaimMap m_claims;
    OfferMap m_offers;
    AssetMap m_assets;
    std::map<uint32, uint32> m_stallByClerk;
    std::map<uint32, MarketStallOrder> m_orders;
    std::map<uint32, uint32> m_orderByPlayer;
    std::map<uint32, uint32> m_lastMutationAt;
    std::map<uint32, uint64> m_lastCraftRequestAt;
    std::set<uint32> m_pendingLeaseAccounts;
    std::map<uint32, uint32> m_pendingLeasePlayers;
};

#define sMarketStallMgr MaNGOS::Singleton<MarketStallMgr>::Instance()

#endif
