/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PetitionSignAction.h"
#include "ArenaTeam.h"
#include "CompanyStanding.h"
#include "Event.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool PetitionSignAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    p.rpos(0);
    ObjectGuid petitionGuid;
    ObjectGuid inviter;
    uint8 unk = 0;
    bool isArena = false;
    p >> petitionGuid >> inviter;

    QueryResult result =
        CharacterDatabase.Query("SELECT `type` FROM `petition` WHERE `petitionguid` = {}", petitionGuid.GetCounter());
    if (!result)
    {
        return false;
    }

    Field* fields = result->Fetch();
    uint32 type = fields[0].Get<uint32>();

    Player* _inviter = ObjectAccessor::FindPlayer(inviter);
    if (!_inviter)
        return false;

    if (_inviter == bot)
        return false;

    // local: company regard gate (custom wow plans/18, step P2). Signing a real player's guild charter means
    // leaving with them: the bot weighs its regard for them, says none of the stock lines, and its answer is
    // written to company_answer. Arena charters and bots' charters keep the stock answer.
    bool const weigh = type == 9 && sPlayerbotAIConfig.companyRegardGate && !GET_PLAYERBOT_AI(_inviter);
    char const* reason = nullptr;

    bool accept = true;

    if (type != 9)
    {
        isArena = true;
        uint8 slot = ArenaTeam::GetSlotByType(ArenaType(type));
        if (bot->GetArenaTeamId(slot))
        {
            // player is already in an arena team
            botAI->TellError("Sorry, I am already in such team");
            accept = false;
        }
    }
    else
    {
        if (bot->GetGuildId())
        {
            if (!weigh)
                botAI->TellError("Sorry, I am in a guild already");
            reason = "in_company";
            accept = false;
        }

        if (bot->GetGuildIdInvited())
        {
            if (!weigh)
                botAI->TellError("Sorry, I am invited to a guild already");
            if (!reason)
                reason = "invited";
            accept = false;
        }

        // check for same acc id
        /*if (QueryResult* result = CharacterDatabase.Query("SELECT playerguid FROM petition_sign WHERE player_account =
        {} AND petitionguid = {}'", bot->GetSession()->GetAccountId(), petitionGuid.GetCounter()))
        {
            botAI->TellError("Sorry, I already signed this pettition");
            accept = false;
        }
        */
    }

    if (accept && !botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, weigh, _inviter, true))
    {
        reason = "unwilling";
        accept = false;
    }

    if (accept && weigh)
    {
        CompanyStanding::Verdict const verdict = CompanyStanding::instance().JudgeCharter(bot, _inviter);
        accept = verdict.accept;
        reason = verdict.reason;
    }

    if (weigh)
        CompanyStanding::instance().RecordAnswer(bot, _inviter, "sign", 0, accept, reason);

    if (!accept)
    {
        WorldPacket data(MSG_PETITION_DECLINE);
        data << petitionGuid;
        bot->GetSession()->HandlePetitionDeclineOpcode(data);
        LOG_INFO("playerbots", "Bot {} <{}> declines {} invite", bot->GetGUID().ToString().c_str(),
                 bot->GetName().c_str(), isArena ? "Arena" : "Guild");
        return false;
    }

    WorldPacket data(CMSG_PETITION_SIGN, 20);
    data << petitionGuid << unk;
    bot->GetSession()->HandlePetitionSignOpcode(data);
    if (!weigh)
        bot->Say("Thanks for the invite!", LANG_UNIVERSAL);
    LOG_INFO("playerbots", "Bot {} <{}> accepts {} invite", bot->GetGUID().ToString().c_str(),
             bot->GetName().c_str(), isArena ? "Arena" : "Guild");
    return true;
}
