#include "ilang/ilang++.h"
#include "ilang/ppa-estimations/ppa_model_registrar.h"
#include <ilang/ppa-estimations/ppa_hardware_block.h>

namespace ilang
{

std::string hardwareBlockToString(HardwareBlock_t op)
{
    std::string names [HardwareBlock_t::bNumBlockTypes] {
        "no hardware",
        "bitwise operation",
        "Shift operation",
        "+ (adder type)",
        "* (multiplier type)",
        "/ (division type)",
        "Memory operation",
        "Remainder operation",
        "Reg Hold",
        "Reg write",
        "Reg read",
        "Multiplexer",
        "UninterpFunc"
    };
    return names[op];
}

}