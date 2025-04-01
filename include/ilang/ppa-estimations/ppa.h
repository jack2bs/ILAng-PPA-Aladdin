/// \file
/// 

#ifndef ILANG_ILA_PPA_ESTIMATIONS_PPA_H__
#define ILANG_ILA_PPA_ESTIMATIONS_PPA_H__

#include <ilang/ila/instr_lvl_abs.h>
#include <ilang/util/log.h>
#include <ilang/ila/ast/expr.h>
#include <ilang/ila-mngr/u_abs_knob.h>
#include <unordered_map>
#include <vector>
#include "ilang/ila/ast/expr_op.h"
#include "ilang/ilang++.h"
#include "ppa_hardware_block.h"
#include "ppa_model_registrar.h"
#include <vcdparser/VCDFileParser.hpp>

namespace ilang {

class PPAAnalyzer {

public:

    struct PPAAnalyzerConfig
    {
        /* Should pipelined operations be required to start at a cycle boundary
         * (this includes all operations that get scheduled across cycle 
         * boundaries, which is to say, anything that would need to be 
         * pipelined if not rescheduled, see readme for examples)
         * default = True */
        bool pipelineStartAtCycleBoundary = true;

        /* Should short operations be allowed to span multiple cycles (has no 
         * effect when pipelineStartAtCycleBoundary = true) default = False */
        bool pipelineShortOperations = false;

        /* What should the threshold be for a 'short' operation (<= 1.0)
         * default = 1.0 */
        double shortOperationLengthThreshold = 1.0;

        /* The default number of ports on the memory */
        int defaultNumMemoryPorts = 2;

        /* Should operations be pushed back to even out the number of blocks
         * being used? */
        bool pushBackToEqualize = true;

        /* How should implicit register count be estimated? */
        enum regCountMethods 
        {
            /* Not recommended, massive undercount */
            CountSlowHardWareBlocks,

            /* Can be inaccurate for off critical path times */
            CountCarriedOverCycleBoundaries
        } regCountMethod = CountSlowHardWareBlocks;

        /* Not recommended, significantly overestimates the # of registers */
        bool PutConstantsInRegisters = true;
        bool PutInputsInRegisters = true;

        /* If the instruction sequence given uses an instruction which is 
         * instantiated in hardware multiple times, should each instantiation
         * be used. If false, then in order to meaningfully use an instruction
         * sequence, no instruction can be instantiated twice. */
        bool instrSequenceRunEachInstance = true;

    };

    PPAAnalyzer
    (
        const InstrLvlAbsPtr & ila,
        IlaModule & ilaMod,
        double cycle_time,
        const std::string & instr_seq_path,
        const std::string & vcd_path
    );

    PPAAnalyzer
    (
        const InstrLvlAbsPtr & ila,
        IlaModule & ilaMod,
        double cycle_time,
        const std::string & instr_seq_path,
        const std::string & vcd_path,
        PPAAnalyzerConfig * config
    );


    // PPAAnalyzer
    // (
    //     const IlaModule & ilaMod,
    //     double cycle_time,
    //     const std::string & instr_seq_path,
    //     const std::string & vcd_path
    // );


    void PPAAnalyze();

private:

    typedef std::array<std::vector<int>, bNumBlockTypes> UseTracker_t;
    struct PPAAnalysisData
    {
        PPAAnalysisData() : m_latestTime(0) {};

        // The time when each expression is finished in the current analysis
        std::unordered_map<uint64_t, double> m_endTimes;

        // The time when each expression begins in the current analysis
        std::unordered_map<ExprPtr, double> m_startTimes;
        
        // The time when each expression is ready in the current analysis
        std::unordered_map<ExprPtr, double> m_readyTimes;

        // Contains the expression pointers which have been put in registers
        std::unordered_map<ExprPtr, int> m_inRegister;

        // The hardware profile mapped to each expression
        std::unordered_map<ExprPtr, PPAProfile_ptr> m_profiles;

        // Maintain a list of parents for each expression
        std::unordered_map<ExprPtr, std::unique_ptr<std::vector<ExprPtr>>> 
            m_parents;

        // Maintain a list of expressions without parents
        std::unordered_set<ExprPtr> m_topExpressions;

        // Maps each memory expression to it's underlying memory partition's 
        // timing tracker 
        std::unordered_map<ExprPtr, std::shared_ptr<std::vector<int>>> 
            m_exprToMemUseByCycle;

        // Tracks the largest value of any expression in the m_endTimes map
        double m_latestTime;
        int m_latestTimeInCycles;

        // Tracks the average dynamic power per cycle
        double m_dynPower;

        // Workload specific active cycles
        int m_activeCycles = 0;

        // Tracks how many of each block have been used each cycle to enforce
        // configuration maximums
        std::vector<std::vector<int>> m_hardwareUseTracker;

        // Tracker for dynamic programming for the HasLoadFromStore check
        std::unordered_set<const ExprPtr *> m_hasLoadFromStoreVisited;

    };
    typedef std::unique_ptr<PPAAnalysisData> PPAAnalysisData_ptr;

    /* Estimates the performance of `expr`, which updates the optional 
     * parameter `exprToUpdate`, and uses the structures in `ppa_time_dat`, 
     * which should be reset outside of this function when applicable 
     * (switching between non-simultaneous expressions) */
    void PerformanceGet
    (
        const ExprPtr & expr,
        PPAAnalysisData & ppa_time_dat,
        const std::string & exprToUpdate = ""
    );

    /* Returns the time `expr` can begin execution. `maximumArgReadyTime` is 
     * the earliest time at which all args for `expr` can be ready. Uses the 
     * structures in `ppa_time_dat`. */
    double FirstSchedule
    (
        const ExprPtr & expr, 
        double maximumArgReadyTime,
        PPAAnalysisData & ppa_time_dat
    );

    /* The helper function which actually moves the hw blocks for 
     * `PushExpressionsLater`. */
    void MoveLater
    (
        const ExprPtr & expr,
        double exprStartTime,
        PPAAnalysisData & ppaData
    );

    /* Reschedules operations to more evenly fill out the time. This is 
     * necessary to avoid overestimating the number of a given block required
     * at the beginning a schedule, when the bottleneck occurs later in the 
     * datapath. */
    void PushExpressionsLater(PPAAnalysisData & ppaData);

    /* Counts the number of explicit registers required for implementing the
     * accelerator's state and inputs. Inputs are only buffered into 
     * architectural registers at the request of the user in the configuration. 
     */
    void CountStateAndInputRegisters();

    /* Counts the number of implicit registers required for implementing
     * the schedule stored in `ppaData`. These implicit registers are required
     * any time data crosses over a cycle boundary. */
    void CountRegistersSpanning
    (
        const ExprPtr & expr,
        PPAAnalysisData & ppaData
    );

    /* Extracts the number of each hardware block required by the schedule
     * in `ppaData` at the time of calling. Updates the PPA profiles based on
     * the information. */
    void CountHardwareBlocks(PPAAnalysisData & ppaData);

    /* Initializes `ppa_time_dat` with tracking vectors for all memory states
     * and resizes the hardware block tracking vector with the number of 
     * profiles in the registrar*/
    void AnalysisDataInitialize(PPAAnalysisData & ppa_time_dat);

    /* Cleans up the tracking vectors in `ppa_time_dat` */
    // Currently DELETED
    // void AnalysisDataDelete(PPAAnalysisData & ppa_time_dat);

    /* Removes duplicates from `topExpr` according to the list in `set`. 
     * `CheckedMap` is used as a visited list (dynamic programming) to speed up
     * the code */
    const ExprPtr * RemoveDuplicates
    (
        const ExprPtr & topExpr, 
        std::unordered_map<uint64_t, const ExprPtr> & set,
        std::unordered_map<uint64_t, const ExprPtr> & checkedMap
    );

    /* Returns the type of hardware block for expr */
    HardwareBlock_t ExprToHardwareBlocks(const ExprPtr & expr);


    // TODO : downgrading operations with constant operands
    /* Returns the type of hardware block which implements operation `op` */
    HardwareBlock_t UidToHardwareBlock(AstUidExprOp op);

    /* Prints the hardware blocks which are being used (debugging, big 
     * performance hit) */
    void PrintHardwareBlocks
    (
        PPAAnalysisData & ppaData,
        const std::string & label,
        int numcycles = -1
    );

    /* Fills in m_instrSequence based on data in file : m_instrSeqPath 
     * returns true if m_instrSeqPath could be opened, false otherwise */
    bool MakeInstrSequence();

    /* Count the  # of registers if regCountMethod == CountSlowHardwareBlocks
     * by counting the number of instances of adders, shifters, multipliers,
     * dividers, remainderers, and the number of memory ports */
    void RegCountSlowHwBlocks();

    /* Creates a VCDFile object with the file in m_vcdPath. Stores the
     * VCDFile object in m_vcdStatistics. Fills m_refToHash with the hashes for
     * each signal in the VCD file. */
    bool MakeVcd();

    /* Estimates the number of multiplexers required by the reused hardware 
     * blocks. Counts are stored in the multiplexer PPA profiles */
    void CountMultiplexers();

    /* Use the data to generate the final PPA estimates for the accelerator. */
    void FinalizeEstimates
    (
        std::unordered_map<std::string, PPAAnalysisData_ptr> & ppaData,
        bool hadInstrSeq,
        bool hadVcd
    );

    const int m_countBlockTypes = HardwareBlock_t::bNumBlockTypes;
    const InstrLvlAbsPtr & m_ila;
    IlaModule & m_ilaMod;
    const double m_cycleTime;
    std::set<ExprPtr> m_constMems;
    PPAAnalyzerConfig m_configuration;
    PPA_Registrar m_registrar;
    const std::string & m_instrSeqPath;
    std::vector<std::string> m_instrSequence;
    const std::string & m_vcdPath;
    VCDFile * m_vcdStatistics;
    std::unordered_map<VCDSignalReference, VCDSignalHash*> m_refToHash;
    std::vector<ExprPtr> m_memVars;




};

}

#endif//ILANG_ILA_PPA_ESTIMATIONS_PPA_H__