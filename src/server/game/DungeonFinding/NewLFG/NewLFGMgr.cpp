
#include "NewLFGMgr.h"
#include "NewLFGQueue.h"
#include "LFGEnums.h"
#include "LFGStructs.h"
#include "Common.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Optional.h"
#include "Player.h"
#include "QueryResult.h"
#include "RBAC.h"

#include <unordered_set>

NewLFGMgr::NewLFGMgr() : _nextAvailableTicketId(0), _lfgQueueUpdateInterval(LFG_QUEUE_UPDATE_INTERVAL)
{
}

NewLFGMgr::~NewLFGMgr()
{
}

inline bool HasInvalidRoles(Player const* player, uint32 const roleMask)
{
    switch (player->getClass())
    {
        case CLASS_DEATH_KNIGHT:
        case CLASS_WARRIOR:
            if (roleMask & LFG_ROLE_HEAL)
                return true;
            break;
        case CLASS_WARLOCK:
        case CLASS_MAGE:
        case CLASS_HUNTER:
        case CLASS_ROGUE:
            if (roleMask & (LFG_ROLE_HEAL | LFG_ROLE_TANK))
                return true;
            break;
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
            if (roleMask & LFG_ROLE_TANK)
                return true;
            break;
        default:
            return false;
    }

    return false;
}

bool IsRoleCheckValid(std::unordered_map<ObjectGuid, uint8> const& selected, uint8 numDps, uint8 numTank, uint8 numHeal)
{
    if ((numDps + numHeal + numTank) < selected.size())
        return false;

    /*
        HOW THIS WORKS (stage 1):
        - We ignore anyone selecting all three roles. They don't matter for validity, they can always fill whatever is open.
        - dpsTank / dpsHeal / tankHeal are the counts of players that could fill either role
    */

    uint8 dpsTank = 0, dpsHeal = 0, tankHeal = 0;
    for (auto [guid, mask] : selected)
    {
        switch (mask & LFG_ROLE_MASK_ALL_NEEDED)
        {
            case LFG_ROLE_DAMAGE:
                if (!numDps--)
                    return false;
                break;
            case LFG_ROLE_TANK:
                if (!numTank--)
                    return false;
                break;
            case LFG_ROLE_HEAL:
                if (!numHeal--)
                    return false;
                break;
            case (LFG_ROLE_DAMAGE | LFG_ROLE_TANK):
                ++dpsTank;
                break;
            case (LFG_ROLE_DAMAGE | LFG_ROLE_HEAL):
                ++dpsHeal;
                break;
            case (LFG_ROLE_TANK | LFG_ROLE_HEAL):
                ++tankHeal;
                break;
            case LFG_ROLE_MASK_ALL_NEEDED:
                break;
            default:
                return false;
        }
    }

    /*
        HOW THIS WORKS (stage 2):
        - At this point:
            - numDps/numTank/numHeal are the roles we can still fill
            - dpsTank/dpsHeal/tankHeal are the hybrids we have to distribute
        - First we shortcut out for any setups that are plainly impossible
        - Then we check all remaining setups (there are at most numTank of those) for validity
    */

    if ((numDps + numTank) < dpsTank)
        return false;
    if ((numDps + numHeal) < dpsHeal)
        return false;
    if ((numTank + numHeal) < tankHeal)
        return false;

    for (uint8 tankHealAsTank = 0, maxTankHealAsTank = std::min(tankHeal, numTank); tankHealAsTank <= maxTankHealAsTank; ++tankHealAsTank)
    {
        /*
            Here's the setup we are testing:
            - TANK slots:
                - tankHealAsTank slots taken by tankHeals                                   <- this is >= 0 because of the loop condition
                - dpsTankAsTank = up to (numTank - tankHealAsTank) slots taken by dpsTanks  <- this is >= 0 because of the loop condition
            - HEAL slots:
                - tankHealAsHeal = (tankHeal - tankHealAsTank) slots taken by tankHeals     <- this is >= 0 because of the loop condition
                - dpsHealAsHeal = up to (numHeal - tankHealAsHeal) slots taken by dpsHeals  <- we need to check whether this is >= 0
            - DPS slots:
                - dpsTankAsDps = (dpsTank - dpsTankAsTank) slots taken by dpsTanks
                - dpsHealAsDps = (dpsHeal - dpsHealAsHeal) slots taken by dpsHeals
                    ^----------------------------------------------------------------------- we need to check whether this fits in numDps!
        */

        // TANK slots
        uint8 const dpsTankAsTank = std::min<uint8>(numTank - tankHealAsTank, dpsTank);

        // HEAL slots
        uint8 const tankHealAsHeal = (tankHeal - tankHealAsTank);
        if (numHeal < tankHealAsHeal) // not enough spots, setup invalid
            continue;
        uint8 const dpsHealAsHeal = std::min<uint8>(numHeal - tankHealAsHeal, dpsHeal);

        // DPS slots
        uint8 const dpsTankAsDps = (dpsTank - dpsTankAsTank);
        uint8 const dpsHealAsDps = (dpsHeal - dpsHealAsHeal);
        if ((dpsTankAsDps + dpsHealAsDps) <= numDps)
            return true;
    }
    return false;
}

// Checks player and party members if they are allowed to join the LFG system without considering dungeon selection yet
static LFGJoinResult GetPlayerAndGroupJoinResult(Player* player, uint32 roleMask)
{
    LFGJoinResult result = LFGJoinResult::OK;

    // Base check if the player of any group member cannot enter the queue
    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_DUNGEON_FINDER))
        result = LFGJoinResult::InternalError;
    if (HasInvalidRoles(player, roleMask))
        result = LFGJoinResult::InternalError;
    else if (player->InBattleground() || player->InArena() || player->InBattlegroundQueue())
        result = LFGJoinResult::UsingBattleground;
    else if (player->HasAura(SPELL_LFG_DUNGEON_DESERTER))
        result = LFGJoinResult::Deserter;
    else if (player->HasAura(SPELL_LFG_DUNGEON_COOLDOWN))
        result = LFGJoinResult::RandomCooldown;
    else if (player->HasAura(9454)) // check if frozen by a GameMaster
        result = LFGJoinResult::InternalError;
    else if (Group* group = player->GetGroup())
    {
        if (group->GetMembersCount() > MAXGROUPSIZE)
            result = LFGJoinResult::TooManyMembers;
        else if (group->GetMembersCount() == MAXGROUPSIZE)
            result = LFGJoinResult::GroupFull;
        else
        {
            uint8 memberCount = 0;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr && result == LFGJoinResult::OK; itr = itr->next())
            {
                if (Player* groupPlayer = itr->GetSource())
                {
                    if (!groupPlayer->GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_DUNGEON_FINDER))
                        result = LFGJoinResult::InternalError;
                    else if (groupPlayer->InBattleground() || groupPlayer->InArena() || groupPlayer->InBattlegroundQueue())
                        result = LFGJoinResult::UsingBattleground;
                    if (groupPlayer->HasAura(SPELL_LFG_DUNGEON_DESERTER))
                        result = LFGJoinResult::Deserter;
                    else if (groupPlayer->HasAura(SPELL_LFG_DUNGEON_COOLDOWN))
                        result = LFGJoinResult::RandomCooldown;
                    else if (player->HasAura(9454)) // check if frozen by a GameMaster
                        result = LFGJoinResult::InternalError;
                    ++memberCount;
                }
            }

            if (result == LFGJoinResult::OK && memberCount != group->GetMembersCount())
                result = LFGJoinResult::Disconnected;
        }
    }

    return result;
}

// Initializes all dungeon data caches for the LFG system. Fills dungeon IDs and rewards
void NewLFGMgr::InitializeDungeonData()
{
    uint32 oldMSTime = getMSTime();

    // Fill teleport locations from DB
    QueryResult result = WorldDatabase.Query("SELECT dungeonId, position_x, position_y, position_z, orientation, requiredItemLevel FROM lfg_dungeon_template");
    if (!result)
    {
        TC_LOG_ERROR("server.loading", ">> Loaded 0 lfg dungeon templates. DB table `lfg_dungeon_template` is empty!");
        return;
    }

    uint32 count = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 dungeonId = fields[0].GetUInt32();

        // Validate template entry
        LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(dungeonId);
        if (!entry)
        {
            TC_LOG_ERROR("sql.sql", "Table `lfg_dungeon_template` contains coordinates for non-existing dungeon (ID %u).", dungeonId);
            continue;
        }

        float x = fields[1].GetFloat();
        float y = fields[2].GetFloat();
        float z = fields[3].GetFloat();
        float o = fields[4].GetFloat();
        uint16 requiredItemLevel = fields[5].GetUInt16();

        // Entrance data for dungeon does not exist in lfg_dungeon_template, draw it from area_trigger_teleport instead
        if (x == 0.f && y == 0.f && z == 0.f)
        {
            if (entry->TypeID != AsUnderlyingType(LFGType::Random))
            {
                AreaTriggerStruct const* at = sObjectMgr->GetMapEntranceTrigger(entry->MapID);
                if (!at)
                {
                    TC_LOG_ERROR("sql.sql", "Failed to load dungeon %s (ID: %u). Cannot find areatrigger for map (ID: %u).", entry->Name, entry->ID, entry->MapID);
                    continue;
                }

                x = at->target_X;
                y = at->target_Y;
                z = at->target_Z;
                o = at->target_Orientation;
            }
        }

        LFGDungeonData data(entry, WorldLocation(entry->MapID, x, y, z, o), requiredItemLevel);
        _lfgDungeonData.emplace(entry->ID, data);

        ++count;
    } while (result->NextRow());

    // ORDER BY is important
    result = WorldDatabase.Query("SELECT dungeonId, maxLevel, firstQuestId, otherQuestId, shortageQuestId, completionsPerPeriod, dailyReset FROM lfg_dungeon_rewards ORDER BY dungeonId, maxLevel ASC");

    if (!result)
    {
        TC_LOG_ERROR("server.loading", ">> Loaded 0 lfg dungeon rewards. DB table `lfg_dungeon_rewards` is empty!");
        return;
    }

    Field* fields = nullptr;
    do
    {
        fields = result->Fetch();
        uint32 dungeonId            = fields[0].GetUInt32();
        uint8 maxLevel              = fields[1].GetUInt8();
        uint32 firstQuestId         = fields[2].GetUInt32();
        uint32 otherQuestId         = fields[3].GetUInt32();
        uint32 shortageQuestId      = fields[4].GetUInt32();
        uint8 completionsPerPeriod  = fields[5].GetUInt8();
        bool dailyReset             = bool(fields[6].GetUInt8());

        auto itr = _lfgDungeonData.find(dungeonId);
        if (itr == _lfgDungeonData.end())
        {
            TC_LOG_ERROR("server.loading", ">> Failed to load LFG reward for dungeon (ID: %u) from `lfg_dungeon_rewards`. Dungeon has no entry in `lfg_dungeon_template`.", dungeonId);
            continue;
        }

        itr->second.CompletionRewards.push_back(LFGRewardData(maxLevel, firstQuestId, otherQuestId, shortageQuestId, completionsPerPeriod, dailyReset));

    }
    while (result->NextRow());

    // Building random dungeon group caches for faster access
    for (std::pair<uint32, LFGDungeonData> const itr : _lfgDungeonData)
    {
        if (itr.second.DungeonEntry->TypeID != AsUnderlyingType(LFGType::Random))
            continue;

        for (LFGDungeonEntry const* entry : sLFGDungeonStore)
            if (_lfgDungeonData.find(entry->ID) != _lfgDungeonData.end())
                if (entry->RandomID == itr.second.DungeonEntry->ID && entry->TypeID != AsUnderlyingType(LFGType::Random))
                    _lfgDungeonsIdsForRandomDungeonId[itr.second.DungeonEntry->ID].insert(entry->ID);

        // Build additional random dungeon groups from grouping map (4.x onwards only)
        for (LFGDungeonsGroupingMapEntry const* entry : sLFGDungeonsGroupingMapStore)
            if (entry->RandomLfgDungeonsID == itr.second.DungeonEntry->ID)
                if (_lfgDungeonData.find(entry->LfgDungeonsID) != _lfgDungeonData.end())
                    _lfgDungeonsIdsForRandomDungeonId[itr.second.DungeonEntry->ID].insert(entry->LfgDungeonsID);
    }

    TC_LOG_INFO("server.loading", ">> Initialized %u LFG dungeon caches in %u ms.", count, GetMSTimeDiffToNow(oldMSTime));
}

// ----- Opcode Handler Helpers
/*
    These helpers are processing all requests that have been submitted by the client and are
    being invoked by their corresponding opcode handlers.
*/

void NewLFGMgr::ProcessLFGJoinRequest(Player* player, std::unordered_set<uint32>& dungeonIds, uint32 roleMask)
{
    // Step 1: validate player based join permissions
    LFGJoinResult result = GetPlayerAndGroupJoinResult(player, roleMask);

    // Step 2: validate selected dungeon IDs, build dungeon list when a random dungeon ID has been selected
    uint32 randomDungeonId = 0;
    if (result == LFGJoinResult::OK)
        ValidateAndBuildDungeonSelection(player, dungeonIds, result, randomDungeonId);

    Group* group = player->GetGroup();
    ObjectGuid guid = group ? group->GetGUID() : player->GetGUID();

    // Step 3: check available dungeon IDs for locks
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, LFGDungeonLockData>> lockdata;
    if (result == LFGJoinResult::OK)
    {
        if (!group)
            CheckDungeonIdsForLocks(player, dungeonIds, lockdata[guid]);
        else
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* groupPlayer = itr->GetSource())
                    CheckDungeonIdsForLocks(groupPlayer, dungeonIds, lockdata[groupPlayer->GetGUID()]);
        }

        // Remove locked dungeons from available dungeon IDs
        for (auto itr : lockdata)
        {
            for (auto it : itr.second)
                dungeonIds.erase(it.first & 0x00FFFFFF);
        }

        for (auto itr : lockdata)
        {
            // A player is locked to a dungeon while trying to queue to a specific dungeon or no dungeon is available at all.
            if (dungeonIds.empty() || (!randomDungeonId && !itr.second.empty()))
            {
                result = LFGJoinResult::NotMeetRequirements;
                break;
            }
        }
    }

    // Player or group has passed all previous checks, prepare data...
    if (result == LFGJoinResult::OK)
    {
        // Prepare join data
        _lfgJoinData[guid].RandomDungeonID = randomDungeonId;
        std::copy(dungeonIds.begin(), dungeonIds.end(), std::back_inserter(_lfgJoinData[guid].SelectedDungeonIDs));

        if (group)
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* groupPlayer = itr->GetSource())
                    _lfgJoinData[guid].PartyMemberRoleData[groupPlayer->GetGUID()].RoleMask = 0;

        _lfgJoinData[guid].PartyMemberRoleData[player->GetGUID()].RoleMask = roleMask;
        _lfgJoinData[guid].PartyMemberRoleData[player->GetGUID()].RoleConfirmed = true;

        // Solo player just joins the queue right away
        if (!group)
            AddTicketToQueue(GenerateTicket(guid));
        else
            LaunchRoleCheck(guid);
    }
    else //... otherwise just send the join result
        SendJoinResult(guid, result, LFGRoleCheckState::Default, lockdata);
}

void NewLFGMgr::ProcessLFGLeaveRequest(uint32 ticketId, ObjectGuid groupGuid)
{
    // Cancel pending role check
    auto itr = _lfgRoleCheckTimers.find(groupGuid);
    if (itr != _lfgRoleCheckTimers.end())
    {
        CancelRoleCheck(groupGuid);
        return;
    }

    // Remove requester from queue and remove its data if not needed for further actions
    auto it = _lfgQueuePlayerData.find(ticketId);
    if (it != _lfgQueuePlayerData.end())
    {
        bool keepData = it->second.CurrentDungeonID != 0 && !it->second.InstanceCompleted;
        RemoveTicketFromQueue(ticketId, !keepData);
    }
}

void NewLFGMgr::ProcessPlayerRoleRequest(ObjectGuid groupGuid, ObjectGuid playerGuid, uint32 desiredRoles)
{
    if (Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid))
    {
        if (HasInvalidRoles(player, desiredRoles))
        {
            TC_LOG_ERROR("lfg", "Player %s tried to pick roles (%u) that are not available to his class. Possible cheater!", player->GetGUID().ToString().c_str(), desiredRoles);
            return;
        }
    }

    auto itr = _lfgJoinData.find(groupGuid);
    if (itr == _lfgJoinData.end())
        return;

    itr->second.PartyMemberRoleData[playerGuid].RoleMask = desiredRoles;
    itr->second.PartyMemberRoleData[playerGuid].RoleConfirmed = true;
    SendRoleChosen(groupGuid, playerGuid, desiredRoles);

    // Player has selected no role. Cancel role check
    if (!desiredRoles)
    {
        SendJoinResult(groupGuid, LFGJoinResult::RolecheckFailed, LFGRoleCheckState::NoRole);
        SendRolecheckUpdate(groupGuid, LFGRoleCheckState::NoRole);
        _lfgJoinData.erase(groupGuid);
        _lfgRoleCheckTimers.erase(groupGuid);
        return;
    }

    uint8 remainingPlayers = 0;

    // Check how many players have yet to pick a role
    for (auto it : itr->second.PartyMemberRoleData)
        if (!it.second.RoleConfirmed)
            ++remainingPlayers;

    // All players have finished their role checks. Check for valid selections and put them in the queue if possible
    if (!remainingPlayers)
    {
        SendRolecheckUpdate(groupGuid, LFGRoleCheckState::Finished);
        AddTicketToQueue(GenerateTicket(groupGuid));
        _lfgRoleCheckTimers.erase(groupGuid);
        //SendStatusUpdate(groupGuid, LFGUpdateReason::AddedToQueue); // For some reason blizzard sends the AddedToQueue update twice after role checks.
    }
    else
        SendRolecheckUpdate(groupGuid, LFGRoleCheckState::Initializing);
}

// ----- Packet Sending Helpers
/*
    These helpers are being used to prepare needed data for server packets and ultimately calling
    the corresponding helper in LFGHandler
*/

void NewLFGMgr::SendStatusUpdate(uint32 ticketId, LFGUpdateReason reason)
{
   auto itr = _lfgQueuePlayerData.find(ticketId);
   if (itr == _lfgQueuePlayerData.end())
        return;

    LFGUpdateStatusData updateData;
    updateData.UpdateReason = reason;

    switch (reason)
    {
        case LFGUpdateReason::JoinQueueInitial:
            updateData.Joined = true;
            break;
        case LFGUpdateReason::AddedToQueue:
        case LFGUpdateReason::JoinQueue:
            updateData.Joined = true;
            updateData.Queued = true;
            break;
        default:
            // Todo: handle the rest.
            break;
    }

    updateData.LFGJoined = reason != LFGUpdateReason::RemovedFromQueue;
    updateData.IsParty = itr->second.JoinData.PartyMemberRoleData.size() > 1;
    updateData.RideTicket = itr->second.RideTicket;
    updateData.Comment = itr->second.Comment;

    // Random dungeon queues only send the entry of the selected random dungeon ID
    if (itr->second.JoinData.RandomDungeonID)
        updateData.Slots.push_back(_lfgDungeonData[itr->second.JoinData.RandomDungeonID].DungeonEntry->Entry());
    else
    {
        // Otherwise send the entry of every dungeon ID that has been queued for
        std::transform(itr->second.JoinData.SelectedDungeonIDs.begin(), itr->second.JoinData.SelectedDungeonIDs.end(), std::back_inserter(updateData.Slots), [&](uint32 dungeonId)
        {
            return _lfgDungeonData[dungeonId].DungeonEntry->Entry();
        });
    }

    for (auto it : itr->second.JoinData.PartyMemberRoleData)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(it.first))
            player->GetSession()->SendLfgUpdateStatusNew(updateData);
}

void NewLFGMgr::SendJoinResult(ObjectGuid guid, LFGJoinResult result, LFGRoleCheckState resultDetail, Optional<std::unordered_map<ObjectGuid, std::unordered_map<uint32, LFGDungeonLockData>>> lockData)
{
    LFGJoinResultData joinResult;
    joinResult.Result = result;
    joinResult.ResultDetail = resultDetail;

    //FillRideTicketData(guid, joinResult.RideTicket);
    if (lockData)
        joinResult.PlayerLockMap = *lockData;

    // Join results are only being sent to the leader of a party
    Player* player = nullptr;
    if (guid.IsPlayer())
        player = ObjectAccessor::FindConnectedPlayer(guid);
    else if (guid.IsGroup())
        if (Group* group = sGroupMgr->GetGroupByGUID(guid))
            player = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());

    if (player)
        player->GetSession()->SendLfgJoinResultNew(joinResult);
}

void NewLFGMgr::SendRoleChosen(ObjectGuid guid, ObjectGuid playerGuid, uint32 selectedRoles)
{
    auto itr = _lfgJoinData.find(guid);
    if (itr == _lfgJoinData.end())
        return;

    for (auto it : itr->second.PartyMemberRoleData)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(it.first))
            player->GetSession()->SendLfgRoleChosen(playerGuid, selectedRoles);
}

void NewLFGMgr::SendRolecheckUpdate(ObjectGuid guid, LFGRoleCheckState state, bool beginning /*= false*/)
{
    auto itr = _lfgJoinData.find(guid);
    if (itr == _lfgJoinData.end())
        return;

    LFGRolecheckUpdateData rolecheckData;

    if (itr->second.RandomDungeonID)
        rolecheckData.Slots.push_back(_lfgDungeonData[itr->second.RandomDungeonID].DungeonEntry->Entry());
    else
    {
        std::transform(itr->second.SelectedDungeonIDs.begin(), itr->second.SelectedDungeonIDs.end(), std::back_inserter(rolecheckData.Slots), [&](uint32 dungeonId)
        {
            return _lfgDungeonData[dungeonId].DungeonEntry->Entry();
        });
    }

    rolecheckData.State = state;
    rolecheckData.IsBeginning = beginning;
    for (auto it : itr->second.PartyMemberRoleData)
        rolecheckData.PartyMemberRoles[it.first] = it.second;

    for (auto it : itr->second.PartyMemberRoleData)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(it.first))
            player->GetSession()->SendLfgRoleCheckUpdateNew(rolecheckData);
}

void NewLFGMgr::SendQueueStatus(uint32 ticketId)
{

    /*
    for (auto it : data.JoinData.PartyMemberRoleData)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(it.first))
            player->GetSession()->SendLfgQueueStatusNew(data.QueueStatusData);
    */
}

// ----- Internal Helpers
/*
    These helpers process all further internal actions, such as container management.
*/

/*
    Validates the dungeon selection of the player by checking for illegal dungeon type combinations and available data (loaded at startup)
    Also sanitizes the selection by replacing random dungeon IDs with a set of pre-cached dungeon IDs and stores the random dungeon ID in a referenced variable.
    Corrects the join result if dungeon selection is illegal.
*/
void NewLFGMgr::ValidateAndBuildDungeonSelection(Player const* player, std::unordered_set<uint32>& dungeonIdSet, LFGJoinResult& result, uint32& randomDungeonId)
{
    // Remove invalid or unavailable dungeon selections
    Trinity::Containers::EraseIf(dungeonIdSet, [&](uint32 const dungeonId)
    {
        return _lfgDungeonData.find(dungeonId) == _lfgDungeonData.end();
    });

    if (dungeonIdSet.empty())
    {
        result = LFGJoinResult::InternalError;
        return;
    }

    // Validate dungeon selection
    bool hasDungeon = false;
    bool hasRaid = false;

    for (uint32 dungeonId : dungeonIdSet)
    {
        LFGDungeonData const& data = _lfgDungeonData.at(dungeonId);
        switch (LFGType(data.DungeonEntry->TypeID))
        {
            case LFGType::Dungeon:
                hasDungeon = true;
                break;
            case LFGType::Raid:
                hasRaid = true;
                break;
            case LFGType::Random:
                if (dungeonIdSet.size() > 1)
                {
                    // Players cannot queue up for multiple dungeon IDs when queueing up for a random dungeon
                    result = LFGJoinResult::InternalError;
                    return;
                }
                randomDungeonId = data.DungeonEntry->ID;
                break;
            default:
                // Unsupported dungeon types cannot be queued for
                result = LFGJoinResult::InternalError;
                return;
        }
    }

    uint8 dungeonTypeCount = 0;
    for (bool hasDungeonType : { hasDungeon, hasRaid, randomDungeonId != 0 })
        if (hasDungeonType)
            ++dungeonTypeCount;

    // Player cannot select multiple lfg dungeons of different types
    if (dungeonTypeCount > 1)
    {
        result = LFGJoinResult::MixedRaidAndDungeon;
        return;
    }

    // If we have selected a random dungeon, we now gather the available dungeons of its group
    if (randomDungeonId)
        dungeonIdSet = _lfgDungeonsIdsForRandomDungeonId[randomDungeonId];
}

inline bool IsSeasonActive(uint32 dungeonId)
{
    switch (dungeonId)
    {
        case 285: // The Headless Horseman
            return IsHolidayActive(HOLIDAY_HALLOWS_END);
        case 286: // The Frost Lord Ahune
            return IsHolidayActive(HOLIDAY_FIRE_FESTIVAL);
        case 287: // Coren Direbrew
            return IsHolidayActive(HOLIDAY_BREWFEST);
        case 288: // The Crown Chemical Co.
            return IsHolidayActive(HOLIDAY_LOVE_IS_IN_THE_AIR);
    }
    return false;
}

// Checks if a player is locked for given dungeonIDs and builds a lockmap for the locked dungeon for join result packets. 
void NewLFGMgr::CheckDungeonIdsForLocks(Player* player, std::unordered_set<uint32>& dungeonIds, std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData>& lockMap)
{
    for (uint32 dungeonId : dungeonIds)
    {
        // The data has been validated at this point so no need for checks.
        LFGDungeonData& data = _lfgDungeonData.at(dungeonId);
        if (data.DungeonEntry->TypeID == AsUnderlyingType(LFGType::Random))
            continue;

        uint8 playerLevel = player->getLevel();
        uint8 expansion = player->GetSession()->GetExpansion();
        bool denyJoin = !player->GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_DUNGEON_FINDER);

        Optional<LFGLockStatusType> reason;

        if (denyJoin)
            reason = LFGLockStatusType::None;
        else if (data.DungeonEntry->ExpansionLevel > expansion)
            reason = LFGLockStatusType::InsufficientExpansion;
        else if (DisableMgr::IsDisabledFor(DISABLE_TYPE_MAP, data.DungeonEntry->MapID, player))
            reason = LFGLockStatusType::None;
        else if (DisableMgr::IsDisabledFor(DISABLE_TYPE_LFG_MAP, data.DungeonEntry->MapID, player))
            reason = LFGLockStatusType::None;
        else if (data.DungeonEntry->DifficultyID > DUNGEON_DIFFICULTY_NORMAL && player->GetBoundInstance(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID)))
            reason = LFGLockStatusType::RaidLocked;
        else if (data.DungeonEntry->DifficultyID > DUNGEON_DIFFICULTY_NORMAL && player->GetBoundInstance(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID), data.DungeonEntry->TypeID == AsUnderlyingType(LFGType::Raid)))
            reason = LFGLockStatusType::RaidLocked;
        else if (data.DungeonEntry->MinLevel > playerLevel)
            reason = LFGLockStatusType::TooLowLevel;
        else if (data.DungeonEntry->Maxlevel < playerLevel)
            reason = LFGLockStatusType::TooHighLevel;
        else if ((data.DungeonEntry->Flags & AsUnderlyingType(LFGFlags::Seasonal)) != 0 && !IsSeasonActive(data.DungeonEntry->ID))
            reason = LFGLockStatusType::NotInSeason;
        else if (data.RequiredItemLevel > player->GetAverageItemLevel())
            reason = LFGLockStatusType::TooLowGearScore;
        else if (AccessRequirement const* ar = sObjectMgr->GetAccessRequirement(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID)))
        {
            if (ar->achievement && !player->HasAchieved(ar->achievement))
                reason = LFGLockStatusType::MissingAchievement;
            else if (player->GetTeam() == ALLIANCE && ar->quest_A && !player->GetQuestRewardStatus(ar->quest_A))
                reason = LFGLockStatusType::QuestNotCompleted;
            else if (player->GetTeam() == HORDE && ar->quest_H && !player->GetQuestRewardStatus(ar->quest_H))
                reason = LFGLockStatusType::QuestNotCompleted;
            else
                if (ar->item)
                {
                    if (!player->HasItemCount(ar->item) && (!ar->item2 || !player->HasItemCount(ar->item2)))
                        reason = LFGLockStatusType::MissingItem;
                }
                else if (ar->item2 && !player->HasItemCount(ar->item2))
                    reason = LFGLockStatusType::MissingItem;
        }

        if (reason.is_initialized())
        {
            if (*reason == LFGLockStatusType::TooLowGearScore)
                lockMap.emplace(data.DungeonEntry->Entry(), LFGDungeonLockData(*reason, uint32(data.RequiredItemLevel), uint32(player->GetAverageItemLevel())));
            else
                lockMap.emplace(data.DungeonEntry->Entry(), LFGDungeonLockData(*reason, 0, 0));
        }
    }
}

// Adds the requester to the queued requester set to make it available for the matchmaking.
void NewLFGMgr::AddTicketToQueue(LFGRideTicket ticket)
{
    _lfgQueuePlayerData.emplace(ticket.ID, LFGQueuePlayerData(_lfgJoinData.at(ticket.RequesterGUID), ticket));
    SendStatusUpdate(ticket.ID, LFGUpdateReason::JoinQueueInitial);

    _lfgQueue.AddTicketToQueue(_lfgQueuePlayerData.at(ticket.ID));
    SendStatusUpdate(ticket.ID, LFGUpdateReason::AddedToQueue);
}

// Removes the requester from the queued requester set, making him unavailable for the matchmaking and sends the removed from queue status update.
void NewLFGMgr::RemoveTicketFromQueue(uint32 ticketId, bool erasePlayerData)
{
    auto itr = _lfgQueuePlayerData.find(ticketId);
    if (itr == _lfgQueuePlayerData.end())
        return;

    _lfgQueue.RemoveTicketFromQueue(ticketId);
    SendStatusUpdate(ticketId, LFGUpdateReason::RemovedFromQueue);

    if (erasePlayerData)
    {
        _lfgJoinData.erase(itr->second.RideTicket.RequesterGUID);
        _lfgQueuePlayerData.erase(ticketId);
    }
}

// Initializes the role check for the party and sets the expiry timer .
void NewLFGMgr::LaunchRoleCheck(ObjectGuid guid)
{
    // Initialize the rolecheck timer
    _lfgRoleCheckTimers[guid] = LFG_ROLE_CHECK_TIME_LIMIT;
    SendRolecheckUpdate(guid, LFGRoleCheckState::Initializing, true);
}

// Cancels role check if exists and sends an abort role check update to the party.
void NewLFGMgr::CancelRoleCheck(ObjectGuid guid)
{
    SendRolecheckUpdate(guid, LFGRoleCheckState::Aborted);
    _lfgJoinData.erase(guid);
}

// The update helper that updates role check timers and calls queue updates at given intervals
void NewLFGMgr::Update(uint32 diff)
{
    // Updating pending role checks
    for (std::unordered_map<ObjectGuid, Milliseconds>::iterator itr = _lfgRoleCheckTimers.begin(); itr != _lfgRoleCheckTimers.end();)
    {
        itr->second -= Milliseconds(diff);
        if (itr->second <= 0s)
        {
            // Rolecheck has expired, cancel role check and join procedure
            SendRolecheckUpdate(itr->first, LFGRoleCheckState::MissingRole);
            _lfgJoinData.erase(itr->first);

            // Remove queue player data when the instance has been finished already or no instance has been started yet
            //if (!_lfgQueuePlayerData[itr->first].CurrentDungeonID || (_lfgQueuePlayerData[itr->first].CurrentDungeonID != 0  && _lfgQueuePlayerData[itr->first].InstanceCompleted))
           //     _lfgQueuePlayerData.erase(itr->first);

            itr = _lfgRoleCheckTimers.erase(itr);
        }
        else
            ++itr;
    }

    _lfgQueueUpdateInterval -= Milliseconds(diff);
    if (_lfgQueueUpdateInterval <= 0s)
    {
        _lfgQueueUpdateInterval = LFG_QUEUE_UPDATE_INTERVAL;
        _lfgQueue.Update(_lfgQueuePlayerData);

        for (std::pair<uint32 const, LFGQueuePlayerData>& data : _lfgQueuePlayerData)
        {
            if (!data.second.NeedsQueueUpdate)
                continue;

            SendQueueStatus(data.first);
            data.second.NeedsQueueUpdate = false;
        }
    }


    // Todo: update queue
}

// ----- Getters
/*
    These helpers return all kinds of data for making life easier for all of us.
*/

std::unordered_set<uint32> const NewLFGMgr::GetAvailableSeasonalRandomAndRaidDungeonIds(uint8 playerLevel, uint8 playerExpansion) const
{
    std::unordered_set<uint32> dungeons;

    for (auto itr : _lfgDungeonData)
    {
        LFGDungeonData& data = itr.second;

        switch (LFGType(data.DungeonEntry->TypeID))
        {
            case LFGType::Random:
            case LFGType::Raid:
                // Random dungeons and raids always pass this stage of checks
                break;
            case LFGType::Dungeon:
                // Regular dungeons are only allowed if they are seasonal or a LFR raid
                if (!(data.DungeonEntry->Flags & (AsUnderlyingType(LFGFlags::Seasonal) | AsUnderlyingType(LFGFlags::Unk4))))
                    continue;
                break;
            default:
                continue;
        }

        // Skip seasonal dungeons that need a Holiday to be active
         if ((data.DungeonEntry->Flags & AsUnderlyingType(LFGFlags::Seasonal)) != 0 && !IsSeasonActive(data.DungeonEntry->ID))
             continue;

        // Skip entries that are not within the player's level range
        if (playerLevel < data.DungeonEntry->MinLevel || playerLevel > data.DungeonEntry->Maxlevel)
            continue;

        // Skip entries that require a more recent expansion for the account
        if (playerExpansion < data.DungeonEntry->ExpansionLevel)
            continue;

        // All checks passed, time to add the data to our set
        dungeons.insert(data.DungeonEntry->ID);
    }

    return dungeons;
}

LFGDungeonData const* NewLFGMgr::GetDungeonDataForDungeonId(uint32 dungeonId)
{
    if (_lfgDungeonData.find(dungeonId) != _lfgDungeonData.end())
        return &_lfgDungeonData.at(dungeonId);

    return nullptr;
}

std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData> const NewLFGMgr::GetLockedDungeonsForPlayer(ObjectGuid playerGuid) const
{
    std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData> lockMap;
    Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid);
    if (!player)
    {
        TC_LOG_WARN("lfg.data.player.dungeons.locked.get", "Player: %s not ingame while trying to determine his locked dungeons.", playerGuid.ToString().c_str());
        return lockMap;
    }

    uint8 playerLevel = player->getLevel();
    uint8 expansion = player->GetSession()->GetExpansion();
    bool denyJoin = !player->GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_DUNGEON_FINDER);

    // Dungeons that do not have a template entry will be locked by default
    for (LFGDungeonEntry const* entry : sLFGDungeonStore)
    {
        if (_lfgDungeonData.find(entry->ID) != _lfgDungeonData.end())
            continue;

        if (entry->TypeID == AsUnderlyingType(LFGType::World))
            continue;

        lockMap.emplace(entry->Entry(), LFGDungeonLockData(LFGLockStatusType::None, 0, 0));
    }

    // Check for restrictions
    for (auto itr : _lfgDungeonData)
    {
        LFGDungeonData& data = itr.second;
        Optional<LFGLockStatusType> reason;

        if (denyJoin)
            reason = LFGLockStatusType::None;
        else if (data.DungeonEntry->ExpansionLevel > expansion)
            reason = LFGLockStatusType::InsufficientExpansion;
        else if (DisableMgr::IsDisabledFor(DISABLE_TYPE_MAP, data.DungeonEntry->MapID, player))
            reason = LFGLockStatusType::None;
        else if (DisableMgr::IsDisabledFor(DISABLE_TYPE_LFG_MAP, data.DungeonEntry->MapID, player))
            reason = LFGLockStatusType::None;
        else if (data.DungeonEntry->DifficultyID > DUNGEON_DIFFICULTY_NORMAL && player->GetBoundInstance(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID)))
            reason = LFGLockStatusType::RaidLocked;
        else if (data.DungeonEntry->DifficultyID > DUNGEON_DIFFICULTY_NORMAL && player->GetBoundInstance(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID), data.DungeonEntry->TypeID == AsUnderlyingType(LFGType::Raid)))
            reason = LFGLockStatusType::RaidLocked;
        else if (data.DungeonEntry->MinLevel > playerLevel)
            reason = LFGLockStatusType::TooLowLevel;
        else if (data.DungeonEntry->Maxlevel < playerLevel)
            reason = LFGLockStatusType::TooHighLevel;
        else if ((data.DungeonEntry->Flags & AsUnderlyingType(LFGFlags::Seasonal)) != 0 && !IsSeasonActive(data.DungeonEntry->ID))
            reason = LFGLockStatusType::NotInSeason;
        else if (data.RequiredItemLevel > player->GetAverageItemLevel())
            reason = LFGLockStatusType::TooLowGearScore;
        else if (AccessRequirement const* ar = sObjectMgr->GetAccessRequirement(data.DungeonEntry->MapID, Difficulty(data.DungeonEntry->DifficultyID)))
        {
            if (ar->achievement && !player->HasAchieved(ar->achievement))
                reason = LFGLockStatusType::MissingAchievement;
            else if (player->GetTeam() == ALLIANCE && ar->quest_A && !player->GetQuestRewardStatus(ar->quest_A))
                reason = LFGLockStatusType::QuestNotCompleted;
            else if (player->GetTeam() == HORDE && ar->quest_H && !player->GetQuestRewardStatus(ar->quest_H))
                reason = LFGLockStatusType::QuestNotCompleted;
            else
                if (ar->item)
                {
                    if (!player->HasItemCount(ar->item) && (!ar->item2 || !player->HasItemCount(ar->item2)))
                        reason = LFGLockStatusType::MissingItem;
                }
                else if (ar->item2 && !player->HasItemCount(ar->item2))
                    reason = LFGLockStatusType::MissingItem;
        }

        if (reason.is_initialized())
        {
            if (*reason == LFGLockStatusType::TooLowGearScore)
                lockMap.emplace(data.DungeonEntry->Entry(), LFGDungeonLockData(*reason, uint32(data.RequiredItemLevel), uint32(player->GetAverageItemLevel())));
            else
                lockMap.emplace(data.DungeonEntry->Entry(), LFGDungeonLockData(*reason, 0, 0));
        }
    }

    return lockMap;
}

LFGRideTicket const NewLFGMgr::GenerateTicket(ObjectGuid requesterGuid)
{
    LFGRideTicket ticket;
    ticket.RequesterGUID = requesterGuid;
    ticket.Time = GameTime::GetGameTime();
    ticket.ID = GenerateTicketID();

    return ticket;
}

uint32 const NewLFGMgr::GenerateTicketID()
{
    // There is no way that we will ever hit the numeric limit of uint32 before a crash or restarts. But just in case, let's trigger a happy little accident here.
    ASSERT(_nextAvailableTicketId != std::numeric_limits<uint32>::max(), "LFG player data container has exceeded its numeric limit!");
    return _nextAvailableTicketId++; // Post increment intended
}

NewLFGMgr* NewLFGMgr::instance()
{
    static NewLFGMgr instance;
    return &instance;
}
