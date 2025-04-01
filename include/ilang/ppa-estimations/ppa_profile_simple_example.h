#ifndef ILANG_ILA_PPA_ESTIMATIONS_PPA_PROFILE_SIMPLE_EXAMPLE_H__
#define ILANG_ILA_PPA_ESTIMATIONS_PPA_PROFILE_SIMPLE_EXAMPLE_H__

#include "ilang/ppa-estimations/ppa_callbacks.h"
#include "ppa_profile_base.h"
// #include "../ilang++.h"


namespace ilang {

class PPAProfile_Simple_Example : public PPAProfile_Base
{
private:
    double m_blockTime;
    double m_blockDynamicPower;
    double m_blockLeakagePower;
    double m_blockArea;
    int m_maximumBitwidth;
    int m_maximumInstances;
    reuse_t m_isReusable;
    int m_numInputs;

public:
    PPAProfile_Simple_Example
    (
        double blockTime,
        double blockDynamicPower,
        double blockLeakagePower,
        double blockArea,
        int maximumBitwidth,
        int maximumInstances,
        reuse_t isReusable,
        int numInputs
    ) : 
    m_blockTime(blockTime),
    m_blockDynamicPower(blockDynamicPower),
    m_blockLeakagePower(blockLeakagePower),
    m_blockArea(blockArea),
    m_maximumBitwidth(maximumBitwidth),
    m_maximumInstances(maximumInstances),
    m_isReusable(isReusable),
    m_numInputs(numInputs)
    {};

    double getBlockTime() override { return m_blockTime; }
    double getBlockDynamicPower() override { return m_blockDynamicPower * PPA_Callbacks::getSwitchingActivity(); }
    double getBlockLeakagePower() override { return m_blockLeakagePower; }
    double getBlockArea() override { return m_blockArea; }
    int getMaximumBitwidth() override { return m_maximumBitwidth; }
    int getMaximumInstances() override { return m_maximumInstances; }
    reuse_t getIsReusable() override { return m_isReusable; }
    int getNumInputs() override { return m_numInputs; }

};

}

#endif//ILANG_ILA_PPA_ESTIMATIONS_PPA_PROFILE_SIMPLE_EXAMPLE_H__
