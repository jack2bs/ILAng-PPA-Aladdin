
#ifndef ILANG_ILA_PPA_ESTIMATIONS_PPA_HARDWARE_BLOCK_H__
#define ILANG_ILA_PPA_ESTIMATIONS_PPA_HARDWARE_BLOCK_H__

#include <string>

namespace ilang
{


// TODO: Add constant shifters
enum HardwareBlock_t {
        bNoHardware,
        bBitwise,
        bShifter,
        bAddition,
        bMultiplication,
        bDivision,
        bMemory,
        bRemainder,
        bRegisterHold,
        bRegisterWrite,
        bRegisterRead,
        bMultiplexer,
        bApplyFunc,
        bNumBlockTypes,
        bInvalid
};

/* Returns a string representation of op */
std::string hardwareBlockToString(HardwareBlock_t op);



}

#endif//ILANG_ILA_PPA_ESTIMATIONS_PPA_HARDWARE_BLOCK__
