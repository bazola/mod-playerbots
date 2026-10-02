/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GroupScript.h"
#include "ObjectAccessor.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "RandomBotLevelMgr.h"
#include "World.h"

class PlayerbotEncounterPlayerScript : public PlayerScript
{
public:
    PlayerbotEncounterPlayerScript() : PlayerScript("PlayerbotEncounterPlayerScript", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_ON_TEXT_EMOTE
    }) {}

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
                            Player* receiver) override
    {
        if (type == CHAT_MSG_WHISPER && language != LANG_ADDON && !message.empty())
            RandomBotLevelMgr::instance().RecordMeeting(receiver, player);
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message) override
    {
        if (!sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting ||
            sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::OFF ||
            !player || type != CHAT_MSG_SAY || language == LANG_ADDON || message.empty() ||
            (!IsRealPlayer(player) && !IsSelfBot(player)))
            return true;

        // Selection makes the addressee explicit; overhearing nearby speech does not count.
        Player* target = ObjectAccessor::FindPlayer(player->GetTarget());
        if (target && player->IsWithinDistInMap(target, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY)))
            RandomBotLevelMgr::instance().RecordMeeting(target, player);
        return true;
    }

    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid guid) override
    {
        if (!sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting ||
            sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::OFF ||
            !player || !guid.IsPlayer() || (!IsRealPlayer(player) && !IsSelfBot(player)))
            return;

        switch (textEmote)
        {
            case TEXT_EMOTE_BOW:
            case TEXT_EMOTE_CHEER:
            case TEXT_EMOTE_GREET:
            case TEXT_EMOTE_HELLO:
            case TEXT_EMOTE_HUG:
            case TEXT_EMOTE_NOD:
            case TEXT_EMOTE_SALUTE:
            case TEXT_EMOTE_SMILE:
            case TEXT_EMOTE_THANK:
            case TEXT_EMOTE_WAVE:
                break;
            default:
                return;
        }

        Player* target = ObjectAccessor::FindPlayer(guid);
        if (target && player->IsWithinDistInMap(target, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE)))
            RandomBotLevelMgr::instance().RecordMeeting(target, player);
    }
};

class PlayerbotEncounterGroupScript : public GroupScript
{
public:
    PlayerbotEncounterGroupScript() : GroupScript("PlayerbotEncounterGroupScript", {
        GROUPHOOK_ON_ADD_MEMBER
    }) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting ||
            sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::OFF ||
            !group || group->isBGGroup() || group->isBFGroup())
            return;
        Player* joined = ObjectAccessor::FindPlayer(guid);
        if (!joined)
            return;

        // Actual membership, including LFG, not a possibly rejected invitation.
        bool const joinedIsHuman = IsRealPlayer(joined) || IsSelfBot(joined);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == joined)
                continue;
            if (joinedIsHuman)
                RandomBotLevelMgr::instance().RecordMeeting(member, joined);
            else
                RandomBotLevelMgr::instance().RecordMeeting(joined, member);
        }
    }
};

void AddPlayerbotEncounterScripts()
{
    new PlayerbotEncounterPlayerScript();
    new PlayerbotEncounterGroupScript();
}
