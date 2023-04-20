// Copyright (c) 2023. ByteDance Inc. All rights reserved.


#pragma once

#include <cstdint>
#include <chrono>
#include <list>
#include "utils/thirdparty/quiche/rtt_stats.h"
#include "basefw/base/log.h"
#include "utils/rttstats.h"
#include "utils/transporttime.h"
#include "utils/defaultclock.hpp"
#include "sessionstreamcontroller.hpp"
#include "packettype.h"

enum class CongestionCtlType : uint8_t
{
    none = 0,
    copa = 1,
};

struct LossEvent
{
    bool valid{ false };
    // There may be multiple timeout events at one time
    std::vector<InflightPacket> lossPackets;
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "lossPackets:{";
        for (const auto& pkt: lossPackets)
        {
            ss << pkt;
        }

        ss << "} "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

struct AckEvent
{
    /** since we receive packets one by one, each packet carries only one data piece*/
    bool valid{ false };
    DataPacket ackPacket;
    Timepoint sendtic{ Timepoint::Infinite() };
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "ackpkt:{"
           << "seq: " << ackPacket.seq << " "
           << "dataid: " << ackPacket.pieceId << " "
           << "} "
           << "sendtic: " << sendtic.ToDebuggingValue() << " "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

/** This is a loss detection algorithm interface
 *  similar to the GeneralLossAlgorithm interface in Quiche project
 * */
class LossDetectionAlgo
{

public:
    /** @brief this function will be called when loss detection may happen, like timer alarmed or packet acked
     * input
     * @param downloadingmap all the packets inflight, sent but not acked or lost
     * @param eventtime timpoint that this function is called
     * @param ackEvent  ack event that trigger this function, if any
     * @param maxacked max sequence number that has acked
     * @param rttStats RTT statics module
     * output
     * @param losses loss event
     * */
    virtual void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime,
            const AckEvent& ackEvent, uint64_t maxacked, LossEvent& losses, RttStats& rttStats)
    {
    };

    virtual ~LossDetectionAlgo() = default;

};

class DefaultLossDetectionAlgo : public LossDetectionAlgo
{/// Check loss event based on RTO
public:
    void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime, const AckEvent& ackEvent,
            uint64_t maxacked, LossEvent& losses, RttStats& rttStats) override
    {
        SPDLOG_TRACE("inflight: {} eventtime: {} ackEvent:{} ", downloadingmap.DebugInfo(),
                eventtime.ToDebuggingValue(), ackEvent.DebugInfo());
        /** RFC 9002 Section 6
         * */
        Duration maxrtt = std::max(rttStats.previous_srtt(), rttStats.latest_rtt());
        if (maxrtt == Duration::Zero())
        {
            SPDLOG_DEBUG(" {}", maxrtt == Duration::Zero());
            maxrtt = rttStats.SmoothedOrInitialRtt();
        }
        Duration loss_delay = maxrtt + (maxrtt * (5.0 / 4.0));
        loss_delay = std::max(loss_delay, Duration::FromMicroseconds(1));
        SPDLOG_TRACE(" maxrtt: {}, loss_delay: {}", maxrtt.ToDebuggingValue(), loss_delay.ToDebuggingValue());
        for (const auto& pkt_itor: downloadingmap.inflightPktMap)
        {
            const auto& pkt = pkt_itor.second;
            if (Timepoint(pkt.sendtic + loss_delay) <= eventtime)
            {
                losses.lossPackets.emplace_back(pkt);
            }
        }
        if (!losses.lossPackets.empty())
        {
            losses.losttic = eventtime;
            losses.valid = true;
            SPDLOG_DEBUG("losses: {}", losses.DebugInfo());
        }
    }

    ~DefaultLossDetectionAlgo() override
    {
    }

private:
};


class CongestionCtlAlgo
{
public:

    virtual ~CongestionCtlAlgo() = default;

    virtual CongestionCtlType GetCCtype() = 0;

    /////  Event
    virtual void OnDataSent(const InflightPacket& sentpkt) = 0;

    virtual void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) = 0;

    /////
    virtual uint32_t GetCWND() = 0;

//    virtual uint32_t GetFreeCWND() = 0;

};

/// config or setting for specific cc algo
/// used for pass parameters to CongestionCtlAlgo
struct CopaCongestionCtlConfig
{
    uint32_t minCwnd{ 1 };
    uint32_t maxCwnd{ 64 };
};

class CopaCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit CopaCongestionContrl(const CopaCongestionCtlConfig& ccConfig)
    {
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_minCwnd:{}, m_maxCwnd:{} ", m_minCwnd, m_maxCwnd);
    }

    ~CopaCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::copa;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            OnDataRecv(ackEvent, rttstats);
        }

    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

//    virtual uint32_t GetFreeCWND() = 0;

private:

    bool IsIncrease(RttStats& rttstats)
    {
        bool rt = false;
        QuicTime::Delta dq = m_rttStanding - rttstats.min_rtt();
        QuicTime::Delta a = m_cwnd * (1 / m_InverseDelta) * dq ;
        if (m_cwnd * (1 / m_InverseDelta) * dq <= m_rttStanding)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_DEBUG(" m_cwnd:{}, m_rttStanding(us):{}, dq(us):{}, IsIncrease:{}, direction:{}", m_cwnd, m_rttStanding.ToMicroseconds(), dq.ToMicroseconds(), rt, m_velocityState.direction);
        SPDLOG_DEBUG(" m_cwnd * 1/m_InverseDelta * dq:{}, m_rttStanding(us):{}", a.ToMicroseconds(), m_rttStanding.ToMicroseconds());
        return rt;
    }



    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        updateRttHistory(rttstats);

        if (!(IsIncrease(rttstats) && isSlowStart)) {
            // Update direction except for the case where we are in slow start mode,
            checkAndUpdateDirection(rttstats);
        }

        if (IsIncrease(rttstats))
        {
            if(isSlowStart)
            {
                if (lastCwndDoubleTime == QuicTime::Zero())
                {
                    lastCwndDoubleTime = clock.Now();
                }
                else if (clock.Now() - lastCwndDoubleTime >= rttstats.smoothed_rtt())
                {
                    m_cwnd = m_cwnd * 2;
                    m_cwnd = BoundCwnd(m_cwnd);
                    lastCwndDoubleTime = clock.Now();
                }
            }
            else
            {
                if(m_velocityState.direction != VelocityState::Direction::Up && m_velocityState.velocity > 1.0 )
                {
                    changeDirection(VelocityState::Direction::Up);
                }
                m_cwnd = m_cwnd + m_base*(uint32_t)((m_velocityState.velocity * m_InverseDelta) / m_cwnd);
                m_cwnd = BoundCwnd(m_cwnd);
            }

        }
        else
        {
            if(m_velocityState.direction != VelocityState::Direction::Down && m_velocityState.velocity > 1.0 )
            {
                changeDirection(VelocityState::Direction::Down);
            }
            m_cwnd = m_cwnd - m_base*(uint32_t)((m_velocityState.velocity * m_InverseDelta) / m_cwnd);
            m_cwnd = BoundCwnd(m_cwnd);
            isSlowStart = false;
        }


        SPDLOG_DEBUG("ackevent:{},m_cwnd:{}, velocity:{}, numTimesDirectionSame:{}",
                     ackEvent.DebugInfo(), m_cwnd, m_velocityState.velocity, m_velocityState.numTimesDirectionSame);

    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
    }

    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    double BoundVelocity(double trySetCwnd)
    {
        return std::min(m_maxVelocity, trySetCwnd);
    }

    void checkAndUpdateDirection(RttStats& rttstats_)
    {
        if (lastCwndRecordTime == QuicTime::Zero())
        {
            lastCwndRecordTime = clock.Now();
            lastCwnd = m_cwnd;
            return;
        }
        auto dir_elapsed_time = clock.Now() - lastCwndRecordTime;
        SPDLOG_DEBUG("elapsed time for direction update:{}, srtt:{}", dir_elapsed_time.ToMicroseconds(), rttstats_.smoothed_rtt().ToMicroseconds());
        if (dir_elapsed_time >= rttstats_.smoothed_rtt())
        {
            auto newDirection = m_cwnd > lastCwnd ? VelocityState::Direction::Up : VelocityState::Direction::Down;
            if (newDirection != m_velocityState.direction)
            {
                // if direction changes, change velocity to 1
                m_velocityState.velocity = 1;
                m_velocityState.numTimesDirectionSame = 0;
            }
            else
            {
                m_velocityState.numTimesDirectionSame++;
                uint32_t velocityDirectionThreshold = 3;
                if (m_velocityState.numTimesDirectionSame >= velocityDirectionThreshold)
                {
                    m_velocityState.velocity = 2 * m_velocityState.velocity;
                    m_velocityState.velocity = BoundVelocity(m_velocityState.velocity);
                }
            }
            m_velocityState.direction = newDirection;
            lastCwnd = m_cwnd;
            lastCwndRecordTime = clock.Now();
        }
    }

    struct VelocityState {
        double velocity{1};
        enum Direction {
            None,
            Up, // cwnd is increasing
            Down, // cwnd is decreasing
        };
        Direction direction{None};
        // number of rtts direction has remained same
        uint64_t numTimesDirectionSame{0};
    };

    void changeDirection(VelocityState::Direction newDirection)
    {
        if (m_velocityState.direction == newDirection)
        {
            return;
        }
        SPDLOG_DEBUG("change direction to:{}", newDirection);
        m_velocityState.direction = newDirection;
        m_velocityState.velocity = 1;
        m_velocityState.numTimesDirectionSame = 0;
        lastCwndRecordTime = clock.Now();
        lastCwnd = m_cwnd;
    }

    uint32_t  m_base{ 32 };
    DefaultClock clock;
    uint32_t m_cwnd{ 1 };
    uint32_t lastCwnd{ 0 };

    bool isSlowStart{ true };

    QuicTime lastCwndRecordTime = QuicTime::Zero();
    QuicTime lastCwndDoubleTime = QuicTime::Zero();

    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 2048 };

    double m_maxVelocity{ 2048 };

    double m_InverseDelta{ 2 };

    VelocityState m_velocityState;

    struct RttNode {
        QuicTime::Delta rtt;
        QuicTime updateTime;
    };

    QuicTime::Delta m_rttStanding = QuicTime::Delta::Zero();
    std::list<RttNode> m_RttHistory;

    QuicTime::Delta getMinRtt(std::list<RttNode> RttHistory) {
        QuicTime::Delta minRtt = RttHistory.begin()->rtt;
        for (auto it = RttHistory.begin(); it != RttHistory.end(); it++) {
            if (it->rtt < minRtt) {
                minRtt = it->rtt;
            }
        }
        return minRtt;
    }

    void updateRttHistory(RttStats& rttstats) {
        RttNode node = {rttstats.latest_rtt(), rttstats.last_update_time()};
        m_RttHistory.push_back(node);
        while (2*(m_RttHistory.back().updateTime - m_RttHistory.front().updateTime) > rttstats.smoothed_rtt()) {
            m_RttHistory.pop_front();
        }
        m_rttStanding = getMinRtt(m_RttHistory);
    }


};
