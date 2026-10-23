/*
 * Copyright (c) 2013-2014 ARM Limited
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

/**
 * @file
 *
 *  Fetch2 receives lines of data from Fetch1, separates them into
 *  instructions and passes them to Decode
 */

#ifndef __CPU_MINOR_FETCH2_HH__
#define __CPU_MINOR_FETCH2_HH__

#include <deque>
#include <vector>

#include "base/named.hh"
#include "cpu/minor/buffers.hh"
#include "cpu/minor/cpu.hh"
#include "cpu/minor/execute.hh"
#include "cpu/minor/pipe_data.hh"
#include "cpu/minor/scoreboard.hh"
#include "cpu/pred/bpred_unit.hh"
#include "enums/MinorIFADAUSource.hh"
#include "params/BaseMinorCPU.hh"

namespace gem5
{

namespace minor
{

/** This stage receives lines of data from Fetch1, separates them into
 *  instructions and passes them to Decode */
class Fetch2 : public Named
{
  protected:
    /** Pointer back to the containing CPU */
    MinorCPU &cpu;

    /** Input port carrying lines from Fetch1 */
    Latch<ForwardLineData>::Output inp;

    /** Input port carrying branches from Execute.  This is a snoop of the
     *  data provided to F1. */
    Latch<BranchData>::Output branchInp;

    /** Output port carrying predictions back to Fetch1 */
    Latch<BranchData>::Input predictionOut;

    /** Output port carrying instructions into Decode */
    Latch<ForwardInstData>::Input out;

    /** Interface to reserve space in the next stage */
    std::vector<InputBuffer<ForwardInstData>> &nextStageReserve;

    /** Width of output of this stage/input of next in instructions */
    unsigned int outputWidth;

    /** If true, more than one input word can be processed each cycle if
     *  there is room in the output to contain its processed data */
    bool processMoreThanOneInput;

    /** Branch predictor passed from Python configuration */
    branch_prediction::BPredUnit &branchPredictor;

    /** Instruction Flow Amplifier (IFA) mode.  0: baseline Fetch2 (IFA
     *  off).  1: decoded instructions go through the front-end buffer
     *  (FEB) and leave it in program order.  2: the dispatch unit (DU)
     *  sends the oldest instructions the dependency analysis unit (DAU)
     *  finds ready, which may skip a blocked head */
    const unsigned int ifaMode;

    /** Number of FEB entries */
    const unsigned int ifaBufferSize;

    /** Maximum number of instructions decoded into the FEB each cycle.
     *  0 means no limit other than FEB space and available input */
    const unsigned int ifaDecodeWidth;

    /** Cycles an instruction spends in the FEB before it can be
     *  dispatched.  0 allows dispatch in the cycle it is decoded */
    const Cycles ifaLatency;

    /** Discard FEB entries from the old stream when Execute changes
     *  stream.  If false, they are dispatched and dropped by Execute */
    const bool ifaFlushOnStreamChange;

    /** Where the DAU finds producers which are still in flight */
    const enums::MinorIFADAUSource ifaDAUSource;

    /** Load-to-use latency assumed by the shadow scoreboard */
    const Cycles ifaLoadLatency;

    /** Allow instructions which can be executed again with the same result
     *  to pass older memory references.  If such a memory reference
     *  faults (for example, a stack page fault in SE mode), the passing
     *  instructions are executed and committed again after the fault */
    const bool ifaPassMemoryRefs;

    /** Allow memory references to be dispatched before older (non memory)
     *  instructions they don't depend on, so that they start earlier.
     *  Only the first instruction to pass the head may be a memory
     *  reference, and if it faults, Execute restarts from the head
     *  (MinorDynInst::ifaRestartPC), keeping faults precise */
    const bool ifaHoistMemoryRefs;

    /** Keep decoding into the FEB while Decode can't accept instructions */
    const bool ifaFillWhenBlocked;

    /** Include the instructions queued between Fetch2 and issue in the
     *  DAU's estimate of when an instruction will reach issue */
    const bool ifaQueueAware;

    /** Capacity of Decode's input buffer */
    const unsigned int decodeInputBufferSize;

    /** Choose what to dispatch with a model of Execute's in-order issue:
     *  the instruction which would issue earliest goes first, so a younger
     *  instruction only passes the head if it would issue before it.  If
     *  false, the oldest ready instruction goes first */
    const bool ifaIssueModel;

    /** Execute's executeIssueLimit */
    const unsigned int ifaIssueLimit;

    /** Issue model: by OpClass, as Execute's FUs see it: the latency
     *  added to the issue cycle to give the scoreboard's returnCycle, the
     *  source relative latencies, the FU used and its issue latency */
    std::vector<Cycles> ifaFUOpLat;
    std::vector<std::vector<Cycles>> ifaFURelLats;
    std::vector<int> ifaOpFU;
    std::vector<Cycles> ifaFUIssueLat;

    /** Issue model state: the estimated issue cycle of the last dispatched
     *  instruction, the number issued in that cycle, whether one of them
     *  was a memory reference (which ends Execute's issue for the cycle)
     *  and, by FU, the first cycle it can accept an instruction */
    Cycles ifaModelLastIssue;
    unsigned int ifaModelNumAtLast;
    bool ifaModelMemAtLast;
    std::vector<Cycles> ifaModelFUFree;

    /** Execute, whose scoreboard the DAU reads */
    const Execute &execute;

    /** Earliest number of cycles between an instruction leaving Fetch2
     *  and it reaching issue in Execute */
    const Cycles ifaDispatchToIssue;

    /** An FEB entry */
    struct IFAEntry
    {
        MinorDynInstPtr inst;

        /** Cycle at which the entry can first be dispatched */
        Cycles dispatchableCycle;
    };

    /** The FEB, oldest instruction first.  Entries leave it when they are
     *  dispatched, so the front is always the oldest undispatched
     *  instruction (the head) */
    std::deque<IFAEntry> ifaBuffer;

    /** Shadow scoreboard.  Only its register indexing and returnCycle are
     *  used: returnCycle holds, for each register, the estimated cycle
     *  from which an instruction can issue in Execute after reading the
     *  value of the last dispatched instruction writing that register */
    Scoreboard ifaShadow;

    /** Estimated cycles between a producer and a consumer issuing, by the
     *  producer's OpClass.  Taken from the Execute FU descriptions,
     *  except for memory operations, which use ifaLoadLatency */
    std::vector<Cycles> ifaOpLatency;

    /** Relative latency at which an integer consumer reads its sources,
     *  used when comparing against Execute's own returnCycle */
    Cycles ifaSrcRelativeLatency;

    /** Estimated cycle at which instructions dispatched in this cycle reach
     *  issue in Execute */
    Cycles ifaArrival;

    /** Youngest (largest fetchSeqNum) instruction dispatched so far */
    InstSeqNum ifaMaxDispatchedSeqNum;

  public:
    /* Public so that Pipeline can pass it to Fetch1 */
    std::vector<InputBuffer<ForwardLineData>> inputBuffer;

  protected:
    /** Data members after this line are cycle-to-cycle state */

    struct Fetch2ThreadInfo
    {
        Fetch2ThreadInfo() {}

        Fetch2ThreadInfo(const Fetch2ThreadInfo& other) :
            inputIndex(other.inputIndex),
            havePC(other.havePC),
            lastStreamSeqNum(other.lastStreamSeqNum),
            expectedStreamSeqNum(other.expectedStreamSeqNum),
            predictionSeqNum(other.predictionSeqNum),
            blocked(other.blocked)
        {
            set(pc, other.pc);
        }

        /** Index into an incompletely processed input line that instructions
         *  are to be extracted from */
        unsigned int inputIndex = 0;


        /** Remembered program counter value.  Between contiguous lines, this
         *  is just updated with advancePC.  For lines following changes of
         *  stream, a new PC must be loaded and havePC be set.
         *  havePC is needed to accomodate instructions which span across
         *  lines meaning that Fetch2 and the decoder need to remember a PC
         *  value and a partially-offered instruction from the previous line */
        std::unique_ptr<PCStateBase> pc;

        /** PC is currently valid.  Initially false, gets set to true when a
         *  change-of-stream line is received and false again when lines are
         *  discarded for any reason */
        bool havePC = false;

        /** Stream sequence number of the last seen line used to identify
         *  changes of instruction stream */
        InstSeqNum lastStreamSeqNum = InstId::firstStreamSeqNum;

        /** Fetch2 is the source of fetch sequence numbers.  These represent the
         *  sequence that instructions were extracted from fetched lines. */
        InstSeqNum fetchSeqNum = InstId::firstFetchSeqNum;

        /** Stream sequence number remembered from last time the
         *  predictionSeqNum changed.  Lines should only be discarded when their
         *  predictionSeqNums disagree with Fetch2::predictionSeqNum *and* they
         *  are from the same stream that bore that prediction number */
        InstSeqNum expectedStreamSeqNum = InstId::firstStreamSeqNum;

        /** Fetch2 is the source of prediction sequence numbers.  These
         *  represent predicted changes of control flow sources from branch
         *  prediction in Fetch2. */
        InstSeqNum predictionSeqNum = InstId::firstPredictionSeqNum;

        /** Blocked indication for report */
        bool blocked = false;
    };

    std::vector<Fetch2ThreadInfo> fetchInfo;
    ThreadID threadPriority;

    struct Fetch2Stats : public statistics::Group
    {
        Fetch2Stats(MinorCPU *cpu, unsigned int ifa_buffer_size);
        /** Stats */
        statistics::Scalar totalInstructions;
        statistics::Scalar intInstructions;
        statistics::Scalar fpInstructions;
        statistics::Scalar vecInstructions;
        statistics::Scalar loadInstructions;
        statistics::Scalar storeInstructions;
        statistics::Scalar amoInstructions;

        /** IFA stats */
        statistics::Scalar ifaDispatched;
        statistics::Scalar ifaDispatchedOutOfOrder;
        statistics::Scalar ifaHoisted;
        statistics::Scalar ifaSkipped;
        statistics::Scalar ifaHeadBlockedCycles;
        statistics::Scalar ifaReorderCycles;
        statistics::Scalar ifaBufferFullCycles;
        statistics::Scalar ifaFlushes;
        statistics::Scalar ifaFlushedInsts;
        statistics::Distribution ifaOccupancy;
    } stats;

  protected:
    /** Get a piece of data to work on from the inputBuffer, or 0 if there
     *  is no data. */
    const ForwardLineData *getInput(ThreadID tid);

    /** Pop an element off the input buffer, if there are any */
    void popInput(ThreadID tid);

    /** Dump the whole contents of the input buffer.  Useful after a
     *  prediction changes control flow */
    void dumpAllInput(ThreadID tid);

    /** Update local branch prediction structures from feedback from
     *  Execute. */
    void updateBranchPrediction(const BranchData &branch);

    /** Predicts branches for the given instruction.  Updates the
     *  instruction's predicted... fields and also the branch which
     *  carries the prediction to Fetch1 */
    void predictBranch(MinorDynInstPtr inst, BranchData &branch);

    /** Use the current threading policy to determine the next thread to
     *  fetch from. */
    ThreadID getScheduledThread();

    /** IFA: does this instruction have to leave the FEB in program order?
     *  Nothing younger may pass it and it may not pass anything older */
    bool ifaIsBarrier(const MinorDynInstPtr &inst) const;

    /** IFA: DAU test that all the sources of inst will be available when
     *  it reaches issue, if it is dispatched now */
    bool ifaSourcesReady(const MinorDynInstPtr &inst);

    /** IFA: DAU test that inst can be dispatched before all the (older)
     *  instructions in skipped, which are still in the FEB */
    bool ifaCanPass(const MinorDynInstPtr &inst,
        const std::vector<MinorDynInstPtr> &skipped) const;

    /** IFA: note the destinations of a dispatched instruction in the shadow
     *  scoreboard */
    void ifaMarkupDispatch(const MinorDynInstPtr &inst);

    /** IFA: dispatch instructions from the FEB into insts_out */
    void ifaDispatch(ForwardInstData &insts_out);

    /** IFA issue model: the cycle in which inst would issue in Execute if
     *  it were dispatched next */
    Cycles ifaEstimateIssue(const MinorDynInstPtr &inst) const;

    /** IFA issue model: account for inst being dispatched to issue at
     *  issue_cycle */
    void ifaModelDispatch(const MinorDynInstPtr &inst, Cycles issue_cycle);

  public:
    Fetch2(const std::string &name,
        MinorCPU &cpu_,
        const BaseMinorCPUParams &params,
        const Execute &execute_,
        Latch<ForwardLineData>::Output inp_,
        Latch<BranchData>::Output branchInp_,
        Latch<BranchData>::Input predictionOut_,
        Latch<ForwardInstData>::Input out_,
        std::vector<InputBuffer<ForwardInstData>> &next_stage_input_buffer);

  public:
    /** Pass on input/buffer data to the output if you can */
    void evaluate();

    void minorTrace() const;


    /** Is this stage drained?  For Fetch2, draining is initiated by
     *  Execute halting Fetch1 causing Fetch2 to naturally drain.
     *  Branch predictions are ignored by Fetch1 during halt */
    bool isDrained();
};

} // namespace minor
} // namespace gem5

#endif /* __CPU_MINOR_FETCH2_HH__ */
