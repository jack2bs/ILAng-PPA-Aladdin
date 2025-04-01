
#include "ilang/ppa-estimations/ppa_hardware_block.h"
#include "ilang/ppa-estimations/ppa_profile_base.h"
#include "ilang/util/log.h"
#include <ilang/ppa-estimations/ppa_model_registrar.h>
#include <ilang/ppa-estimations/ppa_profile_const_example.h>
#include <ilang/ppa-estimations/ppa_profile_simple_example.h>
#include <memory>
#include <climits>
#include <ilang/ppa-estimations/ppa_callbacks.h>

namespace ilang 
{

typedef PPAProfile_Const_Example constProf;
typedef std::shared_ptr<PPAProfile_Const_Example> constProf_ptr;

typedef PPAProfile_Simple_Example simpleProf;
typedef std::shared_ptr<PPAProfile_Simple_Example> simpleProf_ptr;

void registerAllModels(PPA_Registrar & registrar)
{
    ILA_WARN << "RegisterAllModels was not overwritten, defaulting to using `defaultRegisterAllModels`";
    defaultRegisterAllModels(registrar);
}

void defaultRegisterAllModels(PPA_Registrar & registrar)
{

    constProf_ptr shifter_32b {
        std::make_shared<constProf>(74, 17331.483, 2270.682, 64.684, 32, 400, AcceleratorWide, 2)
    };

    constProf_ptr adder_32b {
        std::make_shared<constProf>(302, 6711.251, 942.731, 36.913, 32, 400, AcceleratorWide, 2)
    };

    constProf_ptr divider_32b {
        std::make_shared<constProf>(5356, 189528.282, 26600.057, 804.422, 32, 1, AcceleratorWide, 2)
    };

    constProf_ptr multiplier_32b {
        std::make_shared<constProf>(355, 228240.874, 17879.126, 509.903, 32, 30, AcceleratorWide, 2)
    };

    constProf_ptr remainder_32b { 
        std::make_shared<constProf>(5612, 203004.908, 27450.345, 833.176, 32, 1, AcceleratorWide, 2)
    };

    simpleProf_ptr shifter_32 {
        std::make_shared<simpleProf>(69, 205.67323147552182, 244.15774916682975, 8.602, 32, INT_MAX, AcceleratorWide, 2)
    };

    registrar.registerProfile(shifter_32b, bShifter);
    registrar.registerProfile(adder_32b, bAddition);
    registrar.registerProfile(divider_32b, bDivision);
    registrar.registerProfile(multiplier_32b, bMultiplication);
    registrar.registerProfile(remainder_32b, bRemainder);

    class ProfRegisterHold : public PPAProfile_Base
    {
    private:
        const int m_bitwidth;

    public:
        ProfRegisterHold(int bitwidth) : m_bitwidth(bitwidth){}

        double getBlockTime() override { return 0.0; }

        virtual double getBlockDynamicPower() override { return 0.0; }

        virtual double getBlockLeakagePower() override 
            { return 47.339 * m_bitwidth; }

        virtual double getBlockArea() override 
            { return 1.278 * m_bitwidth; }

        virtual int getMaximumBitwidth() override { return m_bitwidth; }

        virtual int getMaximumInstances() override { return INT_MAX; }

        virtual reuse_t getIsReusable() override { return None; }

        virtual int getNumInputs() override { return 1; }

    };



    class ProfRegisterRW : public PPAProfile_Base
    {
    private:
        const double m_dynPowerConst;
        const int m_bitwidth;

    public:
        ProfRegisterRW(double dynPowerConst, int bitwidth) 
            : m_dynPowerConst(dynPowerConst), m_bitwidth(bitwidth){}

        double getBlockTime() override { return 0.0; }

        virtual double getBlockDynamicPower() override 
        { 
            double switches = PPA_Callbacks::getSwitchingActivity();
            return switches * m_dynPowerConst;
        }

        virtual double getBlockLeakagePower() override { return 0.0; }

        virtual double getBlockArea() override { return 0.0; }

        virtual int getMaximumBitwidth() override { return m_bitwidth; }

        virtual int getMaximumInstances() override { return INT_MAX; }

        virtual reuse_t getIsReusable() override { return None; }

        virtual int getNumInputs() override { return 1; }

    };

    /* TODO : These numbers are approximates but totally fake! */
    class ProfMux2to1 : public PPAProfile_Base
    {
    private:
        const double m_dynPowerConst;
        const int m_bitwidth;

    public:
        ProfMux2to1(double dynPowerConst, int bitwidth)
            : m_dynPowerConst(dynPowerConst), m_bitwidth(bitwidth){}
        
        virtual double getBlockTime() override { return (m_bitwidth * 0.8125) + 6; }

        virtual double getBlockArea() override { return m_bitwidth * 0.5; }

        virtual double getBlockLeakagePower() override { return m_bitwidth * 19.525 + 7.702; }

        virtual double getBlockDynamicPower() override 
        { 
            double switches = PPA_Callbacks::getSwitchingActivity();
            return switches * m_dynPowerConst;
        }

        virtual int getMaximumBitwidth() override { return m_bitwidth; }

        virtual int getMaximumInstances() override { return INT_MAX; }

        virtual reuse_t getIsReusable() override { return None; }

        virtual int getNumInputs() override { return 3; }

    };

    class ProfBitwiseAnd : public PPAProfile_Base
    {
    private:
        const int m_bitwidth;

    public:
        ProfBitwiseAnd(int bitwidth)
            : m_bitwidth(bitwidth){}
        
        virtual double getBlockTime() override { return 3; }

        virtual double getBlockArea() override { return m_bitwidth * 0.295; }

        virtual double getBlockLeakagePower() override { return m_bitwidth * 15.421; }

        virtual double getBlockDynamicPower() override 
        { 
            double switches = PPA_Callbacks::getSwitchingActivity();
            return switches * 50.367 * m_bitwidth;
        }

        virtual int getMaximumBitwidth() override { return m_bitwidth; }

        virtual int getMaximumInstances() override { return INT_MAX; }

        virtual reuse_t getIsReusable() override { return None; }

        virtual int getNumInputs() override { return 1; }

    };

    for (int i = 1; i <= 256; i++)
    {
        std::shared_ptr<ProfRegisterRW> register_read =
            std::make_shared<ProfRegisterRW>((404.466 + 19.181) * i, i);
        std::shared_ptr<ProfRegisterRW> register_write =
            std::make_shared<ProfRegisterRW>((404.466 + 19.181) * i, i);
        std::shared_ptr<ProfRegisterHold> register_hold =
            std::make_shared<ProfRegisterHold>(i);
        std::shared_ptr<ProfMux2to1> mux =
            std::make_shared<ProfMux2to1>(i * (58.22), i);
        std::shared_ptr<ProfBitwiseAnd> bit =
            std::make_shared<ProfBitwiseAnd>(i);

        registrar.registerProfile(register_read, bRegisterRead);
        registrar.registerProfile(register_write, bRegisterWrite);
        registrar.registerProfile(register_hold, bRegisterHold);
        registrar.registerProfile(mux, bMultiplexer);
        registrar.registerProfile(bit, bBitwise);
    }

    registrar.finalizeRegistrar();
}


}