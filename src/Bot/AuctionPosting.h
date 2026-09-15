/*
 * local: auctions (custom wow plans/17, §3.E.5).
 *
 * A random bot standing at an auctioneer may put up surplus loot from its bags. Prices come from market_price in
 * acore_characters (kept by the market service outside the worldserver), else from the vendor price by quality.
 * The listing goes through the bot's own session as CMSG_AUCTION_SELL_ITEM, so the core's handler does every check,
 * takes the deposit and saves the auction exactly as for a player. AiPlayerbot.AuctionPostChance = 0 (default) is
 * stock behaviour.
 */

#ifndef PLAYERBOTS_AUCTIONPOSTING_H
#define PLAYERBOTS_AUCTIONPOSTING_H

#include "Define.h"
#include "ObjectGuid.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

class Creature;
class Player;
class PlayerbotAI;

class AuctionPosting
{
public:
    static AuctionPosting& instance();

    // Bot AI: the bot has just reached the creature. Rolls AiPlayerbot.AuctionPostChance once per
    // AiPlayerbot.AuctionPostCooldown per bot. True when at least one listing was sent.
    bool TryPost(PlayerbotAI* botAI, Player* bot, Creature* auctioneer);

private:
    using Prices = std::unordered_map<uint32, uint32>;  // item entry -> copper per unit

    void Refresh();
    void Load();
    std::shared_ptr<Prices const> Snapshot() const;

    mutable std::mutex _mutex;
    std::shared_ptr<Prices const> _prices;
    std::atomic<bool> _loading{false};
    std::atomic<uint32> _lastLoad{0};
    std::atomic<bool> _loadedOnce{false};

    std::mutex _visitMutex;
    std::unordered_map<ObjectGuid::LowType, uint32> _lastVisit;  // bot -> getMSTime of its last roll
};

#endif
