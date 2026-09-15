/*
 * local: company standing (custom wow plans/14, step C, the behaviour tier).
 *
 * Guild seats, contested holdings and stances between guilds are kept outside the worldserver by
 * /opt/wow/regard/regard.py and rivalry.py in acore_characters (guild_seat, guild_holding,
 * guild_relation). This cache reloads them on a background thread and swaps a snapshot in whole, so
 * the checks below cost a mutex and a hash lookup. Every behaviour has a chance in
 * AiPlayerbot.Company*; all default to 0, which leaves stock behaviour untouched.
 */

#ifndef PLAYERBOTS_COMPANYSTANDING_H
#define PLAYERBOTS_COMPANYSTANDING_H

#include "Define.h"
#include "ObjectGuid.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Guild;
class Player;
class PlayerbotAI;

class CompanyStanding
{
public:
    static CompanyStanding& instance();

    // World thread: starts a reload every AiPlayerbot.CompanyRefreshSeconds while any chance is set.
    void Update();

    // The zone is one of the company's seats, or a land it holds against a challenger or is contesting.
    bool PullsToward(uint32 guildId, uint32 zoneId) const;

    // Stance of two different companies at or below AiPlayerbot.CompanyRivalStance.
    bool AreRivals(uint32 guildA, uint32 guildB) const;

    // An idle bot may call out a nearby bot of a rival company to a duel (AiPlayerbot.CompanyRivalDuelChance,
    // rolled at most every 30 s per bot). Never a real player. True when the challenge was made.
    bool TryChallengeRival(PlayerbotAI* botAI, Player* bot);

    // local: company regard gate (custom wow plans/18, step P2). With AiPlayerbot.CompanyRegardGate = 1 an
    // unguilded bot answers a real player's charter or company invite from its regard for them, kept in the
    // same snapshot. The reason is a short code for company_answer.
    struct Verdict
    {
        bool accept;
        char const* reason;
    };

    // Put its name to the player's guild charter: regard at least CompanySignRegard, known at least
    // CompanySignFamiliarity moments.
    Verdict JudgeCharter(Player* bot, Player* from) const;

    // Join the player's company: regard at least CompanyJoinRegard, or CompanyJoinFriendRegard with a friend
    // (CompanyFriendRegard) inside; never with someone it hates (CompanyEnemyRegard) inside.
    Verdict JudgeInvite(Player* bot, Player* from, uint32 guildId) const;

    // Async insert into company_answer when the table exists. kind is "sign" or "join"; guildId 0 for a charter.
    void RecordAnswer(Player* bot, Player* from, char const* kind, uint32 guildId, bool accepted, char const* reason) const;

    // local: company defection (custom wow plans/18, step P7). A guilded bot answering a real player's company invite
    // leaves its own company for theirs: regard for the player at least CompanyDefectRegard, attachment (mean regard
    // for its own company's members; none recorded counts as +20) below CompanyDefectAttachment, nobody it hates
    // inside, a company that can spare it (never its leader; a bot-led company keeps more than CompanyFloor members
    // and its last officer), and CompanyDefectChance, rolled once per bot and player every 6 hours. World thread.
    Verdict JudgeDefection(Player* bot, Player* from, uint32 guildId) const;

private:
    struct Feeling
    {
        float score = 0.0f;
        uint32 familiarity = 0;
    };

    // local: company actions (custom wow plans/18, step P4). regard.py decides that a company invites, raises,
    // lowers or casts out a real player (company_action); with AiPlayerbot.CompanyActions = 1 an officer bot of
    // the company carries it out through its own session, face to face while nearOnly is set.
    struct CompanyAction
    {
        uint32 id = 0;
        uint32 guildId = 0;
        uint32 playerGuid = 0;
        uint32 preferGuid = 0;
        uint8 kind = 0;
        bool nearOnly = true;
        std::string words;
    };

    struct Data
    {
        std::unordered_map<uint32, std::unordered_set<uint32>> pulls;  // guild id -> zone ids
        std::unordered_map<uint64, float> stances;                     // PairKey -> -100 .. 100
        // Regard gate only (plans/18 P2):
        std::unordered_map<uint64, Feeling> towardPlayers;             // (bot << 32) | real player -> feeling
        std::unordered_map<uint64, uint32> friendsInside;              // (bot << 32) | guild -> members it likes
        std::unordered_map<uint64, uint32> enemiesInside;              // (bot << 32) | guild -> members it hates
        bool answerTable = false;
        // Company defection only (plans/18 P7):
        std::unordered_map<uint32, float> attachment;   // bot -> mean regard for its own company's members
        std::unordered_map<uint32, uint32> officers;    // guild id -> members at officer rank
        // Company actions only (plans/18 P4): the rows not yet done.
        std::vector<CompanyAction> actions;
    };

    static bool Enabled();
    void Load();
    std::shared_ptr<Data const> Snapshot() const;

    // World thread, every few seconds: carry out each pending action an able officer is placed to.
    void ActOnCompanyActions();
    // plans/18 P6: leave and whisper rows are carried out by the bot named in prefer_guid itself.
    void ActAsSelf(CompanyAction const& action);
    Player* FindActor(Guild* guild, Player* player, CompanyAction const& action, uint8 playerRank) const;
    void FinishAction(CompanyAction const& action, Player* actor, char const* result);

    mutable std::mutex _mutex;
    std::shared_ptr<Data const> _data;
    std::atomic<bool> _loading{false};
    uint32 _lastLoad = 0;
    bool _loadedOnce = false;

    std::mutex _rollMutex;
    std::unordered_map<ObjectGuid::LowType, uint32> _lastDuelRoll;

    uint32 _lastActionCheck = 0;
    std::unordered_set<uint32> _finishedActions;  // world thread only; kept until the snapshot drops the row

    // plans/18 P7: FeelerKey(bot, player) -> (getMSTime of the roll, whether it defects); one roll per 6 hours.
    mutable std::mutex _defectMutex;
    mutable std::unordered_map<uint64, std::pair<uint32, bool>> _defectRolls;
};

#endif
