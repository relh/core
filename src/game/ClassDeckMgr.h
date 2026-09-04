#ifndef MANGOS_CLASS_DECK_MGR_H
#define MANGOS_CLASS_DECK_MGR_H

#include "Common.h"
#include "Objects/CreatureDefines.h"

#include <array>
#include <string>
#include <vector>

class Player;
class WorldSession;

enum ClassDeckDraftState
{
    CLASS_DECK_DRAFT_PENDING = 0,
    CLASS_DECK_DRAFT_SELECTED = 1,
    CLASS_DECK_DRAFT_APPLIED = 2,
    CLASS_DECK_DRAFT_BLOCKED = 3,
};

enum ClassDeckResultCode
{
    CLASS_DECK_RESULT_APPLIED = 0,
    CLASS_DECK_RESULT_UNAVAILABLE = 1,
    CLASS_DECK_RESULT_STALE = 2,
    CLASS_DECK_RESULT_BLOCKED = 3,
    CLASS_DECK_RESULT_DISABLED = 4,
    CLASS_DECK_RESULT_MALFORMED = 5,
    CLASS_DECK_RESULT_BUSY = 6,
    CLASS_DECK_RESULT_DEFERRED = 7,
};

enum ClassDeckApplyResult
{
    CLASS_DECK_APPLY_APPLIED,
    CLASS_DECK_APPLY_RETRYABLE,
    CLASS_DECK_APPLY_DEFERRED,
    CLASS_DECK_APPLY_BLOCKED,
};

struct ClassDeckDraft
{
    uint32 index = 0;
    uint32 earnedLevel = 0;
    std::string catalogVersion;
    std::array<uint32, 3> offers = {{0, 0, 0}};
    uint32 selectedService = 0;
    ClassDeckDraftState state = CLASS_DECK_DRAFT_PENDING;
};

class ClassDeckMgr
{
public:
    static ClassDeckMgr& Instance();

    void LoadCatalog();
    bool IsEnabled() const { return m_enabled; }
    bool IsCoveredService(uint8 classId, uint32 serviceSpell) const;
    void OnLevelChanged(Player* player);
    void SendState(WorldSession* session, bool reconcile = true);
    void Choose(
        WorldSession* session,
        uint8 protocolVersion,
        uint32 draftIndex,
        uint32 serviceSpell,
        std::string const& catalogVersion);

private:
    using Catalog = std::array<std::vector<TrainerSpell>, MAX_CLASSES>;

    std::vector<ClassDeckDraft> LoadDrafts(uint32 guid) const;
    void Reconcile(Player* player);
    bool EnsureNextDraft(Player* player, std::vector<ClassDeckDraft> const& drafts);
    bool BlockPending(Player* player, ClassDeckDraft const& draft);
    ClassDeckApplyResult ApplySelected(
        Player* player, ClassDeckDraft const& draft);
    std::vector<TrainerSpell> EligibleServices(Player* player) const;
    std::array<uint32, 3> SampleOffers(
        Player const* player,
        uint32 draftIndex,
        std::vector<TrainerSpell> eligible) const;
    TrainerSpell const* FindService(uint8 classId, uint32 serviceSpell) const;
    std::vector<uint32> TaughtSpells(uint32 serviceSpell) const;
    void SendResult(
        WorldSession* session,
        uint32 draftIndex,
        uint32 serviceSpell,
        ClassDeckResultCode result) const;

    Catalog m_catalog;
    std::string m_catalogVersion;
    bool m_enabled = false;
    bool m_testSeedSet = false;
    bool m_interruptAfterSelectOnce = false;
    bool m_interruptConsumed = false;
    uint32 m_testSeed = 0;
};

#define sClassDeckMgr ClassDeckMgr::Instance()

#endif
