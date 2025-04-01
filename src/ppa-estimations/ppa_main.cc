/// \file
/// Implementation of the class PPAAnalyzer

#include "VCDParser.hpp"
#include "ilang/ila-mngr/u_abs_knob.h"
#include "ilang/ila/ast/expr.h"
#include "ilang/ila/ast/expr_op.h"
#include "ilang/ila/ast/sort_value.h"
#include "ilang/ila/ast_hub.h"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <ilang/ppa-estimations/ppa.h>
#include <memory>
#include <unordered_map>

// #include "ilang/ilang++.h"
#include "ilang/ppa-estimations/ppa_callbacks.h"
#include "ilang/ppa-estimations/ppa_hardware_block.h"
#include "ilang/ppa-estimations/ppa_profile_base.h"

/// \namespace ilang
namespace ilang {



/*****************************************************************************/

void PPAAnalyzer::CountHardwareBlocks(PPAAnalysisData & ppaData)
{
    /* Methodology: for each registered profile, grab the hardware use tracker
     * which is filled in originally by the combo of `PerformanceGet` and
     * `FirstSchedule` and then made more accurate by the combo of
     * `PushExpressionsLater` and `MoveLater`. 
     * 
     * For each tracker, go through, cycle by cycle, and based on the profiles
     * reusability, modify the number of instances of the PPA Profile, and
     * increment it's number of unique uses. */

    for (const auto & profVec : *m_registrar.getRegisteredProfiles())
    {
        for (const PPAProfile_ptr & prof : profVec)
        {
            int profIndex = prof->getGlobalIndex();

            const std::vector<int> & vec = 
                ppaData.m_hardwareUseTracker.at(profIndex);

            reuse_t isReusable = prof->getIsReusable();

            if (isReusable == AcceleratorWide)
            {
                /* If the blocks can be reused AcceleratorWide, then we need
                 * to instantiate the maximum required by any single cycle in 
                 * any instruction (or the flattened fetch valid decode). To do
                 * this, we check the existing maximum, and if it's lower than
                 * used this cycle, update it to be the amount in this cycle.
                 * We also increment the number of unique uses. Later this is 
                 * used to estimate mux requirements. */

                for (int cycle = 0; cycle < vec.size(); cycle++)
                {
                    if (prof->getNumInstances() < vec.at(cycle))
                    {
                        prof->setNumInstances(vec.at(cycle));
                    }
                    prof->incUniqueUses(vec.at(cycle));
                }
            }
            else if (isReusable == InstructionWide)
            {
                /* If the blocks can be reused InstructionWide, then we need
                 * to instantiate the maximum required by any single cycle in 
                 * this instruction. To do this, we track the maximum by a 
                 * single cycle of this instruction, and then increment the 
                 * number of instances by that amount.
                 * We also increment the number of unique uses. Later this is 
                 * used to estimate mux requirements. */

                int maxInstances = 0;
                for (int cycle = 0; cycle < vec.size(); cycle++)
                {
                    if (maxInstances < vec.at(cycle))
                    {
                        maxInstances = vec.at(cycle);
                    }
                    prof->incUniqueUses(vec.at(cycle));
                }
                prof->incNumInstances(maxInstances);
            }
            else// isReusable == None
            {
                /* If the blocks cannot be reused, then each use is a unique
                 * instance, so we increment the number of instances each time
                 * the block is used.
                 * For blocks that aren't reused, there is no need to 
                 * instantiate implicit muxes, so we don't count unique uses.
                 */

                for (int cycle = 0; cycle < vec.size(); cycle++)
                {
                    prof->incNumInstances(vec.at(cycle));
                }
            }
        }
    }
}

/*****************************************************************************/

void PPAAnalyzer::MoveLater
(
    const ExprPtr & expr,
    double latestAllowedEndTime,
    PPAAnalysisData & ppaData
)
{
    // See `PushExpressionsLater` for context

    /* Methodology: Take each operation, and move it as far back as possible,
     * without requiring the instantiation of more hardware than already 
     * exists. We go as far back as possible, not as evenly as possible, as
     * this leaves room for future operations which can't start as late to 
     * fill in gaps (this is necessary, as otherwise the operations will still
     * frontload). */

    // Non ops can't be rescheduled, as they're always ready at t == 0
    if (!expr->is_op()) 
    {
        return;
    }

    AstUidExprOp exprOp = asthub::GetUidExprOp(expr);

    PPAProfile_ptr prof = ppaData.m_profiles.at(expr);

    double opExecutionTime = 0.0;
    if (exprOp == kLoad)
    {
        opExecutionTime = m_cycleTime;
    }
    else if (exprOp == kStore)
    {
        opExecutionTime = 0.0;
    }
    else
    {
        opExecutionTime = prof->getBlockTime();
    }
    
    // Technically, this is not correct, as technically this could allow for 
    // unintended spanning cycle boundaries... but because we only allow one 
    // operation per block per cycle (block reuse within a single cycle is not
    // allowed), this is okay. This is because this variable will always be in
    // the same cycle as a more accurate version (so `latestAllowedStartCycle`
    // will always be correct)
    double latestAllowedStartTime = latestAllowedEndTime - opExecutionTime;

    int latestAllowedStartCycle = static_cast<int>(
        latestAllowedStartTime / m_cycleTime
    );

    double oldStartTime = ppaData.m_startTimes.at(expr);

    int oldStartCycle = static_cast<int>(oldStartTime / m_cycleTime);

    std::vector<int> & useTracker = 
        ((exprOp == kLoad || exprOp == kStore)
        ? *ppaData.m_exprToMemUseByCycle.at(expr)
        : ppaData.m_hardwareUseTracker.at(prof->getGlobalIndex()));

    // Avoid vector out of bounds errors
    if (useTracker.size() <= latestAllowedStartCycle)
    {
        useTracker.resize(latestAllowedStartCycle+1, 0);
    }

    int oldStartUses = useTracker.at(oldStartCycle);
    for (int i = latestAllowedStartCycle; i > oldStartCycle; i--)
    {
        // If there are less uses at the later time than the earlier time
        if (useTracker.at(i) < (oldStartUses))
        {
            useTracker.at(i)++;
            useTracker.at(oldStartCycle)--;

            double newStartTime = m_cycleTime * i;

            ppaData.m_startTimes[expr] = newStartTime;

            ppaData.m_endTimes.at(expr->name().id()) = 
                newStartTime + opExecutionTime;

            return;
        }
    }
}

/*****************************************************************************/

void PPAAnalyzer::PushExpressionsLater(PPAAnalysisData & ppaData)
{
    /* Context: A pipeline diagram of a loop of instructions will look like a 
     * staircase where, each iteration is in a different part of the loop. 
     * However, our first scheduling methodology only begins to form a 
     * staircase at the bottlenecking operation, as it schedules every 
     * operation as early as it can. This function moves operations later in 
     * order to make the staircase start from the op at the beginning of the 
     * loop, instead of the bottleneck, as this more efficiently distributes
     * resources. */

    /* Methodology: First, create a vector containing each instruction in a 
     * topological ordering such that every child of an ExprPtr has a lower 
     * index in the vector than the ExprPtr itself (these trees are acyclic).
     * Next traverse the vector from back to front (such that each ExprPtr
     * is operated on before each of it's children), moving it backwards as 
     * described by the methodology in `MoveLater`. */

    std::vector<ExprPtr> topologicalOrder;
    std::unordered_set<ExprPtr> topologicalVisited;

    // Create topological order
    auto func = 
    [&topologicalOrder, &topologicalVisited](const ExprPtr & e)
    {
        if (topologicalVisited.count(e))
        {
            return;
        }
        
        topologicalVisited.insert(e);
        topologicalOrder.emplace_back(e);
    };

    for (const ExprPtr & expr : ppaData.m_topExpressions) {
        expr->DepthFirstVisit(func);
    }

    // Traverse topology in reverse, calling `MoveLater`
    for (int i = topologicalOrder.size()-1; i >= 0; i--)
    {
        const ExprPtr & e = topologicalOrder.at(i);

        
        double latestAllowedEndTime = INFINITY;
        for (const ExprPtr & parent : *ppaData.m_parents.at(e))
        {
            double parentStart = ppaData.m_startTimes.at(parent);
            if (parentStart < latestAllowedEndTime)
            {
                latestAllowedEndTime = parentStart;
            }
        }
        if (latestAllowedEndTime == INFINITY)
        {
            continue;
        }

        MoveLater(e, latestAllowedEndTime, ppaData);
    }
}

/*****************************************************************************/

void PPAAnalyzer::CountStateAndInputRegisters()
{
    /* Methodology: For each variable in the state tree (and optionally the 
     * input tree) add another holding register instance. */


    for (const ExprPtr & var : absknob::GetSttTree(m_ila))
    {
        if (var->is_mem())
        {
            continue;
        }

        int bw = 0;
        if (var->is_bool())
        {
            bw = 1;
        }
        else//var->is_bv()
        {
            bw = var->sort()->bit_width();
        }

        m_registrar.getMatchingProfile_LowestBitwidth(bRegisterHold, bw)
            ->incNumInstances(1);
    }

    if (!m_configuration.PutInputsInRegisters)
    {
        return;
    }

    for (const ExprPtr & var : absknob::GetInpTree(m_ila))
    {
        if (var->is_mem())
        {
            continue;
        }

        int bw = 0;
        if (var->is_bool())
        {
            bw = 1;
        }
        else//var->is_bv()
        {
            bw = var->sort()->bit_width();
        }

        m_registrar.getMatchingProfile_LowestBitwidth(bRegisterHold, bw)
            ->incNumInstances(1);
    }
}

/*****************************************************************************/

void PPAAnalyzer::CountRegistersSpanning
(
    const ExprPtr & e,
    PPAAnalysisData & ppaData
)
{
    /* Extract registers - if data spans cycles then it needs to 
    * be put in a register. Writing to and reading from a register, 
    * for now, does not come with a delay. Delays specified in the 
    * profiles will be ignored. Registers do consume power and area. 
    * leakage power and area are specified in the hold profile, and 
    * dynamic power is specified in the read and write profiles. A 
    * maximum number of registers may not be specified, as the number
    * must be implied. */

    /* Methodology: for each child of `e`, if the child finishes before `e` 
     * starts execution, then it needs to be stored in a register until e 
     * starts. It needs to be held in registers until the last of it's parents
     * has begun execution, so if it gets revisited, and the revisit requires 
     * it for longer, then it's register lifetime is extended. This may not be
     * the most efficient structure for this, but operationally it's 
     * equivalent. */


    int startCycle = static_cast<int>(
        ppaData.m_startTimes.at(e) / m_cycleTime
    );

    int num_args = e->arg_num();
    for (int i = 0; i < num_args; i++)
    {
        CountRegistersSpanning(e->arg(i), ppaData);
        if (e->arg(i)->is_mem())
        {
            continue;
        }

        if (!m_configuration.PutConstantsInRegisters && e->arg(i)->is_const())
        {
            continue;
        }
        

        SortPtr srt = e->arg(i)->sort();
        int bitwidth = 
            srt->is_bool() ? 1 :
            srt->is_bv() ? srt->bit_width() :
            /*srt->is_mem() ?*/ srt->data_width();

        const PPAProfile_ptr & holdProf = m_registrar.
            getMatchingProfile_LowestBitwidth(bRegisterHold, bitwidth);
        int holdProfInd = holdProf->getGlobalIndex();
        
        const PPAProfile_ptr & readProf = m_registrar.
            getMatchingProfile_LowestBitwidth(bRegisterRead, bitwidth);
        int readProfInd = readProf->getGlobalIndex();

        const PPAProfile_ptr & writeProf = m_registrar.
            getMatchingProfile_LowestBitwidth(bRegisterWrite, bitwidth);
        int writeProfInd = writeProf->getGlobalIndex();


        int argReadyCycle = static_cast<size_t>
            (ppaData.m_endTimes.at(e->arg(i)->name().id()) / m_cycleTime);


        if (argReadyCycle != startCycle)
        {
            if (ppaData.m_inRegister.find(e->arg(i)) 
                == ppaData.m_inRegister.end())
            {
                std::vector<int> & writeVec = 
                    ppaData.m_hardwareUseTracker.at(writeProfInd);

                if (writeVec.size() <= argReadyCycle)
                {
                    writeVec.resize(argReadyCycle+1, 0);
                }

                int writes = writeVec.at(argReadyCycle)++;

                if (!writeProf->getIsReusable() 
                    || writeProf->getNumInstances() < writes)
                {
                    writeProf->incNumInstances(1);
                }

                ppaData.m_inRegister.insert({e->arg(i), 0});
            }

            if ((ppaData.m_inRegister.at(e->arg(i)) < startCycle )
                && (m_configuration.regCountMethod 
                == PPAAnalyzerConfig::CountCarriedOverCycleBoundaries))
            {

                int heldTill = std::max(
                    argReadyCycle, ppaData.m_inRegister.at(e->arg(i))
                );

                std::vector<int> & holdVec =
                    ppaData.m_hardwareUseTracker.at(holdProfInd);

                if (holdVec.size() <= startCycle)
                {
                    holdVec.resize(startCycle+1, 0);
                }

                for (; heldTill < startCycle; heldTill++)
                {
                    int holds = holdVec.at(heldTill)++;
                    
                    if (!holdProf->getIsReusable()
                        || holdProf->getNumInstances() < holds)
                    {
                        holdProf->incNumInstances(1);
                    }
                }

                ppaData.m_inRegister.at(e->arg(i)) = startCycle;
            }

            std::vector<int> & readVec =
                ppaData.m_hardwareUseTracker.at(readProfInd);

            if (readVec.size() <= startCycle)
            {
                readVec.resize(startCycle+1, 0);
            }

            int reads = readVec.at(startCycle)++;
            
            if (!readProf->getIsReusable()
                || readProf->getNumInstances() < reads)
            {
                readProf->incNumInstances(1);
            }
        }
    }
}

/*****************************************************************************/

// This function needs a refactor to remove all of the duplicate code.
// It's super doable just not worth doing yet since it won't actually come
// with a performance improvement, just a sloc one.

double PPAAnalyzer::FirstSchedule
(
    const ExprPtr & expr,
    double maximumArgReadyTime,
    PPAAnalysisData & ppaData
)
{
    /* The methodology below is extra conservative for memory `if_then_else` */
    AstUidExprOp exprOp = asthub::GetUidExprOp(expr);
    if (exprOp == kLoad || exprOp == kStore)
    {
        /* Methodology: Assume for now that each memory device can be written 
         * to or read from in one whole cycle, and that 
         * m_configuration->defaultNumMemoryPorts operations are permited per 
         * cycle. Track the memory usage as vectors of ints, stored in the 
         * PPAAnalysisData struct in `m_exprToMemUseByCycle`. Then find and use
         * the earliest available cycle after the operation is ready to execute
         */

        /* Possible performance issue: Lte and Gte operations aren't primitive,
         * they are implemented instead as (Gt | Eq), and (Lt | Eq) 
         * respectively. This leads to extra operations :( */

        /* Use the mem use tracker associated with the 0th argument which for
         * both loads and stores is the memory state they use */

        std::shared_ptr<std::vector<int>> & memUseTracker =
            ppaData.m_exprToMemUseByCycle.at(expr->arg(0));

        // Convert `maximumArgReadyTime` from time to cycles
        size_t firstCycleReady = static_cast<size_t>
            (maximumArgReadyTime / m_cycleTime);
        
        if (firstCycleReady >= memUseTracker->size())
        {
            memUseTracker->resize(firstCycleReady + 1, 0);
        }

        size_t cycleToCheck = firstCycleReady;
        while (memUseTracker->at(cycleToCheck) >= 
            m_configuration.defaultNumMemoryPorts)
        {
            cycleToCheck++;

            if (cycleToCheck >= memUseTracker->size())
            {
                memUseTracker->push_back(0);
                break;
            }
        }

        memUseTracker->at(cycleToCheck)++;

        // Check this later for off by 1 error (I don't think there's one)
        double startTime = ((cycleToCheck)  * m_cycleTime);
        return startTime;
    }

    /* Methodology: Track the hardware block usage as vectors of ints, stored 
    * in the PPAAnalysisData struct in `m_hardwareUseTracker`. Then find and 
    * use the earliest cycle after the operation is ready to execute, which 
    * isn't full. Fullness is based on whether the hardware block is reusable,
    * and the maximum number of the hardware block allowed.
    */

    HardwareBlock_t hbInd = ExprToHardwareBlocks(expr);

    const SortPtr & srt = expr->sort();
    int bitwidth = 
        srt->is_bool() ? 1 :
        srt->is_bv() ? srt->bit_width() :
        /*srt->is_mem() ?*/ srt->data_width();

    PPAProfile_ptr prof;
    if (hbInd == bApplyFunc)
    {
        
        ExprOpAppFunc & exprFunc = dynamic_cast<ExprOpAppFunc&>(*expr);
        const FuncPtr & fptr = exprFunc.func();
        prof = m_registrar.getMatchingProfile_UninterpFunc(fptr);
        
    }
    else
    {
        prof = m_registrar
            .getMatchingProfile_LowestBitwidth(hbInd, bitwidth);
    }



    int profileIndex = prof->getGlobalIndex();

    std::vector<int> & blockTracker = 
        ppaData.m_hardwareUseTracker.at(profileIndex);

    int maxBlocksPerCycle = prof->getMaximumInstances();

    // Convert `maximumArgReadyTime` from time to cycles
    size_t firstCycleReady = static_cast<size_t>
        (maximumArgReadyTime / m_cycleTime);

    while (firstCycleReady >= blockTracker.size())
    {
        blockTracker.push_back(0);
    }

    size_t cycleToCheck = firstCycleReady;

    double startTime = maximumArgReadyTime;

    if (prof->getIsReusable())
    {
        // While the cycle being checked is full
        while (blockTracker.at(cycleToCheck) >= maxBlocksPerCycle)
        {
            cycleToCheck++;

            if (cycleToCheck >= blockTracker.size())
            {
                blockTracker.push_back(0);
                break;
            }
        }
    }

    blockTracker.at(cycleToCheck)++;
    startTime = fmax(cycleToCheck * m_cycleTime, startTime);

    return startTime;
}

/*****************************************************************************/

/* Adapted from ilator.cc - added dynamic programming :) */
static bool HasLoadFromStore
(
    const ExprPtr& expr,
    std::unordered_set<const ExprPtr *> & visited
) 
{
    auto monitor = false;

    std::function<void(const ExprPtr & e)> LoadFromStore = 
    [&monitor, &visited, &LoadFromStore] (const ExprPtr& e) {
        if (visited.count(&e))
        {
            return;
        }

        if (e->is_op() && asthub::GetUidExprOp(e) == AstUidExprOp::kLoad) {
            monitor |= e->arg(0)->is_op();
        }
        visited.insert(&e);

        for (int i = 0; i < e->arg_num(); i++)
        {
            LoadFromStore(e->arg(i));
        }
    };
    LoadFromStore(expr);
    return monitor;
}

/*****************************************************************************/

void PPAAnalyzer::PerformanceGet
(
    const ExprPtr & expr,
    PPAAnalysisData & ppaData,
    const std::string & exprToUpdate
)
{

    if (HasLoadFromStore(expr, ppaData.m_hasLoadFromStoreVisited))
    {
        ILA_ERROR << "There is a load from a store which is not allowed for these estimations";
    }

    /* If `expr` updates a memory state then we need to go through and check 
     * for store operations, to make sure that they're mapped to the vector 
     * which tracks the memory state they store to, which has to be the one 
     * `expr` updates (`exprToUpdate`) */
    if (!exprToUpdate.empty() && expr->is_mem())
    {
        ExprPtr exprToUpdatePtr = nullptr;

        auto findState =
        [&exprToUpdate, &exprToUpdatePtr](const InstrLvlAbsCnstPtr& m) -> void
        {
            ExprPtr temp = m->find_state(exprToUpdate);
            if (temp) {exprToUpdatePtr = temp;}
        };
        m_ila->DepthFirstVisit(findState);

        ILA_ASSERT(ppaData.m_exprToMemUseByCycle.count(exprToUpdatePtr))
            << "Update to memory not in m_exprToMemUseByCycle";

        std::shared_ptr<std::vector<int>> exprMemoryUsage = 
            ppaData.m_exprToMemUseByCycle.at(exprToUpdatePtr);
        
        std::function<void(const ExprPtr & e)> preCheckForStores =
        [&exprMemoryUsage, &ppaData, &preCheckForStores]
        (const ExprPtr & e)
        {

            if (e->is_mem() && e->is_op())
            {
                ppaData.m_exprToMemUseByCycle.insert(
                    {e, exprMemoryUsage}
                );

                if (asthub::GetUidExprOp(e) == kStore)
                {
                    preCheckForStores(e->arg(0));
                }
                else if (asthub::GetUidExprOp(e))
                {
                    preCheckForStores(e->arg(1));
                    preCheckForStores(e->arg(2));
                }

            }
        };

        preCheckForStores(expr);
    }

    auto determineTiming =
    [&ppaData, this](const ExprPtr & e)
    {

        /* If e is a load operation, then we need to make sure that it is
         * mapped to the correct vector for tracking memory usage for the state
         * e loads from */
        if (e->is_op() && asthub::GetUidExprOp(e) == AstUidExprOp::kLoad)
        {
            ILA_ASSERT(ppaData.m_exprToMemUseByCycle.count(e->arg(0)))
                 << "Load op loading from memory not in m_exprToMemUseByCycle";

            std::shared_ptr<std::vector<int>> exprMemoryUsage = 
                ppaData.m_exprToMemUseByCycle.at(e->arg(0));
                
            ppaData.m_exprToMemUseByCycle.insert(
                {e, exprMemoryUsage}
            );
        }

        /* Methodology: First, find time at which all args are ready, and how 
         * long the operation will take. From this, get a provisional start
         * time (earliest possible start time). The length the operation will
         * take is required for this provisional start time because it 
         * determines if it can start in the same cycle the args are ready or 
         * if it will need to start in the next cycle.
         * 
         * After this, send the provisional start time to `FirstSchedule` which
         * will pick a start time based on it's methodology (earliest non-full
         * cycle) and return it.
         * 
         * Then fill in the `m_endTimes`, `m_startTimes`, and `m_readyTimes` 
         * variables in `ppaData` */

        double maximumArgReadyTime = 0.0;

        for (size_t i = 0; i < e->arg_num(); i++)
        {
            double currArgReadyTime = 
                ppaData.m_endTimes.at(e->arg(i)->name().id());

            if (currArgReadyTime > maximumArgReadyTime)
            {
                maximumArgReadyTime = currArgReadyTime; 
            }
        }

        ppaData.m_readyTimes.insert({e, maximumArgReadyTime});

        double eProvisionalFinishTime = 0.0;
        double opPerformance = 0.0;
        if (e->is_op())
        {
            SortPtr srt = e->sort();
            int bitwidth = 
                srt->is_bool() ? 1 :
                srt->is_bv() ? srt->bit_width() :
                /*srt->is_mem() ?*/ srt->data_width();

            PPA_Callbacks::s_m_bitwidth = bitwidth;

            AstUidExprOp exprOp = asthub::GetUidExprOp(e);
            HardwareBlock_t hbInd = ExprToHardwareBlocks(e);

            PPAProfile_ptr prof;
            if (hbInd == bApplyFunc)
            {
                ExprOpAppFunc & exprFunc = dynamic_cast<ExprOpAppFunc&>(*e);
                const FuncPtr & fptr = exprFunc.func();
                prof = m_registrar.getMatchingProfile_UninterpFunc(fptr);
            }
            else
            {
                prof = m_registrar.getMatchingProfile_LowestBitwidth
                    (ExprToHardwareBlocks(e), bitwidth);
            }

            // TODO : I don't love the way this deals with memory, think ab it.

            if (exprOp == kLoad)
            {
                opPerformance = m_cycleTime;
            }
            else if (exprOp == kStore)
            {
                opPerformance = 0.0;
            }
            else
            {
                opPerformance = prof->getBlockTime();
            }

            ppaData.m_profiles.insert({e, prof});

            eProvisionalFinishTime = maximumArgReadyTime + opPerformance;
        }
        else
        {
            /* If we stumble accross a memory constant, we need to add it to
             * our tracking of memory usage and a list of constant mems so that
             * it can be deleted at the end of the program */
            if (e->is_mem() && e->is_const())
            {
                std::shared_ptr<std::vector<int>> newVec = 
                     std::make_shared<std::vector<int>>();
                ppaData.m_exprToMemUseByCycle.insert({e, newVec});
                m_constMems.insert(e);
            }

            // I think this should always be 0 if here
            ILA_WARN_IF(maximumArgReadyTime > 0.001) 
                << "Non op found with start time after 0.0";

            eProvisionalFinishTime = maximumArgReadyTime;

        }


        /* If it is configured not to pipeline short operations, then we need
         * to wait until the start of the next cycle to start the operation */
        double readyCycleUncasted = (maximumArgReadyTime / m_cycleTime);
        int readyCycle = static_cast<int>(readyCycleUncasted);
        int endCycle = static_cast<int>(eProvisionalFinishTime / m_cycleTime);

        bool spansCycles = readyCycle != endCycle;

        bool isShorterThanThreshold = opPerformance 
            < (m_configuration.shortOperationLengthThreshold * m_cycleTime);

        bool shortOpRetime = !m_configuration.pipelineShortOperations 
                                && isShorterThanThreshold;

        bool longOpRetime = m_configuration.pipelineStartAtCycleBoundary;

        double provisionalStartTime = maximumArgReadyTime;
        if ((longOpRetime || shortOpRetime) && spansCycles)
        {
            /* This gets the next cycle except for the case where we are 
             * exactly or almost exactly on a cycle boundary */
            provisionalStartTime = ceil(readyCycleUncasted - 0.000001)
                * m_cycleTime;
        }

        double startTime = e->is_op()
            ? FirstSchedule(e, provisionalStartTime, ppaData)
            : maximumArgReadyTime;

        // std::cout << e->name() << '\n';
        ppaData.m_startTimes[e] = startTime;

        double eFinishTime = startTime + opPerformance;
        
        ppaData.m_endTimes.insert({e->name().id(), eFinishTime});

        ILA_ASSERT(eFinishTime - startTime > -0.001) << "finish pre start";

    };


    /* Run determine timing once for each expression. The methodology for
     * `determineTiming` is described above. */
    std::function<void(const ExprPtr & e)> dfvpp =
    [&ppaData, &determineTiming, &dfvpp](const ExprPtr & e) -> void
    {

        if (ppaData.m_endTimes.find(e->name().id()) 
            != ppaData.m_endTimes.end())
        {
            return;
        }

        if (ppaData.m_parents.find(e) == ppaData.m_parents.end())
        {
            ppaData.m_parents.insert(
                {e, std::make_unique<std::vector<ExprPtr>>()}
            );
        }

        for (int i = 0; i < e->arg_num(); i++)
        {
            dfvpp(e->arg(i));

            ppaData.m_parents.at(e->arg(i))->emplace_back(e);
        }
        determineTiming(e);
    };

    dfvpp(expr);


    // Fill in m_latestTime. This won't change with `PushExpressionsLater`
    // so can be trusted.
    double exprEndTime = ppaData.m_endTimes.at(expr->name().id());
    if (exprEndTime > ppaData.m_latestTime)
    {
        ppaData.m_latestTime = exprEndTime;
    }

    ILA_INFO << expr->name().str() << " performance estimated at " 
        << ppaData.m_endTimes.at(expr->name().id());
}

/*****************************************************************************/

// This is subject to frequent renaming and changes
void PPAAnalyzer::AnalysisDataInitialize(PPAAnalysisData & ppaData)
{
    static bool isInit = false;

    if (isInit)
    {
        auto end = m_memVars.end();
        for (const ExprPtr & var : m_memVars)
        {
            std::shared_ptr<std::vector<int>> newVec =
                std::make_shared<std::vector<int>>();

            ppaData.m_exprToMemUseByCycle.insert({var, newVec});
        }

        ppaData.m_hardwareUseTracker.resize(m_registrar.getSize());

        return;
    }

    /* Methodology: If this is the first time we're filling in a ppaData, then
     * we need to find all of the non-constant memory variables, and save them
     * for later. This is because the ppaData var needs to track each memory
     * device's usage, so needs a vector of memory expressions. */

    for (const ExprPtr & var : absknob::GetSttTree(m_ila))
    {
        if (var->is_mem())
        {
            m_memVars.push_back(var);

            ILA_INFO << "Found mem state, " << var->name().str() 
                << " added vector to list of memVars";
        }
    }

    isInit = true;
    AnalysisDataInitialize(ppaData);
}


/*****************************************************************************/

// This is subject to frequent renaming and changes

// Currently DELETED
// void PPAAnalyzer::AnalysisDataDelete(PPAAnalysisData & ppaData)
// {
//     ppaData.m_exprToMemUseByCycle.clear();
//     ppaData.m_endTimes.clear();
//     ppaData.m_readyTimes.clear();
//     ppaData.m_startTimes.clear();
//     ppaData.m_latestTime = 0.0;
// }

/*****************************************************************************/

/* Returns the number of ones in the csd representation of `a` */
uint64_t onesInCSD(uint64_t a)
{
    enum cntState
    {
        noOnes,
        oneOne,
        manyOnes
    };

    int count = 0;
    cntState state = noOnes;
    for (int i = 0; i < 64/*bits in a*/; i++) 
    {
        int bit = (a & (1 << i));

        switch (state) {
        case noOnes:
            if (bit) {
                count++;
                state = oneOne;
            }
            //else nothing
            break;
        case oneOne:
            if (bit) {
                state = manyOnes;
            } else {
                state = noOnes;
            }
            break;
        case manyOnes:
            if (!bit) {
                count++;
                state = oneOne;
            }
            //else nothing
            break;
        }

    }

    return count;
}

/*****************************************************************************/

HardwareBlock_t PPAAnalyzer::ExprToHardwareBlocks(const ExprPtr & expr)
{
    ILA_ASSERT(expr->is_op()) << "Non op passed to ExprToHardwareBlocks";

    int numConstantArguments = 0;
    int numArguments = expr->arg_num();

    for (int i = 0; i < numArguments; i++)
    {
        if (expr->arg(i)->is_const())
        {
            numConstantArguments++;
        }
    }

    // I suspect this should never happen, but just in case
    if (numConstantArguments == numArguments)
    {
        // ILA_WARN << "Expression with only constant arguments found: "
        //     << expr->name().str();

        return bNoHardware;
    }

    AstUidExprOp op = asthub::GetUidExprOp(expr);

    if (numConstantArguments == 0)
    {
        return UidToHardwareBlock(op);
    }

    // if some but not all args are constants
    switch (op)
    {
    // Regardless of constant args, these require no hardware
    case kConcatenate:
    case kExtract:
    case kZeroExtend:
    case kSignedExtend:
        return bNoHardware;

    // Bitwise other than ITE remain bitwise
    case kNegate:
    case kNot:
    case kComplement:
    case kAnd:
    case kOr:
    case kXor: 
    case kImply:
    case kEqual:
        return bBitwise;

    // If the condition is constant then this doesn't require hardware
    case kIfThenElse:
        if (expr->arg(0)->is_const())
        {
            return bNoHardware;
        }
        return bMultiplexer;

    // Shifts become no hardware if the second input is a constant
    case kShiftLeft:
    case kArithShiftRight:
    case kLogicShiftRight:
    case kRotateLeft:
    case kRotateRight:
        if (expr->arg(1)->is_const())
        {
            return bNoHardware;
        }
        return bShifter;
    
    // These don't change with a constant
    case kAdd:
    case kSubtract:
        return bAddition;

    // These become bitwise with any constants
    case kLessThan:
    case kGreaterThan:
    case kUnsignedLessThan:
    case kUnsignedGreaterThan:
        // This case can't be reached here
        // if (!numConstantArguments)
        // {
        //     return bAddition;
        // }
        return bBitwise;

    // If there's a multiply by constant still in after running 
    // `DownGradeConstArgs`, then this means we should still do a multiply
    case kMultiply:
    {
        int ind = 0;
        if (!expr->arg(0)->is_const())
        {
            ind = 1;
        }
        
        const ExprPtr & arg = expr->arg(ind);
        const ilang::BvValType & v = dynamic_cast<const ExprConst &>(*arg)
                                .val_bv()->val();

        // If it's a power of two then it's just a shift
        int num1s = onesInCSD(v);
        if (num1s < 2)
        {
            return bNoHardware;
        }

        if (num1s == 2)
        {
            return bAddition;
        }

        // TODO : decide on whether to do multiple adds if num1s > 2. for now
        // assume multiplication
        return bMultiplication;
    }

    // Divide can become combo of a multiply by constant and free shift
    // That being said, the csd of the constnat should have enough ones that it
    // should just be thrown in a normal multiplier
    // The reality is that these are rare enough that this shouldn't really 
    // affect a user's results very often
    case kDivide:
    {
        if (expr->arg(1)->is_const())
        {
            const ExprPtr & arg = expr->arg(1);
            const ilang::BvValType & v = dynamic_cast<const ExprConst &>(*arg)
                                    .val_bv()->val();

            // If it's a power of two then it's just a shift by a constant
            if (((v & (v-1)) == 0) && v)
            {
                return bNoHardware;
            }

            return bMultiplication;
        }
        return bDivision;
    }

    // These can become 2 constant multiplications and a free shift
    // 2 constant multiplications is 1 multiplication, by the associative
    // property of multiplication, though that's a bit of an oversimplification
    // here
    // The reality is that these are rare enough that this shouldn't really 
    // affect a user's results very often
    case kSignedRemainder:
    case kUnsignedRemainder:
    case kSignedModular:
    {
        if (expr->arg(1)->is_const())
        {
            const ExprPtr & arg = expr->arg(1);
            const ilang::BvValType & v = dynamic_cast<const ExprConst &>(*arg)
                                    .val_bv()->val();

            // If it's a power of two then it's just an extraction
            if (((v & (v-1)) == 0) && v)
            {
                return bNoHardware;
            }
            
            return bMultiplication;
        }
        return bRemainder;
    }

    case kLoad:
    case kStore:
        return bMemory;

    case kApplyFunc:
        return bApplyFunc;
    default:
        return bInvalid;
    }

}

/*****************************************************************************/

HardwareBlock_t PPAAnalyzer::UidToHardwareBlock(AstUidExprOp op)
{
    switch (op)
    {
    case kConcatenate:
    case kExtract:
    case kZeroExtend:
    case kSignedExtend:
        return bNoHardware;
    case kNegate:
    case kNot:
    case kComplement:
    case kAnd:
    case kOr:
    case kXor: 
    case kImply:
    case kEqual:
        return bBitwise;
    case kIfThenElse:
        return bMultiplexer;
    case kShiftLeft:
    case kArithShiftRight:
    case kLogicShiftRight:
    case kRotateLeft:
    case kRotateRight:
        return bShifter;
    case kAdd:
    case kSubtract:
    case kLessThan:
    case kGreaterThan:
    case kUnsignedLessThan:
    case kUnsignedGreaterThan:
        return bAddition;
    case kMultiply:
        return bMultiplication;
    case kDivide:
        return bDivision;
    case kSignedRemainder:
    case kUnsignedRemainder:
    case kSignedModular:
        return bRemainder;
    case kLoad:
    case kStore:
        return bMemory;
    case kApplyFunc:
        return bApplyFunc;
    default:
        return bInvalid;
    }
}

/*****************************************************************************/

void PPAAnalyzer::PrintHardwareBlocks
(
    PPAAnalysisData & ppaData,
    const std::string & label,
    int numcycles
)
{
    std::cout << label << ':' << std::endl;

    size_t numCycles = (numcycles < 0) ? ppaData.m_latestTimeInCycles : numcycles;

    size_t registrarSize = m_registrar.getSize();

    for (size_t i = 0; i < numCycles; i++)
    {
        std::cout << "Cycle: " << i << "\n";
        for (int j = 0; j < registrarSize; j++)
        {
            HardwareBlock_t hb = m_registrar.blockTypeFromIndex(j);
            PPAProfile_ptr prof = m_registrar.profileFromIndex(j);

            int numToPrint = (ppaData.m_hardwareUseTracker.at(j).size() <= i) 
                ? 0 : ppaData.m_hardwareUseTracker.at(j).at(i);

            if (numToPrint != 0)
            {
                std::cout << "\t" << hardwareBlockToString(hb) << " bw: "
                    << prof->getMaximumBitwidth() << " #: " << numToPrint
                    << "\n";
            }
        }

        for (auto state : absknob::GetSttTree(m_ila))
        {
            if (state->is_mem())
            {
                int numToPrint = 
                    (ppaData.m_exprToMemUseByCycle.at(state)->size() <= i)
                    ? 0 
                    : ppaData.m_exprToMemUseByCycle.at(state)->at(i);

                

                std::cout << '\t' << "Mem: " << state->name().str() << " "
                    << numToPrint;
            }
        }

        std::cout << '\n';
    }
    std::cout << '\n' << '\n';
}


/*****************************************************************************/

const ExprPtr * findDuplicates
(
    const ExprPtr & e,
    std::unordered_map<uint64_t, const ExprPtr> & set
)
{
    /* Methodology: First, hash the expression. If it's an operation, use the 
     * id's of the arguments and the operation type to create a 64 bit hash. If
     * it's a constant, then hash it with it's value, then a 1 in the 62nd 
     * position if it's a boolean or the 63rd position if it's a bv (with the
     * 0th position set to 0). So far no collisions have been seen, though they
     * are theoretically possible. 
     * 
     * If the hash matches something we've seen, confirm it's a duplicate, then
     * return a ptr to the ExprPtr that the duplicate should be swapped out 
     * for. */

    if (e->is_var())
    {
        return nullptr;
    }

    size_t key = 0;
    if (e->is_op())
    {
        for (int i = 0; i < e->arg_num(); i++)
        {
            key |= (e->arg(i)->name().id() & 0x7ffffUL) << (38 - (i * 19));
        }
        for (int i = 0; i < e->param_num(); i++)
        {
            key |= (e->param(i) & 0x7ffffUL) << (i * 19);
        }

        key |= (asthub::GetUidExprOp(e) & 0x3fUL) << 57UL;
    }
    else if (e->is_const())
    {
        ExprConst & eConst = dynamic_cast<ExprConst&>(*e);
        if (eConst.is_bool())
        {
            key = eConst.val_bool()->val() | (1UL << 62);
        }
        else if (eConst.is_bv())
        {
            key = (eConst.val_bv()->val() << 1) | (1UL << 63);
        }
        else // eConst is mem and we don't expect duplicate constant mems
        {
            return nullptr;
        }
    }

    while (true)
    {
        auto found = set.find(key);
        if (found == set.end())
        {
            // This path is followed the vast majority of the time
            set.insert({key, e});
            return nullptr;
        }

        const ExprPtr & match = found->second;
        if (match->is_op() && e->is_op())
        {
            /* If the hashes matched and they're both ops, then the ops match
            * as we dedicate enough bits to guarantee it. Hence, their arities
            * are equal */

            ILA_ASSERT(e->arg_num() == match->arg_num()) << "Arg nums don't match";
            int args = e->arg_num();
            bool theyMatch = true;
            for (int i = 0; i < args; i++)
            {
                theyMatch &=
                    (e->arg(i)->name().id() == match->arg(i)->name().id());
            }
            int params = e->param_num();
            for (int i = 0; i < params; i++)
            {
                theyMatch &=
                    (e->param(i) == match->param(i));
            }
            if (theyMatch)
            {
                // if (asthub::GetUidExprOp(e) == AstUidExprOp::kLoad)
                // {
                //     key++;
                //     continue;
                // }
                return &match;
            }
        
        }
        if (match->is_const() && e->is_const())
        {
            ExprConst & matchConst = dynamic_cast<ExprConst&>(*match);
            ExprConst & eConst = dynamic_cast<ExprConst&>(*e);

            if (matchConst.is_bool() && eConst.is_bool())
            {
                return &match;
            }
            else if (matchConst.is_bv() && eConst.is_bv() 
                && matchConst.val_bv()->val() == eConst.val_bv()->val())
            {
                return &match;
            }
        }
        /* If there was a collision (incorrect match), just increment the
         * key and try again */ 
        static long collisionCount = 0;
        collisionCount++;
        ILA_INFO << "Duplicate search found collision, #" << collisionCount;
        key++;
    }
    // Unreachable
    ILA_ASSERT(false) << "Reached supposedly unreachable code in findDuplicates";
}

/*****************************************************************************/

const ExprPtr * PPAAnalyzer::RemoveDuplicates
(
    const ExprPtr & topExpr,
    std::unordered_map<uint64_t, const ExprPtr> & set,
    std::unordered_map<uint64_t, const ExprPtr> & checkedMap
)
{
    /* Methodology: For each ExprPtr, check all of it's children recursively 
     * for duplicates and remove duplicates. Then check the ExprPtr itself. */
    for (int i = 0; i < topExpr->arg_num(); i++)
    {
        const ExprPtr & arg = topExpr->arg(i);
        const ExprPtr * newId = nullptr;

        auto found = checkedMap.find(arg->name().id());
        if (found != checkedMap.end())
        {
            newId = &found->second;
        }
        else
        {
            newId = RemoveDuplicates(arg, set, checkedMap);
            if (newId)
            {
                checkedMap.insert({arg->name().id(), *newId});
            }
            else
            {
                checkedMap.insert({arg->name().id(), arg});
            }
        }

        if (newId)
        {
            // ILA_INFO << "Found duplicate! :)";
            topExpr->replace_arg(i, *newId);
        }
    }

    return findDuplicates(topExpr, set);
}

/*****************************************************************************/

bool PPAAnalyzer::MakeInstrSequence()
{
    if (!m_instrSeqPath.compare(""))
    {
        return false;
    }

    std::ifstream instr_seq_in(m_instrSeqPath, std::ios_base::in);

    if (!instr_seq_in.is_open())
    {
        ILA_WARN << "Name of instruction sequence file: " << m_instrSeqPath <<
            "could not be found/opened";

        return false;
    }
    
    /* Methodology: For each line of the instruction log, grab the name of the
     * instruction, and store it into m_instrSequence. */

    std::string instrLine;
    // size_t instrCount;

    while (std::getline(instr_seq_in, instrLine))
    {
        if (instrLine.empty())
        {
            continue;
        }
        
        std::string & instrName = m_instrSequence.emplace_back();

        instrName = instrLine.substr(instrLine.find('\t') + 1);
        instrName = instrName.substr(0, instrName.find(' '));

        // instrCount++;
    }

    return true;
}

/*****************************************************************************/

bool PPAAnalyzer::MakeVcd()
{
    if (m_vcdPath.empty())
    {
        return false;
    }
    

    VCDFileParser parser = VCDFileParser();
    m_vcdStatistics = parser.parse_file(m_vcdPath);
    if (!m_vcdStatistics)
    {
        return false;
    }
    

    std::vector<VCDSignal *> * signals = m_vcdStatistics->get_signals();
    for (VCDSignal * sig : *signals)
    {
        m_refToHash.insert({sig->reference, &sig->hash});
    }

    return true;
}


/*****************************************************************************/

void PPAAnalyzer::RegCountSlowHwBlocks()
{
    /* Methodology: One method for estimating the total number of registers 
     * (albeit a wildly innacurate one in our testing) is to count the number
     * of slow hardware blocks, and provide a register at the output of each
     * one of them. This function loops through every profile and adds the
     * number of instances of the slow profiles (determined by the static 
     * `slowHardwareBlock` variable) to the number of registers. */

    int numRegs = m_memVars.size();
    for (const ExprPtr & memVar : m_memVars)
    {
        int bw = memVar->sort()->data_width();
        PPAProfile_ptr regProf = m_registrar
            .getMatchingProfile_LowestBitwidth(bRegisterHold, bw);
        
        regProf->setNumInstances(
            regProf->getNumInstances() + m_configuration.defaultNumMemoryPorts
        );
    }

    RegistrarType & regis = *m_registrar.getRegisteredProfiles();

    int hwb = 0;
    for (const std::vector<PPAProfile_ptr> & vec : regis)
    {
        static std::array<bool, bNumBlockTypes> slowHardwareBlock = 
            {false, false, true, true, true, true, 
             false, true, false, false, false };

        if (slowHardwareBlock[hwb])
        {
            for (const PPAProfile_ptr & prof : vec)
            {
                int bw = prof->getMaximumBitwidth();
                PPAProfile_ptr regProf = m_registrar
                    .getMatchingProfile_LowestBitwidth(bRegisterHold, bw);
                
                regProf->setNumInstances( 
                    regProf->getNumInstances() + prof->getNumInstances()
                );

                numRegs += prof->getNumInstances();
            }
        }
        hwb++;
    }
}

/*****************************************************************************/

void PPAAnalyzer::CountMultiplexers()
{
    /* Methodology: Estimates the number of multiplexers by subtracting the 
     * number of uses of each hardware block from the number of instances
     * of the hardware block (which gives an estimate of the number of 
     * different input sources each adder is expected to have) and multiplying 
     * it by the number of inputs. A tradeoff of reuse is the need for many 
     * multiplexers. */

    RegistrarType & regis = *m_registrar.getRegisteredProfiles();

    int registrarIndex = 0;
    int numMuxes = 0;
    for (const std::vector<PPAProfile_ptr> & vec : regis)
    {
        if (registrarIndex == bMultiplexer || registrarIndex == bMemory)
        {
            continue;
        }
        registrarIndex++;

        for (const PPAProfile_ptr & prof : vec)
        {
            if (!prof->getIsReusable())
            {
                continue;
            }
            
            const PPAProfile_ptr & muxProf = 
                m_registrar.getMatchingProfile_LowestBitwidth(
                    bMultiplexer,
                    prof->getMaximumBitwidth()
                );

            if (!prof->getUniqueUses())
            {
                continue;
            }
            
            muxProf->incNumInstances(prof->getNumInputs() *
                (prof->getUniqueUses() - prof->getNumInstances()));
        }
    }
}

/*****************************************************************************/

void PPAAnalyzer::FinalizeEstimates
(
    std::unordered_map<std::string, PPAAnalysisData_ptr> & ppaData,
    bool hadInstrSeq,
    bool hadVcd
)
{

    /* Methodology: For leakage power and area, go through each profile, 
     * and multiply it's leakage power/area with the number of instances. Sum
     * it up and that's the result.
     * 
     * For timing, look at each instructions latest time and that is the time
     * it takes. For dynamic power, look through each instruction's operations
     * and sum together their dynamic powers (with switching activity) if 
     * available. If an instruction sequence is available, give information
     * about the specific workload by getting total time and power, as well
     * as percentages of the totals each instruction is responsible for. Use 
     * VCD for signal statistics if availble (VCD still TODO) */

    std::ofstream outFile("PPAOutput", std::ios_base::trunc);

    /* Count the  # of registers if regCountMethod == CountSlowHardwareBlocks
     * by counting the number of instances of adders, shifters, multipliers,
     * dividers, remainderers, and the number of memory ports */
    if (m_configuration.regCountMethod == 
        PPAAnalyzerConfig::CountSlowHardWareBlocks)
    {
        RegCountSlowHwBlocks();
    }

    // CountMultiplexers();

    RegistrarType & regis = *m_registrar.getRegisteredProfiles();

    outFile << "Leakage Power Information:" << std::endl << std::endl;

    // Start by getting hardware block counts for leakage and area
    double runningLeakageCount = 0.0;
    double registerLeakageCount = 0.0;
    int registrarIndex = 0;
    for (const std::vector<PPAProfile_ptr> & vec : regis)
    {
        outFile << hardwareBlockToString((HardwareBlock_t)registrarIndex) << '\n';
        for (const PPAProfile_ptr & prof : vec)
        {
            double newLeakage = 
                prof->getNumInstances() * prof->getBlockLeakagePower();

            if (registrarIndex == bRegisterHold)
            {
                registerLeakageCount += newLeakage; 
            }

            if (newLeakage > 0)
            {
                outFile << prof->getMaximumBitwidth() << "bit : " << newLeakage
                    << "    " << prof->getNumInstances() << " insts" << std::endl;
            }

            runningLeakageCount += newLeakage;
        }
        outFile << '\n';
        registrarIndex++;
    }

    outFile << "Total Leakage power: " << runningLeakageCount 
        << std::endl << std::endl;

    outFile << "Register Leakage power: " << registerLeakageCount
        << std::endl << std::endl;

    outFile << "Logic Leakage power: " << 
        runningLeakageCount - registerLeakageCount << std::endl << std::endl;

    outFile << "Area Information:" << std::endl << std::endl;

    double runningAreaCount = 0.0;
    registrarIndex = 0;
    for (const std::vector<PPAProfile_ptr> & vec : regis)
    {
        registrarIndex++;
        if (registrarIndex == bRegisterHold)
        {
            outFile << '\n';
            outFile << runningAreaCount;
            outFile << "\n\n";
        }
        for (const PPAProfile_ptr & prof : vec)
        {
            double newArea = 
                prof->getNumInstances() * prof->getBlockArea();

            if (newArea > 0)
            {
                outFile << prof->getMaximumBitwidth() << "bit : " << newArea << std::endl;
            }
            runningAreaCount += newArea;
        }
    }
    outFile << "Total area: " << runningAreaCount << std::endl << std::endl;


    if (!hadVcd)
    {

        outFile << "Timing Information:" << std::endl << std::endl;
        for (auto & data : ppaData)
        {
            PPAAnalysisData_ptr & ppaDataAnalysis = data.second;

            outFile << data.first << " : " << ppaDataAnalysis->m_latestTime;
            outFile << " (" << ppaDataAnalysis->m_latestTimeInCycles;
            outFile << "cycles)";
            outFile << std::endl;
        }

        outFile << std::endl;

        PPA_Callbacks::s_m_currSwitchingActivity = 0.5;

        outFile << "Dynamic Power Information: " << std::endl << std::endl;
        for (auto & data : ppaData)
        {
            PPAAnalysisData_ptr & ppaDataAnalysis = data.second;

            double instrDynPwr = 0.0;
            for (auto & pair : ppaDataAnalysis->m_startTimes)
            {
                const ExprPtr & expr = pair.first;
                if (!expr->is_op())
                {
                    continue;
                }


                const SortPtr & srt = expr->sort();
                int bitwidth = 
                    srt->is_bool() ? 1 :
                    srt->is_bv() ? srt->bit_width() :
                    /*srt->is_mem() ?*/ srt->data_width();

                PPA_Callbacks::s_m_bitwidth = bitwidth;
                
                instrDynPwr += ppaDataAnalysis->m_profiles.at(expr)
                    ->getBlockDynamicPower();
            }

            double numCycles = ppaDataAnalysis->m_latestTimeInCycles;

            ppaDataAnalysis->m_dynPower = instrDynPwr / numCycles;

            outFile << data.first << " : " << ppaDataAnalysis->m_dynPower
                << "nW per cycle on average";
            outFile << std::endl;
        }

        if (hadInstrSeq)
        {
            int cycleCounter = 0;
            double dynPwrCounter = 0.0;

            for (std::string & instrName : m_instrSequence)
            {
                PPAAnalysisData & ppaDataAnalysis = *ppaData.at(instrName);

                cycleCounter += ppaDataAnalysis.m_latestTimeInCycles;

                ppaDataAnalysis.m_activeCycles += 
                    ppaDataAnalysis.m_latestTimeInCycles;

                dynPwrCounter += ppaDataAnalysis.m_dynPower * 
                    ppaDataAnalysis.m_latestTimeInCycles;

            }

            double avgDynPwrPerCycle = dynPwrCounter / cycleCounter;

            outFile << "For the workload specified : \n";
            outFile << "  Avg dynamic power per cycle: " << avgDynPwrPerCycle;
            outFile << std::endl;
            outFile << "  Cycles required: " << cycleCounter << std::endl;

            outFile << std::endl;



            for (auto & data : ppaData)
            {
                PPAAnalysisData & ppaDataAnalysis = *data.second;

                double energy = ppaDataAnalysis.m_activeCycles 
                    * ppaDataAnalysis.m_dynPower;

                double percentOfTotalDyn = (energy / dynPwrCounter) * 100;

                double percentOfTotalTime = 
                    (static_cast<double>(ppaDataAnalysis.m_activeCycles) / 
                    static_cast<double>(cycleCounter)) * 100;

                outFile << data.first << ":" << std::endl << percentOfTotalDyn;
                outFile << "% of dyn power, " << percentOfTotalTime;
                outFile << "% of workload time" << std::endl;

            }
        }
    }
    else if (hadInstrSeq && hadVcd)
    {
        outFile << "TODO";
    }

    ILA_INFO << "DONE!!!";

    outFile.close();
    
}

/*****************************************************************************/
/*
void PPAAnalyzer::PPAAnalyze()
{

    ILA_INFO << "Begin PPA Estimation of " << m_ila;

    // std::vector<PPAAnalysisData_ptr> ppaData {};

    CountStateAndInputRegisters();

    std::unordered_map<std::string, PPAAnalysisData_ptr> ppaData {};

    // std::unordered_map<uint64_t, const ExprPtr> set;
    // std::unordered_map<uint64_t, const ExprPtr> checkedMap;

    {// for repeated naming purposes

    ILA_INFO << "Beginning fetch, valid, decode";

    // Wow I hate this syntax.
    PPAAnalysisData_ptr & ppaDataTest = 
        ppaData.insert(
            {"__fvd__", std::make_unique<PPAAnalysisData>()}
        ).first->second;

    AnalysisDataInitialize(*ppaDataTest);

    std::unordered_map<uint64_t, const ExprPtr> set;
    std::unordered_map<uint64_t, const ExprPtr> checkedMap;

    auto PerIla = 
    [this, &set, &checkedMap, &ppaDataTest](const InstrLvlAbsCnstPtr & m)
    {
        const ExprPtr & fetch_expr = m->fetch();
        if (fetch_expr)
        {        
            ppaDataTest->m_topExpressions.insert(fetch_expr);

            RemoveDuplicates(fetch_expr, set, checkedMap);
            PerformanceGet(fetch_expr, *ppaDataTest);
        }

        const ExprPtr & valid_expr = m->valid();
        if (valid_expr)
        {
            ppaDataTest->m_topExpressions.insert(valid_expr);


            RemoveDuplicates(valid_expr, set, checkedMap);
            PerformanceGet(valid_expr, *ppaDataTest);
        }
    };
    m_ila->DepthFirstVisit(PerIla);

    for (InstrPtr & instr : absknob::GetInstrTree(m_ila))
    {
        const ExprPtr & decode_expr = instr->decode();

        ppaDataTest->m_topExpressions.insert(decode_expr);


        RemoveDuplicates(decode_expr, set, checkedMap);
        PerformanceGet(decode_expr, *ppaDataTest);
    }

    ppaDataTest->m_latestTimeInCycles = static_cast<int>(
        ceil((ppaDataTest->m_latestTime + 0.000001) / m_cycleTime)
    );

    if (m_configuration.pushBackToEqualize)
    {
        PushExpressionsLater(*ppaDataTest);
    }
    CountHardwareBlocks(*ppaDataTest);

    for (const ExprPtr & expr : ppaDataTest->m_topExpressions)
    {
        CountRegistersSpanning(expr, *ppaDataTest);
    }

    const std::string s = "__fvd__";
    PrintHardwareBlocks(*ppaDataTest, s);

    ppaDataTest->m_hasLoadFromStoreVisited.clear();

    }

    for (InstrPtr & instr : absknob::GetInstrTree(m_ila))
    {
        std::unordered_map<uint64_t, const ExprPtr> set;
        std::unordered_map<uint64_t, const ExprPtr> checkedMap;

        ILA_INFO << "Beginning instruction : " << instr->name().c_str();
        // std::cout << "Beginning Instruction\n";

        PPAAnalysisData_ptr & ppaDataTest = ppaData.insert(
            {instr->name().str(), std::make_unique<PPAAnalysisData>()}
        ).first->second;

        AnalysisDataInitialize(*ppaDataTest);

        // std::unordered_map<uint64_t, const ExprPtr> set;
        // std::unordered_map<uint64_t, const ExprPtr> checkedMap;

        Instr::StateNameSet updated_states = instr->updated_states();
        for (const std::string& s : updated_states) {
            const ExprPtr & update_expr = instr->update(s);

            ppaDataTest->m_topExpressions.insert(update_expr);

            RemoveDuplicates(update_expr, set, checkedMap);
            PerformanceGet(update_expr, *ppaDataTest, s);
        }

        ppaDataTest->m_latestTimeInCycles = static_cast<int>(
            ceil((ppaDataTest->m_latestTime + 0.000001) / m_cycleTime)
        );

        if (m_configuration.pushBackToEqualize)
        {
            PushExpressionsLater(*ppaDataTest);
        }

        CountHardwareBlocks(*ppaDataTest);

        for (const ExprPtr & expr : ppaDataTest->m_topExpressions)
        {
            CountRegistersSpanning(expr, *ppaDataTest);

        }

        PrintHardwareBlocks(*ppaDataTest, instr->name().str());

        ppaDataTest->m_hasLoadFromStoreVisited.clear();
    }

    // for (auto & ppaDataTest : ppaData)
    // {
    //     AnalysisDataDelete(*ppaDataTest.second);
    // }

    bool hadInstrSeq = MakeInstrSequence();
    bool hadVcd = false;
    if (hadInstrSeq)
    {
        hadVcd = MakeVcd();
    }


    FinalizeEstimates(ppaData, hadInstrSeq, hadVcd);

    // ExtractHardwareBlocks(ppaData);
    // AnalysisDataDelete(*ppaDataToTest);
}
*/

/*****************************************************************************/


void PPAAnalyzer::PPAAnalyze()
{
    ILA_INFO << "Begin PPA Estimation of a module";

    // std::vector<PPAAnalysisData_ptr> ppaData {};

    CountStateAndInputRegisters();

    std::unordered_map<std::string, PPAAnalysisData_ptr> ppaData {};

    // std::unordered_map<uint64_t, const ExprPtr> set;
    // std::unordered_map<uint64_t, const ExprPtr> checkedMap;

    {// for repeated naming purposes

    // Wow I hate this syntax.
    PPAAnalysisData_ptr & ppaDataTest = 
        ppaData.insert(
            {"__fvd__", std::make_unique<PPAAnalysisData>()}
        ).first->second;

    AnalysisDataInitialize(*ppaDataTest);

    std::unordered_map<uint64_t, const ExprPtr> set;
    std::unordered_map<uint64_t, const ExprPtr> checkedMap;

    if (m_ilaMod.includesValidChecking())
    {

        ILA_INFO << "Beginning fetch, valid";

        std::unordered_set<InstrLvlAbsCnstPtr> hosts;

        // Find all the host ILAs recursively
        for (const InstrPtr & instr : m_ilaMod.getMod())
        {
            InstrLvlAbsCnstPtr host = instr->host();

            if (!hosts.count(host))
            {
                hosts.insert(host);
            }

            while (host->parent() && !hosts.count(host->parent()))
            {
                host = host->parent();

                if (!hosts.count(host))
                {
                    hosts.insert(host);
                }
            }
        }

        auto PerIla = 
        [this, &set, &checkedMap, &ppaDataTest](const InstrLvlAbsCnstPtr & m)
        {
            const ExprPtr & fetch_expr = m->fetch();
            if (fetch_expr)
            {        
                ppaDataTest->m_topExpressions.insert(fetch_expr);

                RemoveDuplicates(fetch_expr, set, checkedMap);
                PerformanceGet(fetch_expr, *ppaDataTest);
            }

            const ExprPtr & valid_expr = m->valid();
            if (valid_expr)
            {
                ppaDataTest->m_topExpressions.insert(valid_expr);


                RemoveDuplicates(valid_expr, set, checkedMap);
                PerformanceGet(valid_expr, *ppaDataTest);
            }
        };
        for (const InstrLvlAbsCnstPtr & m : hosts)
        {
            PerIla(m);
        }
    }

    ILA_INFO << "Beginning decode";


    for (InstrPtr & instr : m_ilaMod.getMod())
    {
        const ExprPtr & decode_expr = instr->decode();
        ppaDataTest->m_topExpressions.insert(decode_expr);
        RemoveDuplicates(decode_expr, set, checkedMap);
        PerformanceGet(decode_expr, *ppaDataTest);
    }

    ppaDataTest->m_latestTimeInCycles = static_cast<int>(
        ceil((ppaDataTest->m_latestTime + 0.000001) / m_cycleTime)
    );

    if (m_configuration.pushBackToEqualize)
    {
        PushExpressionsLater(*ppaDataTest);
    }
    CountHardwareBlocks(*ppaDataTest);

    for (const ExprPtr & expr : ppaDataTest->m_topExpressions)
    {
        CountRegistersSpanning(expr, *ppaDataTest);
    }

    const std::string s = "__fvd__";
    // PrintHardwareBlocks(*ppaDataTest, s);

    ppaDataTest->m_hasLoadFromStoreVisited.clear();

    }

    for (InstrPtr & instr : m_ilaMod.getMod())
    {
        std::unordered_map<uint64_t, const ExprPtr> set;
        std::unordered_map<uint64_t, const ExprPtr> checkedMap;

        ILA_INFO << "Beginning instruction : " << instr->name().c_str();
        // std::cout << "Beginning Instruction\n";

        PPAAnalysisData_ptr & ppaDataTest = ppaData.insert(
            {instr->name().str(), std::make_unique<PPAAnalysisData>()}
        ).first->second;

        AnalysisDataInitialize(*ppaDataTest);

        // std::unordered_map<uint64_t, const ExprPtr> set;
        // std::unordered_map<uint64_t, const ExprPtr> checkedMap;

        Instr::StateNameSet updated_states = instr->updated_states();
        for (const std::string& s : updated_states) {
            const ExprPtr & update_expr = instr->update(s);

            ppaDataTest->m_topExpressions.insert(update_expr);

            RemoveDuplicates(update_expr, set, checkedMap);
            PerformanceGet(update_expr, *ppaDataTest, s);
        }

        ppaDataTest->m_latestTimeInCycles = static_cast<int>(
            ceil((ppaDataTest->m_latestTime + 0.000001) / m_cycleTime)
        );

        if (m_configuration.pushBackToEqualize)
        {
            PushExpressionsLater(*ppaDataTest);
        }

        CountHardwareBlocks(*ppaDataTest);

        for (const ExprPtr & expr : ppaDataTest->m_topExpressions)
        {
            CountRegistersSpanning(expr, *ppaDataTest);

        }

        PrintHardwareBlocks(*ppaDataTest, instr->name().str(), -1);

        ppaDataTest->m_hasLoadFromStoreVisited.clear();
    }

    // for (auto & ppaDataTest : ppaData)
    // {
    //     AnalysisDataDelete(*ppaDataTest.second);
    // }

    bool hadInstrSeq = MakeInstrSequence();
    bool hadVcd = false;
    if (hadInstrSeq)
    {
        hadVcd = MakeVcd();
    }


    FinalizeEstimates(ppaData, hadInstrSeq, hadVcd);

    // ExtractHardwareBlocks(ppaData);
    // AnalysisDataDelete(*ppaDataToTest);
}

/*****************************************************************************/

PPAAnalyzer::PPAAnalyzer
(
    const InstrLvlAbsPtr & ila,
    IlaModule & ilaMod,
    double cycle_time,
    const std::string & instr_seq_path,
    const std::string & vcd_path
)
    : m_ila(ila), m_ilaMod(ilaMod), m_cycleTime(cycle_time),
      m_instrSeqPath(instr_seq_path), m_vcdPath(vcd_path)
{

    std::chrono::time_point<std::chrono::system_clock> st 
        = std::chrono::system_clock::now();

    m_configuration = {
        .pipelineStartAtCycleBoundary = true,

        .pipelineShortOperations = false,
        .shortOperationLengthThreshold = 1.0,

        .defaultNumMemoryPorts = 2,

        .pushBackToEqualize = true,

        .regCountMethod = PPAAnalyzerConfig::CountSlowHardWareBlocks,

        .PutConstantsInRegisters = true,
        .PutInputsInRegisters = true,

    };

    ILA_WARN_IF(m_configuration.pipelineStartAtCycleBoundary 
        && m_configuration.pipelineShortOperations) 
        << "Setting pipelineShortOperations while pipelineStartAtCycleBoundary is set will have no effect";
    
    if (m_configuration.shortOperationLengthThreshold > 1.0)
    {
        m_configuration.shortOperationLengthThreshold = 1.0;
    }
    

    ILA_ASSERT(m_configuration.shortOperationLengthThreshold <= 1.0)
        << "Short operation threshold is too large";

    registerAllModels(m_registrar);

    PPAAnalyze();

    std::chrono::time_point<std::chrono::system_clock> en 
        = std::chrono::system_clock::now();

    std::chrono::duration<float> difference = en - st;

    ILA_INFO << "PPA time taken in seconds: " << difference.count();
}

/*****************************************************************************/

PPAAnalyzer::PPAAnalyzer
(
    const InstrLvlAbsPtr & ila,
    IlaModule & ilaMod,
    double cycle_time,
    const std::string & instr_seq_path,
    const std::string & vcd_path,
    PPAAnalyzerConfig * config
)   : m_ila(ila), m_ilaMod(ilaMod), m_cycleTime(cycle_time),
      m_configuration(*config), m_instrSeqPath(instr_seq_path), 
      m_vcdPath(vcd_path)
{

    std::chrono::time_point<std::chrono::system_clock> st 
        = std::chrono::system_clock::now();

    ILA_ASSERT(m_configuration.shortOperationLengthThreshold <= 1.0) 
        << "Short operation threshold is too large";

    registerAllModels(m_registrar);

    PPAAnalyze();

    std::chrono::time_point<std::chrono::system_clock> en 
        = std::chrono::system_clock::now();

    std::chrono::duration<float> difference = en - st;

    ILA_INFO << "PPA time taken in seconds: " << difference.count();
}

}