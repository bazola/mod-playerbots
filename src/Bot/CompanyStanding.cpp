/*
 * local: company standing (custom wow plans/14, step C). See CompanyStanding.h.
 */

#include "CompanyStanding.h"

#include "AiObjectContext.h"
#include "CharacterCache.h"
#include "ChatHelper.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace
{
    constexpr uint32 DUEL_ROLL_INTERVAL_MS = 30000;
    constexpr float RIVAL_CHALLENGE_DISTANCE = 30.0f;
    constexpr int32 RIVAL_LEVEL_GAP = 5;
    constexpr uint32 SPELL_DUEL = 7266;
    constexpr uint32 ACTION_CHECK_INTERVAL_MS = 5000;
    constexpr uint8 NOT_A_MEMBER = 0xFF;

    constexpr uint32 DEFECT_ROLL_INTERVAL_MS = 6 * HOUR * IN_MILLISECONDS;

    enum ActionKind : uint8
    {
        ACTION_INVITE,
        ACTION_PROMOTE,
        ACTION_DEMOTE,
        ACTION_REMOVE,
        ACTION_LEAVE,     // plans/18 P6: the bot itself leaves its company
        ACTION_WHISPER,   // plans/18 P6: the bot itself says something to the player (e.g. why it turned them down)
        ACTION_UNKNOWN
    };

    ActionKind ParseAction(std::string const& text)
    {
        if (text == "invite")
            return ACTION_INVITE;
        if (text == "promote")
            return ACTION_PROMOTE;
        if (text == "demote")
            return ACTION_DEMOTE;
        if (text == "remove")
            return ACTION_REMOVE;
        if (text == "leave")
            return ACTION_LEAVE;
        if (text == "whisper")
            return ACTION_WHISPER;
        return ACTION_UNKNOWN;
    }

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
           sPlayerbotAIConfig.companyRivalDuelChance || sPlayerbotAIConfig.companyRegardGate ||
           sPlayerbotAIConfig.companyActions;
}

void CompanyStanding::Update()
{
    if (!Enabled())
        return;

    ActOnCompanyActions();

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

        // local: company defection (custom wow plans/18, step P7).
        if (sPlayerbotAIConfig.companyDefectChance)
        {
            if (QueryResult result = CharacterDatabase.Query(
                    "SELECT r.bot_guid, AVG(r.score) FROM regard r JOIN guild_member mb ON mb.guid = r.bot_guid "
                    "JOIN guild_member mo ON mo.guid = r.other_guid AND mo.guildid = mb.guildid GROUP BY r.bot_guid"))
            {
                do
                {
                    Field* f = result->Fetch();
                    data->attachment[f[0].Get<uint32>()] = float(f[1].Get<double>());
                } while (result->NextRow());
            }

            if (QueryResult result = CharacterDatabase.Query(
                    "SELECT guildid, CAST(SUM(`rank` = 1) AS UNSIGNED) FROM guild_member GROUP BY guildid"))
            {
                do
                {
                    Field* f = result->Fetch();
                    data->officers[f[0].Get<uint32>()] = uint32(f[1].Get<uint64>());
                } while (result->NextRow());
            }
        }
    }

    // local: company actions (custom wow plans/18, step P4).
    if (sPlayerbotAIConfig.companyActions && TableExists("company_action"))
    {
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT id, guildid, player_guid, COALESCE(prefer_guid, 0), action, near_only, words "
                "FROM company_action WHERE done_at IS NULL ORDER BY id"))
        {
            do
            {
                Field* f = result->Fetch();
                CompanyAction action;
                action.id = uint32(f[0].Get<uint64>());
                action.guildId = f[1].Get<uint32>();
                action.playerGuid = f[2].Get<uint32>();
                action.preferGuid = f[3].Get<uint32>();
                action.kind = ParseAction(f[4].Get<std::string>());
                action.nearOnly = f[5].Get<int8>() != 0;
                action.words = f[6].Get<std::string>();
                if (action.kind != ACTION_UNKNOWN)
                    data->actions.push_back(std::move(action));
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

CompanyStanding::Verdict CompanyStanding::JudgeDefection(Player* bot, Player* from, uint32 guildId) const
{
    auto data = Snapshot();
    uint32 const botGuid = bot->GetGUID().GetCounter();
    Feeling feeling;
    float attachment = 20.0f;   // no feelings recorded about its own people: the same-company baseline
    uint32 enemies = 0;
    uint32 officers = 0;
    if (data)
    {
        auto it = data->towardPlayers.find(FeelerKey(botGuid, from->GetGUID().GetCounter()));
        if (it != data->towardPlayers.end())
            feeling = it->second;

        auto attached = data->attachment.find(botGuid);
        if (attached != data->attachment.end())
            attachment = attached->second;

        auto enemyIt = data->enemiesInside.find(FeelerKey(botGuid, guildId));
        if (enemyIt != data->enemiesInside.end())
            enemies = enemyIt->second;

        auto officerIt = data->officers.find(bot->GetGuildId());
        if (officerIt != data->officers.end())
            officers = officerIt->second;
    }

    if (feeling.score <= float(sPlayerbotAIConfig.companyEnemyRegard))
        return {false, "dislikes"};

    if (enemies)
        return {false, "enemy_inside"};

    if (feeling.score < float(sPlayerbotAIConfig.companyDefectRegard))
        return {false, "loyal"};

    if (attachment >= float(sPlayerbotAIConfig.companyDefectAttachment))
        return {false, "attached"};

    Guild* own = sGuildMgr->GetGuildById(bot->GetGuildId());
    if (!own)
        return {false, "in_company"};

    if (own->GetLeaderGUID() == bot->GetGUID())
        return {false, "leads"};

    // A company a bot leads (a seeded one) keeps its floor and its last officer (plans/18 §6b).
    if (sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(own->GetLeaderGUID())))
    {
        Guild::Member const* member = own->GetMember(bot->GetGUID());
        bool const lastOfficer = member && member->GetRankId() == GR_OFFICER && officers <= 1;
        if (own->GetMemberCount() <= sPlayerbotAIConfig.companyFloor || lastOfficer)
            return {false, "needed"};
    }

    uint32 const now = getMSTime();
    uint64 const key = FeelerKey(botGuid, from->GetGUID().GetCounter());
    std::lock_guard<std::mutex> lock(_defectMutex);
    auto roll = _defectRolls.find(key);
    if (roll == _defectRolls.end() || getMSTimeDiff(roll->second.first, now) >= DEFECT_ROLL_INTERVAL_MS)
        roll = _defectRolls.insert_or_assign(key, std::make_pair(now, urand(0, 99) < sPlayerbotAIConfig.companyDefectChance)).first;

    return roll->second.second ? Verdict{true, "defects"} : Verdict{false, "stays"};
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

void CompanyStanding::ActOnCompanyActions()
{
    if (!sPlayerbotAIConfig.companyActions)
        return;

    uint32 const now = getMSTime();
    if (_lastActionCheck && getMSTimeDiff(_lastActionCheck, now) < ACTION_CHECK_INTERVAL_MS)
        return;
    _lastActionCheck = now ? now : 1;

    auto data = Snapshot();
    if (!data)
        return;

    for (CompanyAction const& action : data->actions)
    {
        if (_finishedActions.count(action.id))
            continue;

        if (action.kind == ACTION_LEAVE || action.kind == ACTION_WHISPER)
        {
            ActAsSelf(action);
            continue;
        }

        // Until the player is about, the action waits.
        Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(action.playerGuid));
        if (!player || !player->IsInWorld())
            continue;

        Guild* guild = sGuildMgr->GetGuildById(action.guildId);
        if (!guild)
        {
            FinishAction(action, nullptr, "gone");
            continue;
        }

        Guild::Member const* member = guild->GetMember(player->GetGUID());
        bool const moot = action.kind == ACTION_INVITE ? (player->GetGuildId() || player->GetGuildIdInvited()) : !member;
        if (moot)
        {
            FinishAction(action, nullptr, "moot");
            continue;
        }

        Player* actor = FindActor(guild, player, action, member ? member->GetRankId() : NOT_A_MEMBER);
        if (!actor)
            continue;

        uint16 const opcode = action.kind == ACTION_INVITE    ? CMSG_GUILD_INVITE
                              : action.kind == ACTION_PROMOTE ? CMSG_GUILD_PROMOTE
                              : action.kind == ACTION_DEMOTE  ? CMSG_GUILD_DEMOTE
                                                              : CMSG_GUILD_REMOVE;

        // The officer says why before it is done; the core then sends the invite dialog or tells the company.
        if (!action.words.empty())
            actor->Whisper(action.words, LANG_UNIVERSAL, player);

        WorldPacket* packet = new WorldPacket(opcode);  // QueuePacket takes ownership
        *packet << player->GetName();
        actor->GetSession()->QueuePacket(packet);

        LOG_INFO("playerbots", "Company action {}: {} <{}> {} {} for {}", action.id, actor->GetName(), guild->GetName(),
                 opcode == CMSG_GUILD_INVITE ? "invites" : opcode == CMSG_GUILD_PROMOTE ? "promotes"
                 : opcode == CMSG_GUILD_DEMOTE ? "demotes" : "removes", player->GetName(),
                 action.nearOnly ? "face to face" : "by word");
        FinishAction(action, actor, "sent");
    }
}

// plans/18 P6. The bot named in prefer_guid acts for itself: a whisper waits until both it and the player are about; a
// leaving waits for the bot, whispers the player first if they are about, and is moot for a company's leader or one
// that would fall below the floor while a bot leads it.
void CompanyStanding::ActAsSelf(CompanyAction const& action)
{
    Player* actor = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(action.preferGuid));
    if (!actor || !actor->IsInWorld() || !GET_PLAYERBOT_AI(actor))
        return;

    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(action.playerGuid));
    bool const playerAbout = player && player->IsInWorld();

    if (action.kind == ACTION_WHISPER)
    {
        if (!playerAbout)
            return;

        if (!action.words.empty())
            actor->Whisper(action.words, LANG_UNIVERSAL, player);
        FinishAction(action, actor, "sent");
        return;
    }

    Guild* guild = sGuildMgr->GetGuildById(action.guildId);
    if (!guild || actor->GetGuildId() != action.guildId || guild->GetLeaderGUID() == actor->GetGUID())
    {
        FinishAction(action, nullptr, "moot");
        return;
    }

    uint32 const leaderAccount = sCharacterCache->GetCharacterAccountIdByGuid(guild->GetLeaderGUID());
    if (sPlayerbotAIConfig.IsInRandomAccountList(leaderAccount) && guild->GetMemberCount() <= sPlayerbotAIConfig.companyFloor)
    {
        FinishAction(action, nullptr, "moot");
        return;
    }

    if (playerAbout && !action.words.empty())
        actor->Whisper(action.words, LANG_UNIVERSAL, player);

    actor->GetSession()->QueuePacket(new WorldPacket(CMSG_GUILD_LEAVE));  // QueuePacket takes ownership
    LOG_INFO("playerbots", "Company action {}: {} leaves <{}>", action.id, actor->GetName(), guild->GetName());
    FinishAction(action, actor, "sent");
}

// A bot of the company, alive and out of combat, whose rank has the right and stands high enough for the core to
// allow it: raise only to below its own rank, lower or cast out only those below it. The preferred member wins.
Player* CompanyStanding::FindActor(Guild* guild, Player* player, CompanyAction const& action, uint8 playerRank) const
{
    uint32 const right = action.kind == ACTION_INVITE    ? GR_RIGHT_INVITE
                         : action.kind == ACTION_PROMOTE ? GR_RIGHT_PROMOTE
                         : action.kind == ACTION_DEMOTE  ? GR_RIGHT_DEMOTE
                                                         : GR_RIGHT_REMOVE;
    float const distance = float(sPlayerbotAIConfig.companyActionDistance);

    Player* found = nullptr;
    for (auto const& [guid, other] : ObjectAccessor::GetPlayers())
    {
        if (!other || other == player || !other->IsInWorld() || other->GetGuildId() != action.guildId ||
            !GET_PLAYERBOT_AI(other))
            continue;

        if (!other->IsAlive() || other->IsInCombat())
            continue;

        Guild::Member const* member = guild->GetMember(other->GetGUID());
        if (!member)
            continue;

        uint8 const rank = member->GetRankId();
        if (!(guild->GetRankRights(rank) & right))
            continue;

        if (action.kind == ACTION_PROMOTE && int32(rank) + 1 >= int32(playerRank))
            continue;

        if ((action.kind == ACTION_DEMOTE || action.kind == ACTION_REMOVE) && rank >= playerRank)
            continue;

        if (action.nearOnly && (other->GetMapId() != player->GetMapId() || !other->IsWithinDistInMap(player, distance)))
            continue;

        if (other->GetGUID().GetCounter() == action.preferGuid)
            return other;

        if (!found)
            found = other;
    }

    return found;
}

void CompanyStanding::FinishAction(CompanyAction const& action, Player* actor, char const* result)
{
    _finishedActions.insert(action.id);
    CharacterDatabase.Execute("UPDATE company_action SET done_at = NOW(), result = '{}', actor_guid = {} "
                              "WHERE id = {} AND done_at IS NULL",
                              result, actor ? std::to_string(actor->GetGUID().GetCounter()) : "NULL", action.id);
}
