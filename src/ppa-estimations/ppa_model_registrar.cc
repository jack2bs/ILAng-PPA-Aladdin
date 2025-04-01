#include "ilang/ppa-estimations/ppa_hardware_block.h"
#include "ilang/ppa-estimations/ppa_profile_base.h"
#include "ilang/ppa-estimations/ppa_profile_const_example.h"
#include <ilang/ppa-estimations/ppa_model_registrar.h>
#include <ilang/ilang++.h>
#include <memory>
#include <limits.h>
#include <algorithm>
#include <ilang/ila/ast/func.h>

namespace ilang
{

PPA_Registrar::PPA_Registrar()
{
    typedef PPAProfile_Const_Example constProf;
    typedef std::shared_ptr<PPAProfile_Const_Example> constProf_ptr;   
    
    constProf_ptr noHardwareProf {
        std::make_shared<constProf>(
            0, 0, 0, 0, INT_MAX, -1, None, 0
        )
    };

    registerProfile(noHardwareProf, bNoHardware);

    /* For now we don't model the memory use power */
    constProf_ptr memoryProf {
        std::make_shared<constProf>(
            0, 0, 0, 0, INT_MAX, -1, AcceleratorWide, 1
        )
    };

    registerProfile(memoryProf, bMemory);

}

/*****************************************************************************/

void PPA_Registrar::registerProfile
(
    const PPAProfile_ptr & newProfile,
    HardwareBlock_t opType
)
{

    // if (opType == bApplyFunc)
    // {
    //     ILA_ERROR << "Func opType being registered with `registerProfile()` instead of `registerFuncProfile()`";
    //     return;
    // }

    currentlySorted = false;

    m_registeredProfiles.at(opType).push_back(newProfile);

    m_registeredProfilesByGlobalIndex.push_back({newProfile, opType});
    newProfile->setGlobalIndex(m_registeredProfilesByGlobalIndex.size()-1);
}

/*****************************************************************************/

PPAProfile_ptr PPA_Registrar::profileFromIndex(int index)
{
    return m_registeredProfilesByGlobalIndex.at(index).first;
}

/*****************************************************************************/

HardwareBlock_t PPA_Registrar::blockTypeFromIndex(int index)
{
    return m_registeredProfilesByGlobalIndex.at(index).second;
}

/*****************************************************************************/

bool sortComparator(const PPAProfile_ptr & profA, const PPAProfile_ptr & profB)
{
    return profA->getMaximumBitwidth() < profB->getMaximumBitwidth();
}

/*****************************************************************************/

void PPA_Registrar::finalizeRegistrar()
{
    for (std::vector<PPAProfile_ptr> & prof : m_registeredProfiles)
    {
        std::sort(prof.begin(), prof.end(), sortComparator);
    }

    currentlySorted = true;
}

/*****************************************************************************/

int PPA_Registrar::getSize()
{
    return m_registeredProfilesByGlobalIndex.size();
}

/*****************************************************************************/

PPAProfile_ptr PPA_Registrar::getMatchingProfile_LowestBitwidth
(
    HardwareBlock_t opType,
    int bitwidth
)
{
    // for lists of the size we expect here, linear search will be faster
    // than binary search
    for (PPAProfile_ptr & prof : m_registeredProfiles.at(opType))
    {
        if (prof->getMaximumBitwidth() >= bitwidth)
        {
            return prof;
        }
    }

    ILA_ASSERT(false) << "No profile can handle " << hardwareBlockToString(opType)
        << " with a bitwidth = " << bitwidth;

    // Not reachable without error
    return *(m_registeredProfiles.at(opType).end());
}

/*****************************************************************************/

PPAProfile_ptr PPA_Registrar::getMatchingProfile_UninterpFunc(FuncPtr func)
{
    auto found = uninterpretedFuncMap.find(func->name().str());
    if (found == uninterpretedFuncMap.end())
    {
        ILA_ERROR << "No profile registered for func: " << func->name();
        return nullptr;
    }
    return found->second;
}

/*****************************************************************************/

RegistrarType * PPA_Registrar::getRegisteredProfiles()
{
    return &m_registeredProfiles;
}

/*****************************************************************************/

void PPA_Registrar::registerFuncWithPPAModel
(
    FuncRef & f,
    const PPAProfile_ptr & newProfile
)
{
    uninterpretedFuncMap.insert({f.get()->name().str(), newProfile});
    this->registerProfile(newProfile, bApplyFunc);
}

}