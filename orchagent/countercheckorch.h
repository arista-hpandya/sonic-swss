#ifndef COUNTERCHECK_ORCH_H
#define COUNTERCHECK_ORCH_H

#include "orch.h"
#include "port.h"
#include "timer.h"
#include <queue>
#include <array>

#define PFC_WD_TC_MAX 8
#define PFC_ACTIVE 1
#define PFC_INACTIVE 0

extern "C" {
#include "sai.h"
}

typedef std::vector<uint64_t> QueueMcCounters;
typedef std::array<uint64_t, PFC_WD_TC_MAX> PfcFrameCounters;

class CounterCheckOrch: public Orch
{
public:
    static CounterCheckOrch& getInstance(swss::DBConnector *db = nullptr);
    virtual void doTask(swss::SelectableTimer &timer);
    virtual void doTask(Consumer &consumer) {}
    void addPort(const swss::Port& port);
    void removePort(const swss::Port& port);
    void calculatePfcStats();

    // Member variables to capture additional Pfc stats
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_prevRxPauseTransition;
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_totalRxPauseTransitions;
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_maxRxPauseTime;
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_maxRxPauseTimestamp; //TODO: Figure out a timestamp datatype
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_currentRxPauseTime;
    std::map<sai_object_id_t, std::map<size_t, uint64_t>> m_totalRxPauseTime;

private:
    CounterCheckOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~CounterCheckOrch(void);
    QueueMcCounters getQueueMcCounters(const swss::Port& port);
    PfcFrameCounters getPfcFrameCounters(sai_object_id_t portId);
    void mcCounterCheck();
    void pfcFrameCounterCheck();

    std::map<sai_object_id_t, QueueMcCounters> m_mcCountersMap;
    std::map<sai_object_id_t, PfcFrameCounters> m_pfcFrameCountersMap;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::Table> m_countersTable = nullptr;
};

#endif
