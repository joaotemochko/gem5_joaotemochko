/*
 * Copyright (c) 2013-2014,2016 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/minor/fetch2.hh"

#include <algorithm>
#include <string>

#include "arch/generic/decoder.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/minor/pipeline.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/pred/bpred_unit.hh"
#include "debug/Branch.hh"
#include "debug/Fetch.hh"
#include "debug/MinorTrace.hh"

namespace gem5
{

namespace minor
{

Fetch2::Fetch2(const std::string &name,
    MinorCPU &cpu_,
    const BaseMinorCPUParams &params,
    const Execute &execute_,
    Latch<ForwardLineData>::Output inp_,
    Latch<BranchData>::Output branchInp_,
    Latch<BranchData>::Input predictionOut_,
    Latch<ForwardInstData>::Input out_,
    std::vector<InputBuffer<ForwardInstData>> &next_stage_input_buffer) :
    Named(name),
    cpu(cpu_),
    inp(inp_),
    branchInp(branchInp_),
    predictionOut(predictionOut_),
    out(out_),
    nextStageReserve(next_stage_input_buffer),
    outputWidth(params.decodeInputWidth),
    processMoreThanOneInput(params.fetch2CycleInput),
    branchPredictor(*params.branchPred),
    ifaMode(params.ifa_mode),
    ifaBufferSize(params.ifa_buffer_size),
    ifaDecodeWidth(params.ifa_decode_width),
    ifaLatency(params.ifa_latency),
    ifaFlushOnStreamChange(params.ifa_flush_on_stream_change),
    ifaDAUSource(params.ifa_dau_source),
    ifaLoadLatency(params.ifa_load_latency),
    ifaPassMemoryRefs(params.ifa_pass_memory_refs),
    ifaHoistMemoryRefs(params.ifa_hoist_memory_refs),
    ifaFillWhenBlocked(params.ifa_mode != 0 && params.ifa_fill_when_blocked),
    ifaQueueAware(params.ifa_queue_aware),
    decodeInputBufferSize(params.decodeInputBufferSize),
    ifaIssueModel(params.ifa_issue_model),
    ifaIssueLimit(params.executeIssueLimit),
    ifaFUOpLat(Num_OpClasses, Cycles(0)),
    ifaFURelLats(Num_OpClasses),
    ifaOpFU(Num_OpClasses, -1),
    ifaFUIssueLat(Num_OpClasses, Cycles(1)),
    ifaModelLastIssue(0),
    ifaModelNumAtLast(0),
    ifaModelMemAtLast(false),
    ifaModelFUFree(params.executeFuncUnits->funcUnits.size(), Cycles(0)),
    execute(execute_),
    ifaDispatchToIssue(params.fetch2ToDecodeForwardDelay +
        params.decodeToExecuteForwardDelay),
    ifaShadow(name + ".ifaShadow",
        cpu_.threads[0]->getIsaPtr()->regClasses()),
    ifaOpLatency(Num_OpClasses, Cycles(0)),
    ifaSrcRelativeLatency(0),
    ifaArrival(0),
    ifaMaxDispatchedSeqNum(0),
    fetchInfo(params.numThreads),
    threadPriority(0), stats(&cpu_, params.ifa_buffer_size)
{
    if (outputWidth < 1)
        fatal("%s: decodeInputWidth must be >= 1 (%d)\n", name, outputWidth);

    if (params.fetch2InputBufferSize < 1) {
        fatal("%s: fetch2InputBufferSize must be >= 1 (%d)\n", name,
        params.fetch2InputBufferSize);
    }

    /* Per-thread input buffers */
    for (ThreadID tid = 0; tid < params.numThreads; tid++) {
        inputBuffer.push_back(
            InputBuffer<ForwardLineData>(
                name + ".inputBuffer" + std::to_string(tid), "lines",
                params.fetch2InputBufferSize));
    }

    if (ifaMode > 2)
        fatal("%s: ifa_mode must be 0, 1 or 2 (%d)\n", name, ifaMode);

    if (ifaMode != 0) {
        if (params.numThreads != 1) {
            fatal("%s: the IFA only supports one thread (numThreads: %d)\n",
                name, params.numThreads);
        }

        if (ifaBufferSize < 1) {
            fatal("%s: ifa_buffer_size must be >= 1 (%d)\n", name,
                ifaBufferSize);
        }

        if (ifaPassMemoryRefs && ifaHoistMemoryRefs) {
            fatal("%s: ifa_pass_memory_refs and ifa_hoist_memory_refs"
                " can't be used together\n", name);
        }
    }

    /* Take the DAU's latency estimates from the Execute FU descriptions.
     *  The FU used is the first that provides the OpClass, as in
     *  Execute::issue */
    const std::vector<MinorFU *> &fus = params.executeFuncUnits->funcUnits;

    for (MinorFU *fu : fus) {
        if (fu->opClasses->provides(IntAluOp)) {
            if (!fu->timings.empty() &&
                !fu->timings[0]->srcRegsRelativeLats.empty())
            {
                ifaSrcRelativeLatency =
                    fu->timings[0]->srcRegsRelativeLats.back();
            }
            break;
        }
    }

    for (int op_class = 0; op_class < Num_OpClasses; op_class++) {
        for (int fu_index = 0; fu_index < fus.size(); fu_index++) {
            MinorFU *fu = fus[fu_index];

            if (fu->opClasses->provides(static_cast<OpClass>(op_class))) {
                ifaFUOpLat[op_class] = fu->opLat + (fu->timings.empty() ?
                    Cycles(0) : fu->timings[0]->extraAssumedLat);
                if (!fu->timings.empty()) {
                    ifaFURelLats[op_class] =
                        fu->timings[0]->srcRegsRelativeLats;
                }
                ifaOpFU[op_class] = fu_index;
                ifaFUIssueLat[op_class] = fu->issueLat;

                Cycles latency = fu->opLat + (fu->timings.empty() ?
                    Cycles(0) : fu->timings[0]->extraAssumedLat);

                ifaOpLatency[op_class] = (latency > ifaSrcRelativeLatency ?
                    Cycles(latency - ifaSrcRelativeLatency) : Cycles(0));
                break;
            }
        }
    }
}

const ForwardLineData *
Fetch2::getInput(ThreadID tid)
{
    /* Get a line from the inputBuffer to work with */
    if (!inputBuffer[tid].empty()) {
        return &(inputBuffer[tid].front());
    } else {
        return NULL;
    }
}

void
Fetch2::popInput(ThreadID tid)
{
    if (!inputBuffer[tid].empty()) {
        inputBuffer[tid].front().freeLine();
        inputBuffer[tid].pop();
    }

    fetchInfo[tid].inputIndex = 0;
}

void
Fetch2::dumpAllInput(ThreadID tid)
{
    DPRINTF(Fetch, "Dumping whole input buffer\n");
    while (!inputBuffer[tid].empty())
        popInput(tid);

    fetchInfo[tid].inputIndex = 0;
}

void
Fetch2::updateBranchPrediction(const BranchData &branch)
{
    MinorDynInstPtr inst = branch.inst;

    /* Don't even consider instructions we didn't try to predict or faults */
    if (inst->isFault() || !inst->triedToPredict)
        return;

    switch (branch.reason) {
      case BranchData::NoBranch:
        /* No data to update */
        break;
      case BranchData::Interrupt:
        /* Never try to predict interrupts */
        break;
      case BranchData::SuspendThread:
        /* Don't need to act on suspends */
        break;
      case BranchData::HaltFetch:
        /* Don't need to act on fetch wakeup */
        break;
      case BranchData::BranchPrediction:
        /* Shouldn't happen.  Fetch2 is the only source of
         *  BranchPredictions */
        break;
      case BranchData::UnpredictedBranch:
        /* Unpredicted branch or barrier */
        DPRINTF(Branch, "Unpredicted branch seen inst: %s\n", *inst);
        branchPredictor.squash(inst->id.fetchSeqNum,
            *branch.target, true, inst->id.threadId);
        // Update after squashing to accomodate O3CPU
        // using the branch prediction code.
        branchPredictor.update(inst->id.fetchSeqNum,
            inst->id.threadId);
        break;
      case BranchData::CorrectlyPredictedBranch:
        /* Predicted taken, was taken */
        DPRINTF(Branch, "Branch predicted correctly inst: %s\n", *inst);
        branchPredictor.update(inst->id.fetchSeqNum,
            inst->id.threadId);
        break;
      case BranchData::BadlyPredictedBranch:
        /* Predicted taken, not taken */
        DPRINTF(Branch, "Branch mis-predicted inst: %s\n", *inst);
        branchPredictor.squash(inst->id.fetchSeqNum,
            *branch.target /* Not used */, false, inst->id.threadId);
        // Update after squashing to accomodate O3CPU
        // using the branch prediction code.
        branchPredictor.update(inst->id.fetchSeqNum,
            inst->id.threadId);
        break;
      case BranchData::BadlyPredictedBranchTarget:
        /* Predicted taken, was taken but to a different target */
        DPRINTF(Branch, "Branch mis-predicted target inst: %s target: %s\n",
            *inst, *branch.target);
        branchPredictor.squash(inst->id.fetchSeqNum,
            *branch.target, true, inst->id.threadId);
        break;
    }
}

void
Fetch2::predictBranch(MinorDynInstPtr inst, BranchData &branch)
{
    Fetch2ThreadInfo &thread = fetchInfo[inst->id.threadId];

    assert(!inst->predictedTaken);

    /* Skip non-control/sys call instructions */
    if (inst->staticInst->isControl() || inst->staticInst->isSyscall()){
        std::unique_ptr<PCStateBase> inst_pc(inst->pc->clone());

        /* Tried to predict */
        inst->triedToPredict = true;

        DPRINTF(Branch, "Trying to predict for inst: %s\n", *inst);

        cpu.fetchStats[inst->id.threadId]->numBranches++;
        if (branchPredictor.predict(inst->staticInst,
                    inst->id.fetchSeqNum, *inst_pc, inst->id.threadId)) {
            set(branch.target, *inst_pc);
            inst->predictedTaken = true;
            set(inst->predictedTarget, inst_pc);
        }
    } else {
        DPRINTF(Branch, "Not attempting prediction for inst: %s\n", *inst);
    }

    /* If we predict taken, set branch and update sequence numbers */
    if (inst->predictedTaken) {
        /* Update the predictionSeqNum and remember the streamSeqNum that it
         *  was associated with */
        thread.expectedStreamSeqNum = inst->id.streamSeqNum;

        BranchData new_branch = BranchData(BranchData::BranchPrediction,
            inst->id.threadId,
            inst->id.streamSeqNum, thread.predictionSeqNum + 1,
            *inst->predictedTarget, inst);

        /* Mark with a new prediction number by the stream number of the
         *  instruction causing the prediction */
        thread.predictionSeqNum++;
        branch = new_branch;

        DPRINTF(Branch, "Branch predicted taken inst: %s target: %s"
            " new predictionSeqNum: %d\n",
            *inst, *inst->predictedTarget, thread.predictionSeqNum);
    }
}

bool
Fetch2::ifaIsBarrier(const MinorDynInstPtr &inst) const
{
    if (inst->isFault())
        return true;

    const StaticInstPtr &static_inst = inst->staticInst;

    /* Control instructions are barriers because Execute discards everything
     *  it receives after a mispredicted branch.  Things which can fault and
     *  aren't memory references, or which act on more state than their
     *  register operands, must stay in order for exceptions to be precise */
    return static_inst->isControl() ||
        static_inst->isSyscall() ||
        static_inst->isSerializing() ||
        static_inst->isNonSpeculative() ||
        static_inst->isSquashAfter() ||
        static_inst->isReadBarrier() ||
        static_inst->isWriteBarrier() ||
        static_inst->isAtomic() ||
        static_inst->isMacroop() ||
        static_inst->isMicroop() ||
        static_inst->isQuiesce() ||
        static_inst->isHtmCmd() ||
        static_inst->isInvalid();
}

/** Do the register operands of a (dests or srcs) and b (dests or srcs)
 *  have a register in common?  Invalid registers (such as RISC-V's x0)
 *  carry no dependencies */
static bool
ifaRegsOverlap(const StaticInstPtr &a, bool a_dests,
    const StaticInstPtr &b, bool b_dests)
{
    int num_a = (a_dests ? a->numDestRegs() : a->numSrcRegs());
    int num_b = (b_dests ? b->numDestRegs() : b->numSrcRegs());

    for (int i = 0; i < num_a; i++) {
        const RegId &reg_a = (a_dests ? a->destRegIdx(i) : a->srcRegIdx(i));

        if (reg_a.classValue() == InvalidRegClass)
            continue;

        for (int j = 0; j < num_b; j++) {
            if (reg_a == (b_dests ? b->destRegIdx(j) : b->srcRegIdx(j)))
                return true;
        }
    }

    return false;
}

bool
Fetch2::ifaSourcesReady(const MinorDynInstPtr &inst)
{
    if (inst->isFault())
        return true;

    const StaticInstPtr &static_inst = inst->staticInst;
    ThreadID tid = inst->id.threadId;
    auto *isa = cpu.getContext(tid)->getIsaPtr();
    /* inst can reach issue in Execute no earlier than this */
    Cycles arrival = ifaArrival;

    for (int i = 0; i < static_inst->numSrcRegs(); i++) {
        RegId reg = static_inst->srcRegIdx(i).flatten(*isa);
        Scoreboard::Index index;

        if (!ifaShadow.findIndex(reg, index))
            continue;

        /* Producers dispatched by Fetch2, including those not yet in
         *  Execute, with their estimated latency */
        if (ifaDAUSource != enums::IFAScoreboard &&
            ifaShadow.returnCycle[index] > arrival)
        {
            return false;
        }

        /* Producers in Execute.  A memory reference which hasn't completed
         *  is waited for whatever the shadow estimate (it may have
         *  missed in the cache) */
        if (ifaDAUSource != enums::IFAShadow) {
            Execute::IFARegState state = execute.ifaRegState(tid, reg);

            if (state.memPending)
                return false;

            if (ifaDAUSource == enums::IFAScoreboard && state.pending &&
                state.returnCycle > arrival + ifaSrcRelativeLatency)
            {
                return false;
            }
        }
    }

    return true;
}

bool
Fetch2::ifaCanPass(const MinorDynInstPtr &inst,
    const std::vector<MinorDynInstPtr> &skipped) const
{
    const StaticInstPtr &static_inst = inst->staticInst;

    /* Barriers only leave in program order */
    if (ifaIsBarrier(inst))
        return false;

    /* Memory references normally leave in program order too.  This keeps
     *  the order of memory accesses and makes a faulting access precise:
     *  nothing older is left undispatched behind it.  When hoisting, a
     *  memory reference can pass the head if nothing younger than the
     *  head has been dispatched: if it faults, restarting from the head
     *  executes nothing twice */
    if (static_inst->isMemRef()) {
        if (!ifaHoistMemoryRefs ||
            ifaMaxDispatchedSeqNum >= skipped.front()->id.fetchSeqNum)
        {
            return false;
        }
    }

    /* If an older memory reference which inst passes faults after inst has
     *  committed, inst will be executed again when the fault returns.  That
     *  is only safe if it doesn't overwrite one of its own sources, and
     *  still adds to the committed instruction count */
    bool can_pass_memory_refs = ifaPassMemoryRefs &&
        !ifaRegsOverlap(static_inst, true, static_inst, false);

    for (const MinorDynInstPtr &older : skipped) {
        const StaticInstPtr &older_inst = older->staticInst;

        if (older_inst->isMemRef() &&
            (!can_pass_memory_refs || static_inst->isMemRef()))
        {
            return false;
        }

        if (ifaRegsOverlap(older_inst, true, static_inst, false) || /* RAW */
            ifaRegsOverlap(older_inst, false, static_inst, true) || /* WAR */
            ifaRegsOverlap(older_inst, true, static_inst, true)) /* WAW */
        {
            return false;
        }
    }

    return true;
}

void
Fetch2::ifaMarkupDispatch(const MinorDynInstPtr &inst)
{
    if (inst->isFault())
        return;

    const StaticInstPtr &static_inst = inst->staticInst;
    auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();
    Cycles ready = ifaArrival +
        (static_inst->isMemRef() ? ifaLoadLatency :
            ifaOpLatency[static_inst->opClass()]);

    for (int i = 0; i < static_inst->numDestRegs(); i++) {
        RegId reg = static_inst->destRegIdx(i).flatten(*isa);
        Scoreboard::Index index;

        if (ifaShadow.findIndex(reg, index))
            ifaShadow.returnCycle[index] = ready;
    }
}

Cycles
Fetch2::ifaEstimateIssue(const MinorDynInstPtr &inst) const
{
    /* In order: not before the last dispatched instruction */
    Cycles issue = std::max(ifaModelLastIssue, ifaArrival);

    if (!inst->isFault()) {
        const StaticInstPtr &static_inst = inst->staticInst;
        OpClass op_class = static_inst->opClass();
        const std::vector<Cycles> &rel_lats = ifaFURelLats[op_class];
        auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();

        /* As Scoreboard::canInstIssue: a source is available from its
         *  returnCycle less the source's relative latency */
        for (int i = 0; i < static_inst->numSrcRegs(); i++) {
            RegId reg = static_inst->srcRegIdx(i).flatten(*isa);
            Scoreboard::Index index;

            if (!ifaShadow.findIndex(reg, index))
                continue;

            Cycles rel_lat = (rel_lats.empty() ? Cycles(0) :
                rel_lats[std::min<size_t>(i, rel_lats.size() - 1)]);
            Cycles ready = ifaShadow.returnCycle[index];

            if (ready > issue + rel_lat)
                issue = Cycles(ready - rel_lat);
        }

        int fu_index = ifaOpFU[op_class];
        if (fu_index >= 0)
            issue = std::max(issue, ifaModelFUFree[fu_index]);
    }

    /* Execute issues up to ifaIssueLimit instructions a cycle and stops
     *  after a memory reference */
    if (issue == ifaModelLastIssue &&
        (ifaModelNumAtLast >= ifaIssueLimit || ifaModelMemAtLast))
    {
        issue = Cycles(issue + 1);
    }

    return issue;
}

void
Fetch2::ifaModelDispatch(const MinorDynInstPtr &inst, Cycles issue_cycle)
{
    bool is_mem = !inst->isFault() && inst->staticInst->isMemRef();

    if (issue_cycle > ifaModelLastIssue) {
        ifaModelLastIssue = issue_cycle;
        ifaModelNumAtLast = 1;
        ifaModelMemAtLast = is_mem;
    } else {
        ifaModelNumAtLast++;
        ifaModelMemAtLast = ifaModelMemAtLast || is_mem;
    }

    if (inst->isFault())
        return;

    const StaticInstPtr &static_inst = inst->staticInst;
    OpClass op_class = static_inst->opClass();
    auto *isa = cpu.getContext(inst->id.threadId)->getIsaPtr();

    for (int i = 0; i < static_inst->numDestRegs(); i++) {
        RegId reg = static_inst->destRegIdx(i).flatten(*isa);
        Scoreboard::Index index;

        if (ifaShadow.findIndex(reg, index)) {
            ifaShadow.returnCycle[index] =
                Cycles(issue_cycle + ifaFUOpLat[op_class]);
        }
    }

    int fu_index = ifaOpFU[op_class];
    if (fu_index >= 0 && ifaFUIssueLat[op_class] > 1) {
        ifaModelFUFree[fu_index] =
            Cycles(issue_cycle + ifaFUIssueLat[op_class]);
    }
}

void
Fetch2::ifaDispatch(ForwardInstData &insts_out)
{
    const Cycles now = cpu.curCycle();
    unsigned int output_index = 0;
    bool passed_head = false;

    auto dispatchable = [&](const IFAEntry &entry) {
        return entry.dispatchableCycle <= now;
    };

    /* Move an entry to the output, returning the entry after it */
    auto dispatch = [&](std::deque<IFAEntry>::iterator entry,
        bool out_of_order)
    {
        MinorDynInstPtr inst = entry->inst;

        if (output_index == 0)
            insts_out.resize(outputWidth);
        insts_out.insts[output_index] = inst;
        output_index++;

        if (ifaIssueModel)
            ifaModelDispatch(inst, ifaEstimateIssue(inst));
        else
            ifaMarkupDispatch(inst);
        ifaMaxDispatchedSeqNum = std::max(ifaMaxDispatchedSeqNum,
            inst->id.fetchSeqNum);
        stats.ifaDispatched++;
        if (out_of_order) {
            stats.ifaDispatchedOutOfOrder++;
            passed_head = true;
        }

        DPRINTF(Fetch, "IFA dispatching%s inst: %s\n",
            (out_of_order ? " out of order" : ""), *inst);

        return ifaBuffer.erase(entry);
    };

    if (ifaBuffer.empty() || !dispatchable(ifaBuffer.front()))
        return;

    /* Instructions dispatched now reach issue after the latches and, if
     *  asked, after the instructions already queued in Decode's and
     *  Execute's input buffers (each holding one cycle's worth of
     *  instructions) */
    ifaArrival = now + ifaDispatchToIssue;
    if (ifaQueueAware) {
        ThreadID tid = ifaBuffer.front().inst->id.threadId;

        ifaArrival += Cycles(decodeInputBufferSize -
            nextStageReserve[tid].unreservedRemainingSpace() +
            execute.ifaInputBufferOccupancy(tid));
    }

    bool head_blocked = (ifaIssueModel ?
        ifaEstimateIssue(ifaBuffer.front().inst) >
            Cycles(std::max(ifaModelLastIssue, ifaArrival) + 1) :
        !ifaSourcesReady(ifaBuffer.front().inst));

    if (head_blocked)
        stats.ifaHeadBlockedCycles++;

    if (ifaMode == 2 && ifaIssueModel) {
        /* List scheduling: each slot gets the legal instruction which would
         *  issue earliest, the oldest on a tie */
        while (output_index < outputWidth && !ifaBuffer.empty()) {
            std::vector<MinorDynInstPtr> older;
            auto best = ifaBuffer.end();
            Cycles best_issue(0);
            MinorDynInstPtr best_passed_head = nullptr;
            unsigned int best_num_passed = 0;

            for (auto entry = ifaBuffer.begin();
                entry != ifaBuffer.end() && dispatchable(*entry); entry++)
            {
                const MinorDynInstPtr &inst = entry->inst;

                if (older.empty() || ifaCanPass(inst, older)) {
                    Cycles issue = ifaEstimateIssue(inst);

                    if (best == ifaBuffer.end() || issue < best_issue) {
                        best = entry;
                        best_issue = issue;
                        best_passed_head =
                            (older.empty() ? nullptr : older.front());
                        best_num_passed = older.size();
                    }
                }

                /* Nothing younger can pass a barrier */
                if (ifaIsBarrier(inst))
                    break;
                older.push_back(inst);
            }

            if (best == ifaBuffer.end())
                break;

            const MinorDynInstPtr inst = best->inst;

            if (best_num_passed != 0 && inst->staticInst->isMemRef()) {
                /* A hoisted memory reference: a fault in it must restart
                 *  from the oldest instruction it passed */
                set(inst->ifaRestartPC, best_passed_head->pc);
                stats.ifaHoisted++;
            }
            stats.ifaSkipped += best_num_passed;

            dispatch(best, best_num_passed != 0);
        }
    } else if (ifaMode == 2) {
        /* Older instructions which the scan has left in the FEB */
        std::vector<MinorDynInstPtr> skipped;
        auto entry = ifaBuffer.begin();

        while (entry != ifaBuffer.end() && dispatchable(*entry) &&
            output_index < outputWidth)
        {
            const MinorDynInstPtr inst = entry->inst;

            if (ifaSourcesReady(inst) &&
                (skipped.empty() || ifaCanPass(inst, skipped)))
            {
                if (!skipped.empty() && inst->staticInst->isMemRef()) {
                    /* A hoisted memory reference: a fault in it must
                     *  restart from the oldest instruction it passed */
                    set(inst->ifaRestartPC, skipped.front()->pc);
                    stats.ifaHoisted++;
                }
                entry = dispatch(entry, !skipped.empty());
            } else if (ifaIsBarrier(inst)) {
                /* Nothing younger can pass a barrier */
                break;
            } else {
                DPRINTF(Fetch, "IFA skipping inst: %s\n", *inst);
                skipped.push_back(inst);
                stats.ifaSkipped++;
                entry++;
            }
        }
    }

    /* Fill the rest of the output in program order.  This is all of
     *  dispatch in mode 1 and, in mode 2, makes sure the IFA never sends
     *  fewer instructions than in-order dispatch would */
    while (output_index < outputWidth && !ifaBuffer.empty() &&
        dispatchable(ifaBuffer.front()))
    {
        dispatch(ifaBuffer.begin(), false);
    }

    if (head_blocked && passed_head)
        stats.ifaReorderCycles++;
}

void
Fetch2::evaluate()
{
    /* Push input onto appropriate input buffer */
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->id.threadId].setTail(*inp.outputWire);

    ForwardInstData &insts_out = *out.inputWire;
    BranchData prediction;
    BranchData &branch_inp = *branchInp.outputWire;

    assert(insts_out.isBubble());

    /* React to branches from Execute to update local branch prediction
     *  structures */
    updateBranchPrediction(branch_inp);

    /* If a branch arrives, don't try and do anything about it.  Only
     *  react to your own predictions */
    if (branch_inp.isStreamChange()) {
        DPRINTF(Fetch, "Dumping all input as a stream changing branch"
            " has arrived\n");
        dumpAllInput(branch_inp.threadId);
        fetchInfo[branch_inp.threadId].havePC = false;

        /* Instructions in the FEB from the old stream would only be
         *  discarded by Execute */
        if (ifaMode != 0 && ifaFlushOnStreamChange) {
            auto old_stream = std::remove_if(ifaBuffer.begin(),
                ifaBuffer.end(), [&](const IFAEntry &entry) {
                    return entry.inst->id.streamSeqNum !=
                        branch_inp.newStreamSeqNum;
                });
            unsigned int num_flushed = ifaBuffer.end() - old_stream;

            if (num_flushed != 0) {
                DPRINTF(Fetch, "IFA discarding %d FEB entries\n",
                    num_flushed);
                stats.ifaFlushes++;
                stats.ifaFlushedInsts += num_flushed;
                ifaBuffer.erase(old_stream, ifaBuffer.end());
            }
        }
    }

    assert(insts_out.isBubble());
    /* Even when blocked, clear out input lines with the wrong
     *  prediction sequence number */
    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        Fetch2ThreadInfo &thread = fetchInfo[tid];

        thread.blocked = !nextStageReserve[tid].canReserve();

        const ForwardLineData *line_in = getInput(tid);

        while (line_in &&
            thread.expectedStreamSeqNum == line_in->id.streamSeqNum &&
            thread.predictionSeqNum != line_in->id.predictionSeqNum)
        {
            DPRINTF(Fetch, "Discarding line %s"
                " due to predictionSeqNum mismatch (expected: %d)\n",
                line_in->id, thread.predictionSeqNum);

            popInput(tid);
            fetchInfo[tid].havePC = false;

            if (processMoreThanOneInput) {
                DPRINTF(Fetch, "Wrapping\n");
                line_in = getInput(tid);
            } else {
                line_in = NULL;
            }
        }
    }

    ThreadID tid = getScheduledThread();
    DPRINTF(Fetch, "Scheduled Thread: %d\n", tid);

    assert(insts_out.isBubble());
    if (tid != InvalidThreadID) {
        Fetch2ThreadInfo &fetch_info = fetchInfo[tid];

        const ForwardLineData *line_in = getInput(tid);

        unsigned int output_index = 0;

        /* Number of instructions put into the FEB this cycle */
        unsigned int ifa_num_decoded = 0;

        /* Pack instructions into the output (or, with the IFA, the FEB)
         * while we can.  This may involve using more than one input line.
         * Note that lineWidth will be 0 for faulting lines */
        while (line_in &&
            (line_in->isFault() ||
                fetch_info.inputIndex < line_in->lineWidth) && /* More input */
            (ifaMode == 0 ?
                output_index < outputWidth : /* More output to fill */
                (ifaBuffer.size() < ifaBufferSize && /* FEB space */
                    (ifaDecodeWidth == 0 ||
                        ifa_num_decoded < ifaDecodeWidth))) &&
            prediction.isBubble() /* No predicted branch */)
        {
            ThreadContext *thread = cpu.getContext(line_in->id.threadId);
            InstDecoder *decoder = thread->getDecoderPtr();

            /* Discard line due to prediction sequence number being wrong but
             * without the streamSeqNum number having changed */
            bool discard_line =
                fetch_info.expectedStreamSeqNum == line_in->id.streamSeqNum &&
                fetch_info.predictionSeqNum != line_in->id.predictionSeqNum;

            /* Set the PC if the stream changes.  Setting havePC to false in
             *  a previous cycle handles all other change of flow of control
             *  issues */
            bool set_pc =
                fetch_info.lastStreamSeqNum != line_in->id.streamSeqNum;

            if (!discard_line && (!fetch_info.havePC || set_pc)) {
                /* Set the inputIndex to be the MachInst-aligned offset
                 *  from lineBaseAddr of the new PC value */
                fetch_info.inputIndex =
                    (line_in->pc->instAddr() & decoder->pcMask()) -
                    line_in->lineBaseAddr;
                DPRINTF(Fetch, "Setting new PC value: %s inputIndex: 0x%x"
                    " lineBaseAddr: 0x%x lineWidth: 0x%x\n",
                    *line_in->pc, fetch_info.inputIndex, line_in->lineBaseAddr,
                    line_in->lineWidth);
                set(fetch_info.pc, line_in->pc);
                fetch_info.havePC = true;
                decoder->reset();
            }

            /* The generated instruction.  Leave as NULL if no instruction
             *  is to be packed into the output */
            MinorDynInstPtr dyn_inst = NULL;

            if (discard_line) {
                /* Rest of line was from an older prediction in the same
                 *  stream */
                DPRINTF(Fetch, "Discarding line %s (from inputIndex: %d)"
                    " due to predictionSeqNum mismatch (expected: %d)\n",
                    line_in->id, fetch_info.inputIndex,
                    fetch_info.predictionSeqNum);
            } else if (line_in->isFault()) {
                /* Pack a fault as a MinorDynInst with ->fault set */

                /* Make a new instruction and pick up the line, stream,
                 *  prediction, thread ids from the incoming line */
                dyn_inst = new MinorDynInst(nullStaticInstPtr, line_in->id);

                /* Fetch and prediction sequence numbers originate here */
                dyn_inst->id.fetchSeqNum = fetch_info.fetchSeqNum;
                dyn_inst->id.predictionSeqNum = fetch_info.predictionSeqNum;
                /* To complete the set, test that exec sequence number has
                 *  not been set */
                assert(dyn_inst->id.execSeqNum == 0);

                set(dyn_inst->pc, fetch_info.pc);

                /* Pack a faulting instruction but allow other
                 *  instructions to be generated. (Fetch2 makes no
                 *  immediate judgement about streamSeqNum) */
                dyn_inst->fault = line_in->fault;
                DPRINTF(Fetch, "Fault being passed output_index: "
                    "%d: %s\n", output_index, dyn_inst->fault->name());
            } else {
                uint8_t *line = line_in->line;

                /* The instruction is wholly in the line, can just copy. */
                memcpy(decoder->moreBytesPtr(), line + fetch_info.inputIndex,
                        decoder->moreBytesSize());

                if (!decoder->instReady()) {
                    decoder->moreBytes(*fetch_info.pc,
                        line_in->lineBaseAddr + fetch_info.inputIndex);
                    DPRINTF(Fetch, "Offering MachInst to decoder addr: 0x%x\n",
                            line_in->lineBaseAddr + fetch_info.inputIndex);
                }

                /* Maybe make the above a loop to accomodate ISAs with
                 *  instructions longer than sizeof(MachInst) */

                if (decoder->instReady()) {
                    /* Note that the decoder can update the given PC.
                     *  Remember not to assign it until *after* calling
                     *  decode */
                    StaticInstPtr decoded_inst =
                        decoder->decode(*fetch_info.pc);

                    /* Make a new instruction and pick up the line, stream,
                     *  prediction, thread ids from the incoming line */
                    dyn_inst = new MinorDynInst(decoded_inst, line_in->id);

                    /* Fetch and prediction sequence numbers originate here */
                    dyn_inst->id.fetchSeqNum = fetch_info.fetchSeqNum;
                    dyn_inst->id.predictionSeqNum = fetch_info.predictionSeqNum;
                    /* To complete the set, test that exec sequence number
                     *  has not been set */
                    assert(dyn_inst->id.execSeqNum == 0);

                    set(dyn_inst->pc, fetch_info.pc);
                    DPRINTF(Fetch, "decoder inst %s\n", *dyn_inst);

                    // Collect some basic inst class stats
                    if (decoded_inst->isLoad()) {
                        stats.loadInstructions++;
                    } else if (decoded_inst->isStore()) {
                        stats.storeInstructions++;
                    } else if (decoded_inst->isAtomic()) {
                        stats.amoInstructions++;
                    } else if (decoded_inst->isVector()) {
                        stats.vecInstructions++;
                    } else if (decoded_inst->isFloating()) {
                        stats.fpInstructions++;
                    } else if (decoded_inst->isInteger()) {
                        stats.intInstructions++;
                    }

                    stats.totalInstructions++;
                    cpu.fetchStats[tid]->numInsts++;

                    DPRINTF(Fetch, "Instruction extracted from line %s"
                        " lineWidth: %d output_index: %d inputIndex: %d"
                        " pc: %s inst: %s\n",
                        line_in->id,
                        line_in->lineWidth, output_index, fetch_info.inputIndex,
                        *fetch_info.pc, *dyn_inst);

                    /*
                     * In SE mode, it's possible to branch to a microop when
                     * replaying faults such as page faults (or simply
                     * intra-microcode branches in X86).  Unfortunately,
                     * as Minor has micro-op decomposition in a separate
                     * pipeline stage from instruction decomposition, the
                     * following advancePC (which may follow a branch with
                     * microPC() != 0) *must* see a fresh macroop.
                     *
                     * X86 can branch within microops so we need to deal with
                     * the case that, after a branch, the first un-advanced PC
                     * may be pointing to a microop other than 0.  Once
                     * advanced, however, the microop number *must* be 0
                     */
                    fetch_info.pc->uReset();

                    /* Advance PC for the next instruction */
                    decoded_inst->advancePC(*fetch_info.pc);

                    /* Predict any branches and issue a branch if
                     *  necessary */
                    predictBranch(dyn_inst, prediction);
                } else {
                    DPRINTF(Fetch, "Inst not ready yet\n");
                }

                /* Step on the pointer into the line if there's no
                 *  complete instruction waiting */
                if (decoder->needMoreBytes()) {
                    fetch_info.inputIndex += decoder->moreBytesSize();

                DPRINTF(Fetch, "Updated inputIndex value PC: %s"
                    " inputIndex: 0x%x lineBaseAddr: 0x%x lineWidth: 0x%x\n",
                    *line_in->pc, fetch_info.inputIndex, line_in->lineBaseAddr,
                    line_in->lineWidth);
                }
            }

            if (dyn_inst) {
                /* Step to next sequence number */
                fetch_info.fetchSeqNum++;

                if (ifaMode != 0) {
                    /* The IFA dispatches from the FEB below */
                    ifaBuffer.push_back(
                        IFAEntry{dyn_inst, cpu.curCycle() + ifaLatency});
                    ifa_num_decoded++;
                } else {
                    /* Correctly size the output before writing */
                    if (output_index == 0) {
                        insts_out.resize(outputWidth);
                    }
                    /* Pack the generated dynamic instruction into the
                     *  output */
                    insts_out.insts[output_index] = dyn_inst;
                    output_index++;
                }

                /* Output MinorTrace instruction info for
                 *  pre-microop decomposition macroops */
                if (debug::MinorTrace && !dyn_inst->isFault() &&
                    dyn_inst->staticInst->isMacroop()) {
                    dyn_inst->minorTraceInst(*this);
                }
            }

            /* Remember the streamSeqNum of this line so we can tell when
             *  we change stream */
            fetch_info.lastStreamSeqNum = line_in->id.streamSeqNum;

            /* Asked to discard line or there was a branch or fault */
            if (!prediction.isBubble() || /* The remains of a
                    line with a prediction in it */
                line_in->isFault() /* A line which is just a fault */)
            {
                DPRINTF(Fetch, "Discarding all input on branch/fault\n");
                dumpAllInput(tid);
                fetch_info.havePC = false;
                line_in = NULL;
            } else if (discard_line) {
                /* Just discard one line, one's behind it may have new
                 *  stream sequence numbers.  There's a DPRINTF above
                 *  for this event */
                popInput(tid);
                fetch_info.havePC = false;
                line_in = NULL;
            } else if (fetch_info.inputIndex == line_in->lineWidth) {
                /* Got to end of a line, pop the line but keep PC
                 *  in case this is a line-wrapping inst. */
                popInput(tid);
                line_in = NULL;
            }

            if (!line_in && processMoreThanOneInput) {
                DPRINTF(Fetch, "Wrapping\n");
                line_in = getInput(tid);
            }
        }

        if (ifaMode != 0) {
            if (ifaBuffer.size() >= ifaBufferSize && getInput(tid))
                stats.ifaBufferFullCycles++;

            /* With ifaFillWhenBlocked, this thread may only have been
             *  scheduled to fill the FEB */
            if (!fetch_info.blocked)
                ifaDispatch(insts_out);
        }

        /* The rest of the output (if any) should already have been packed
         *  with bubble instructions by insts_out's initialisation */
    }
    if (tid == InvalidThreadID) {
        assert(insts_out.isBubble());
    }
    /** Reserve a slot in the next stage and output data */
    *predictionOut.inputWire = prediction;

    /* If we generated output, reserve space for the result in the next stage
     *  and mark the stage as being active this cycle */
    if (!insts_out.isBubble()) {
        /* Note activity of following buffer */
        cpu.activityRecorder->activity();
        insts_out.threadId = tid;
        nextStageReserve[tid].reserve();
    }

    /* If we still have input to process and somewhere to put it,
     *  mark stage as active */
    for (ThreadID i = 0; i < cpu.numThreads; i++)
    {
        if (((getInput(i) || !ifaBuffer.empty()) &&
                nextStageReserve[i].canReserve()) ||
            (ifaFillWhenBlocked && getInput(i) &&
                ifaBuffer.size() < ifaBufferSize))
        {
            cpu.activityRecorder->activateStage(Pipeline::Fetch2StageId);
            break;
        }
    }

    if (ifaMode != 0)
        stats.ifaOccupancy.sample(ifaBuffer.size());

    /* Make sure the input (if any left) is pushed */
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->id.threadId].pushTail();
}

inline ThreadID
Fetch2::getScheduledThread()
{
    /* Select thread via policy. */
    std::vector<ThreadID> priority_list;

    switch (cpu.threadPolicy) {
      case enums::SingleThreaded:
        priority_list.push_back(0);
        break;
      case enums::RoundRobin:
        priority_list = cpu.roundRobinPriority(threadPriority);
        break;
      case enums::Random:
        priority_list = cpu.randomPriority();
        break;
      default:
        panic("Unknown fetch policy");
    }

    for (auto tid : priority_list) {
        if (((getInput(tid) || !ifaBuffer.empty()) &&
                !fetchInfo[tid].blocked) ||
            (ifaFillWhenBlocked && getInput(tid) &&
                ifaBuffer.size() < ifaBufferSize))
        {
            threadPriority = tid;
            return tid;
        }
    }

   return InvalidThreadID;
}

bool
Fetch2::isDrained()
{
    for (const auto &buffer : inputBuffer) {
        if (!buffer.empty())
            return false;
    }

    return ifaBuffer.empty() &&
           (*inp.outputWire).isBubble() &&
           (*predictionOut.inputWire).isBubble();
}

Fetch2::Fetch2Stats::Fetch2Stats(MinorCPU *cpu,
    unsigned int ifa_buffer_size)
    : statistics::Group(cpu, "fetch2"),
      ADD_STAT(totalInstructions, statistics::units::Count::get(),
               "Total number of instructions successfully decoded"),
      ADD_STAT(intInstructions, statistics::units::Count::get(),
               "Number of integer instructions successfully decoded"),
      ADD_STAT(fpInstructions, statistics::units::Count::get(),
               "Number of floating point instructions successfully decoded"),
      ADD_STAT(vecInstructions, statistics::units::Count::get(),
               "Number of SIMD instructions successfully decoded"),
      ADD_STAT(loadInstructions, statistics::units::Count::get(),
               "Number of memory load instructions successfully decoded"),
      ADD_STAT(storeInstructions, statistics::units::Count::get(),
               "Number of memory store instructions successfully decoded"),
      ADD_STAT(amoInstructions, statistics::units::Count::get(),
               "Number of memory atomic instructions successfully decoded"),
      ADD_STAT(ifaDispatched, statistics::units::Count::get(),
               "Number of instructions dispatched from the IFA FEB"),
      ADD_STAT(ifaDispatchedOutOfOrder, statistics::units::Count::get(),
               "Number of instructions dispatched from the IFA FEB while an"
               " older instruction stayed in it"),
      ADD_STAT(ifaHoisted, statistics::units::Count::get(),
               "Number of memory references dispatched from the IFA FEB"
               " before older instructions"),
      ADD_STAT(ifaSkipped, statistics::units::Count::get(),
               "Number of times the IFA dispatch scan left an instruction in"
               " the FEB to look at younger ones"),
      ADD_STAT(ifaHeadBlockedCycles, statistics::units::Cycle::get(),
               "Number of cycles in which the oldest FEB instruction could"
               " be dispatched but the DAU found it not ready"),
      ADD_STAT(ifaReorderCycles, statistics::units::Cycle::get(),
               "Number of cycles in which the oldest FEB instruction was not"
               " ready and a younger one was dispatched before it"),
      ADD_STAT(ifaBufferFullCycles, statistics::units::Cycle::get(),
               "Number of cycles in which input could not be decoded"
               " because the IFA FEB was full"),
      ADD_STAT(ifaFlushes, statistics::units::Count::get(),
               "Number of stream changes which discarded IFA FEB entries"),
      ADD_STAT(ifaFlushedInsts, statistics::units::Count::get(),
               "Number of IFA FEB entries discarded on stream changes"),
      ADD_STAT(ifaOccupancy, statistics::units::Count::get(),
               "Number of IFA FEB entries at the end of each cycle")
{
    totalInstructions.flags(statistics::total);
    intInstructions.flags(statistics::total);
    fpInstructions.flags(statistics::total);
    vecInstructions.flags(statistics::total);
    loadInstructions.flags(statistics::total);
    storeInstructions.flags(statistics::total);
    amoInstructions.flags(statistics::total);

    ifaOccupancy.init(0, ifa_buffer_size, 1);
}

void
Fetch2::minorTrace() const
{
    std::ostringstream data;

    if (fetchInfo[0].blocked)
        data << 'B';
    else
        (*out.inputWire).reportData(data);

    minor::minorTrace("inputIndex=%d havePC=%d predictionSeqNum=%d insts=%s\n",
        fetchInfo[0].inputIndex, fetchInfo[0].havePC,
        fetchInfo[0].predictionSeqNum, data.str());
    inputBuffer[0].minorTrace();
}

} // namespace minor
} // namespace gem5
