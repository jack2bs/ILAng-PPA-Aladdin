#include <cmath>
#include <ilang/ppa-estimations/ppa.h>
#include <memory>
#include <unordered_map>


namespace ilang {

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
    PrintHardwareBlocks(*ppaDataTest, s);

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

}