
#ifndef _LFGSTRUCTS_H
#define _LFGSTRUCTS_H

#include "Define.h"
#include "Position.h"
#include <vector>

struct LFGDungeonEntry;
enum class LFGLockStatusType : uint16;
enum class LFGJoinResult : uint8;
enum class LFGRoleCheckState : uint8;
enum class LFGUpdateReason : uint8;

// Stores information about dungeon rewards for available dungeons. Primary struct for `lfg_dungeon_rewards`.
struct LFGRewardData
{
    LFGRewardData() : MaxLevel(0), MainRewardQuestID(0), AlternatvieRewardQuestID(0), ShortateRewardQuestID(0), CompletionsPerPeriod(0), DailyReset(false) { }
    LFGRewardData(uint8 maxLevel, uint32 mainRewardQuestId, uint32 alternativeRewardQuestId, uint32 shortateRewardQuestId, uint8 completionsPerPeriod, bool dailyReset) :
        MaxLevel(maxLevel), MainRewardQuestID(mainRewardQuestId), AlternatvieRewardQuestID(alternativeRewardQuestId), ShortateRewardQuestID(shortateRewardQuestId),
        CompletionsPerPeriod(completionsPerPeriod), DailyReset(dailyReset) { }

    uint8 const MaxLevel;
    uint32 const MainRewardQuestID;
    uint32 const AlternatvieRewardQuestID;
    uint32 const ShortateRewardQuestID;
    uint8 const CompletionsPerPeriod;
    bool const DailyReset;
};

// Stores all informations about available LFG dungeons. Primary struct for `lfg_dungeon_template`.
struct LFGDungeonData
{
    LFGDungeonData() : DungeonEntry(nullptr), Entrance(WorldLocation()), RequiredItemLevel(0) { }
    LFGDungeonData(LFGDungeonEntry const* dungeonEntry, WorldLocation entrance, uint16 requiredItemLevel) :
        DungeonEntry(dungeonEntry), Entrance(entrance), RequiredItemLevel(requiredItemLevel) { }

    LFGDungeonEntry const* DungeonEntry;
    WorldLocation Entrance;
    uint16 const RequiredItemLevel;
    std::vector<LFGRewardData> CompletionRewards;
};

// Stores lock info data for building packets and to check for valid dungeons to join
struct LFGDungeonLockData
{
    LFGDungeonLockData(LFGLockStatusType reason, uint32 subReason1, uint32 subReason2) :
        Reason(reason), SubReason1(subReason1), SubReason2(subReason2) { }

    LFGLockStatusType Reason;
    uint32 SubReason1;
    uint32 SubReason2;
};

struct LFGRideTicket
{
    uint32 ID = 0;
    uint32 Type = 3; // sniffed value
    int32 Time = 0;
    ObjectGuid RequesterGUID;
};

// Stores data for SMSG_LFG_JOIN_RESULT
struct LFGJoinResultData
{
    LFGJoinResultData() : Result(LFGJoinResult(0)), ResultDetail(LFGRoleCheckState(0)) { }
    LFGJoinResultData(LFGJoinResult result, LFGRoleCheckState detail) :
        Result(result), ResultDetail(detail) { }

    LFGJoinResult Result;
    LFGRoleCheckState ResultDetail;
    LFGRideTicket RideTicket;
    std::unordered_map<ObjectGuid /*playerGUID*/, std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData>> PlayerLockMap;
};

// Stores data for SMSG_LFG_UPDATE_STATUS
struct LFGUpdateStatusData
{
    bool IsParty = false;
    bool Joined = false;
    bool LFGJoined = false;
    bool Queued = false;
    std::string Comment;
    LFGUpdateReason UpdateReason;
    LFGRideTicket RideTicket;
    std::vector<uint32> Slots;
};

struct LFGPartyMemberRoleData
{
    uint8 RoleMask = 0;
    bool RoleConfirmed = false;
};

// Stores data for SMSG_LFG_ROLE_CHECK_UPDATE
struct LFGRolecheckUpdateData
{
    LFGRoleCheckState State = LFGRoleCheckState(0);
    std::vector<uint32> Slots;
    std::unordered_map<ObjectGuid /*memberGUID*/, LFGPartyMemberRoleData> PartyMemberRoles;
    bool IsBeginning = false;
};

struct LFGJoinData
{
    std::vector<uint32> SelectedDungeonIDs;
    uint32 RandomDungeonID = 0;
    std::unordered_map<ObjectGuid, LFGPartyMemberRoleData> PartyMemberRoleData;
};

// Stores data for SMSG_LFG_QUEUE_STATUS
struct LFGQueueStatusData
{
    LFGRideTicket RideTicket;
    uint32 TimeInQueue = 0;
    uint32 AverageWaitTime = 0;
    std::array<uint32, 3> AverageWaitTimeByRole = { };
    std::array<uint8, 3> RemainingNeededRoles = { };
};



struct LFGQueueData
{
};

struct LFGQueuePlayerData
{
    LFGQueuePlayerData(LFGJoinData& joinData, LFGRideTicket rideTicket) :
        JoinData(joinData), RideTicket(rideTicket), CurrentDungeonID(0), InstanceCompleted(false) { }

    LFGJoinData& JoinData;
    LFGRideTicket RideTicket;
    uint32 CurrentDungeonID;        // Stores the dungeon ID of the instance that is currently being run by the requesters lfg group
    std::string Comment;            // Stores the comment of the player. Used in raid browser tool
    bool InstanceCompleted;         // Stores whenever the instance is completed or not
    bool NeedsQueueUpdate = true;
};


#endif // _LFGSTRUCTS_H
