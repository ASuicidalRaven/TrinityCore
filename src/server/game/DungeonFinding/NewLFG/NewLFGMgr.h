
#ifndef _NEWLFGMGR_H
#define _NEWLFGMGR_H

#include "Common.h"
#include "OptionalFwd.h"
#include "LFGStructs.h"
#include "NewLFGQueue.h"
#include "Hash.h"
#include <unordered_map>
#include <unordered_set>

class Player;
struct LFGDungeonEntry;

enum class LFGState : uint8;
enum class LFGJoinResult : uint8;
enum class LFGUpdateReason: uint8;
enum class LFGRoleCheckState : uint8;

class TC_GAME_API NewLFGMgr
{
private:
    NewLFGMgr();
    ~NewLFGMgr();

public:

    // Loading
    void InitializeDungeonData();

    // Opcode Handler Helpers
    void ProcessLFGJoinRequest(Player* player, std::unordered_set<uint32>& dungeonIds, uint32 roleMask);
    void ProcessLFGLeaveRequest(uint32 ticketId, ObjectGuid groupGuid);
    void ProcessPlayerRoleRequest(ObjectGuid groupGuid, ObjectGuid playerGuid, uint32 desiredRoles);

    // Packet Sending Helpers
    void SendStatusUpdate(uint32 ticketId, LFGUpdateReason reason);
    void SendJoinResult(ObjectGuid playerGuid, LFGJoinResult result, LFGRoleCheckState resultDetail, Optional<std::unordered_map<ObjectGuid, std::unordered_map<uint32, LFGDungeonLockData>>> lockdata = { });
    void SendRoleChosen(ObjectGuid guid, ObjectGuid playerGuid, uint32 selectedRoles);
    void SendRolecheckUpdate(ObjectGuid guid, LFGRoleCheckState state, bool beginning = false);
    void SendQueueStatus(uint32 ticketId);

    // Internal Helpers
    void ValidateAndBuildDungeonSelection(Player const* player, std::unordered_set<uint32>& dungeonIdSet, LFGJoinResult& result, uint32& randomDungeonId);
    void CheckDungeonIdsForLocks(Player* player, std::unordered_set<uint32>& dungeonIds, std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData>& lockMap);

    void AddTicketToQueue(LFGRideTicket ticket);
    void RemoveTicketFromQueue(uint32 ticketId, bool erasePlayerData);

    void LaunchRoleCheck(ObjectGuid guid);
    void CancelRoleCheck(ObjectGuid guid);
    void Update(uint32 diff);

    // Getters
    std::unordered_set<uint32> const GetAvailableSeasonalRandomAndRaidDungeonIds(uint8 playerLevel, uint8 playerExpansion) const;
    LFGDungeonData const* GetDungeonDataForDungeonId(uint32 dungeonId);
    std::unordered_map<uint32 /*Slot*/, LFGDungeonLockData> const GetLockedDungeonsForPlayer(ObjectGuid playerGuid) const;
    LFGRideTicket const GenerateTicket(ObjectGuid requesterGuid);
    uint32 const GenerateTicketID();

    static NewLFGMgr* instance();

private:
    // Dungeon data caches
    std::unordered_map<uint32 /*dungeonId*/, LFGDungeonData> _lfgDungeonData;                // Stores all available informations for a dungeon
    std::unordered_map<uint32 /*dungeonId*/, std::unordered_set<uint32>> _lfgDungeonsIdsForRandomDungeonId; // stores available dungeon IDs for given key random dungeon ID

    // Queue data caches
    std::unordered_map<ObjectGuid /*requesterGUID*/, LFGJoinData> _lfgJoinData; // Stores prepared data for joining the LFG queue
    std::unordered_map<ObjectGuid /*requesterGUID*/, Milliseconds> _lfgRoleCheckTimers;
    std::map<uint32 /*ticketID*/, LFGQueuePlayerData> _lfgQueuePlayerData;      // Stores all queue related data of a player or party for matchmaking and RideTicket generation


    //std::unordered_set<ObjectGuid /*guid*/> _lfgQueuedRequesters;                           // Stores the key guid for _lfgQueuePlayerData for all requesters that are currently queued up for matchmaking
    //std::unordered_map<ObjectGuid, Milliseconds> _lfgRolecheckTimers;                       // Stores remaining rolecheck times when queueing up as group with the key guid of _lfgQueuePlayerData as key
    uint32 _nextAvailableTicketId;                                                          // Stores the next available ticket ID for RideTicket generation
    NewLFGQueue _lfgQueue;                                                                  // Handles the matchmaking of queued up groups and players.
    Milliseconds _lfgQueueUpdateInterval;                                                   // Stores the remaining time until the queue will be updated and tries to match players
};

#define sNewLFGMgr NewLFGMgr::instance()

#endif // _NEWLFGMGR_H
