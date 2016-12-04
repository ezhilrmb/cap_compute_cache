#ifndef PERFORMANCE_MODEL_H
#define PERFORMANCE_MODEL_H
// This class represents the actual performance model for a given core

#include "instruction.h"
#include "fixed_types.h"
#include "mt_circular_queue.h"
#include "lock.h"
#include "dynamic_instruction_info.h"
#include "subsecond_time.h"
#include "instruction_tracer.h"

#include <queue>
#include <iostream>
#include "magic_server.h"

// Forward Decls
class Core;
class BranchPredictor;
class FastforwardPerformanceModel;

class PerformanceModel
{
public:
	 //#ifdef PIC_IS_MICROBENCH
   	void psuedo_iterate(bool is_last_loop, int target_instruction_cnt);
	 	Instruction *dummy_inst;
	 //#endif
   static const SubsecondTime DyninsninfoNotAvailable() { return SubsecondTime::MaxTime(); }

   PerformanceModel(Core* core);
   virtual ~PerformanceModel();

   void queueDynamicInstruction(Instruction *i);
   void queueInstruction(Instruction *i, bool is_pic_ins = false);
   void handleIdleInstruction(Instruction *instruction);
   void iterate();
   virtual void synchronize();

   UInt64 getInstructionCount() const { return m_instruction_count; }

   SubsecondTime getElapsedTime() const { return m_elapsed_time.getElapsedTime(); }
   SubsecondTime getNonIdleElapsedTime() const { return getElapsedTime() - m_idle_elapsed_time.getElapsedTime(); }

   void countInstructions(IntPtr address, UInt32 count);
   void handleMemoryLatency(SubsecondTime latency, HitWhere::where_t hit_where);
   void handleBranchMispredict();

   void pushDynamicInstructionInfo(DynamicInstructionInfo &i, bool is_pic_ins = false);
   void popDynamicInstructionInfo();
   DynamicInstructionInfo* getDynamicInstructionInfo(const Instruction &instruction, bool exec_loads = true);

   static PerformanceModel *create(Core* core);

   BranchPredictor *getBranchPredictor() { return m_bp; }
   BranchPredictor const* getConstBranchPredictor() const { return m_bp; }

   FastforwardPerformanceModel *getFastforwardPerformanceModel() { return m_fastforward_model; }
   FastforwardPerformanceModel const* getFastforwardPerformanceModel() const { return m_fastforward_model; }

   void traceInstruction(const DynamicMicroOp *uop, InstructionTracer::uop_times_t *times)
   {
      if (m_instruction_tracer)
         m_instruction_tracer->traceInstruction(uop, times);
   }

   virtual void barrierEnter() { }
   virtual void barrierExit() { }

   void disable();
   void enable();
   bool isEnabled() { return m_enabled; }
   void setHold(bool hold) { m_hold = hold; }

   bool isFastForward() { return m_fastforward; }
   void setFastForward(bool fastforward, bool detailed_sync = true)
   {
      if (m_fastforward == fastforward)
         return;
      m_fastforward = fastforward;
      m_detailed_sync = detailed_sync;
      // Fastforward performance model has controlled time for a while, now let the detailed model know time has advanced
      if (fastforward == false)
      {
         enableDetailedModel();
         notifyElapsedTimeUpdate();
      }
      else
         disableDetailedModel();
   }
   void setIgnoreFunctionalMode() { m_ignore_functional_model = true;}
   void resetIgnoreFunctionalMode() { m_ignore_functional_model = false;}

protected:
   friend class SpawnInstruction;
   friend class FastforwardPerformanceModel;

   void setElapsedTime(SubsecondTime time);
   void incrementElapsedTime(SubsecondTime time) { m_elapsed_time.addLatency(time); }
   void incrementIdleElapsedTime(SubsecondTime time);

   #ifdef ENABLE_PERF_MODEL_OWN_THREAD
      typedef MTCircularQueue<DynamicInstructionInfo> DynamicInstructionInfoQueue;
      typedef MTCircularQueue<Instruction *> InstructionQueue;
   #else
      typedef CircularQueue<DynamicInstructionInfo> DynamicInstructionInfoQueue;
      typedef CircularQueue<Instruction *> InstructionQueue;
   #endif

   Core* getCore() { return m_core; }

private:

   DynamicInstructionInfo* getDynamicInstructionInfo();

   // Simulate a single instruction
   virtual bool handleInstruction(Instruction const* instruction) = 0;

   // When time is jumped ahead outside of control of the performance model (synchronization instructions, etc.)
   // notify it here. This may be used to synchronize internal time or to flush various instruction queues
   virtual void notifyElapsedTimeUpdate() {}
   // Called when the detailed model is enabled/disabled. Used to release threads from the SMT barrier.
   virtual void enableDetailedModel() {}
   virtual void disableDetailedModel() {}

   Core* m_core;

   bool m_enabled;

   bool m_fastforward;
   FastforwardPerformanceModel* m_fastforward_model;
   bool m_detailed_sync;

   bool m_hold;
   bool m_ignore_functional_model;

protected:
   UInt64 m_instruction_count;

   ComponentTime m_elapsed_time;
private:
   ComponentTime m_idle_elapsed_time;

   SubsecondTime m_cpiStartTime;
   // CPI components for Sync and Recv instructions
   SubsecondTime m_cpiSyncFutex;
   SubsecondTime m_cpiSyncPthreadMutex;
   SubsecondTime m_cpiSyncPthreadCond;
   SubsecondTime m_cpiSyncPthreadBarrier;
   SubsecondTime m_cpiSyncJoin;
   SubsecondTime m_cpiSyncPause;
   SubsecondTime m_cpiSyncSleep;
   SubsecondTime m_cpiSyncSyscall;
   SubsecondTime m_cpiSyncUnscheduled;
   SubsecondTime m_cpiSyncDvfsTransition;
   SubsecondTime m_cpiRecv;

   InstructionQueue m_instruction_queue;
   DynamicInstructionInfoQueue m_dynamic_info_queue;

   UInt32 m_current_ins_index;

   BranchPredictor *m_bp;

   InstructionTracer *m_instruction_tracer;
   static SInt64 hookProcessAppMagic(UInt64 object, UInt64 argument) {
   	((PerformanceModel*)object)->processAppMagic(argument); return 0;
   }
   void processAppMagic(UInt64 argument);
};

#endif