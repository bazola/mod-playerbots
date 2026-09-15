/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildAcceptAction.h"
#include "CompanyStanding.h"
#include "Event.h"
#include "GuildPackets.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

bool GuildAcceptAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    p.rpos(0);
    Player* inviter = nullptr;
    std::string Invitedname;
    p >> Invitedname;

    if (normalizePlayerName(Invitedname))
        inviter = ObjectAccessor::FindPlayerByName(Invitedname.c_str());

    if (!inviter)
        return false;

    // local: company regard gate (custom wow plans/18, step P2). An unguilded bot joins a real player's company
    // only as far as it trusts them and the people already inside; it says none of the stock lines, and its
    // answer is written to company_answer. Invites from bots keep the stock answer.
    bool const weigh = sPlayerbotAIConfig.companyRegardGate && !GET_PLAYERBOT_AI(inviter);
    char const* reason = nullptr;

    bool accept = true;
    uint32 guildId = inviter->GetGuildId();
    if (!guildId)
    {
        if (!weigh)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_inviter_not_in_guild", "You are not in a guild!", {}));
        reason = "unwilling";
        accept = false;
    }
    else if (bot->GetGuildId())
    {
        if (!weigh)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_already_in_guild", "Sorry, I am in a guild already", {}));
        reason = "in_company";
        accept = false;
    }
    else if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, weigh, inviter, true))
    {
        if (!weigh)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_declined", "Sorry, I don't want to join your guild :(", {}));
        reason = "unwilling";
        accept = false;
    }
    else if (weigh)
    {
        CompanyStanding::Verdict const verdict = CompanyStanding::instance().JudgeInvite(bot, inviter, guildId);
        accept = verdict.accept;
        reason = verdict.reason;
    }

    if (weigh)
        CompanyStanding::instance().RecordAnswer(bot, inviter, "join", guildId, accept, reason);

    if (accept)
    {
        WorldPackets::Guild::AcceptGuildInvite data = WorldPacket(CMSG_GUILD_ACCEPT);
        bot->GetSession()->HandleGuildAcceptOpcode(data);
    }
    else
    {
        WorldPackets::Guild::GuildDeclineInvitation data = WorldPacket(CMSG_GUILD_DECLINE);
        bot->GetSession()->HandleGuildDeclineOpcode(data);
    }

    return true;
}
