/// \file
///

#ifndef ILANG_ILA_PPA_ESTIMATIONS_PPA_CALLBACKS_H_
#define ILANG_ILA_PPA_ESTIMATIONS_PPA_CALLBACKS_H_

// #include "ilang/ppa-estimations/ppa.h"

namespace ilang
{

class PPA_Callbacks
{

private:

    static int s_m_bitwidth;
    static double s_m_currSwitchingActivity;

public:

    /* Returns the operand bitwidth */
    static int getBitwidth() { return s_m_bitwidth; }

    /* For dynamic power checks, returns the switching activity between 
     * 0 and 1, which is the probability that an input operand's bit flips
     * during the operation. Value is garbage for other checks */
    static int getSwitchingActivity() { return s_m_currSwitchingActivity; }

    friend class PPAAnalyzer;
};

}



#endif//ILANG_ILA_PPA_ESTIMATIONS_PPA_CALLBACKS_H_