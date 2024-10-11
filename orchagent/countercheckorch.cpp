#include "countercheckorch.h"
#include "portsorch.h"
#include "select.h"
#include "notifier.h"
#include "sai_serialize.h"
#include <inttypes.h>

#define COUNTER_CHECK_POLL_TIMEOUT_SEC   (5 * 60)

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;

CounterCheckOrch& CounterCheckOrch::getInstance(DBConnector *db)
{
    SWSS_LOG_ENTER();

    static vector<string> tableNames = {};
    static CounterCheckOrch *wd = new CounterCheckOrch(db, tableNames);

    return *wd;
}

CounterCheckOrch::CounterCheckOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE))
{
    SWSS_LOG_ENTER();

    auto interv = timespec { .tv_sec = COUNTER_CHECK_POLL_TIMEOUT_SEC, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "MC_COUNTERS_POLL");
    Orch::addExecutor(executor);
    timer->start();
}

CounterCheckOrch::~CounterCheckOrch(void)
{
    SWSS_LOG_ENTER();
}

void CounterCheckOrch::calculatePfcStats()
{
    for (auto& i : m_pfcFrameCountersMap)
    {
        // Get the current value of the counters
        auto oid = i.first;
        auto counters = i.second;

        // Fetch the new value of the counter
	auto newCounters = getPfcFrameCounters(oid);

	uint64_t delta_stat;
	for (size_t prio = 0; prio < counters.size(); prio++){
		// Calculate the delta of the pfc counter values
		delta_stat = newCounters[prio] - counters[prio];

		// Calculate Rx Pause transitions
		if (!m_prevRxPauseTransition.empty() && m_prevRxPauseTransition[oid][prio] == PFC_ACTIVE && delta_stat == 0)
		{
			// There was transition from pfc pause ON to PFC Pause OFF
			m_totalRxPauseTransitions[oid][prio] += 1;
		}

		// Update m_prevRxPauseTransition
		m_prevRxPauseTransition[oid][prio] = delta_stat == 0 ? PFC_INACTIVE : PFC_ACTIVE;

		// Calculate Max Rx Pause Time and Max Rx Pause Timestamp
		if (delta_stat > 0)
		{
			m_currentRxPauseTime[oid][prio] += delta_stat;
			if (m_currentRxPauseTime[oid][prio] > m_maxRxPauseTime[oid][prio])
			{
				m_maxRxPauseTime[oid][prio] = m_currentRxPauseTime[oid][prio];
				m_maxRxPauseTimestamp[oid][prio] = 0; //TODO: Get current timestamp using chrono
			}
		}
		else{
			m_currentRxPauseTime[oid][prio] = 0;
		}

		// Calculate Total Rx Pause Time
		m_totalRxPauseTime[oid][prio] += delta_stat;

		// Populate the COUNTERS_DB with the generated stats
		std::string db_key = "COUNTERS:" + sai_serialize_object_id(oid);
		std::string totalRxPauseTime = "SAI_PORT_STAT_PFC_" + std::to_string(prio) + "_TOTAL_RX_PAUSE_TIME";
		std::string value = m_totalRxPauseTime[oid][prio];
		m_countersDb->hset(COUNTERS_TABLE, db_key, totalRxPauseTime, value);

		std::string rxPauseTrans = "SAI_PORT_STAT_PFC_" + std::to_string(prio) + "_RX_PAUSE_TRANS";
		value = m_totalRxPauseTransitions[oid][prio];
		m_countersDb->hset(COUNTERS_TABLE, db_key, rxPauseTrans, value);

		std::string maxRxPauseTime = "SAI_PORT_STAT_PFC_" + std::to_string(prio) + "_MAX_RX_PAUSE_TIME";
		value = m_maxRxPauseTime[oid][prio];
		m_countersDb->hset(COUNTERS_TABLE, db_key, maxRxPauseTime, value);

		std::string maxRxPauseTimestamp = "SAI_PORT_STAT_PFC_" + std::to_string(prio) + "_MAX_RX_PAUSE_TMSP";
		value = m_maxRxPauseTimestamp[oid][prio];
		m_countersDb->hset(COUNTERS_TABLE, db_key, maxRxPauseTimestamp, value);
	}
    }
}

void CounterCheckOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    mcCounterCheck();
    pfcFrameCounterCheck();
    calculatePfcStats();
}

void CounterCheckOrch::mcCounterCheck()
{
    SWSS_LOG_ENTER();

    for (auto& i : m_mcCountersMap)
    {
        auto oid = i.first;
        auto mcCounters = i.second;
        uint8_t pfcMask = 0;

        Port port;
        if (!gPortsOrch->getPort(oid, port))
        {
            SWSS_LOG_ERROR("Invalid port oid 0x%" PRIx64, oid);
            continue;
        }

        auto newMcCounters = getQueueMcCounters(port);

        if (!gPortsOrch->getPortPfc(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC mask on port %s", port.m_alias.c_str());
            continue;
        }

        for (size_t prio = 0; prio != mcCounters.size(); prio++)
        {
            bool isLossy = ((1 << prio) & pfcMask) == 0;
            if (newMcCounters[prio] == numeric_limits<uint64_t>::max())
            {
                SWSS_LOG_WARN("Could not retreive MC counters on queue %zu port %s",
                        prio,
                        port.m_alias.c_str());
            }
            else if (!isLossy && mcCounters[prio] < newMcCounters[prio])
            {
                SWSS_LOG_WARN("Got Multicast %" PRIu64 " frame(s) on lossless queue %zu port %s",
                        newMcCounters[prio] - mcCounters[prio],
                        prio,
                        port.m_alias.c_str());
            }
        }

        i.second= newMcCounters;
    }
}

void CounterCheckOrch::pfcFrameCounterCheck()
{
    SWSS_LOG_ENTER();

    for (auto& i : m_pfcFrameCountersMap)
    {
        auto oid = i.first;
        auto counters = i.second;
        auto newCounters = getPfcFrameCounters(oid);
        uint8_t pfcMask = 0;

        Port port;
        if (!gPortsOrch->getPort(oid, port))
        {
            SWSS_LOG_ERROR("Invalid port oid 0x%" PRIx64, oid);
            continue;
        }

        if (!gPortsOrch->getPortPfc(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC mask on port %s", port.m_alias.c_str());
            continue;
        }

        for (size_t prio = 0; prio != counters.size(); prio++)
        {
            bool isLossy = ((1 << prio) & pfcMask) == 0;
            if (newCounters[prio] == numeric_limits<uint64_t>::max())
            {
                SWSS_LOG_WARN("Could not retreive PFC frame count on queue %zu port %s",
                        prio,
                        port.m_alias.c_str());
            }
            else if (isLossy && counters[prio] < newCounters[prio])
            {
                SWSS_LOG_WARN("Got PFC %" PRIu64 " frame(s) on lossy queue %zu port %s",
                        newCounters[prio] - counters[prio],
                        prio,
                        port.m_alias.c_str());
            }
        }

        i.second = newCounters;
    }
}


PfcFrameCounters CounterCheckOrch::getPfcFrameCounters(sai_object_id_t portId)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fieldValues;
    PfcFrameCounters counters;
    counters.fill(numeric_limits<uint64_t>::max());

    static const array<string, PFC_WD_TC_MAX> counterNames =
    {
        "SAI_PORT_STAT_PFC_0_RX_PKTS",
        "SAI_PORT_STAT_PFC_1_RX_PKTS",
        "SAI_PORT_STAT_PFC_2_RX_PKTS",
        "SAI_PORT_STAT_PFC_3_RX_PKTS",
        "SAI_PORT_STAT_PFC_4_RX_PKTS",
        "SAI_PORT_STAT_PFC_5_RX_PKTS",
        "SAI_PORT_STAT_PFC_6_RX_PKTS",
        "SAI_PORT_STAT_PFC_7_RX_PKTS"
    };

    if (!m_countersTable->get(sai_serialize_object_id(portId), fieldValues))
    {
        return counters;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);


        for (size_t prio = 0; prio != counterNames.size(); prio++)
        {
            if (field == counterNames[prio])
            {
                counters[prio] = stoul(value);
            }
        }
    }

    return counters;
}

QueueMcCounters CounterCheckOrch::getQueueMcCounters(
        const Port& port)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fieldValues;
    QueueMcCounters counters;

    for (uint8_t prio = 0; prio < port.m_queue_ids.size(); prio++)
    {
        sai_object_id_t queueId = port.m_queue_ids[prio];
        auto queueIdStr = sai_serialize_object_id(queueId);
        auto queueType = m_countersDb->hget(COUNTERS_QUEUE_TYPE_MAP, queueIdStr);

        if (queueType.get() == nullptr || *queueType != "SAI_QUEUE_TYPE_MULTICAST" || !m_countersTable->get(queueIdStr, fieldValues))
        {
            continue;
        }

        uint64_t pkts = numeric_limits<uint64_t>::max();
        for (const auto& fv : fieldValues)
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);

            if (field == "SAI_QUEUE_STAT_PACKETS")
            {
                pkts = stoul(value);
            }
        }
        counters.push_back(pkts);
    }

    return counters;
}

void CounterCheckOrch::addPort(const Port& port)
{
    m_mcCountersMap.emplace(port.m_port_id, getQueueMcCounters(port));
    m_pfcFrameCountersMap.emplace(port.m_port_id, getPfcFrameCounters(port.m_port_id));
}

void CounterCheckOrch::removePort(const Port& port)
{
    m_mcCountersMap.erase(port.m_port_id);
    m_pfcFrameCountersMap.erase(port.m_port_id);
}
