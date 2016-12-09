#pragma once

#include "memory_manager_base.h"
#include "cache_base.h"
#include "cache_cntlr.h"
#include "../pr_l1_pr_l2_dram_directory_msi/dram_directory_cntlr.h"
#include "../pr_l1_pr_l2_dram_directory_msi/dram_cntlr.h"
#include "address_home_lookup.h"
#include "../pr_l1_pr_l2_dram_directory_msi/shmem_msg.h"
#include "mem_component.h"
#include "semaphore.h"
#include "fixed_types.h"
#include "shmem_perf_model.h"
#include "shared_cache_block_info.h"
#include "subsecond_time.h"

#include <map>
#include "hooks_manager.h"

class DramCache;
class ShmemPerf;

// CAP: another way to extract cache size/number of rows per sub-array
#define CACHE_LINES_PER_SUBARRAY 256
#define NUM_SUBARRAYS 1

#define PIC_IS_MICROBENCH_COPY 0 
#define PIC_IS_MICROBENCH_CMP 1
#define PIC_IS_MICROBENCH_SEARCH 2
#define PIC_IS_MICROBENCH_LOGICAL 3	//AND/OR etc
#define PIC_IS_MICROBENCH_BMM 4	//use mutiple CLMULT
#define PIC_IS_MICROBENCH_FASTBIT 5	//fastbit modelling

namespace ParametricDramDirectoryMSI
{
   class TLB;

   typedef std::pair<core_id_t, MemComponent::component_t> CoreComponentType;
   typedef std::map<CoreComponentType, CacheCntlr*> CacheCntlrMap;

   class MemoryManager : public MemoryManagerBase
   {
      private:
         CacheCntlr* m_cache_cntlrs[MemComponent::LAST_LEVEL_CACHE + 1];
         NucaCache* m_nuca_cache;
         DramCache* m_dram_cache;
         PrL1PrL2DramDirectoryMSI::DramDirectoryCntlr* m_dram_directory_cntlr;
         PrL1PrL2DramDirectoryMSI::DramCntlr* m_dram_cntlr;
         AddressHomeLookup* m_tag_directory_home_lookup;
         AddressHomeLookup* m_dram_controller_home_lookup;
         TLB *m_itlb, *m_dtlb, *m_stlb;
         ComponentLatency m_tlb_miss_penalty;
         ComponentLatency m_ss_program_time; 
         UInt32 m_min_dummy_inst;

         Instruction *dummy_inst;

         bool m_tlb_miss_parallel;

         core_id_t m_core_id_master;

         bool m_tag_directory_present;
         bool m_dram_cntlr_present;

         Semaphore* m_user_thread_sem;
         Semaphore* m_network_thread_sem;

         UInt32 m_cache_block_size;
         MemComponent::component_t m_last_level_cache;
         bool m_enabled;

         // Performance Models
         CachePerfModel* m_cache_perf_models[MemComponent::LAST_LEVEL_CACHE + 1];

         // Global map of all caches on all cores (within this process!)
         static CacheCntlrMap m_all_cache_cntlrs;

         void accessTLB(TLB * tlb, IntPtr address, bool isIfetch, Core::MemModeled modeled);

         //CAP: CAP Mode Enable Ops
         bool m_cap_on;
         struct CAPInsInfo {
						CacheCntlr::cap_ops_t op;
						IntPtr addr;	
                        Byte* cap_data_buf;
					};

         //CAP 
         typedef std::unordered_map<IntPtr, struct CAPInsInfo> CAPInsInfoMap;
					CAPInsInfoMap capInsInfoMap;

         //CAP: Do we need a pic_ops_t equivalent/or a struct called PicInsInfo?
         //CAP: Begin- CAP-apps

          //CAP: Initializing CAP 
          void init_cacheprogram(Byte* cap_file);
          void init_ssprogram(Byte* ss_pgm_file);
          void init_pattern_match(Byte* match_file);
          void init_rep_ste_program(Byte* ste_file);


          void create_cache_program_instructions(Byte* cap_file);
          void create_cap_ss_instructions(Byte* ss_file);
          void create_cap_match_instructions(Byte* match_file);
          void create_cap_rep_ste_instructions(Byte* ste_file);
          void schedule_cap_instructions();
          void create_schedule_dummy_instructions();

          std::vector< Instruction *> m_cap_ins;
          //CAP: TODO Do you need this? Why was it there in PIC?
          std::vector<DynamicInstructionInfo> m_cap_dyn_ins_info;

					//#ifdef PIC_ENABLE_CHECKPOINT
      			static const UInt32 CHKPT_PAGE_SHIFT = 12; // 4KB
      			static const IntPtr CHKPT_PAGE_SIZE = (1L << CHKPT_PAGE_SHIFT);
				
						//Configurable
						bool m_chkpt_run;
      			IntPtr m_chkpt_opsize;
						IntPtr m_chkpt_interval;

						std::set<IntPtr> m_app_pages;
						std::set<IntPtr> m_checkpointed_pages;
						std::set<IntPtr> m_no_checkpointed_pages;
						bool m_is_checkpointing;
						IntPtr m_last_checkpoint_page_addr;
      			SubsecondTime m_last_checkpoint_time;
						std::vector< Instruction *> m_chkpt_loads;
						std::vector< Instruction *> m_chkpt_stores;
						std::vector< DynamicInstructionInfo> m_chkpt_l_dyn_ins_info;
						std::vector< DynamicInstructionInfo> m_chkpt_s_dyn_ins_info;
						IntPtr m_chkpt_load_inst_addr, m_chkpt_store_inst_addr;
						std::vector<int> m_chkpt_regs;

						bool startNewInterval();
      			void stopCheckpointing() { m_is_checkpointing = false; }
						//we don't want to checkpoint in warmup
						void startCheckpointing(); 
						bool isCheckpointing () const {return m_is_checkpointing; }
						bool needs_checkpointing(IntPtr address);
						void inspect_page_access(IntPtr address);
						void take_checkpoint(IntPtr address);
						void create_checkpoint_instructions(int num_pairs, 
							IntPtr l_address, IntPtr s_address);
						UInt64 m_num_checkpoints, m_num_intervals;

      			IntPtr m_last_chkpt_ins_cnt;
						IntPtr getCheckpointInstructionsCount();
					//#endif

				 	//#ifdef PIC_ENABLE_OPERATIONS
					bool m_pic_on;
					bool m_pic_use_vpic;
					//Store 
					struct PicInsInfo {
						CacheCntlr::pic_ops_t op;
						IntPtr other_source;	
						IntPtr other_source2;	
						bool is_vpic; //is this a vector pic instruction
						UInt32 count;	
					};
   				typedef std::unordered_map<IntPtr, struct PicInsInfo> PicInsInfoMap;
					PicInsInfoMap picInsInfoMap;
					void create_pic_checkpoint_instructions(int num_pairs, 
						IntPtr l_address, IntPtr s_address);
					void create_vpic_checkpoint_instructions(int num_pairs, 
						IntPtr l_address, IntPtr s_address);
					//#endif

					//#ifdef PIC_IS_MICROBENCH
						bool m_microbench_run;
						int m_microbench_type;	//copy=0, comp=1, search=2
      			IntPtr m_microbench_opsize;
      			IntPtr m_microbench_loopsize;
      			IntPtr m_microbench_totalsize;
						IntPtr m_microbench_loops;
						IntPtr m_microbench_outer_loops;

						//Based on the type of benchmark, insert instructions
						void create_microbench_instructions();

						void create_microbench_copy_instructions();
						void create_microbench_pic_copy_instructions();

						void create_microbench_comp_instructions();
						void create_microbench_pic_comp_instructions();

						void create_microbench_search_instructions();
						void create_microbench_pic_search_instructions();

						void schedule_microbench_copy_instructions();
						void schedule_microbench_comp_instructions();
						void schedule_microbench_search_instructions();

						//void create_microbench_logical_instructions();
						//void create_microbench_pic_logical_instructions();
						//void schedule_microbench_logical_instructions();

						void create_microbench_bmm_instructions();
						void create_microbench_pic_bmm_instructions();
						void schedule_microbench_bmm_instructions();
						Instruction* create_single_load(IntPtr ins_addr, int reg);
						Instruction* create_single_store(IntPtr ins_addr, int reg,	
									bool is_pic=false);
						Instruction* create_single_cmp(IntPtr ins_addr, 
														int reg1, int reg2, int reg3);

						void create_microbench_fastbit_instructions();
						void create_microbench_pic_fastbit_instructions();
						void schedule_microbench_fastbit_instructions();

						std::vector< Instruction *> m_mbench_src_ins;
						std::vector< Instruction *> m_mbench_dest_ins;
						std::vector< Instruction *> m_mbench_comp_ins;
						std::vector< DynamicInstructionInfo> m_mbench_src_dyn_ins_info;
						std::vector< DynamicInstructionInfo> m_mbench_dest_dyn_ins_info;
						IntPtr m_mbench_src_inst_addr, m_mbench_dest_inst_addr, m_mbench_comp_inst_addr;
						IntPtr m_mbench_src_addr, m_mbench_dest_addr;
						IntPtr m_mbench_src2_addr, m_mbench_spare_addr;
						std::vector<int> m_mbench_regs;

						bool more_microbench_loops() {
												return (m_microbench_loops > 1) || (m_microbench_outer_loops > 1);
						}
						IntPtr microbench_target_ins_cnt();
					//#endif

					//Begin- pic-apps
   				static SInt64 hookProcessAppMagic(UInt64 object, UInt64 argument) {
      			((MemoryManager*)object)->processAppMagic(argument); return 0;
   				}
   				void processAppMagic(UInt64 argument);

					void init_strmatch( UInt64 word_size);
					void init_wordcount(UInt64 cam_id, UInt64 num_words);

					void create_app_search_instructions_stash(IntPtr max_search_size, int word_size, int key_count, bool is_strcmp);
					int create_app_search_instructions(int word_size, int key_count, bool is_strcmp);
					void schedule_app_search_instructions(int words_per_search, int key_count, bool is_strcmp);

					std::vector< Instruction *> m_app_search_ins;
					std::vector< Instruction *> m_app_maskcomp_ins;
					std::vector< DynamicInstructionInfo> m_app_dyn_ins_info;

					std::vector< Instruction *> m_app_search_ins_stash;
					std::vector< Instruction *> m_app_maskcomp_ins_stash;
					std::vector< DynamicInstructionInfo> m_app_dyn_ins_info_stash;

					std::vector<int> m_app_regs;
   				typedef std::vector<std::pair<IntPtr, struct PicInsInfo> >PicInsInfoVec;
					PicInsInfoVec picInsInfoVec;
					IntPtr m_app_key_addr;
					IntPtr m_app_data_addr;
					IntPtr m_app_search_inst_addr;
					IntPtr m_app_mask_inst_addr;
					IntPtr m_app_comp_inst_addr;
					IntPtr m_app_search_size;
					IntPtr m_wc_cam_size;
					//End- pic-apps

      public:

         MemoryManager(Core* core, Network* network, ShmemPerfModel* shmem_perf_model);
         ~MemoryManager();

         UInt64 getCacheBlockSize() const { return m_cache_block_size; }

         Cache* getCache(MemComponent::component_t mem_component) {
              return m_cache_cntlrs[mem_component == MemComponent::LAST_LEVEL_CACHE ? MemComponent::component_t(m_last_level_cache) : mem_component]->getCache();
         }
         Cache* getL1ICache() { return getCache(MemComponent::L1_ICACHE); }
         Cache* getL1DCache() { return getCache(MemComponent::L1_DCACHE); }
         Cache* getLastLevelCache() { return getCache(MemComponent::LAST_LEVEL_CACHE); }
         PrL1PrL2DramDirectoryMSI::DramDirectoryCache* getDramDirectoryCache() { return m_dram_directory_cntlr->getDramDirectoryCache(); }
         PrL1PrL2DramDirectoryMSI::DramCntlr* getDramCntlr() { return m_dram_cntlr; }
         AddressHomeLookup* getTagDirectoryHomeLookup() { return m_tag_directory_home_lookup; }
         AddressHomeLookup* getDramControllerHomeLookup() { return m_dram_controller_home_lookup; }

         CacheCntlr* getCacheCntlrAt(core_id_t core_id, MemComponent::component_t mem_component) { return m_all_cache_cntlrs[CoreComponentType(core_id, mem_component)]; }
         void setCacheCntlrAt(core_id_t core_id, MemComponent::component_t mem_component, CacheCntlr* cache_cntlr) { m_all_cache_cntlrs[CoreComponentType(core_id, mem_component)] = cache_cntlr; }

         HitWhere::where_t coreInitiateMemoryAccess(
               MemComponent::component_t mem_component,
               Core::lock_signal_t lock_signal,
               Core::mem_op_t mem_op_type,
               IntPtr address, UInt32 offset,
               Byte* data_buf, UInt32 data_length,
               Core::MemModeled modeled);

         void handleMsgFromNetwork(NetPacket& packet);

         void sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, HitWhere::where_t where = HitWhere::UNKNOWN, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS , IntPtr other_pic_address = 0, IntPtr other_pic_address2 = 0);

         void broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);

         SubsecondTime getL1HitLatency(void) { return m_cache_perf_models[MemComponent::L1_ICACHE]->getLatency(CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS); }
         void addL1Hits(bool icache, Core::mem_op_t mem_op_type, UInt64 hits) {
            (icache ? m_cache_cntlrs[MemComponent::L1_ICACHE] : m_cache_cntlrs[MemComponent::L1_DCACHE])->updateHits(mem_op_type, hits);
         }

         void enableModels();
         void disableModels();

         void showCapInsInfoMap();

         core_id_t getShmemRequester(const void* pkt_data)
         { return ((PrL1PrL2DramDirectoryMSI::ShmemMsg*) pkt_data)->getRequester(); }

         UInt32 getModeledLength(const void* pkt_data)
         { return ((PrL1PrL2DramDirectoryMSI::ShmemMsg*) pkt_data)->getModeledLength(); }

         SubsecondTime getCost(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type);
         void incrElapsedTime(SubsecondTime latency, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);
         void incrElapsedTime(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);
   };
}
