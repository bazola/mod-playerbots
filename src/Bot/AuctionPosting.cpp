/*
 * local: auctions (custom wow plans/17, §3.E.5). See AuctionPosting.h.
 */

#include "AuctionPosting.h"

#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32 ZERO_PRICE_FLOOR = 100;  // copper per unit for items with no vendor price

    bool TableExists(char const* name)
    {
        return bool(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '{}'", name));
    }

    uint32 QualityMultiplier(uint32 quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR:
            case ITEM_QUALITY_NORMAL:
                return 2;
            case ITEM_QUALITY_UNCOMMON:
                return 4;
            case ITEM_QUALITY_RARE:
                return 6;
            default:
                return 10;
        }
    }

    // Bags only: the backpack and the four equipped bags, never equipped gear or the bank.
    std::vector<Item*> BagItems(Player* bot)
    {
        std::vector<Item*> items;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back(item);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(slot))
                        items.push_back(item);

        return items;
    }
}

AuctionPosting& AuctionPosting::instance()
{
    static AuctionPosting posting;
    return posting;
}

void AuctionPosting::Refresh()
{
    uint32 const now = getMSTime();
    uint32 const interval = std::max<uint32>(60, sPlayerbotAIConfig.auctionMarketRefreshSeconds) * IN_MILLISECONDS;
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

void AuctionPosting::Load()
{
    auto prices = std::make_shared<Prices>();
    if (TableExists("market_price"))
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT item_entry, unit_copper FROM market_price WHERE unit_copper > 0"))
        {
            do
            {
                Field* fields = result->Fetch();
                (*prices)[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _prices = prices;
}

std::shared_ptr<AuctionPosting::Prices const> AuctionPosting::Snapshot() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _prices;
}

bool AuctionPosting::TryPost(PlayerbotAI* botAI, Player* bot, Creature* auctioneer)
{
    if (!sPlayerbotAIConfig.auctionPostChance || !botAI || !bot || !auctioneer)
        return false;

    Refresh();

    if (!auctioneer->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (!bot->IsAlive() || bot->IsInCombat() || !bot->GetNPCIfCanInteractWith(auctioneer->GetGUID(), UNIT_NPC_FLAG_AUCTIONEER))
        return false;

    AuctionHouseEntry const* house = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!house)
        return false;

    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> lock(_visitMutex);
        auto visit = _lastVisit.find(bot->GetGUID().GetCounter());
        if (visit != _lastVisit.end() &&
            getMSTimeDiff(visit->second, now) < sPlayerbotAIConfig.auctionPostCooldown * IN_MILLISECONDS)
            return false;
        _lastVisit[bot->GetGUID().GetCounter()] = now;
    }

    if (urand(1, 100) > sPlayerbotAIConfig.auctionPostChance)
        return false;

    std::shared_ptr<Prices const> prices = Snapshot();
    uint32 const maxItems = sPlayerbotAIConfig.auctionPostMaxItems;
    uint64 moneyLeft = bot->GetMoney();
    uint32 posted = 0;

    for (Item* item : BagItems(bot))
    {
        if (posted >= maxItems)
            break;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality < sPlayerbotAIConfig.auctionPostMinQuality)
            continue;

        // The handler refuses these too; checked here so a refused item doesn't use up a slot.
        if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->Class == ITEM_CLASS_QUEST ||
            proto->StartQuest || proto->HasFlag(ITEM_FLAG_CONJURED) || proto->Duration ||
            item->GetUInt32Value(ITEM_FIELD_DURATION) || !item->CanBeTraded() || item->IsNotEmptyBag() ||
            item->GetCount() > proto->GetMaxStackSize())
            continue;

        // Only what the bot itself would sell: not gear it would wear, nor anything for its skills, quests or use.
        ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", int32(proto->ItemId))->Get();
        if (usage != ITEM_USAGE_AH && usage != ITEM_USAGE_VENDOR)
            continue;

        uint32 const count = item->GetCount();
        uint32 unit = 0;
        if (prices)
        {
            auto known = prices->find(proto->ItemId);
            if (known != prices->end())
                unit = known->second;
        }
        if (!unit)
            unit = proto->SellPrice ? proto->SellPrice * QualityMultiplier(proto->Quality) : ZERO_PRICE_FLOOR;

        uint64 buyout = uint64(unit) * count * urand(85, 115) / 100;

        // Whole stack, 12, 24 or 48 hours, like the auction window offers.
        uint32 const minutes = (MIN_AUCTION_TIME / MINUTE) * (1u << urand(0, 2));
        uint32 const deposit = AuctionHouseMgr::GetAuctionDeposit(house, minutes * MINUTE, item, count);
        if (deposit > moneyLeft)
            continue;

        // Never at a loss: what is left after the deposit and the house's cut is at least the vendor price.
        float const cutShare = std::min(0.9f, float(house->cutPercent) / 100.0f * sWorld->getRate(RATE_AUCTION_CUT));
        uint64 const floor = uint64((deposit + uint64(proto->SellPrice) * count) / (1.0f - cutShare)) + 1;
        buyout = std::max(buyout, floor);
        if (buyout > MAX_MONEY_AMOUNT)
            continue;

        uint32 const bid = std::max<uint32>(1, uint32(buyout * urand(75, 95) / 100));

        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_SELL_ITEM);  // QueuePacket takes ownership
        *packet << auctioneer->GetGUID();
        *packet << uint32(1);
        *packet << item->GetGUID();
        *packet << count;
        *packet << bid;
        *packet << uint32(buyout);
        *packet << minutes;
        bot->GetSession()->QueuePacket(packet);

        moneyLeft -= deposit;
        ++posted;
    }

    return posted > 0;
}
