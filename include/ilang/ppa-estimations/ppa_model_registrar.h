
#ifndef ILANG_ILA_PPA_ESTIMATIONS_PPA_MODEL_REGISTRAR_H__
#define ILANG_ILA_PPA_ESTIMATIONS_PPA_MODEL_REGISTRAR_H__

#include <unordered_map>
#include <vector>
#include <array>
#include "ppa_profile_base.h"
#include "ppa_hardware_block.h"
#include <ilang/util/log.h>
#include <string.h>

namespace ilang
{

class Func;
class FuncRef;
typedef std::shared_ptr<Func> FuncPtr;

typedef std::array<std::vector<PPAProfile_ptr>, 
        bNumBlockTypes> RegistrarType;
        
class PPA_Registrar
{
private:

    bool currentlySorted = false;

    std::unordered_map<std::string, PPAProfile_ptr> uninterpretedFuncMap;

    RegistrarType m_registeredProfiles;

    std::vector<std::pair<PPAProfile_ptr, HardwareBlock_t>>
         m_registeredProfilesByGlobalIndex;

public:

    PPA_Registrar();

    /* Register newProfile as a usable PPA profile for opType operations for 
     * this accelerator. This essentially adds it to the vector of profiles
     * available for the opType*/
    void registerProfile
    (
        const PPAProfile_ptr & newProfile,
        HardwareBlock_t opType
    );

    PPAProfile_ptr profileFromIndex(int index);
    HardwareBlock_t blockTypeFromIndex(int index);


    /* Inform the program that the registrar has been finalized. The registrar 
     * must be finalized before other code is run. Profiles can still be added 
     * later, after finalization, but the registrar should be finalized again
     * before any other code is executed */
    void finalizeRegistrar();

    /* Get the current size of the registrar */
    int getSize(); 

    /* Returns the profile for the opType which has the lowest max bitwidth 
     * while still having a bitwidth greater than the bitwidth argument */
    PPAProfile_ptr getMatchingProfile_LowestBitwidth
    (
        HardwareBlock_t opType,
        int bitwidth
    );

    /* Returns the uninterpreted function profile matching the function */
    PPAProfile_ptr getMatchingProfile_UninterpFunc(FuncPtr func);

    /* USE THIS INSTEAD OF `registerProfile()` FOR ANY UNINTERPRETED FUNCTION
     * Register newProfile as the PPA profile which models f, each time f is
     * executed (calls `registerProfile()` internally). */
    void registerFuncWithPPAModel
    (
        FuncRef & f,
        const PPAProfile_ptr & newProfile
    );

    RegistrarType * getRegisteredProfiles();

};


/* Default version of the registerAllModels function which uses the OpenPDK
 * models to create a set of PPAProfiles. */
void defaultRegisterAllModels(PPA_Registrar & registrar);

// This is just the forward declaration
/* Function called at start of code, which is written by the user for custom 
 * or use the ILAng provided version for the premade models. Defines and 
 * registers all PPA profiles */
void registerAllModels(PPA_Registrar & registrar);

}

#endif//ILANG_ILA_PPA_ESTIMATIONS_PPA_MODEL_REGISTRAR_H__