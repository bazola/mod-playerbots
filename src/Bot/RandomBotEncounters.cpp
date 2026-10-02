/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RandomBotLevelMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <utility>

void RandomBotLevelMgr::RecordMeeting(Player* bot, Player* player)
{
    if (!sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting ||
        sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::OFF ||
        !bot || !player || bot == player)
        return;
    if (!IsRealPlayer(player) && !IsSelfBot(player))
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->IsBotAI() || IsSelfBot(bot) || !bot->GetSession() || !bot->GetSession()->IsBot() ||
        !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
        return;

    std::lock_guard<std::mutex> lock(_anchorMutex);
    uint32 const botId = bot->GetGUID().GetCounter();
    uint32 const playerId = player->GetGUID().GetCounter();
    if (_anchors.emplace(botId, playerId).second)
        _pendingMeetingAnchors.emplace(botId, playerId);
}

void RandomBotLevelMgr::FlushMeetingAnchors()
{
    // World-thread only. The module database pool is synchronous; never call it
    // from a chat/emote/packet hook or hold the cache mutex during database work.
    std::vector<std::pair<uint32, uint32>> batch;
    {
        std::lock_guard<std::mutex> lock(_anchorMutex);
        if (_pendingMeetingAnchors.empty())
            return;
        std::size_t constexpr maxMeetingsPerBatch = 100;
        batch.reserve(std::min(maxMeetingsPerBatch, _pendingMeetingAnchors.size()));
        for (auto const& meeting : _pendingMeetingAnchors)
        {
            batch.push_back(meeting);
            if (batch.size() == maxMeetingsPerBatch)
                break;
        }
    }

    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    for (auto const& meeting : batch)
    {
        auto* statement = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_MEETING_ANCHOR);
        statement->SetData(0, meeting.first);
        statement->SetData(1, meeting.second);
        transaction->Append(statement);
    }
    PlayerbotsDatabase.CommitTransaction(transaction);

    for (auto const& meeting : batch)
        sRandomPlayerbotMgr.PreserveCurrentState(meeting.first);

    std::lock_guard<std::mutex> lock(_anchorMutex);
    for (auto const& meeting : batch)
        _pendingMeetingAnchors.erase(meeting.first);
}

void RandomBotLevelMgr::ObserveMeetingTradePacket(Player* bot, WorldPacket const& packet)
{
    if (!sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting ||
        sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::OFF ||
        !bot || packet.GetOpcode() != SMSG_TRADE_STATUS || packet.size() < sizeof(uint32) ||
        !bot->GetSession() || !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
        return;

    WorldPacket copy(packet);
    copy.rpos(0);
    uint32 status;
    copy >> status;
    ObjectGuid partnerGuid;
    {
        std::lock_guard<std::mutex> lock(_anchorMutex);
        if (status == TRADE_STATUS_BEGIN_TRADE)
        {
            if (copy.size() < sizeof(uint32) + sizeof(uint64))
                return;
            copy >> partnerGuid;
            _meetingTradePartners[bot->GetGUID()] = partnerGuid;
            return;
        }
        if (status == TRADE_STATUS_OPEN_WINDOW)
        {
            // The initiator does not receive BEGIN_TRADE, but still has TradeData here.
            if (Player* trader = bot->GetTrader())
                _meetingTradePartners[bot->GetGUID()] = trader->GetGUID();
            return;
        }
        if (status == TRADE_STATUS_TRADE_COMPLETE)
        {
            auto it = _meetingTradePartners.find(bot->GetGUID());
            if (it != _meetingTradePartners.end())
                partnerGuid = it->second;
            _meetingTradePartners.erase(bot->GetGUID());
        }
        else if (status == TRADE_STATUS_TRADE_CANCELED || status == TRADE_STATUS_CLOSE_WINDOW)
            _meetingTradePartners.erase(bot->GetGUID());
    }

    // Core clears TradeData before sending COMPLETE. Only that success packet counts,
    // using the saved GUID rather than a Player pointer retained across ticks.
    if (partnerGuid.IsPlayer())
        RecordMeeting(bot, ObjectAccessor::FindPlayer(partnerGuid));
}
