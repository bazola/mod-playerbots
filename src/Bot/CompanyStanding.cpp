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
           sPlayerbotAIConfig.companyRivalDuelChance;
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
