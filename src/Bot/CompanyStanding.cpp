/*
 * local: company standing (custom wow plans/14, step C). See CompanyStanding.h.
 */

#include "CompanyStanding.h"

#include "AiObjectContext.h"
#include "ChatHelper.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace
{
    constexpr uint32 DUEL_ROLL_INTERVAL_MS = 30000;
    constexpr float RIVAL_CHALLENGE_DISTANCE = 30.0f;
    constexpr int32 RIVAL_LEVEL_GAP = 5;
    constexpr uint32 SPELL_DUEL = 7266;

    uint64 PairKey(uint32 a, uint32 b)
    {
        return (uint64(std::min(a, b)) << 32) | std::max(a, b);
    }

    // Ordered: the first id is the one who feels.
    uint64 FeelerKey(uint32 feeler, uint32 other)
    {
        return (uint64(feeler) << 32) | other;
    }

    bool TableExists(char const* name)
    {
        return bool(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '{}'", name));
    }
}

CompanyStanding& CompanyStanding::instance()
{
    static CompanyStanding standing;
    return standing;
}

bool CompanyStanding::Enabled()
{
    return sPlayerbotAIConfig.companySeatTeleportChance || sPlayerbotAIConfig.companyRivalDeclineChance ||
           sPlayerbotAIConfig.companyRivalDuelChance || sPlayerbotAIConfig.companyRegardGate;
}

void CompanyStanding::Update()
{
    if (!Enabled())
        return;

    uint32 const now = getMSTime();
    uint32 const interval = std::max<uint32>(30, sPlayerbotAIConfig.companyRefreshSeconds) * IN_MILLISECONDS;
    if (_loadedOnce && getMSTimeDiff(_lastLoad, now) < interval)
        return;

    if (World::IsStopped() || _loading.exchange(true))
        return;

    _lastLoad = now;
    _loadedOnce = true;
    std::thread([this]
    {
        Load();
        _loading = false;
    }).detach();
}

void CompanyStanding::Load()
{
    auto data = std::make_shared<Data>();

    if (TableExists("guild_seat"))
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT guildid, zone_id FROM guild_seat"))
        {
            do
            {
                Field* f = result->Fetch();
                data->pulls[f[0].Get<uint32>()].insert(f[1].Get<uint32>());
            } while (result->NextRow());
        }
    }

    // Contested lands pull both sides: the holder to keep them, the challenger to take them.
    if (TableExists("guild_holding"))
    {
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT zone_id, holder, challenger FROM guild_holding WHERE challenger IS NOT NULL"))
        {
            do
            {
                Field* f = result->Fetch();
                data->pulls[f[1].Get<uint32>()].insert(f[0].Get<uint32>());
                data->pulls[f[2].Get<uint32>()].insert(f[0].Get<uint32>());
            } while (result->NextRow());
        }
    }

    if (TableExists("guild_relation"))
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT guild_a, guild_b, stance FROM guild_relation"))
        {
            do
            {
                Field* f = result->Fetch();
                data->stances[PairKey(f[0].Get<uint32>(), f[1].Get<uint32>())] = f[2].Get<float>();
            } while (result->NextRow());
        }
    }

    // local: company regard gate (custom wow plans/18, step P2).
    if (sPlayerbotAIConfig.companyRegardGate && TableExists("regard"))
    {
        // Real players are the characters outside the random bot accounts; there are only a handful, so their
        // rows are fetched by id rather than by joining the auth database.
        std::string realGuids;
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, account FROM characters"))
        {
            do
            {
                Field* f = result->Fetch();
                if (!sPlayerbotAIConfig.IsInRandomAccountList(f[1].Get<uint32>()))
                    realGuids += (realGuids.empty() ? "" : ",") + std::to_string(f[0].Get<uint32>());
            } while (result->NextRow());
        }

        if (!realGuids.empty())
        {
            if (QueryResult result = CharacterDatabase.Query(
                    "SELECT bot_guid, other_guid, score, familiarity FROM regard WHERE other_guid IN ({})", realGuids))
            {
                do
                {
                    Field* f = result->Fetch();
                    data->towardPlayers[FeelerKey(f[0].Get<uint32>(), f[1].Get<uint32>())] =
                        Feeling{f[2].Get<float>(), f[3].Get<uint32>()};
                } while (result->NextRow());
            }
        }

        int32 const friendRegard = sPlayerbotAIConfig.companyFriendRegard;
        int32 const enemyRegard = sPlayerbotAIConfig.companyEnemyRegard;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT r.bot_guid, m.guildid, CAST(SUM(r.score >= {}) AS UNSIGNED), CAST(SUM(r.score <= {}) AS UNSIGNED) "
                "FROM regard r JOIN guild_member m ON m.guid = r.other_guid "
                "WHERE r.score >= {} OR r.score <= {} GROUP BY r.bot_guid, m.guildid",
                friendRegard, enemyRegard, friendRegard, enemyRegard))
        {
            do
            {
                Field* f = result->Fetch();
                uint64 const key = FeelerKey(f[0].Get<uint32>(), f[1].Get<uint32>());
                if (uint64 const friends = f[2].Get<uint64>())
                    data->friendsInside[key] = uint32(friends);
                if (uint64 const enemies = f[3].Get<uint64>())
                    data->enemiesInside[key] = uint32(enemies);
            } while (result->NextRow());
        }

        data->answerTable = TableExists("company_answer");
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _data = std::move(data);
}

std::shared_ptr<CompanyStanding::Data const> CompanyStanding::Snapshot() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _data;
}

bool CompanyStanding::PullsToward(uint32 guildId, uint32 zoneId) const
{
    if (!guildId)
        return false;

    auto data = Snapshot();
    if (!data)
        return false;

    auto it = data->pulls.find(guildId);
    return it != data->pulls.end() && it->second.count(zoneId);
}

bool CompanyStanding::AreRivals(uint32 guildA, uint32 guildB) const
{
    if (!guildA || !guildB || guildA == guildB)
        return false;

    auto data = Snapshot();
    if (!data)
        return false;

    auto it = data->stances.find(PairKey(guildA, guildB));
    return it != data->stances.end() && it->second <= float(sPlayerbotAIConfig.companyRivalStance);
}

bool CompanyStanding::TryChallengeRival(PlayerbotAI* botAI, Player* bot)
{
    uint32 const chance = sPlayerbotAIConfig.companyRivalDuelChance;
    if (!chance || !botAI || !bot || !bot->GetGuildId() || bot->duel || bot->IsInCombat() || bot->GetLevel() < 10)
        return false;

    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> lock(_rollMutex);
        uint32& last = _lastDuelRoll[bot->GetGUID().GetCounter()];
        if (last && getMSTimeDiff(last, now) < DUEL_ROLL_INTERVAL_MS)
            return false;
        last = now ? now : 1;
    }

    if (urand(0, 99) >= chance)
        return false;

    if (sPlayerbotAIConfig.IsInPvpProhibitedZone(bot->GetZoneId()))
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (!area || !(area->flags & AREA_FLAG_ALLOW_DUELS) || bot->GetHealthPct() < 90.0f)
        return false;

    GuidVector nearby = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get();
    for (ObjectGuid const& guid : nearby)
    {
        Player* other = ObjectAccessor::FindPlayer(guid);
        if (!other || other == bot || !GET_PLAYERBOT_AI(other))  // bots only: a real player is never called out
            continue;

        if (other->duel || other->IsInCombat() || other->GetHealthPct() < 90.0f)
            continue;

        if (std::abs(int32(other->GetLevel()) - int32(bot->GetLevel())) > RIVAL_LEVEL_GAP)
            continue;

        if (!AreRivals(bot->GetGuildId(), other->GetGuildId()))
            continue;

        if (bot->GetDistance(other) > RIVAL_CHALLENGE_DISTANCE || !bot->IsWithinLOSInMap(other))
            continue;

        return botAI->DoSpecificAction(
            "cast custom spell",
            Event("rpg action", botAI->GetChatHelper()->FormatWorldobject(other) + " " + std::to_string(SPELL_DUEL)),
            true);
    }

    return false;
}

// Until the first reload lands everyone is a stranger, so the gate declines rather than signs.
CompanyStanding::Verdict CompanyStanding::JudgeCharter(Player* bot, Player* from) const
{
    auto data = Snapshot();
    Feeling feeling;
    if (data)
    {
        auto it = data->towardPlayers.find(FeelerKey(bot->GetGUID().GetCounter(), from->GetGUID().GetCounter()));
        if (it != data->towardPlayers.end())
            feeling = it->second;
    }

    if (feeling.score >= float(sPlayerbotAIConfig.companySignRegard) &&
        feeling.familiarity >= sPlayerbotAIConfig.companySignFamiliarity)
        return {true, "trusts_them"};

    if (feeling.score <= float(sPlayerbotAIConfig.companyEnemyRegard))
        return {false, "dislikes"};

    if (!feeling.familiarity)
        return {false, "stranger"};

    if (feeling.familiarity < sPlayerbotAIConfig.companySignFamiliarity)
        return {false, "hardly_knows"};

    return {false, "not_enough"};
}

CompanyStanding::Verdict CompanyStanding::JudgeInvite(Player* bot, Player* from, uint32 guildId) const
{
    auto data = Snapshot();
    Feeling feeling;
    uint32 friends = 0;
    uint32 enemies = 0;
    if (data)
    {
        uint32 const botGuid = bot->GetGUID().GetCounter();
        auto it = data->towardPlayers.find(FeelerKey(botGuid, from->GetGUID().GetCounter()));
        if (it != data->towardPlayers.end())
            feeling = it->second;

        auto friendIt = data->friendsInside.find(FeelerKey(botGuid, guildId));
        if (friendIt != data->friendsInside.end())
            friends = friendIt->second;

        auto enemyIt = data->enemiesInside.find(FeelerKey(botGuid, guildId));
        if (enemyIt != data->enemiesInside.end())
            enemies = enemyIt->second;
    }

    if (feeling.score <= float(sPlayerbotAIConfig.companyEnemyRegard))
        return {false, "dislikes"};

    if (enemies)
        return {false, "enemy_inside"};

    if (feeling.score >= float(sPlayerbotAIConfig.companyJoinRegard))
        return {true, "trusts_them"};

    if (friends && feeling.score >= float(sPlayerbotAIConfig.companyJoinFriendRegard))
        return {true, "friend_inside"};

    if (!feeling.familiarity)
        return {false, "stranger"};

    return {false, "not_enough"};
}

void CompanyStanding::RecordAnswer(Player* bot, Player* from, char const* kind, uint32 guildId, bool accepted,
                                   char const* reason) const
{
    auto data = Snapshot();
    if (!data || !data->answerTable || !bot || !from || !kind || !reason)
        return;

    CharacterDatabase.Execute(
        "INSERT INTO company_answer (bot_guid, player_guid, guildid, kind, accepted, reason) VALUES ({}, {}, {}, '{}', {}, '{}')",
        bot->GetGUID().GetCounter(), from->GetGUID().GetCounter(), guildId ? std::to_string(guildId) : "NULL", kind,
        accepted ? 1 : 0, reason);
}
