
#ifndef _NEWLFGQUEUE_
#define _NEWLFGQUEUE_

#include "Common.h"
#include "LFGStructs.h"
#include <unordered_map>

struct LFGQueuePlayerData;

class TC_GAME_API NewLFGQueue
{
public:
    NewLFGQueue() { }
    ~NewLFGQueue() { }

    void AddTicketToQueue(LFGQueuePlayerData const& queueData);
    void RemoveTicketFromQueue(uint32 ticketId);
    void Update(uint32 diff);

private:
    std::map<uint32 /*ticketId*/, LFGQueueData> _lfgQueueData;
};

#endif // _NEWLFGQUEUE_
