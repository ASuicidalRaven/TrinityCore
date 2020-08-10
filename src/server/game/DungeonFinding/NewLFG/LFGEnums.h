
#ifndef _LFGENUMS_H
#define _LFGENUMS_H

#include "Define.h"
#include "Duration.h"

constexpr uint32 const SPELL_LFG_DUNGEON_DESERTER = 71041;
constexpr uint32 const SPELL_LFG_DUNGEON_COOLDOWN = 71328;
constexpr Milliseconds const LFG_ROLE_CHECK_TIME_LIMIT = 2min;
constexpr Milliseconds const LFG_QUEUE_UPDATE_INTERVAL = 5s;

enum LFGRoles : uint8
{
    LFG_ROLE_LEADER = 0x1,
    LFG_ROLE_TANK   = 0x2,
    LFG_ROLE_HEAL   = 0x4,
    LFG_ROLE_DAMAGE = 0x8,

    LFG_ROLE_MASK_ALL_NEEDED = (LFG_ROLE_TANK | LFG_ROLE_HEAL | LFG_ROLE_DAMAGE)
};

enum class LFGUpdateReason : uint8
{
    Default                 = 0, // Internal Value
    LeaderUnk1              = 1, // Todo: name and handle
    LeaveRaidbrowser        = 2,
    JoinRaidbrowser         = 3,
    RolecheckAborted        = 4,
    Unk1                    = 5, // Todo: name and handle
    JoinQueue               = 6,
    RolecheckFailed         = 7,
    RemovedFromQueue        = 8,
    ProposalFailed          = 9,
    ProposalDeclined        = 10,
    GroupFound              = 11,
    Unk2                    = 12, // Todo: name and handle
    AddedToQueue            = 13,
    ProposalBegin           = 14,
    UpdateStatus            = 15,
    GroupMemberOffline      = 16,
    GroupDisbandUnk         = 17, // Todo: name and handle
    // 18 - 23 unused?
    JoinQueueInitial        = 24,
    DungeonFinished         = 25,
    // 26 - 42 unused?
    PartyRoleNotAvailable   = 43,
    JoinLFGObjectFailed     = 45
};

enum class LFGState : uint8
{
    None            = 0, // Not using any LFG tool
    Rolecheck       = 1, // Rolecheck in progress
    Queue           = 2, // Queued
    Proposal        = 3, // Instance is being proposed to group
    Boot            = 4, // Vote Kick is in progress
    Dungeon         = 5, // LFG group is in instance
    DungeonFinished = 6, // LFG group has finished the instance
    Raidbrowser     = 7  // Using raid browser tool
};

enum class LFGLockStatusType : uint16
{
    None                    = 0,
    InsufficientExpansion   = 1,
    TooLowLevel             = 2,
    TooHighLevel            = 3,
    TooLowGearScore         = 4,
    TooHighGearScore        = 5,
    RaidLocked              = 6,
    AttunementTooLowLevel   = 1001,
    AttunementTooHighLevel  = 1002,
    QuestNotCompleted       = 1022,
    MissingItem             = 1025,
    NotInSeason             = 1031,
    MissingAchievement      = 1034
};

enum class LFGFlags : uint16
{
    Unk1        = 0x001,
    Unk2        = 0x002,
    Seasonal    = 0x004,
    Unk3        = 0x008,
    Unk4        = 0x040, // Unk4 and Unk5 only seen in LFR raids
    Unk5        = 0x200
};

enum class LFGType : uint8
{
    None    = 0,
    Dungeon = 1,
    Raid    = 2,
    World   = 4,
    Heroic  = 5,
    Random  = 6
};

enum class LFGProposalState : uint8
{
    Initiating  = 0,
    Failed      = 1,
    Success     = 2
};

enum class LFGTeleportResult : uint8
{
    None                = 0, // Internal use
    Dead                = 1,
    Falling             = 2,
    OnTransport         = 3,
    Exhaustion          = 4,
    Unk1                = 5, // old enum says it triggers no client reaction.
    NoReturnLocation    = 6,
    Unk2                = 7, // Todo: name and handle
    ImmuneToSummons     = 8, // "You can't do that right now

    // unknown values
    //LFG_TELEPORT_RESULT_NOT_IN_DUNGEON,
    //LFG_TELEPORT_RESULT_NOT_ALLOWED,
    //LFG_TELEPORT_RESULT_ALREADY_IN_DUNGEON
};

enum class LFGJoinResult : uint8
{
    // 3 = No client reaction | 18 = "Rolecheck failed"
    OK                  = 0x00,   // Joined (no client msg)
    JoinFailed          = 0x1B,   // RoleCheck Failed
    GroupFull           = 0x1C,   // Your group is full
    InternalError       = 0x1E,   // Internal LFG Error
    NotMeetRequirements = 0x1F,   // You do not meet the requirements for the chosen dungeons
    //LFG_JOIN_PARTY_NOT_MEET_REQS   = 6,      // One or more party members do not meet the requirements for the chosen dungeons
    MixedRaidAndDungeon = 0x20,   // You cannot mix dungeons, raids, and random when picking dungeons
    MultipleRealms      = 0x21,   // The dungeon you chose does not support players from multiple realms
    Disconnected        = 0x22,   // One or more party members are pending invites or disconnected
    PartyInfoFailed     = 0x23,   // Could not retrieve information about some party members
    DungeonInvalid      = 0x24,   // One or more dungeons was not valid
    Deserter            = 0x25,   // You can not queue for dungeons until your deserter debuff wears off
    PartyDeserter       = 0x26,   // One or more party members has a deserter debuff
    RandomCooldown      = 0x27,   // You can not queue for random dungeons while on random dungeon cooldown
    PartyRandomCooldown = 0x28,   // One or more party members are on random dungeon cooldown
    TooManyMembers      = 0x29,   // You can not enter dungeons with more that 5 party members
    UsingBattleground   = 0x2A,   // You can not use the dungeon system while in BG or arenas
    RolecheckFailed     = 0x2B    // Role check failed, client shows special error
};

enum class LFGRoleCheckState : uint8
{
    Default         = 0,      // Default value
    Finished        = 1,      // Role check finished
    Initializing    = 2,      // Role check begins
    MissingRole     = 3,      // Someone didn't selected a role after 2 mins
    WrongRoles      = 4,      // Can't form a group with that role selection
    Aborted         = 5,      // "Your group leader has cancelled the Role Check"
    NoRole          = 6       // Someone selected no role
};

#endif // _LFGENUMS_H
