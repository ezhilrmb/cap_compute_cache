#include "core_manager.h"
#include "memory_manager.h"
#include "cache_base.h"
#include "nuca_cache.h"
#include "dram_cache.h"
#include "tlb.h"
#include "simulator.h"
#include "log.h"
#include "dvfs_manager.h"
#include "itostr.h"
#include "instruction.h"
#include "performance_model.h"
#include "config.hpp"
#include "distribution.h"
#include "topology_info.h"

//#ifdef PIC_IS_MICROBENCH
	#include "micro_op.h"
//#endif
#include <algorithm>

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stderr); fprintf(stderr, "[%s] %d%cmm %-25s@%03u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); }
#else
#  define MYLOG(...) {}
#endif


namespace ParametricDramDirectoryMSI
{

std::map<CoreComponentType, CacheCntlr*> MemoryManager::m_all_cache_cntlrs;

MemoryManager::MemoryManager(Core* core,
      Network* network, ShmemPerfModel* shmem_perf_model):
   MemoryManagerBase(core, network, shmem_perf_model),
   m_nuca_cache(NULL),
   m_dram_cache(NULL),
   m_dram_directory_cntlr(NULL),
   m_dram_cntlr(NULL),
   m_itlb(NULL), m_dtlb(NULL), m_stlb(NULL),
   m_tlb_miss_penalty(NULL,0),
   m_tlb_miss_parallel(false),
   m_tag_directory_present(false),
   m_dram_cntlr_present(false),
   m_enabled(false)
{
   // Read Parameters from the Config file
   std::map<MemComponent::component_t, CacheParameters> cache_parameters;
   std::map<MemComponent::component_t, String> cache_names;

   bool nuca_enable = false;
   CacheParameters nuca_parameters;

   const ComponentPeriod *global_domain = Sim()->getDvfsManager()->getGlobalDomain();

   UInt32 smt_cores;
   bool dram_direct_access = false;
   UInt32 dram_directory_total_entries = 0;
   UInt32 dram_directory_associativity = 0;
   UInt32 dram_directory_max_num_sharers = 0;
   UInt32 dram_directory_max_hw_sharers = 0;
   String dram_directory_type_str;
   UInt32 dram_directory_home_lookup_param = 0;
   ComponentLatency dram_directory_cache_access_time(global_domain, 0);

   try
   {
      m_cache_block_size = Sim()->getCfg()->getInt("perf_model/l1_icache/cache_block_size");

      m_last_level_cache = (MemComponent::component_t)(Sim()->getCfg()->getInt("perf_model/cache/levels") - 2 + MemComponent::L2_CACHE);

      UInt32 stlb_size = Sim()->getCfg()->getInt("perf_model/stlb/size");
      if (stlb_size)
         m_stlb = new TLB("stlb", "perf_model/stlb", getCore()->getId(), stlb_size, Sim()->getCfg()->getInt("perf_model/stlb/associativity"), NULL);
      UInt32 itlb_size = Sim()->getCfg()->getInt("perf_model/itlb/size");
      if (itlb_size)
         m_itlb = new TLB("itlb", "perf_model/itlb", getCore()->getId(), itlb_size, Sim()->getCfg()->getInt("perf_model/itlb/associativity"), m_stlb);
      UInt32 dtlb_size = Sim()->getCfg()->getInt("perf_model/dtlb/size");
      if (dtlb_size)
         m_dtlb = new TLB("dtlb", "perf_model/dtlb", getCore()->getId(), dtlb_size, Sim()->getCfg()->getInt("perf_model/dtlb/associativity"), m_stlb);
      m_tlb_miss_penalty = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/tlb/penalty"));
      m_tlb_miss_parallel = Sim()->getCfg()->getBool("perf_model/tlb/penalty_parallel");

      smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");

      for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
      {
         String configName, objectName;
         switch((MemComponent::component_t)i) {
            case MemComponent::L1_ICACHE:
               configName = "l1_icache";
               objectName = "L1-I";
               break;
            case MemComponent::L1_DCACHE:
               configName = "l1_dcache";
               objectName = "L1-D";
               break;
            default:
               String level = itostr(i - MemComponent::L2_CACHE + 2);
               configName = "l" + level + "_cache";
               objectName = "L" + level;
               break;
         }

         const ComponentPeriod *clock_domain = NULL;
         String domain_name = Sim()->getCfg()->getStringArray("perf_model/" + configName + "/dvfs_domain", core->getId());
         if (domain_name == "core")
            clock_domain = core->getDvfsDomain();
         else if (domain_name == "global")
            clock_domain = global_domain;
         else
            LOG_PRINT_ERROR("dvfs_domain %s is invalid", domain_name.c_str());

         cache_parameters[(MemComponent::component_t)i] = CacheParameters(
            configName,
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/cache_size", core->getId()),
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/associativity", core->getId()),
            getCacheBlockSize(),
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/address_hash", core->getId()),
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/replacement_policy", core->getId()),
            Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/perfect", core->getId()),
            i == MemComponent::L1_ICACHE
               ? Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/coherent", core->getId())
               : true,
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/data_access_time", core->getId())),
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/tags_access_time", core->getId())),
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/writeback_time", core->getId())),
            ComponentBandwidthPerCycle(clock_domain,
               i < (UInt32)m_last_level_cache
                  ? Sim()->getCfg()->getIntArray("perf_model/" + configName + "/next_level_read_bandwidth", core->getId())
                  : 0),
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/perf_model_type", core->getId()),
            Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/writethrough", core->getId()),
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/shared_cores", core->getId()) * smt_cores,
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/prefetcher", core->getId()),
            i == MemComponent::L1_DCACHE
               ? Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/outstanding_misses", core->getId())
               : 0,
            i == MemComponent::L1_DCACHE
               ? Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/pic_outstanding", core->getId())
               : 0
         );
         cache_names[(MemComponent::component_t)i] = objectName;

         /* Non-application threads will be distributed at 1 per process, probably not as shared_cores per process.
            Still, they need caches (for inter-process data communication, not for modeling target timing).
            Make them non-shared so we don't create process-spanning shared caches. */
         if (getCore()->getId() >= (core_id_t) Sim()->getConfig()->getApplicationCores())
            cache_parameters[(MemComponent::component_t)i].shared_cores = 1;
      }

      nuca_enable = Sim()->getCfg()->getBoolArray(  "perf_model/nuca/enabled", core->getId());
      if (nuca_enable)
      {
         nuca_parameters = CacheParameters(
            "nuca",
            Sim()->getCfg()->getIntArray(   "perf_model/nuca/cache_size", core->getId()),
            Sim()->getCfg()->getIntArray(   "perf_model/nuca/associativity", core->getId()),
            getCacheBlockSize(),
            Sim()->getCfg()->getStringArray("perf_model/nuca/address_hash", core->getId()),
            Sim()->getCfg()->getStringArray("perf_model/nuca/replacement_policy", core->getId()),
            false, true,
            ComponentLatency(global_domain, Sim()->getCfg()->getIntArray("perf_model/nuca/data_access_time", core->getId())),
            ComponentLatency(global_domain, Sim()->getCfg()->getIntArray("perf_model/nuca/tags_access_time", core->getId())),
            ComponentLatency(global_domain, 0), ComponentBandwidthPerCycle(global_domain, 0), "", false, 0, "", 0, 0 // unused
         );
      }

      // Dram Directory Cache
      dram_directory_total_entries = Sim()->getCfg()->getInt("perf_model/dram_directory/total_entries");
      dram_directory_associativity = Sim()->getCfg()->getInt("perf_model/dram_directory/associativity");
      dram_directory_max_num_sharers = Sim()->getConfig()->getTotalCores();
      dram_directory_max_hw_sharers = Sim()->getCfg()->getInt("perf_model/dram_directory/max_hw_sharers");
      dram_directory_type_str = Sim()->getCfg()->getString("perf_model/dram_directory/directory_type");
      dram_directory_home_lookup_param = Sim()->getCfg()->getInt("perf_model/dram_directory/home_lookup_param");
      dram_directory_cache_access_time = ComponentLatency(global_domain, Sim()->getCfg()->getInt("perf_model/dram_directory/directory_cache_access_time"));

      // Dram Cntlr
      dram_direct_access = Sim()->getCfg()->getBool("perf_model/dram/direct_access");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error reading memory system parameters from the config file");
   }

   m_user_thread_sem = new Semaphore(0);
   m_network_thread_sem = new Semaphore(0);

   std::vector<core_id_t> core_list_with_dram_controllers = getCoreListWithMemoryControllers();
   std::vector<core_id_t> core_list_with_tag_directories;
   String tag_directory_locations = Sim()->getCfg()->getString("perf_model/dram_directory/locations");

   if (tag_directory_locations == "dram")
   {
      // Place tag directories only at DRAM controllers
      core_list_with_tag_directories = core_list_with_dram_controllers;
   }
   else
   {
      SInt32 tag_directory_interleaving;

      // Place tag directores at each (master) cache
      if (tag_directory_locations == "llc")
      {
         tag_directory_interleaving = cache_parameters[m_last_level_cache].shared_cores;
      }
      else if (tag_directory_locations == "interleaved")
      {
         tag_directory_interleaving = Sim()->getCfg()->getInt("perf_model/dram_directory/interleaving") * smt_cores;
      }
      else
      {
         LOG_PRINT_ERROR("Invalid perf_model/dram_directory/locations value %s", tag_directory_locations.c_str());
      }

      for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id += tag_directory_interleaving)
      {
         core_list_with_tag_directories.push_back(core_id);
      }
   }

   m_tag_directory_home_lookup = new AddressHomeLookup(dram_directory_home_lookup_param, core_list_with_tag_directories, getCacheBlockSize());
   m_dram_controller_home_lookup = new AddressHomeLookup(dram_directory_home_lookup_param, core_list_with_dram_controllers, getCacheBlockSize());

   // if (m_core->getId() == 0)
   //   printCoreListWithMemoryControllers(core_list_with_dram_controllers);

   if (find(core_list_with_dram_controllers.begin(), core_list_with_dram_controllers.end(), getCore()->getId()) != core_list_with_dram_controllers.end())
   {
      m_dram_cntlr_present = true;

      m_dram_cntlr = new PrL1PrL2DramDirectoryMSI::DramCntlr(this,
            getShmemPerfModel(),
            getCacheBlockSize());
      Sim()->getStatsManager()->logTopology("dram-cntlr", core->getId(), core->getId());

      if (Sim()->getCfg()->getBoolArray("perf_model/dram/cache/enabled", core->getId()))
      {
         m_dram_cache = new DramCache(this, getShmemPerfModel(), m_dram_controller_home_lookup, getCacheBlockSize(), m_dram_cntlr);
         Sim()->getStatsManager()->logTopology("dram-cache", core->getId(), core->getId());
      }
   }

   if (find(core_list_with_tag_directories.begin(), core_list_with_tag_directories.end(), getCore()->getId()) != core_list_with_tag_directories.end())
   {
      m_tag_directory_present = true;

      if (!dram_direct_access)
      {
         if (nuca_enable)
         {
            m_nuca_cache = new NucaCache(
               this,
               getShmemPerfModel(),
               m_tag_directory_home_lookup,
               getCacheBlockSize(),
               nuca_parameters);
            Sim()->getStatsManager()->logTopology("nuca-cache", core->getId(), core->getId());
         }

         m_dram_directory_cntlr = new PrL1PrL2DramDirectoryMSI::DramDirectoryCntlr(getCore()->getId(),
               this,
               m_dram_controller_home_lookup,
               m_nuca_cache,
               dram_directory_total_entries,
               dram_directory_associativity,
               getCacheBlockSize(),
               dram_directory_max_num_sharers,
               dram_directory_max_hw_sharers,
               dram_directory_type_str,
               dram_directory_cache_access_time,
               getShmemPerfModel());
         Sim()->getStatsManager()->logTopology("tag-dir", core->getId(), core->getId());
      }
   }

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i) {
      CacheCntlr* cache_cntlr = new CacheCntlr(
         (MemComponent::component_t)i,
         cache_names[(MemComponent::component_t)i],
         getCore()->getId(),
         this,
         m_tag_directory_home_lookup,
         m_user_thread_sem,
         m_network_thread_sem,
         getCacheBlockSize(),
         cache_parameters[(MemComponent::component_t)i],
         getShmemPerfModel(),
         i == (UInt32)m_last_level_cache
      );
      m_cache_cntlrs[(MemComponent::component_t)i] = cache_cntlr;
      setCacheCntlrAt(getCore()->getId(), (MemComponent::component_t)i, cache_cntlr);
   }

   m_cache_cntlrs[MemComponent::L1_ICACHE]->setNextCacheCntlr(m_cache_cntlrs[MemComponent::L2_CACHE]);
   m_cache_cntlrs[MemComponent::L1_DCACHE]->setNextCacheCntlr(m_cache_cntlrs[MemComponent::L2_CACHE]);
   for(UInt32 i = MemComponent::L2_CACHE; i <= (UInt32)m_last_level_cache - 1; ++i)
      m_cache_cntlrs[(MemComponent::component_t)i]->setNextCacheCntlr(m_cache_cntlrs[(MemComponent::component_t)(i + 1)]);

   CacheCntlrList prev_cache_cntlrs;
   prev_cache_cntlrs.push_back(m_cache_cntlrs[MemComponent::L1_ICACHE]);
   prev_cache_cntlrs.push_back(m_cache_cntlrs[MemComponent::L1_DCACHE]);
   m_cache_cntlrs[MemComponent::L2_CACHE]->setPrevCacheCntlrs(prev_cache_cntlrs);

   for(UInt32 i = MemComponent::L2_CACHE; i <= (UInt32)m_last_level_cache - 1; ++i) {
      CacheCntlrList prev_cache_cntlrs;
      prev_cache_cntlrs.push_back(m_cache_cntlrs[(MemComponent::component_t)i]);
      m_cache_cntlrs[(MemComponent::component_t)(i + 1)]->setPrevCacheCntlrs(prev_cache_cntlrs);
   }

   // Create Performance Models
   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
      m_cache_perf_models[(MemComponent::component_t)i] = CachePerfModel::create(
       cache_parameters[(MemComponent::component_t)i].perf_model_type,
       cache_parameters[(MemComponent::component_t)i].data_access_time,
       cache_parameters[(MemComponent::component_t)i].tags_access_time
      );


   if (m_dram_cntlr_present)
      LOG_ASSERT_ERROR(m_cache_cntlrs[m_last_level_cache]->isMasterCache() == true,
                       "DRAM controllers may only be at 'master' node of shared caches\n"
                       "\n"
                       "Make sure perf_model/dram/controllers_interleaving is a multiple of perf_model/l%d_cache/shared_cores\n",
                       Sim()->getCfg()->getInt("perf_model/cache/levels")
                      );
   if (m_tag_directory_present)
      LOG_ASSERT_ERROR(m_cache_cntlrs[m_last_level_cache]->isMasterCache() == true,
                       "Tag directories may only be at 'master' node of shared caches\n"
                       "\n"
                       "Make sure perf_model/dram_directory/interleaving is a multiple of perf_model/l%d_cache/shared_cores\n",
                       Sim()->getCfg()->getInt("perf_model/cache/levels")
                      );


   // The core id to use when sending messages to the directory (master node of the last-level cache)
   m_core_id_master = getCore()->getId() - getCore()->getId() % cache_parameters[m_last_level_cache].shared_cores;

   if (m_core_id_master == getCore()->getId())
   {
      UInt32 num_sets = cache_parameters[MemComponent::L1_DCACHE].num_sets;
      // With heterogeneous caches, or fancy hash functions, we can no longer be certain that operations
      // only have effect within a set as we see it. Turn of optimization...
      if (num_sets != (1UL << floorLog2(num_sets)))
         num_sets = 1;
      for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
         if (Sim()->getCfg()->getIntArray("perf_model/l1_dcache/cache_size", core_id) != cache_parameters[MemComponent::L1_DCACHE].size)
            num_sets = 1;
         if (Sim()->getCfg()->getStringArray("perf_model/l1_dcache/address_hash", core_id) != "mask")
            num_sets = 1;
         // FIXME: We really should check all cache levels
      }

      m_cache_cntlrs[(UInt32)m_last_level_cache]->createSetLocks(
         getCacheBlockSize(),
         num_sets,
         m_core_id_master,
         cache_parameters[m_last_level_cache].shared_cores
      );
      if (dram_direct_access && getCore()->getId() < (core_id_t)Sim()->getConfig()->getApplicationCores())
      {
         LOG_ASSERT_ERROR(Sim()->getConfig()->getApplicationCores() <= cache_parameters[m_last_level_cache].shared_cores, "DRAM direct access is only possible when there is just a single last-level cache (LLC level %d shared by %d, num cores %d)", m_last_level_cache, cache_parameters[m_last_level_cache].shared_cores, Sim()->getConfig()->getApplicationCores());
         LOG_ASSERT_ERROR(m_dram_cntlr != NULL, "I'm supposed to have direct access to a DRAM controller, but there isn't one at this node");
         m_cache_cntlrs[(UInt32)m_last_level_cache]->setDRAMDirectAccess(
            m_dram_cache ? (DramCntlrInterface*)m_dram_cache : (DramCntlrInterface*)m_dram_cntlr,
            Sim()->getCfg()->getInt("perf_model/llc/evict_buffers"));
      }
   }

   // Register Call-backs
   getNetwork()->registerCallback(SHARED_MEM_1, MemoryManagerNetworkCallback, this);

   // Set up core topology information
   getCore()->getTopologyInfo()->setup(smt_cores, cache_parameters[m_last_level_cache].shared_cores);

   //Cap operations
   Sim()->getHooksManager()->registerHook(HookType::HOOK_MAGIC_MARKER, MemoryManager::hookProcessAppMagic, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);

   //CAP: constructor changes
   m_cap_on = Sim()->getCfg()->getBool("general/cap_on");
   
	
		//#ifdef PIC_ENABLE_CHECKPOINT
			m_chkpt_run	= Sim()->getCfg()->getBool("general/checkpoint_run");
			if(m_chkpt_run) {
	m_chkpt_opsize	= Sim()->getCfg()->getInt("general/checkpoint_opsize");
	m_chkpt_interval = Sim()->getCfg()->getInt("general/checkpoint_interval");
			}
			m_is_checkpointing = false;
			m_last_checkpoint_time = SubsecondTime::Zero(); 
			m_last_chkpt_ins_cnt	= 0;
			m_checkpointed_pages.clear();
			m_chkpt_load_inst_addr 	= 80;
			m_chkpt_store_inst_addr 	= 96;
			m_num_intervals = 0;
			m_num_checkpoints = 0;
			m_chkpt_regs.push_back(76);
			m_chkpt_regs.push_back(77);
			m_chkpt_regs.push_back(78);
			m_chkpt_regs.push_back(79);
			m_chkpt_regs.push_back(80);
			m_chkpt_regs.push_back(81);
			m_chkpt_regs.push_back(82);
			m_chkpt_regs.push_back(83);
			if(m_chkpt_run) {
   			registerStatsMetric("mem-manager", m_core_id_master, "intervals", &m_num_intervals);
   			registerStatsMetric("mem-manager", m_core_id_master, "checkpoints", 
																												&m_num_checkpoints);
			}
		//#endif

		m_pic_on = Sim()->getCfg()->getBool("general/pic_on");
		if(m_pic_on) {
			m_pic_use_vpic = Sim()->getCfg()->getBool("general/pic_use_vpic");
		}

		//#ifdef PIC_IS_MICROBENCH
		m_microbench_run	= Sim()->getCfg()->getBool("general/microbench_run");
		if(m_microbench_run) {
			m_microbench_type	= Sim()->getCfg()->getInt("general/microbench_type");
			//TODO:TADD: how do these calculations change for LOGICAL/BMM
			m_microbench_loopsize	
												= Sim()->getCfg()->getInt("general/microbench_loopsize");
    	m_microbench_opsize	
												= Sim()->getCfg()->getInt("general/microbench_opsize");
    	m_microbench_totalsize	
											= Sim()->getCfg()->getInt("general/microbench_totalsize");
    	m_microbench_outer_loops
											= Sim()->getCfg()->getInt("general/microbench_outer_loops");
			assert(m_microbench_outer_loops);
			m_mbench_src_inst_addr	= 80;
			m_mbench_dest_inst_addr	= 96;
			m_mbench_comp_inst_addr	= 112;
			m_mbench_regs.push_back(76);
			m_mbench_regs.push_back(77);
			m_mbench_regs.push_back(78);
			m_mbench_regs.push_back(79);
			m_mbench_regs.push_back(80);
			m_mbench_regs.push_back(81);
			m_mbench_regs.push_back(82);
			m_mbench_regs.push_back(83);
			m_microbench_loops = m_microbench_totalsize / m_microbench_loopsize;

			//m_mbench_src_addr		= 4096;		//need to be same across runs
			//m_mbench_dest_addr	= m_mbench_src_addr + m_microbench_totalsize;

			m_mbench_src_addr		= 8388608;		//need to be same across runs
			m_mbench_dest_addr	= m_mbench_src_addr + m_microbench_totalsize;
			if(m_microbench_type == PIC_IS_MICROBENCH_BMM) {
				m_mbench_src2_addr	= m_mbench_src_addr 	+ m_microbench_totalsize;
				m_mbench_dest_addr	= m_mbench_src2_addr 	+ m_microbench_totalsize;		
				m_mbench_spare_addr	= m_mbench_dest_addr 	+ m_microbench_totalsize;
			}
			printf("\nRunning microbench(%d), total=%lu, loop=%lu, op=%lu",
			m_microbench_type, m_microbench_totalsize, m_microbench_loopsize, 
			m_microbench_opsize);
		}
		//#endif

		//Begin- pic-apps
		//For stringmatch application
    Sim()->getHooksManager()->registerHook(HookType::HOOK_MAGIC_MARKER, MemoryManager::hookProcessAppMagic, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
		m_app_regs.push_back(76);
		m_app_regs.push_back(77);
		m_app_regs.push_back(78);
		m_app_regs.push_back(79);
		m_app_regs.push_back(80);
		m_app_regs.push_back(81);
		m_app_regs.push_back(82);
		m_app_regs.push_back(83);
		m_app_search_inst_addr 	= 80;
		m_app_mask_inst_addr 		= 96;
		m_app_comp_inst_addr 		= 112;
		m_app_search_size 			= 0;
		m_app_key_addr 					= 4096;
		m_app_data_addr 				= 0;
		m_wc_cam_size	= Sim()->getCfg()->getInt("general/wc_cam_size");
		//End- pic-apps
}

//Begin- pic-apps
void MemoryManager::processAppMagic(UInt64 argument) {
	MagicServer::MagicMarkerType *args_in = 
		(MagicServer::MagicMarkerType *) argument;
	if(getCore()->getId() == 0) {
		if(args_in->str != NULL) {
			std::string marker (args_in->str);
  		if (marker.compare("strm") == 0) {
				//printf("\nSee a marker: %lu, %s", args_in->arg0, args_in->str);
				if(!m_app_search_ins_stash.size())
					create_app_search_instructions_stash(1024, 16, 4, true);
				init_strmatch(args_in->arg0);
			}
  		if (marker.compare("igrb") == 0) {
				unsigned int * array = (unsigned int*) (args_in->arg0);
				if(!m_app_search_ins_stash.size())
					create_app_search_instructions_stash(m_wc_cam_size, 16, 1, false);
				//printf("\nIN(%u,%u)", array[0], array[1]);
				init_wordcount(array[0], array[1]);
			}
      //CAP: initial cache program
      if (marker.compare("cprg") == 0) {
        Byte * cap_pgm_file = (Byte*) (args_in-> arg0);
        printf("CAP: Mem manager - Cache pgm file ptr :0x%p, content: %d", cap_pgm_file, *(cap_pgm_file+3));
        init_cacheprogram(cap_pgm_file);

      } 
      if(marker.compare("ssprg") == 0) {
        Byte * ss_pgm_file =  (Byte*) (args_in-> arg0);
        init_ssprogram(ss_pgm_file);
      }  
      if(marker.compare("match") == 0) {
        Byte * match_file =  (Byte*) (args_in-> arg0);
        init_pattern_match(match_file);
      }        
		}
	}
}

//CAP: Storing the STE-mapped FSMs into the cache 
void  MemoryManager::init_cacheprogram(Byte* cap_file) {
  create_cache_program_instructions(cap_file);
  schedule_cap_instructions();
} 

//CAP: Programming the SS in the Cache Ctlr
void  MemoryManager::init_ssprogram(Byte* ss_file) {
  create_cap_ss_instructions(ss_file);
  schedule_cap_instructions();
} 


//CAP: Providing patterns to the cache to be matched 
void  MemoryManager::init_pattern_match(Byte* match_file) {
//  create_cap_match_instructions(match_file);
//  schedule_cap_instructions();
}

void  MemoryManager::init_strmatch( UInt64 word_size) {
	UInt64 key_count 		= 4;
	m_app_search_size 	= 1024;
	m_app_key_addr 			= 524288;		//TODO:
	m_app_data_addr 		= m_app_key_addr + 4096; 	
	int words_per_search = 
		create_app_search_instructions(word_size, key_count, true);
	schedule_app_search_instructions(words_per_search,key_count, true);
}

void  MemoryManager::init_wordcount(UInt64 cam_id, UInt64 num_words) {
	UInt64 key_count 					= 1;
	IntPtr m_cam_begin_addr 	= 16777216;
	UInt64 single_cam_size 		= m_wc_cam_size; 
	UInt64 word_size 					= 16;

	//TODO: what on earth is this?
	if(num_words == 0)
		return;
	m_app_data_addr = m_cam_begin_addr + (cam_id * single_cam_size); 	
	m_app_search_size = (num_words * word_size);
	IntPtr multiple = ceil(((float)m_app_search_size)/64);
	m_app_search_size = 64 * multiple;

	int data_home = (m_app_data_addr % 8);
	printf("\nWC[%d]:(%lu,%lu):(%lu,%lu->%lu)", data_home, 
	cam_id, num_words,m_app_data_addr, m_app_key_addr, m_app_search_size);

	//Search instructions first
  IntPtr m_app_key_addr_prev = m_app_key_addr;
	int words_per_search = 
		create_app_search_instructions(word_size, key_count, false);
	schedule_app_search_instructions(words_per_search,key_count, false);
	assert(m_app_key_addr == (m_app_key_addr_prev + 64));
	if(multiple > 8) {
		m_app_key_addr = (15*64) + m_app_key_addr_prev;
	}
	else {
		m_app_key_addr = (7*64) + m_app_key_addr_prev;
	}
		m_app_key_addr = (m_app_key_addr > (4096 + (4096 * 8))) ? 4096 : m_app_key_addr;
}

void  MemoryManager::create_app_search_instructions_stash(
	IntPtr max_search_size, int word_size, int key_count, bool is_strcmp) {
	int total_pairs = 0;
	int words_per_search;	
	if(m_pic_use_vpic) {
		total_pairs	= (max_search_size < 512) ? 1 : 
																			(max_search_size/512);
		words_per_search = (total_pairs == 1) ? 
												(max_search_size/word_size)
												:(512/word_size);
	}
	else {
		total_pairs	= max_search_size/64;
		words_per_search = 64/word_size;
	}
	int keys_done			= 0;
	//We are assuming that we are doing a popcount per search
	words_per_search = is_strcmp ? 1 : words_per_search;

	while(keys_done < key_count) {
		int pairs_created = 0;
		while(pairs_created < total_pairs) {
			int words_done = 0;
			OperandList load_list;
			int search_reg	= m_app_regs[0];
			m_app_regs.erase(m_app_regs.begin());
			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list.push_back(Operand(Operand::REG, search_reg, Operand::WRITE, 
																													"", true));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		load_inst->setAddress(m_app_search_inst_addr);
      printf("\n OMG look the inst addr is %lu and the load inst addr is %lu", m_app_search_inst_addr, load_inst->getAddress());
  		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst->setAtomic(false);
  		load_inst->setDisassembly("");
  		std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
  		MicroOp *currentLMicroOp = new MicroOp();
  		currentLMicroOp->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , 8
  		 );
			currentLMicroOp->addDestinationRegister((xed_reg_enum_t)search_reg, ""); 
  		currentLMicroOp->setOperandSize(64); 
  		currentLMicroOp->setInstruction(load_inst);
  		currentLMicroOp->setFirst(true);
  		currentLMicroOp->setLast(true);
  		load_uops->push_back(currentLMicroOp);
  		load_inst->setMicroOps(load_uops);
			m_app_search_ins_stash.push_back(load_inst);

			while(words_done < words_per_search) {
				OperandList mask_list;
				int mask_reg;
				if (!is_strcmp) {
					mask_reg	= m_app_regs[0];
					m_app_regs.erase(m_app_regs.begin());
					m_app_regs.push_back(mask_reg);
					mask_list.push_back(Operand(Operand::REG, search_reg, Operand::READ,
																															"", true));
  				mask_list.push_back(Operand(Operand::REG, mask_reg, Operand::READ, 
																															"", true));
  				mask_list.push_back(Operand(Operand::REG, mask_reg, Operand::WRITE, 
																															"", true));
  				Instruction *mask_inst 	= new GenericInstruction(mask_list);
					mask_inst->setAddress(m_app_mask_inst_addr);
					mask_inst->setSize(4);
					mask_inst->setAtomic(false);
					mask_inst->setDisassembly("");
					std::vector<const MicroOp *> *mask_uops
															= new std::vector<const MicroOp*>();;
  				MicroOp *currentMMicroOp 	= new MicroOp();
  				currentMMicroOp->makeExecute(
  					0, 0
  				  , XED_ICLASS_AND //TODO: xed_decoded_inst_get_iclass(ins)
  				  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  				  , false	//not conditional branch
  				 );
  				currentMMicroOp->addSourceRegister((xed_reg_enum_t)search_reg, "");
  				currentMMicroOp->addSourceRegister((xed_reg_enum_t)mask_reg, "");
  				currentMMicroOp->addDestinationRegister((xed_reg_enum_t)mask_reg, "");
  				currentMMicroOp->setOperandSize(64); 
  				currentMMicroOp->setInstruction(mask_inst);
  				currentMMicroOp->setFirst(true);
  				currentMMicroOp->setLast(true);
  				mask_uops->push_back(currentMMicroOp);
  				mask_inst->setMicroOps(mask_uops);
					m_app_maskcomp_ins_stash.push_back(mask_inst);
				}
				else 
					mask_reg = search_reg;

				OperandList comp_list;
				int comp_reg	= m_app_regs[0];
				m_app_regs.erase(m_app_regs.begin());
				m_app_regs.push_back(comp_reg);
				comp_list.push_back(Operand(Operand::REG, mask_reg, Operand::READ,
																														"", true));
				comp_list.push_back(Operand(Operand::REG, comp_reg, Operand::READ,
																														"", true));
  			comp_list.push_back(Operand(Operand::REG, comp_reg, Operand::WRITE, 
																													"", true));
  			Instruction *comp_inst 	= new GenericInstruction(comp_list);
				comp_inst->setAddress(m_app_comp_inst_addr);
				comp_inst->setSize(4);
				comp_inst->setAtomic(false);
				comp_inst->setDisassembly("");
				std::vector<const MicroOp *> *comp_uops
														= new std::vector<const MicroOp*>();;
  			MicroOp *currentCMicroOp 	= new MicroOp();
  			currentCMicroOp->makeExecute(
  				0, 0
  			  , XED_ICLASS_CMP //TODO: xed_decoded_inst_get_iclass(ins)
  			  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  			  , false	//not conditional branch
  			 );
  			currentCMicroOp->addSourceRegister((xed_reg_enum_t)mask_reg, "");
  			currentCMicroOp->addSourceRegister((xed_reg_enum_t)comp_reg, "");
  			currentCMicroOp->addDestinationRegister((xed_reg_enum_t)comp_reg, "");
  			currentCMicroOp->setOperandSize(64); 
  			currentCMicroOp->setInstruction(comp_inst);
  			currentCMicroOp->setFirst(true);
  			currentCMicroOp->setLast(true);
  			comp_uops->push_back(currentCMicroOp);
  			comp_inst->setMicroOps(comp_uops);
				m_app_maskcomp_ins_stash.push_back(comp_inst);
				words_done++;
			}
			m_app_regs.push_back(search_reg);
			++pairs_created;
		}
		++keys_done;
	}
	printf("\nStash: Searches(%lu), Comp/Mask(%lu)",
		m_app_search_ins_stash.size(), m_app_maskcomp_ins_stash.size());
}

int  MemoryManager::create_app_search_instructions(
	int word_size, int key_count, bool is_strcmp) {
	int total_pairs = 0;
	int words_per_search;	
	if(m_pic_use_vpic) {
		total_pairs	= (m_app_search_size < 512) ? 1 : 
																			(m_app_search_size/512);
		words_per_search = (total_pairs == 1) ? 
												(m_app_search_size/word_size)
												:(512/word_size);
	}
	else {
		total_pairs	= m_app_search_size/64;
		words_per_search = 64/word_size;
	}
	int keys_done			= 0;
	int mask_cnt = 0;
	int search_cnt = 0;
	//We are assuming that we are doing a popcount per search
	words_per_search = is_strcmp ? 1 : words_per_search;
	while(keys_done < key_count) {
		int pairs_created = 0;
		while(pairs_created < total_pairs) {
			int words_done = 0;
			m_app_search_ins.push_back(m_app_search_ins_stash[search_cnt]);
			search_cnt++;
			while(words_done < words_per_search) {
				if(!is_strcmp) {
					m_app_maskcomp_ins.push_back(m_app_maskcomp_ins_stash[mask_cnt]);
					mask_cnt++;
				}
				m_app_maskcomp_ins.push_back(m_app_maskcomp_ins_stash[mask_cnt]);
				mask_cnt++;
				words_done++;
			}
			++pairs_created;
		}
		++keys_done;
	}
	keys_done			= 0;
	int pairs_created = 0;
	while(keys_done < key_count) {
		IntPtr app_data_addr = m_app_data_addr;
		pairs_created = 0;
		while(pairs_created < total_pairs) {
			DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
				m_app_search_inst_addr,//ins address 
				true, //False if instruction will not be executed because of predication
				SubsecondTime::Zero(), m_app_key_addr, 64, 
				Operand::READ, 0, HitWhere::UNKNOWN);
			m_app_dyn_ins_info.push_back(linfo);

			struct PicInsInfo pii;
			//printf("\npic-ins(%lu)-(%lu)\n", app_data_addr, 
				//m_app_key_addr);
			pii.other_source	= app_data_addr;
			pii.other_source2	= 0;
			pii.op						= CacheCntlr::PIC_SEARCH;	
			if(m_pic_use_vpic) {
				pii.is_vpic	= true;
				pii.count		= (m_app_search_size < 512) ? 
																	(m_app_search_size/64) : 8; 
			}
			else {
				pii.is_vpic	= false;
				pii.count		= 0;
			}
      picInsInfoVec.push_back(std::make_pair(m_app_key_addr,pii));
			if(m_pic_use_vpic) {
				app_data_addr	= app_data_addr +
								(m_app_search_size < 512 ? m_app_search_size : 512);
			}
			else {
				app_data_addr += 64;
			}
			++pairs_created;
		}
		m_app_key_addr += 64;
		++keys_done;
	}
	//printf("\nSearches(%lu), Comp/Mask(%lu)",
		//m_app_search_ins.size(), m_app_maskcomp_ins.size());
	return words_per_search;
}

void  MemoryManager::schedule_app_search_instructions( 
			int words_per_search, int key_count, bool is_strcmp){
	unsigned int count					= 0;
	unsigned int mask_cmp_count	= 0;
	unsigned int num_pics 			= m_app_search_ins.size();
	if(m_pic_use_vpic) {
		assert(num_pics == ((m_app_search_size < 512)? key_count: 
												(key_count*(m_app_search_size/512))));
	}
	else {
		assert(num_pics == (key_count*(m_app_search_size/64)));
	}
	unsigned int bef_search_count		= m_app_search_ins.size();
	unsigned int bef_mask_cmp_count	= m_app_maskcomp_ins.size();
	while(num_pics) {
  	getCore()->getPerformanceModel()->pushDynamicInstructionInfo
			(m_app_dyn_ins_info[0], true);
  	getCore()->getPerformanceModel()->queueInstruction(
																		m_app_search_ins[0], true);
		m_app_dyn_ins_info.erase(m_app_dyn_ins_info.begin());
		m_app_search_ins.erase(m_app_search_ins.begin());
		int words_scheduled = 0;
		while(words_scheduled < words_per_search) {
			if(!is_strcmp) {
  			getCore()->getPerformanceModel()->queueInstruction(
															m_app_maskcomp_ins[0], true);
				m_app_maskcomp_ins.erase(m_app_maskcomp_ins.begin());
				mask_cmp_count += 1;
			}
  		getCore()->getPerformanceModel()->queueInstruction(
															m_app_maskcomp_ins[0], true);
			m_app_maskcomp_ins.erase(m_app_maskcomp_ins.begin());
			mask_cmp_count += 1;
			++words_scheduled;
		}
		--num_pics;
		++count;
	}
	assert(count == bef_search_count);
	assert(mask_cmp_count == bef_mask_cmp_count);
}
//End- pic-apps

//CAP: Instruction Stash of stores for cache programming 
//TBD: Que:What sizes to store in - what is the usual store protocol used for one Cache Line? 
//TBD: Ans:Assumed a single cache line access 
//TBD: Que:Conversion of ASCII to binary
//TBD: Ans:Byte* usage
//TBD: Que:How to extract and pass the right address to the store 
//TBD: Ans: Done 
//TBD: Que:How to read one line of that cap_file and initiate a store with the right operand
//TBD: Ans:Using memcpy and temp_data_buf
void  MemoryManager::create_cache_program_instructions(
  Byte* cap_file) {
	unsigned int cur_subarray = 0;
  unsigned int cur_cache_line = 0;
  UInt32 address; 
  UInt32 block_size = getCacheBlockSize();
//  UInt32 block_size = 8;
  UInt32 subarray_size = CACHE_LINES_PER_SUBARRAY * getCacheBlockSize();
  UInt32 m_log_blocksize = floorLog2(getCacheBlockSize());
//  UInt32 cache_subblocks = block_size/8;
  UInt32 cache_subblocks = 1;
//  UInt64 temp_data_buf;
  Byte* temp_data_buf = new Byte;
  int cur_byte_pos;

  //printf("CAP: create inst - Cache pgm file ptr :0x%p, content: %d", cap_file, *(cap_file+3));

  if(m_cap_ins.size() == 0) {
    while(cur_subarray < NUM_SUBARRAYS) {
      cur_cache_line = 0;
      while(cur_cache_line < CACHE_LINES_PER_SUBARRAY) {
        int sub_block = 0;
       // while(sub_block < cache_subblocks) 
          while(sub_block < block_size) {
          address = (cur_subarray<<(8+m_log_blocksize)) | (cur_cache_line<<m_log_blocksize) | sub_block;
          cur_byte_pos = (cur_subarray * subarray_size) + (cur_cache_line * block_size) + sub_block;
          memcpy(temp_data_buf, cap_file + (cur_byte_pos), 1); 
          //printf("Address: 0x%x, Value: %d\n", address, (unsigned long)*temp_data_buf);
          OperandList store_list;
          store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
          store_list.push_back(Operand(Operand::REG, (unsigned long)(*temp_data_buf), Operand::READ, "", true));
          Instruction *store_inst = new GenericInstruction(store_list);
          store_inst->setAddress(m_mbench_dest_addr); //TODO: To check where inst addr is used
          store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
          store_inst->setAtomic(false);
          store_inst->setDisassembly("");
          std::vector<const MicroOp *> *store_uops 
                                  = new std::vector<const MicroOp*>();
          MicroOp *currentSMicroOp = new MicroOp();
          currentSMicroOp->setInstructionPointer(Memory::make_access(m_mbench_dest_addr));
          currentSMicroOp->makeStore(
            0
            , 0
            , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
            , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
            , 1
           );
          currentSMicroOp->setOperandSize(64); 
          currentSMicroOp->setInstruction(store_inst);
          currentSMicroOp->setFirst(true);
          currentSMicroOp->setLast(true);
          store_uops->push_back(currentSMicroOp);
          store_inst->setMicroOps(store_uops);
          m_cap_ins.push_back(store_inst);
          sub_block++;

          //CAP: Dynamic Instructions Info creation
          DynamicInstructionInfo sinfo = DynamicInstructionInfo::createMemoryInfo(m_mbench_dest_addr,//ins address 
			                                true, //False if instruction will not be executed because of predication
			                                SubsecondTime::Zero(), address, 8, Operand::WRITE, 0, 
			                                HitWhere::UNKNOWN);
		      m_cap_dyn_ins_info.push_back(sinfo);
          // TODO: FIXME: What the hell is this?

          struct CAPInsInfo cii;
          cii.addr	= address;
          cii.op		= CacheCntlr::CAP_NONE;	
          capInsInfoMap[address] 	= cii;
       } 
       cur_cache_line++;
      }
      cur_subarray++;
    }  
  }
}  

void  MemoryManager::schedule_cap_instructions() {
  int num_prg = m_cap_ins.size();
  while(num_prg) {
    getCore()->getPerformanceModel()->pushDynamicInstructionInfo(m_cap_dyn_ins_info[0], true);
    getCore()->getPerformanceModel()->queueInstruction(m_cap_ins[0]);
   	m_cap_dyn_ins_info.erase(m_cap_dyn_ins_info.begin());
    m_cap_ins.erase(m_cap_ins.begin());
    --num_prg;
  } 
  //assert(count == m_cap_ins.size());

}  

void  MemoryManager::create_cap_ss_instructions(Byte* ss_file) {
  UInt32 cur_ste = 0;
  UInt32 ste_num;
  UInt32 ss_subblocks = SWIZZLE_SWITCH_Y/8;
  UInt64 temp_data_buf;
  UInt32 address;
  
  if(m_cap_ins.size() == 0) {
    while(cur_ste < SWIZZLE_SWITCH_X) {
        int sub_block = 0;
        while(sub_block < ss_subblocks) {
          address = cur_ste*SWIZZLE_SWITCH_X + sub_block;
          memcpy(&temp_data_buf, ss_file + address, 8); 
          printf("STE no: 0x%x, Value: %d\n", address, temp_data_buf);
          OperandList store_list;
          store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
          store_list.push_back(Operand(Operand::REG, temp_data_buf, Operand::READ, "", true));
          Instruction *store_inst = new GenericInstruction(store_list);
          store_inst->setAddress(address);
          store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
          store_inst->setAtomic(false);
          store_inst->setDisassembly("");
          std::vector<const MicroOp *> *store_uops 
                                  = new std::vector<const MicroOp*>();
          MicroOp *currentSMicroOp = new MicroOp();
          currentSMicroOp->setInstructionPointer(Memory::make_access(address));
          currentSMicroOp->makeStore(
            0
            , 0
            , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
            , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
            , 8
           );
          currentSMicroOp->setOperandSize(64); 
          currentSMicroOp->setInstruction(store_inst);
          currentSMicroOp->setFirst(true);
          currentSMicroOp->setLast(true);
          store_uops->push_back(currentSMicroOp);
          store_inst->setMicroOps(store_uops);
          m_cap_ins.push_back(store_inst);
          sub_block++;

          //CAP: Dynamic Instructions Info creation
          DynamicInstructionInfo sinfo = DynamicInstructionInfo::createMemoryInfo(address,//ins address 
			                                true, //False if instruction will not be executed because of predication
			                                SubsecondTime::Zero(), address, 64, Operand::WRITE, 0, 
			                                HitWhere::UNKNOWN);
		      m_cap_dyn_ins_info.push_back(sinfo);

          struct CAPInsInfo cii;
          cii.addr	              = address;
          cii.op		              = CacheCntlr::CAP_SS;	
          capInsInfoMap[address] 	= cii;
       } 
       cur_ste++;
      }
  }
}  

//#ifdef PIC_IS_MICROBENCH
IntPtr MemoryManager::microbench_target_ins_cnt() {
	IntPtr num_iterations			= m_microbench_totalsize / m_microbench_loopsize;
	IntPtr ins_per_iteration	= 0;
	num_iterations	= num_iterations * Sim()->getCfg()->getInt("general/microbench_outer_loops");
	if(m_pic_on) {
		if(m_pic_use_vpic) {
			if( m_microbench_type == PIC_IS_MICROBENCH_COPY) //always 1
				ins_per_iteration = 1;
			else if( m_microbench_type == PIC_IS_MICROBENCH_CMP) {//
				ins_per_iteration	= (m_microbench_loopsize < 512) ? 1 : 
																						(m_microbench_loopsize/512);
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_SEARCH) { 
				ins_per_iteration	= (m_microbench_loopsize < 512) ? 1 : 
																						(m_microbench_loopsize/512);
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_BMM) { 
				int column_load_ins = m_microbench_opsize;
				int scatter_ins 		= (m_microbench_opsize < 256)? 
															m_microbench_opsize : 0;
				int store_ins 			= 2* m_microbench_opsize;
				int total_columns 	= m_microbench_opsize;
				int total_instructions = column_load_ins + scatter_ins + store_ins +
				+ total_columns;
				ins_per_iteration	= total_instructions;
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_FASTBIT) {
				int count_per_vec_op 		= 32; //I don't know why
				IntPtr size_per_vec_op	= count_per_vec_op * 64; //cache block size
				ins_per_iteration 		= m_microbench_loopsize/size_per_vec_op;
			}
		}
		else {
			assert(0); //Only vpic for now
			if( m_microbench_type == PIC_IS_MICROBENCH_COPY); //
			else if( m_microbench_type == PIC_IS_MICROBENCH_CMP); //
			else if( m_microbench_type == PIC_IS_MICROBENCH_SEARCH) { 
			}
		}
	}
	else {
		if( m_microbench_type == PIC_IS_MICROBENCH_COPY) //ld, st
			ins_per_iteration = 2*(m_microbench_loopsize/m_microbench_opsize);
		else if( m_microbench_type == PIC_IS_MICROBENCH_CMP) //ld,ld,cmp
			ins_per_iteration = 3*(m_microbench_loopsize/m_microbench_opsize);
		else if( m_microbench_type == PIC_IS_MICROBENCH_SEARCH) {  //ld, cmp, 1(ld key)
			//num_iterations = 2 * num_iterations;			//TODO: What is this?
			ins_per_iteration = 2*(m_microbench_loopsize/m_microbench_opsize) + 1;
		}
		else if( m_microbench_type == PIC_IS_MICROBENCH_BMM) {
			int vec_reg_size			= 256;
			int vec_row_load_ins 	= 
									((m_microbench_opsize * m_microbench_opsize)/vec_reg_size);
			int vec_col_load_ins	= vec_row_load_ins; //square matrix
			int vec_store_ins			= vec_row_load_ins;
			int vec_row_load_per_batch 	= 2;
			int vec_row_batches					= vec_row_load_ins/vec_row_load_per_batch;	
			int total_clmult_ins 		= (m_microbench_opsize * m_microbench_opsize);
			ins_per_iteration = vec_row_load_ins + vec_store_ins 
													+ total_clmult_ins
													+ (vec_col_load_ins * vec_row_batches);
		}
		else if( m_microbench_type == PIC_IS_MICROBENCH_FASTBIT) {
			int vec_store_ins 	= m_microbench_loopsize/m_microbench_opsize;
			int vec_load_ins 		= 2 * vec_store_ins;
			int vec_or_ins			= vec_store_ins;
			ins_per_iteration = vec_load_ins + vec_store_ins + vec_or_ins; 
		}
	}
	return (num_iterations * ins_per_iteration);
}
void MemoryManager::create_microbench_instructions() {
	if(Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY) {
		if(m_microbench_type != PIC_IS_MICROBENCH_BMM) {	
			IntPtr begin = m_mbench_src_addr;
			IntPtr begin1 	= m_mbench_src_addr + (m_microbench_totalsize);
			IntPtr end 	= m_mbench_src_addr + (2*m_microbench_totalsize); //src, dest
    	bool source_is_l3 = false;
			//TODO: This is a HACK!!!!
			source_is_l3 = (m_microbench_totalsize > 131072) ? true : false;
			//Do this fix only for Source = L2/L1
			if( m_microbench_type == PIC_IS_MICROBENCH_COPY && (!source_is_l3) ) {
				printf("\nWARMUP: Touching addresses:(%lu-%lu-%lu = %lu, %lu)\n", 
								begin, begin1, end, (begin1-begin)/64, (end-begin1)/64);
				while(begin1 < end) {
  				m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
    	     Core::NONE, Core::WRITE,
    	     begin1, 0, NULL, 8,
    	     false, false);
				begin1 += 64;
				}
				while(begin < begin1) {
  				m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
    	     Core::NONE, Core::READ,
    	     begin, 0, NULL, 8,
    	     false, false);
					begin += 64;
				}
			}
			else {
				printf("\nWARMUP: Touching addresses:(%lu, %lu = %lu)\n", 
								begin, end, (end-begin)/64);
				while(begin < end) {
  				m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
    	     Core::NONE, Core::READ,
    	     begin, 0, NULL, 8,
    	     false, false);
					begin += 64;
				}
			}
		}
		else {
			IntPtr begin 	= m_mbench_src_addr;
			IntPtr end 		= begin + (2*m_microbench_totalsize);
			//Input matrices
			printf("\nWARMUP: Touching addresses:(%lu, %lu = %lu)\n", 
								begin, end, (end-begin)/64);
			while(begin < end) {
  				m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
    	     Core::NONE, Core::READ,
    	     begin, 0, NULL, 8,
    	     false, false);
					begin += 64;
			}
			//result matrix
			begin 	= end;
			end 		= begin + m_microbench_totalsize + (64 * 4); //4 spares
			printf("\nWARMUP: Touching addresses:(%lu, %lu = %lu)\n", 
								begin, end, (end-begin)/64);
			while(begin < end) {
  				m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
    	     Core::NONE, Core::WRITE,
    	     begin, 0, NULL, 8,
    	     false, false);
					begin += 64;
			}
		}
	}
	if(Sim()->getInstrumentationMode() == InstMode::DETAILED) {	
		assert(m_microbench_loops);
		if(m_pic_on) {
			if( m_microbench_type == PIC_IS_MICROBENCH_COPY) {
				create_microbench_pic_copy_instructions();
				schedule_microbench_copy_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_CMP) {
				create_microbench_pic_comp_instructions();
				schedule_microbench_comp_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_SEARCH) {
				create_microbench_pic_search_instructions();
				schedule_microbench_search_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_LOGICAL) {
				//create_microbench_pic_logical_instructions();
				//schedule_microbench_logical_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_BMM) {
				create_microbench_pic_bmm_instructions();
				schedule_microbench_bmm_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_FASTBIT) {
				create_microbench_pic_fastbit_instructions();
				schedule_microbench_fastbit_instructions();
			}
		}
		else {
			if( m_microbench_type == PIC_IS_MICROBENCH_COPY) {
				create_microbench_copy_instructions();
				schedule_microbench_copy_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_CMP) {
				create_microbench_comp_instructions();
				schedule_microbench_comp_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_SEARCH) {
				create_microbench_search_instructions();
				schedule_microbench_search_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_LOGICAL) {
				//create_microbench_logical_instructions();
				//schedule_microbench_logical_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_BMM) {
				create_microbench_bmm_instructions();
				schedule_microbench_bmm_instructions();
			}
			else if( m_microbench_type == PIC_IS_MICROBENCH_FASTBIT) {
				create_microbench_fastbit_instructions();
				schedule_microbench_fastbit_instructions();
			}
		}
		--m_microbench_loops;
		if((m_microbench_outer_loops > 1) && !m_microbench_loops) {
			m_mbench_src_addr		= 8388608;		//need to be same across runs
			m_mbench_dest_addr	= m_mbench_src_addr + m_microbench_totalsize;
			if(m_microbench_type == PIC_IS_MICROBENCH_BMM) {
				m_mbench_src2_addr	= m_mbench_src_addr 	+ m_microbench_totalsize;
				m_mbench_dest_addr	= m_mbench_src2_addr 	+ m_microbench_totalsize;		
				m_mbench_spare_addr	= m_mbench_dest_addr 	+ m_microbench_totalsize;
			}
			--m_microbench_outer_loops;
			m_microbench_loops =  m_microbench_totalsize / m_microbench_loopsize;
		}
	}
}
void MemoryManager::schedule_microbench_copy_instructions() {
	if(m_pic_on) {
		unsigned int num_pics = m_mbench_dest_ins.size();
		unsigned int count		= 0;
		if(m_pic_use_vpic)
			assert(num_pics == 1);
		else
			assert(num_pics == m_microbench_loopsize/64);
		while(num_pics) {
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
			--num_pics;
			++count;
		}
		assert(count == m_mbench_dest_ins.size());
	}
	else {
		unsigned int loads_8batches		= (m_mbench_src_ins.size()/8);
		unsigned int num_loads_cnt = 0;	
		unsigned int num_stores_cnt = 0;	
		while(loads_8batches) {
			int counter = 8;
			while(counter) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_src_dyn_ins_info[0], true);
				m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_src_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
			}
			counter = 8;
			while(counter) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_stores_cnt], true);
				--counter;
				++num_stores_cnt;
			}
			--loads_8batches;
		}
		if(m_mbench_src_dyn_ins_info.size()) {
			int counter = m_mbench_src_dyn_ins_info.size();
			while(counter){
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_src_dyn_ins_info[0], true);
				m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_src_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
			}
			counter = m_mbench_dest_dyn_ins_info.size();
			while(counter){
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_stores_cnt], true);
				--counter;
				++num_stores_cnt;
			}
		}
		assert(m_mbench_dest_dyn_ins_info.size() == 0);
		assert(m_mbench_src_dyn_ins_info.size() == 0);
	}
}
void MemoryManager::schedule_microbench_comp_instructions() {
	if(m_pic_on) {
		unsigned int num_pics = m_mbench_src_ins.size();
		unsigned int count		= 0;
		if(m_pic_use_vpic) 
			assert(num_pics == ((m_microbench_loopsize < 512) ? 1 : 
													(m_microbench_loopsize/512)));
		else
			assert(num_pics == m_microbench_loopsize/64);
		while(num_pics) {
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_src_dyn_ins_info[0], true);
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_src_ins[count], true);
			m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
			--num_pics;
			++count;
		}
		assert(count == m_mbench_src_ins.size());
	}
	else {
		unsigned int loads_4batches		= (m_mbench_src_ins.size()/4);
		unsigned int num_loads_cnt = 0;	
		unsigned int num_comps_cnt = 0;	
		while(loads_4batches) {
			int counter = 4;
			while(counter) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_src_dyn_ins_info[0], true);
				m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_src_ins[num_loads_cnt], true);

  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
			}
			counter = 4;
			while(counter) {
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_comp_ins[num_comps_cnt], true);
				--counter;
				++num_comps_cnt;
			}
			--loads_4batches;
		}
		unsigned int ins_remaining		= (m_mbench_src_ins.size()%4);
		int comps_remaining = 0;
		if(ins_remaining) {
			int counter = ins_remaining; 
			while(counter){
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_src_dyn_ins_info[0], true);
				m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_src_ins[num_loads_cnt], true);
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
				++comps_remaining;
			}
			counter = comps_remaining;
			assert(counter);
			while(counter){
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_comp_ins[num_comps_cnt], true);
				--counter;
				++num_comps_cnt;
			}
		}
		assert(m_mbench_dest_dyn_ins_info.size() == 0);
		assert(m_mbench_src_dyn_ins_info.size() == 0);
	}
}
void MemoryManager::schedule_microbench_search_instructions() {
	if(m_pic_on) {
		unsigned int num_pics = m_mbench_src_ins.size();
		unsigned int count		= 0;
		if(m_pic_use_vpic) 
			assert(num_pics == ((m_microbench_loopsize < 512) ? 1 : 
													(m_microbench_loopsize/512)));
		else
			assert(num_pics == m_microbench_loopsize/64);
		while(num_pics) {
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_src_dyn_ins_info[0], true);
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_src_ins[count], true);
			m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
			--num_pics;
			++count;
		}
		assert(count == m_mbench_src_ins.size());
	}
	else {
		unsigned int loads_7batches		= (m_mbench_dest_ins.size()/7);
		unsigned int num_loads_cnt = 0;	
		unsigned int num_comps_cnt = 0;	
		//Single load for key
  	getCore()->getPerformanceModel()->pushDynamicInstructionInfo
															(m_mbench_src_dyn_ins_info[0], true);
		m_mbench_src_dyn_ins_info.erase(m_mbench_src_dyn_ins_info.begin());
  	getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_src_ins[num_loads_cnt], true);
		while(loads_7batches) {
			int counter = 7;
			while(counter) {

  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
			}
			counter = 7;
			while(counter) {
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_comp_ins[num_comps_cnt], true);
				--counter;
				++num_comps_cnt;
			}
			--loads_7batches;
		}
		unsigned int ins_remaining		= (m_mbench_dest_ins.size()%7);
		int comps_remaining = 0;
		if(ins_remaining) {
			int counter = ins_remaining; 
			while(counter){
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																		(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_dest_ins[num_loads_cnt], true);
				--counter;
				++num_loads_cnt;
				++comps_remaining;
			}
			counter = comps_remaining;
			assert(counter);
			while(counter){
  			getCore()->getPerformanceModel()->queueInstruction(
															m_mbench_comp_ins[num_comps_cnt], true);
				--counter;
				++num_comps_cnt;
			}
		}
		assert(m_mbench_dest_dyn_ins_info.size() == 0);
		assert(m_mbench_src_dyn_ins_info.size() == 0);
	}
}

void MemoryManager::schedule_microbench_fastbit_instructions() {
	if(m_pic_on) {
		int count_per_vec_op 		= 32; //I don't know why
		IntPtr size_per_vec_op	= count_per_vec_op * 64; //cache block size
		IntPtr pic_vec_lgcl_ins 		= 
			((m_microbench_loopsize%size_per_vec_op) == 0) ? 
			(m_microbench_loopsize/size_per_vec_op) 
			:(m_microbench_loopsize/size_per_vec_op)+1;
		while(pic_vec_lgcl_ins) {
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[0], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
			--pic_vec_lgcl_ins;
		}
		assert(m_mbench_dest_dyn_ins_info.size() == 0);
	}
	else {
		int vec_store_ins 	= m_microbench_loopsize/m_microbench_opsize;
		int vec_load_ins 		= 2 * vec_store_ins;
		int vec_or_ins			= vec_store_ins;
		unsigned int total_instructions = vec_load_ins + vec_store_ins 
														+ vec_or_ins; 
		int unwind_factor = 2;
		int loop_count						= vec_store_ins/unwind_factor;
		unsigned int ins_count = 0;
		while(loop_count) {
			int count = 0;
			int ins_idx = 0;
			while (count < unwind_factor) {
				//load1 ins
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																	(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase
																	(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
																				m_mbench_dest_ins[ins_idx], true);
				++ins_idx;
				++ins_count;

				//load2 ins
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																	(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase
																	(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
																				m_mbench_dest_ins[ins_idx], true);
				++ins_idx;
				++ins_count;

				//compare ins
  			getCore()->getPerformanceModel()->queueInstruction(
																				m_mbench_dest_ins[ins_idx], true);
				++ins_idx;
				++ins_count;

				//store ins
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																	(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase
																	(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
																				m_mbench_dest_ins[ins_idx], true);
				++ins_idx;
				++ins_count;
				count++;
			}
			--loop_count;
		}
		assert(ins_count == total_instructions);
		assert(m_mbench_dest_dyn_ins_info.size() == 0);
	}
}

void MemoryManager::create_microbench_pic_fastbit_instructions() {
	int count_per_vec_op 		= 32; //I don't know why
	IntPtr size_per_vec_op	= count_per_vec_op * 64; //cache block size
		int pic_vec_lgcl_ins 		= 
			((m_microbench_loopsize%size_per_vec_op) == 0) ? 
			(m_microbench_loopsize/size_per_vec_op) 
			:(m_microbench_loopsize/size_per_vec_op)+1;
	printf("\nFASTBIT(total_ins(%d)", pic_vec_lgcl_ins);

	//Lets create count ins with same address..
	int count = 0;
	if(m_mbench_dest_ins.size() == 0) {
		Instruction * pic_lgcl =
			create_single_store(m_mbench_src_inst_addr, 0, false);
		m_mbench_dest_ins.push_back(pic_lgcl);
	}
	IntPtr load1_address 			= m_mbench_dest_addr;
	IntPtr load2_address 			= m_mbench_src_addr;
	IntPtr last_op_size 			= m_microbench_loopsize -  
			(((m_microbench_loopsize/size_per_vec_op)*size_per_vec_op));
	assert((last_op_size%64 )==0);

	count = pic_vec_lgcl_ins;
	while (count) {
		DynamicInstructionInfo sinfo = 
				DynamicInstructionInfo:: createMemoryInfo(
			m_mbench_src_inst_addr,//ins address 
			true, //False if instruction will not be 
											//executed because of predication
			SubsecondTime::Zero(), 
			load1_address, 64, Operand::WRITE, 0, 
			HitWhere::UNKNOWN);
		m_mbench_dest_dyn_ins_info.push_back(sinfo);

		struct PicInsInfo pii;
		pii.other_source	= load2_address;
		pii.other_source2	= 0;
		pii.op						= CacheCntlr::PIC_COPY;	
		pii.is_vpic				= true;
		pii.count					= ((count==1) && last_op_size)? (last_op_size/64) : count_per_vec_op;
		picInsInfoMap[load1_address] 	= pii;
		if((count == 1) && last_op_size) {
			load1_address += last_op_size;
			load2_address += last_op_size;
		}
		else {
			load1_address += size_per_vec_op;
			load2_address += size_per_vec_op;
		}
		--count;
	}
	printf("\n(%lu,%lu,%lu,%lu,%lu)", load1_address, load2_address, m_mbench_dest_addr, m_mbench_src_addr, last_op_size);
	assert((load1_address) == (m_mbench_dest_addr+m_microbench_loopsize));
	assert((load2_address) == (m_mbench_src_addr+m_microbench_loopsize));
	assert(m_mbench_dest_dyn_ins_info.size() == pic_vec_lgcl_ins);
}

void MemoryManager::create_microbench_fastbit_instructions() {
	//Perform 2 loads, 1 OR, 1 store.
	//m_microbench_opsize is vector size
	//m_microbench_loopsize = single array
	int vec_store_ins 	= m_microbench_loopsize/m_microbench_opsize;
	int vec_load_ins 		= 2 * vec_store_ins;
	int vec_or_ins			= vec_store_ins;
	unsigned int total_instructions = vec_load_ins + vec_store_ins 
													+ vec_or_ins; 
	printf("\nFASTBIT(total_ins(%d), load(%d), store(%d), or(%d)", 
		total_instructions, vec_load_ins, vec_store_ins, vec_or_ins);
	
	//Create 2 loads, 1 OR and 1 store
	//Cannot do that register dependency will be bad.
	//Lets however pretend that there are only 4  instructions
	int unwind_factor = 2;
	IntPtr vec_ins_addr_begin 	= m_mbench_src_inst_addr;
	
	if(m_mbench_dest_ins.size() == 0) {
		int count = 0;
		while(count < unwind_factor) {
			int load_reg1	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			Instruction *load1 = create_single_load(vec_ins_addr_begin, load_reg1); 
			m_mbench_dest_ins.push_back(load1);
			m_mbench_regs.push_back(load_reg1);

			int load_reg2	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			Instruction *load2 = create_single_load(vec_ins_addr_begin+16, load_reg2); 
			m_mbench_dest_ins.push_back(load2);
			m_mbench_regs.push_back(load_reg2);

			int res_reg	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			Instruction *cmp = 
			create_single_cmp(vec_ins_addr_begin+32,load_reg1, load_reg2, res_reg);
			m_mbench_dest_ins.push_back(cmp);
			m_mbench_regs.push_back(res_reg);

			Instruction *store = 
				create_single_store(vec_ins_addr_begin+48, res_reg); 
			m_mbench_dest_ins.push_back(store);
			++count;
		}
	}

	IntPtr load1_address 			= m_mbench_dest_addr;
	IntPtr load2_address 			= m_mbench_src_addr;
	int loop_count						= vec_store_ins;

	while(loop_count) {
		DynamicInstructionInfo linfo1 = 
			DynamicInstructionInfo::createMemoryInfo(
			vec_ins_addr_begin,
			true,
			SubsecondTime::Zero(), load1_address, 32, 
			Operand::READ, 0, HitWhere::UNKNOWN);

		DynamicInstructionInfo linfo2 = 
			DynamicInstructionInfo::createMemoryInfo(
			vec_ins_addr_begin+16,
			true,
			SubsecondTime::Zero(), load2_address, 32, 
			Operand::READ, 0, HitWhere::UNKNOWN);

		DynamicInstructionInfo sinfo = 
			DynamicInstructionInfo::createMemoryInfo(
			vec_ins_addr_begin+48,
			true, SubsecondTime::Zero(), load1_address, 32, 
			Operand::WRITE, 0, HitWhere::UNKNOWN);

		load1_address += 32;
		load2_address += 32;
		m_mbench_dest_dyn_ins_info.push_back(linfo1);
		m_mbench_dest_dyn_ins_info.push_back(linfo2);
		m_mbench_dest_dyn_ins_info.push_back(sinfo);
		--loop_count;
	}
	assert(m_mbench_dest_dyn_ins_info.size() == (vec_store_ins + vec_load_ins));
	assert((load1_address) == (m_mbench_dest_addr+m_microbench_loopsize));
	assert((load2_address) == (m_mbench_src_addr+m_microbench_loopsize));
}

void MemoryManager::schedule_microbench_bmm_instructions() {
	if(m_pic_on) {
		int total_columns 		= m_microbench_opsize;
		unsigned int count		= 0;
		//For every column
		while(total_columns) {
			//Schedule load
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
			++count;
			//Scatter
			if(m_microbench_opsize < 256) {
  			getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
				++count;
			}
			//Schedule 2-stores
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
			++count;
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
			++count;
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
			//Schedule pic_mult
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
			m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  		getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[count], true);
			++count;
			--total_columns;
		}
		assert(count == m_mbench_dest_ins.size());
	}
	else {
		unsigned int ins_count = 0;
		int vec_reg_size			= 256;
		int vec_row_load_ins 	= 
									((m_microbench_opsize * m_microbench_opsize)/vec_reg_size);
		int vec_col_load_ins	= vec_row_load_ins; //square matrix
		int vec_row_load_per_batch 	= 2;
		int vec_col_load_per_batch 	= 2;
		int vec_store_per_batch 		= 2;
		int vec_row_batches					= vec_row_load_ins/vec_row_load_per_batch;	
		int vec_col_batches					= vec_col_load_ins/vec_col_load_per_batch;	
		int actual_rows_per_batch		= m_microbench_opsize/vec_row_batches;
		int actual_cols_per_batch		= m_microbench_opsize/vec_col_batches;
		int clmult_per_batch 		= actual_rows_per_batch * actual_cols_per_batch;
		int total_vec_row_batches = vec_row_batches;
		while(total_vec_row_batches) {		//For every *batch* of rows
			int count = 0;
			while(count < vec_row_load_per_batch) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[ins_count], true);
				++ins_count;
				count++;
			}
			int total_vec_col_batches = vec_col_batches;
			while(total_vec_col_batches) {		//Load all columns
				count = 0;
				while(count < vec_col_load_per_batch) {
  				getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
					m_mbench_dest_dyn_ins_info.erase
																			(m_mbench_dest_dyn_ins_info.begin());
  				getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[ins_count], true);
					++ins_count;
					count++;
				}
				count = 0;
				while(count < clmult_per_batch) {		//Perform all clmults
  				getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[ins_count], true);
					++ins_count;
					count++;
				}
				--total_vec_col_batches;
			}

			count = 0;
			while(count < vec_store_per_batch) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																			(m_mbench_dest_dyn_ins_info[0], true);
				m_mbench_dest_dyn_ins_info.erase
																			(m_mbench_dest_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
																			m_mbench_dest_ins[ins_count], true);
				++ins_count;
				count++;
			}
			--total_vec_row_batches;
		}
		assert(ins_count == m_mbench_dest_ins.size());
	}
}

Instruction*  MemoryManager::create_single_load(
	IntPtr ins_addr, int reg) {
	OperandList load_list;
	load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  load_list.push_back(Operand(Operand::REG, reg, Operand::WRITE, 
																														"", true));
  Instruction *load_inst = new GenericInstruction(load_list);
  load_inst->setAddress(ins_addr);
  load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  load_inst->setAtomic(false);
  load_inst->setDisassembly("");
  std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
  MicroOp *currentLMicroOp = new MicroOp();
  currentLMicroOp->makeLoad(
  	0
    , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
    , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    , 32 //memop_load_size[0]
   );
  currentLMicroOp->addDestinationRegister((xed_reg_enum_t)reg, ""); 
  currentLMicroOp->setOperandSize(64); 
  currentLMicroOp->setInstruction(load_inst);
  currentLMicroOp->setFirst(true);
  currentLMicroOp->setLast(true);
  load_uops->push_back(currentLMicroOp);
  load_inst->setMicroOps(load_uops);
	return load_inst;
}

Instruction* MemoryManager::create_single_cmp(IntPtr ins_addr, 
														int reg1, int reg2, int reg3) {
	OperandList comp_list;
	comp_list.push_back(Operand(Operand::REG, reg1, Operand::READ,
																											"", true));
	comp_list.push_back(Operand(Operand::REG, reg2, Operand::READ,
																											"", true));
  comp_list.push_back(Operand(Operand::REG, reg3, Operand::WRITE, 
																											"", true));
  Instruction *comp_inst 	= new GenericInstruction(comp_list);
	comp_inst->setAddress(ins_addr);
	comp_inst->setSize(4);
	comp_inst->setAtomic(false);
	comp_inst->setDisassembly("");
	std::vector<const MicroOp *> *cmp_uops
										= new std::vector<const MicroOp*>();;
  MicroOp *currentMicroOp 	= new MicroOp();
  currentMicroOp->makeExecute(
  	0, 0
    , XED_ICLASS_CMP //TODO: xed_decoded_inst_get_iclass(ins)
    , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    , false	//not conditional branch
   );
  currentMicroOp->addSourceRegister((xed_reg_enum_t)reg1, "");
  currentMicroOp->addSourceRegister((xed_reg_enum_t)reg2, "");
  currentMicroOp->addDestinationRegister((xed_reg_enum_t)reg3, "");
  currentMicroOp->setOperandSize(64); 
  currentMicroOp->setInstruction(comp_inst);
  currentMicroOp->setFirst(true);
  currentMicroOp->setLast(true);
  cmp_uops->push_back(currentMicroOp);
  comp_inst->setMicroOps(cmp_uops);
	return comp_inst;
}

Instruction* MemoryManager::create_single_store(IntPtr ins_addr, 
														int reg, bool is_pic) {
	OperandList store_list;
  store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
	if(!is_pic)
  	store_list.push_back(Operand(Operand::REG, reg, Operand::READ, 
																												"", true));
  Instruction *store_inst = new GenericInstruction(store_list);
  store_inst->setAddress(ins_addr);
  store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  store_inst->setAtomic(false);
  store_inst->setDisassembly("");
	std::vector<const MicroOp *> *store_uops 
															= new std::vector<const MicroOp*>();;
  MicroOp *currentSMicroOp = new MicroOp();
  currentSMicroOp->makeStore(
  	0
		,0
    , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
    , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    , 32 //memop_load_size[0]
   );
	if(!is_pic)
  	currentSMicroOp->addSourceRegister((xed_reg_enum_t)reg, "");
  currentSMicroOp->setOperandSize(64); 
  currentSMicroOp->setInstruction(store_inst);
  currentSMicroOp->setFirst(true);
  currentSMicroOp->setLast(true);
  store_uops->push_back(currentSMicroOp);
  store_inst->setMicroOps(store_uops);
	return store_inst;
}

void  MemoryManager::create_microbench_pic_bmm_instructions() {
	//We only create single BMM worth of instructions
	//core.cc calls it loop times , loop decides number of such matrices present
	//m_microbench_opsize is the matrix dimension
	int column_load_ins = m_microbench_opsize;
	int scatter_ins 		= (m_microbench_opsize < 256)? m_microbench_opsize : 0;
	int store_ins 			= 2* m_microbench_opsize;
	int total_columns 	= m_microbench_opsize;
	unsigned int total_instructions = column_load_ins + scatter_ins + store_ins +
		+ total_columns;
	printf("\nBMM(%lu)->ins(%d)", m_microbench_opsize, total_instructions);
	

	if(m_mbench_dest_ins.size() == 0) {
		while(total_columns) {		//For every columns
			//First load
			OperandList load_list;
			int reg	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			m_mbench_regs.push_back(reg);
			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list.push_back(Operand(Operand::REG, reg, Operand::WRITE, 
																														"", true));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		load_inst->setAddress(m_mbench_src_inst_addr);
  		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst->setAtomic(false);
  		load_inst->setDisassembly("");
  		std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
  		MicroOp *currentLMicroOp = new MicroOp();
  		currentLMicroOp->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentLMicroOp->addDestinationRegister((xed_reg_enum_t)reg, ""); 
  		currentLMicroOp->setOperandSize(64); 
  		currentLMicroOp->setInstruction(load_inst);
  		currentLMicroOp->setFirst(true);
  		currentLMicroOp->setLast(true);
  		load_uops->push_back(currentLMicroOp);
  		load_inst->setMicroOps(load_uops);
			m_mbench_dest_ins.push_back(load_inst);
			//Next scatter
			int store_reg	= m_mbench_regs[0];
			if(m_microbench_opsize < 256) {
				OperandList scat_list;
				int scat_reg	= m_mbench_regs[0];
				m_mbench_regs.erase(m_mbench_regs.begin());
				m_mbench_regs.push_back(scat_reg);
				scat_list.push_back(Operand(Operand::REG, reg, Operand::READ,
																														"", true));
  			scat_list.push_back(Operand(Operand::REG, scat_reg, Operand::WRITE, 
																														"", true));
  			Instruction *scat_inst 	= new GenericInstruction(scat_list);
				scat_inst->setAddress(m_mbench_src_inst_addr+16);
				scat_inst->setSize(4);
				scat_inst->setAtomic(false);
				scat_inst->setDisassembly("");
				std::vector<const MicroOp *> *scat_uops
															= new std::vector<const MicroOp*>();;
  			MicroOp *currentMicroOp 	= new MicroOp();
  			currentMicroOp->makeExecute(
  				0, 0
  		  	, XED_ICLASS_CMP //TODO: xed_decoded_inst_get_iclass(ins)
  		  	, "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  	, false	//not conditional branch
  		 	);
  			currentMicroOp->addSourceRegister((xed_reg_enum_t)reg, "");
  			currentMicroOp->addDestinationRegister((xed_reg_enum_t)scat_reg, "");
  			currentMicroOp->setOperandSize(64); 
  			currentMicroOp->setInstruction(scat_inst);
  			currentMicroOp->setFirst(true);
  			currentMicroOp->setLast(true);
  			scat_uops->push_back(currentMicroOp);
  			scat_inst->setMicroOps(scat_uops);
				m_mbench_dest_ins.push_back(scat_inst);
				store_reg	= scat_reg;
			}
			else
				store_reg = reg;
			//Next 2-stores
			OperandList store_list1;
			OperandList store_list2;
  		store_list1.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		store_list1.push_back(Operand(Operand::REG, store_reg, Operand::READ, 
																														 "", true));
  		store_list2.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		store_list2.push_back(Operand(Operand::REG, store_reg, Operand::READ, 
																														 "", true));
  		Instruction *store_inst1 = new GenericInstruction(store_list1);
  		Instruction *store_inst2 = new GenericInstruction(store_list2);
  		store_inst1->setAddress(m_mbench_src_inst_addr+32);
  		store_inst1->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		store_inst1->setAtomic(false);
  		store_inst1->setDisassembly("");
  		store_inst2->setAddress(m_mbench_src_inst_addr+48);
  		store_inst2->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		store_inst2->setAtomic(false);
  		store_inst2->setDisassembly("");
			std::vector<const MicroOp *> *store_uops1 
															= new std::vector<const MicroOp*>();;
  		MicroOp *currentS1MicroOp = new MicroOp();
			std::vector<const MicroOp *> *store_uops2
															= new std::vector<const MicroOp*>();;
  		MicroOp *currentS2MicroOp = new MicroOp();
  		currentS1MicroOp->makeStore(
  			0
				,0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , 32 //memop_load_size[0]
  		 );
  		currentS2MicroOp->makeStore(
  			0
				,0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , 32 //memop_load_size[0]
  		 );
  		currentS1MicroOp->addSourceRegister((xed_reg_enum_t)store_reg, "");
  		currentS1MicroOp->setOperandSize(64); 
  		currentS1MicroOp->setInstruction(store_inst1);
  		currentS1MicroOp->setFirst(true);
  		currentS1MicroOp->setLast(true);
  		store_uops1->push_back(currentS1MicroOp);
  		store_inst1->setMicroOps(store_uops1);
  		currentS2MicroOp->addSourceRegister((xed_reg_enum_t)store_reg, "");
  		currentS2MicroOp->setOperandSize(64); 
  		currentS2MicroOp->setInstruction(store_inst2);
  		currentS2MicroOp->setFirst(true);
  		currentS2MicroOp->setLast(true);
  		store_uops2->push_back(currentS2MicroOp);
  		store_inst2->setMicroOps(store_uops2);
			m_mbench_dest_ins.push_back(store_inst1);
			m_mbench_dest_ins.push_back(store_inst2);
			//Next pic_clmult
			OperandList store_list;
   		store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		Instruction *store_inst = new GenericInstruction(store_list);
   		store_inst->setAddress(m_mbench_src_inst_addr+64);
   		store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
   		store_inst->setAtomic(false);
   		store_inst->setDisassembly("");
			std::vector<const MicroOp *> *store_uops 
															= new std::vector<const MicroOp*>();;
   		MicroOp *currentSMicroOp = new MicroOp();
   		currentSMicroOp->setInstructionPointer(
				Memory::make_access(m_mbench_src_inst_addr+64));
    	currentSMicroOp->makeStore(
    		0
				,0
      	, XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
      	, "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
      	, 8 //memop_load_size[0]
     	);
    	currentSMicroOp->setOperandSize(64); 
    	currentSMicroOp->setInstruction(store_inst);
    	currentSMicroOp->setFirst(true);
    	currentSMicroOp->setLast(true);
   		store_uops->push_back(currentSMicroOp);
   		store_inst->setMicroOps(store_uops);
			m_mbench_dest_ins.push_back(store_inst);

			//Asume result in cache
			--total_columns;
		}
	}
	assert(m_mbench_dest_ins.size() == total_instructions);
	total_columns 		= m_microbench_opsize;
	while(total_columns) {
		//First load
		DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,//ins address 
			true,
			SubsecondTime::Zero(), m_mbench_src_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);

		//Next 2-stores
		DynamicInstructionInfo sinfo1 = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr+32,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_spare_addr, m_microbench_opsize, 
			Operand::WRITE, 0, HitWhere::UNKNOWN);
		DynamicInstructionInfo sinfo2 = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr+48,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_spare_addr, m_microbench_opsize, 
			Operand::WRITE, 0, HitWhere::UNKNOWN);

		//Next pic_clmult
		DynamicInstructionInfo sinfo = DynamicInstructionInfo::
																		createMemoryInfo(
			m_mbench_src_inst_addr+64,//ins address 
			true, //False if instruction will not be 
											//executed because of predication
			SubsecondTime::Zero(), m_mbench_dest_addr, 64, Operand::WRITE, 0, 
			HitWhere::UNKNOWN);
		struct PicInsInfo pii;
		pii.other_source	= m_mbench_src2_addr;				//row data
		pii.other_source2	= m_mbench_spare_addr;			//column data
		pii.op						= CacheCntlr::PIC_CLMULT;	
		pii.is_vpic				= true;
		pii.count					= m_microbench_opsize; //will be calc later
    picInsInfoVec.push_back(std::make_pair(m_mbench_dest_addr,pii));
		//picInsInfoMap[m_mbench_dest_addr] 	= pii;

		m_mbench_src_addr 		+= (m_microbench_opsize/8); //Next column
		m_mbench_dest_addr	 	+= (m_microbench_opsize/8);	//Next row
		//TODO:
		m_mbench_spare_addr 	+= 64;											//Next spare CB
		m_mbench_dest_dyn_ins_info.push_back(linfo);
		m_mbench_dest_dyn_ins_info.push_back(sinfo1);
		m_mbench_dest_dyn_ins_info.push_back(sinfo2);
		m_mbench_dest_dyn_ins_info.push_back(sinfo);
		if(m_mbench_dest_dyn_ins_info.size() % 16 == 0)
			m_mbench_spare_addr -= (64 * 4);
		--total_columns;
	}
	m_mbench_src2_addr += m_microbench_loopsize;	//next time diff matrix
	//printf("\nAddresses(%lx/%lx, %lx->%lx: (%lu, %lu)", 
	//m_mbench_src_addr, m_mbench_spare_addr,
	//m_mbench_src2_addr, m_mbench_dest_addr,
	//m_mbench_dest_dyn_ins_info.size(), picInsInfoVec.size());
}

void MemoryManager::create_microbench_bmm_instructions() {
	//We only create single BMM worth of instructions
	//core.cc calls it loop times , loop decides number of such matrices present
	//m_microbench_opsize is the matrix dimension
	//Following assumes vector loads
	int vec_reg_size			= 256;
	int vec_row_load_ins 	= 
									((m_microbench_opsize * m_microbench_opsize)/vec_reg_size);
	int vec_col_load_ins	= vec_row_load_ins; //square matrix
	int vec_store_ins			= vec_row_load_ins;

	//We want to do fewer column scans.. So load more rows per loop
	int vec_row_load_per_batch 	= 2;
	int vec_col_load_per_batch 	= 2;
	int vec_store_per_batch 		= 2;
	//two 256b registers store row data
	int vec_row_batches					= vec_row_load_ins/vec_row_load_per_batch;	
	int vec_col_batches					= vec_col_load_ins/vec_col_load_per_batch;	

	int actual_rows_per_batch		= 
			((vec_row_load_per_batch*vec_reg_size)/m_microbench_opsize);
	int actual_cols_per_batch		=
			((vec_col_load_per_batch*vec_reg_size)/m_microbench_opsize);

	int total_clmult_ins 		= (m_microbench_opsize * m_microbench_opsize);
	int clmult_per_batch 		= actual_rows_per_batch * actual_cols_per_batch;
	assert(total_clmult_ins == (vec_row_batches  
													* (vec_col_batches *clmult_per_batch)));

	unsigned int total_instructions = vec_row_load_ins + vec_store_ins 
													+ total_clmult_ins
													+ (vec_col_load_ins * vec_row_batches);
	printf("\nBMM(total_ins(%d), rowl(%d), total_coll(%d), total_clmult(%d)", 
		total_instructions, vec_row_load_ins, (vec_col_load_ins * vec_row_batches), total_clmult_ins);

	IntPtr row_load_ins_addr 	= m_mbench_src_inst_addr;
	IntPtr col_load_ins_addr 	= m_mbench_src_inst_addr + (vec_row_load_per_batch*16);
	IntPtr clmult_ins_addr 		= col_load_ins_addr + (vec_col_load_per_batch*16);
	IntPtr res_store_ins_addr = clmult_ins_addr + (clmult_per_batch*16);

	printf("\nBMM-Addr(%lu, %lu, %lu, %lu)", row_load_ins_addr, col_load_ins_addr, clmult_ins_addr, res_store_ins_addr);
	std::vector<int> row_regs;
	std::vector<int> col_regs;

	if(m_mbench_dest_ins.size() == 0) {
		int total_vec_row_batches = vec_row_batches;
		while(total_vec_row_batches) {		//For every *batch* of rows
			int count = 0;
			while(count < vec_row_load_per_batch) {
				int row_reg	= m_mbench_regs[0];
				m_mbench_regs.erase(m_mbench_regs.begin());
				Instruction *row_load = 
					create_single_load(
					(row_load_ins_addr+(count*16)), row_reg); 
				m_mbench_dest_ins.push_back(row_load);
				row_regs.push_back(row_reg);
				count++;
			}
			
			int total_vec_col_batches = vec_col_batches;
			while(total_vec_col_batches) {		//Load all columns
				count = 0;
				//Loads first
				while(count < vec_col_load_per_batch) {
					int col_reg	= m_mbench_regs[0];
					m_mbench_regs.erase(m_mbench_regs.begin());
					Instruction *col_load = 
					create_single_load
						(col_load_ins_addr+(count*16), col_reg); 					
					m_mbench_dest_ins.push_back(col_load);
					col_regs.push_back(col_reg);
					count++;
				}
				//Now clmult
				count = 0;
				unsigned int row_reg_i = 0;
				unsigned int col_reg_i = 0;
				while(count < clmult_per_batch) {		//Perform all clmults
					int res_reg	= m_mbench_regs[0];
					m_mbench_regs.erase(m_mbench_regs.begin());
					assert(row_reg_i < row_regs.size());
					assert(col_reg_i < col_regs.size());
					Instruction *clmult = 
					create_single_cmp(clmult_ins_addr+(count*16), 
										row_regs[row_reg_i], col_regs[col_reg_i], res_reg); 			
					m_mbench_dest_ins.push_back(clmult);
					m_mbench_regs.push_back(res_reg);
					col_reg_i++;
					if(col_reg_i == col_regs.size()){
						col_reg_i = 0;
						row_reg_i++;
						if(row_reg_i == row_regs.size())
							row_reg_i = 0;
					}
					count++;
				}
				int col_reg_size = col_regs.size();
				while(col_reg_size) {
					m_mbench_regs.push_back(col_regs[0]);
					col_regs.erase(col_regs.begin());
					--col_reg_size;
				}
				assert(col_regs.size() == 0);
				--total_vec_col_batches;
			}
			//Write the results now : 2 vec stores
			count = 0;
			while(count < vec_store_per_batch) {
				int reg	= m_mbench_regs[0];
				m_mbench_regs.erase(m_mbench_regs.begin());
				Instruction *store = 
					create_single_store(
					(res_store_ins_addr+(count*16)), reg); 
				m_mbench_dest_ins.push_back(store);
				m_mbench_regs.push_back(reg);
				count++;
			}
			int row_reg_size = row_regs.size();
			while(row_reg_size) {
				m_mbench_regs.push_back(row_regs[0]);
				row_regs.erase(row_regs.begin());
				--row_reg_size;
			}
			assert(row_regs.size() == 0);
			--total_vec_row_batches;
		}
	}
	assert(total_instructions == m_mbench_dest_ins.size());
	assert(row_regs.size() == 0); assert(col_regs.size() == 0);

	int total_vec_row_batches = vec_row_batches;
	IntPtr row_address = m_mbench_src2_addr;
	IntPtr col_address = m_mbench_src_addr;
	IntPtr res_address = m_mbench_dest_addr;
	while(total_vec_row_batches) {		//For every *batch* of rows
		int count = 0;
		while(count < vec_row_load_per_batch) {
			DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
				row_load_ins_addr+(16*count),
				true,
				SubsecondTime::Zero(), row_address, 32, 
				Operand::READ, 0, HitWhere::UNKNOWN);
			m_mbench_dest_dyn_ins_info.push_back(linfo);
			count++;
			row_address += 32;
		}
		int total_vec_col_batches = vec_col_batches;
		col_address = m_mbench_src_addr;
		while(total_vec_col_batches) {		//Load all columns
			count = 0;
			while(count < vec_col_load_per_batch) {
				DynamicInstructionInfo linfo = 
					DynamicInstructionInfo::createMemoryInfo(
					col_load_ins_addr+(16*count),
					true, SubsecondTime::Zero(), col_address, 32, 
					Operand::READ, 0, HitWhere::UNKNOWN);
					m_mbench_dest_dyn_ins_info.push_back(linfo);
				count++;
				col_address += 32;
			}
			--total_vec_col_batches;
		}
		count = 0;
		while(count < vec_store_per_batch) {
			DynamicInstructionInfo sinfo = DynamicInstructionInfo::createMemoryInfo(
				res_store_ins_addr+(16*count),
				true, SubsecondTime::Zero(), res_address, 32, 
				Operand::WRITE, 0, HitWhere::UNKNOWN);
			m_mbench_dest_dyn_ins_info.push_back(sinfo);
			res_address += 32;
			count++;
		}
		--total_vec_row_batches;
	}
	assert(m_mbench_dest_dyn_ins_info.size() == 
		(vec_row_load_ins + vec_store_ins + 
		(vec_col_load_ins * vec_row_batches)));
	IntPtr matrix_size = 
		((m_microbench_opsize * m_microbench_opsize)/8);
	printf("\nM-size(%lu), row(%lu), col(%lu), res(%lu)",
		matrix_size, row_address, col_address, res_address);
	assert((row_address) == (m_mbench_src2_addr+matrix_size));
	assert((col_address) == (m_mbench_src_addr+matrix_size));
	assert((res_address) == (m_mbench_dest_addr+matrix_size));
}


void MemoryManager::create_microbench_pic_search_instructions() {
	int total_pairs = 0;
	if(m_pic_use_vpic)
		total_pairs	= (m_microbench_loopsize < 512) ? 1 : 
																			(m_microbench_loopsize/512);
	else
		total_pairs	= m_microbench_loopsize/64;
	if(m_mbench_src_ins.size() == 0) {
		while(total_pairs) {
			OperandList load_list;
			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		load_inst->setAddress(m_mbench_src_inst_addr);
  		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst->setAtomic(false);
  		load_inst->setDisassembly("");
	
  		std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
  		MicroOp *currentLMicroOp = new MicroOp();
  		currentLMicroOp->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , 8
  		 );
    	currentLMicroOp->setOperandSize(64); 
    	currentLMicroOp->setInstruction(load_inst);
    	currentLMicroOp->setFirst(true);
    	currentLMicroOp->setLast(true);

   		load_uops->push_back(currentLMicroOp);
   		load_inst->setMicroOps(load_uops);
			m_mbench_src_ins.push_back(load_inst);
			--total_pairs;
		}
	}
	if(m_pic_use_vpic)
		total_pairs	= (m_microbench_loopsize < 512) ? 1 : 
																			(m_microbench_loopsize/512);
	else
		total_pairs	= m_microbench_loopsize/64;
	while(total_pairs) {
		DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_dest_addr, 64, 
			Operand::READ, 0, HitWhere::UNKNOWN);
		m_mbench_src_dyn_ins_info.push_back(linfo);

		struct PicInsInfo pii;
		pii.other_source	= m_mbench_src_addr;
		pii.other_source2	= 0;
		pii.op						= CacheCntlr::PIC_SEARCH;	
		if(m_pic_use_vpic) {
			pii.is_vpic	= true;
			pii.count		= (m_microbench_loopsize < 512) ? 
																(m_microbench_loopsize/64) : 8; 
		}
		else {
			pii.is_vpic	= false;
			pii.count		= 0;
		}
		picInsInfoMap[m_mbench_dest_addr] 	= pii;
		if(m_pic_use_vpic) {
			m_mbench_dest_addr	= m_mbench_dest_addr +
							(m_microbench_loopsize < 512 ? m_microbench_loopsize : 512);
		}
		else {
			m_mbench_dest_addr += 64;
		}
		--total_pairs;
	}
	m_mbench_src_addr 	+= m_microbench_loopsize;
}
void MemoryManager::create_microbench_pic_comp_instructions() {
	int total_pairs = 0;
	if(m_pic_use_vpic)
		total_pairs	= (m_microbench_loopsize < 512) ? 1 : 
																			(m_microbench_loopsize/512);
	else
		total_pairs	= m_microbench_loopsize/64;
	if(m_mbench_src_ins.size() == 0) {
		while(total_pairs) {
			OperandList load_list;
			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		load_inst->setAddress(m_mbench_src_inst_addr);
  		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst->setAtomic(false);
  		load_inst->setDisassembly("");
	
  		std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
  		MicroOp *currentLMicroOp = new MicroOp();
  		currentLMicroOp->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , 8
  		 );
    	currentLMicroOp->setOperandSize(64); 
    	currentLMicroOp->setInstruction(load_inst);
    	currentLMicroOp->setFirst(true);
    	currentLMicroOp->setLast(true);

   		load_uops->push_back(currentLMicroOp);
   		load_inst->setMicroOps(load_uops);
			m_mbench_src_ins.push_back(load_inst);
			--total_pairs;
		}
	}
	if(m_pic_use_vpic)
		total_pairs	= (m_microbench_loopsize < 512) ? 1 : 
																			(m_microbench_loopsize/512);
	else
		total_pairs	= m_microbench_loopsize/64;
	while(total_pairs) {
		DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_src_addr, 64, 
			Operand::READ, 0, HitWhere::UNKNOWN);
		m_mbench_src_dyn_ins_info.push_back(linfo);

		struct PicInsInfo pii;
		pii.other_source	= m_mbench_dest_addr;
		pii.other_source2	= 0;
		pii.op						= CacheCntlr::PIC_CMP;	
		if(m_pic_use_vpic) {
			pii.is_vpic	= true;
			pii.count		= (m_microbench_loopsize < 512) ? 
																(m_microbench_loopsize/64) : 8; 
		}
		else {
			pii.is_vpic	= false;
			pii.count		= 0;
		}
		picInsInfoMap[m_mbench_src_addr] 	= pii;
		if(m_pic_use_vpic) {
			m_mbench_src_addr 	= m_mbench_src_addr + 
							(m_microbench_loopsize < 512 ? m_microbench_loopsize : 512);
			m_mbench_dest_addr	= m_mbench_dest_addr +
							(m_microbench_loopsize < 512 ? m_microbench_loopsize : 512);
		}
		else {
			m_mbench_src_addr += 64;
			m_mbench_dest_addr += 64;
		}
		--total_pairs;
	}
}
void MemoryManager::create_microbench_pic_copy_instructions() {
	int total_pairs = 0;
	if(m_pic_use_vpic)
		total_pairs	= 1;
	else
		total_pairs	= m_microbench_loopsize/64;
	if(m_mbench_dest_ins.size() == 0) {
		while(total_pairs) {
			OperandList store_list;
   		store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		Instruction *store_inst = new GenericInstruction(store_list);
   		store_inst->setAddress(m_mbench_dest_inst_addr);
   		store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
   		store_inst->setAtomic(false);
   		store_inst->setDisassembly("");
	
			std::vector<const MicroOp *> *store_uops 
															= new std::vector<const MicroOp*>();;
   		MicroOp *currentSMicroOp = new MicroOp();
   		currentSMicroOp->setInstructionPointer(
				Memory::make_access(m_mbench_dest_inst_addr));
    	currentSMicroOp->makeStore(
    		0
				,0
      	, XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
      	, "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
      	, 8 //memop_load_size[0]
     	);

    	currentSMicroOp->setOperandSize(64); 
    	currentSMicroOp->setInstruction(store_inst);
    	currentSMicroOp->setFirst(true);
    	currentSMicroOp->setLast(true);

   		store_uops->push_back(currentSMicroOp);
   		store_inst->setMicroOps(store_uops);
			m_mbench_dest_ins.push_back(store_inst);
			--total_pairs;
		}
	}
	if(m_pic_use_vpic)
		total_pairs	= 1;
	else
		total_pairs	= m_microbench_loopsize/64;
	while(total_pairs) {
		DynamicInstructionInfo sinfo = DynamicInstructionInfo::
																		createMemoryInfo(
			m_mbench_dest_inst_addr,//ins address 
			true, //False if instruction will not be 
											//executed because of predication
			SubsecondTime::Zero(), m_mbench_dest_addr, 64, Operand::WRITE, 0, 
			HitWhere::UNKNOWN);
		m_mbench_dest_dyn_ins_info.push_back(sinfo);

		struct PicInsInfo pii;
		pii.other_source	= m_mbench_src_addr;
		pii.other_source2	= 0;
		pii.op						= CacheCntlr::PIC_COPY;	
		if(m_pic_use_vpic) {
			pii.is_vpic	= true;
			pii.count		= m_microbench_loopsize/64;
		}
		else {
			pii.is_vpic	= false;
			pii.count		= 0;
		}
		picInsInfoMap[m_mbench_dest_addr] 	= pii;

		if(m_pic_use_vpic) {
			m_mbench_src_addr += m_microbench_loopsize;
			m_mbench_dest_addr += m_microbench_loopsize;
		}
		else {
			m_mbench_src_addr += 64;
			m_mbench_dest_addr += 64;
		}
		--total_pairs;
	}
}
void MemoryManager::create_microbench_search_instructions() {
	int total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	//Create
	if(m_mbench_dest_ins.size() == 0) {
		int reg1	= m_mbench_regs[0];
		m_mbench_regs.erase(m_mbench_regs.begin());
		OperandList load_list1;
		load_list1.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  	load_list1.push_back(Operand(Operand::REG, reg1, Operand::WRITE, 
																													"", true));
  	Instruction *load_inst1 = new GenericInstruction(load_list1);
		load_inst1->setAddress(m_mbench_src_inst_addr);
  	load_inst1->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  	load_inst1->setAtomic(false);
  	load_inst1->setDisassembly("");
  	std::vector<const MicroOp *> *load_uops1 
														= new std::vector<const MicroOp*>();
  	MicroOp *currentLMicroOp1 = new MicroOp();
  	currentLMicroOp1->makeLoad(
  		0
  	  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  	  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  	  , m_microbench_opsize //memop_load_size[0]
  	 );
  	currentLMicroOp1->addDestinationRegister((xed_reg_enum_t)reg1, ""); 
  	currentLMicroOp1->setOperandSize(64); 
  	currentLMicroOp1->setInstruction(load_inst1);
  	currentLMicroOp1->setFirst(true);
  	currentLMicroOp1->setLast(true);
  	load_uops1->push_back(currentLMicroOp1);
  	load_inst1->setMicroOps(load_uops1);
		m_mbench_src_ins.push_back(load_inst1);

		while(total_pairs) {
			OperandList load_list2;
			OperandList comp_list;
			int reg2	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			m_mbench_regs.push_back(reg2);
			load_list2.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list2.push_back(Operand(Operand::REG, reg2, Operand::WRITE, 
																														"", true));
			comp_list.push_back(Operand(Operand::REG, reg1, Operand::READ,
																														"", true));
			comp_list.push_back(Operand(Operand::REG, reg2, Operand::READ,
																														"", true));
  		comp_list.push_back(Operand(Operand::REG, reg1, Operand::WRITE, 
																														"", true));
  		Instruction *load_inst2 = new GenericInstruction(load_list2);
  		Instruction *comp_inst 	= new GenericInstruction(comp_list);
			load_inst2->setAddress(m_mbench_dest_inst_addr);
			comp_inst->setAddress(m_mbench_comp_inst_addr);
  		load_inst2->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst2->setAtomic(false);
  		load_inst2->setDisassembly("");
			comp_inst->setSize(4);
			comp_inst->setAtomic(false);
			comp_inst->setDisassembly("");
			//Create uops for these instructions..
			std::vector<const MicroOp *> *load_uops2 
															= new std::vector<const MicroOp*>();;
			std::vector<const MicroOp *> *cmp_uops
															= new std::vector<const MicroOp*>();;
  		MicroOp *currentLMicroOp2 = new MicroOp();
  		MicroOp *currentMicroOp 	= new MicroOp();
  		currentLMicroOp2->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentMicroOp->makeExecute(
  			0, 0
  		  , XED_ICLASS_CMP //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , false	//not conditional branch
  		 );
			currentLMicroOp2->addDestinationRegister((xed_reg_enum_t)reg2, ""); 
  		currentLMicroOp2->setOperandSize(64); 
  		currentLMicroOp2->setInstruction(load_inst2);
  		currentLMicroOp2->setFirst(true);
  		currentLMicroOp2->setLast(true);
  		load_uops2->push_back(currentLMicroOp2);
  		load_inst2->setMicroOps(load_uops2);
  		currentMicroOp->addSourceRegister((xed_reg_enum_t)reg1, "");
  		currentMicroOp->addSourceRegister((xed_reg_enum_t)reg2, "");
  		currentMicroOp->setOperandSize(64); 
  		currentMicroOp->setInstruction(comp_inst);
  		currentMicroOp->setFirst(true);
  		currentMicroOp->setLast(true);
  		cmp_uops->push_back(currentMicroOp);
  		comp_inst->setMicroOps(cmp_uops);
			m_mbench_dest_ins.push_back(load_inst2);
			m_mbench_comp_ins.push_back(comp_inst);
			--total_pairs;
		}
	}
	total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	DynamicInstructionInfo linfo1 = 
			DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,true, SubsecondTime::Zero(), 
			m_mbench_src_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);
	m_mbench_src_dyn_ins_info.push_back(linfo1);
	while(total_pairs) {
		DynamicInstructionInfo linfo2 = 
			DynamicInstructionInfo::createMemoryInfo(
			m_mbench_dest_inst_addr,true, SubsecondTime::Zero(), 
			m_mbench_dest_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);

		m_mbench_dest_dyn_ins_info.push_back(linfo2);
		m_mbench_src_addr += m_microbench_opsize;
		m_mbench_dest_addr += m_microbench_opsize;
		--total_pairs;
	}
}
void MemoryManager::create_microbench_comp_instructions() {
	int total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	//Create
	if(m_mbench_src_ins.size() == 0) {
		while(total_pairs) {
			OperandList load_list1;
			OperandList load_list2;
			OperandList comp_list;
			int reg1	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			m_mbench_regs.push_back(reg1);

			int reg2	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			m_mbench_regs.push_back(reg2);

			load_list1.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list1.push_back(Operand(Operand::REG, reg1, Operand::WRITE, 
																														"", true));
			load_list2.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list2.push_back(Operand(Operand::REG, reg2, Operand::WRITE, 
																														"", true));

			comp_list.push_back(Operand(Operand::REG, reg1, Operand::READ,
																														"", true));
			comp_list.push_back(Operand(Operand::REG, reg2, Operand::READ,
																														"", true));
  		comp_list.push_back(Operand(Operand::REG, reg1, Operand::WRITE, 
																														"", true));

  		Instruction *load_inst1 = new GenericInstruction(load_list1);
  		Instruction *load_inst2 = new GenericInstruction(load_list2);
  		Instruction *comp_inst 	= new GenericInstruction(comp_list);
  		
			load_inst1->setAddress(m_mbench_src_inst_addr);
			load_inst2->setAddress(m_mbench_dest_inst_addr);
			comp_inst->setAddress(m_mbench_comp_inst_addr);

  		load_inst1->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst2->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
			comp_inst->setSize(4);
  		load_inst1->setAtomic(false);
  		load_inst2->setAtomic(false);
			comp_inst->setAtomic(false);
  		load_inst1->setDisassembly("");
  		load_inst2->setDisassembly("");
			comp_inst->setDisassembly("");

			//Create uops for these instructions..
  		std::vector<const MicroOp *> *load_uops1 
															= new std::vector<const MicroOp*>();
			std::vector<const MicroOp *> *load_uops2 
															= new std::vector<const MicroOp*>();;
			std::vector<const MicroOp *> *cmp_uops
															= new std::vector<const MicroOp*>();;

  		MicroOp *currentLMicroOp1 = new MicroOp();
  		MicroOp *currentLMicroOp2 = new MicroOp();
  		MicroOp *currentMicroOp 	= new MicroOp();
  		currentLMicroOp1->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentLMicroOp2->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentMicroOp->makeExecute(
  			0, 0
  		  , XED_ICLASS_CMP //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , false	//not conditional branch
  		 );
  		currentLMicroOp1->addDestinationRegister((xed_reg_enum_t)reg1, ""); 
  		currentLMicroOp1->setOperandSize(64); 
  		currentLMicroOp1->setInstruction(load_inst1);
  		currentLMicroOp1->setFirst(true);
  		currentLMicroOp1->setLast(true);
  		load_uops1->push_back(currentLMicroOp1);
  		load_inst1->setMicroOps(load_uops1);
  		
			currentLMicroOp2->addDestinationRegister((xed_reg_enum_t)reg2, ""); 
  		currentLMicroOp2->setOperandSize(64); 
  		currentLMicroOp2->setInstruction(load_inst2);
  		currentLMicroOp2->setFirst(true);
  		currentLMicroOp2->setLast(true);
  		load_uops2->push_back(currentLMicroOp2);
  		load_inst2->setMicroOps(load_uops2);

  		currentMicroOp->addSourceRegister((xed_reg_enum_t)reg1, "");
  		currentMicroOp->addSourceRegister((xed_reg_enum_t)reg2, "");
  		currentMicroOp->setOperandSize(64); 
  		currentMicroOp->setInstruction(comp_inst);
  		currentMicroOp->setFirst(true);
  		currentMicroOp->setLast(true);
  		cmp_uops->push_back(currentMicroOp);
  		comp_inst->setMicroOps(cmp_uops);

			m_mbench_src_ins.push_back(load_inst1);
			m_mbench_dest_ins.push_back(load_inst2);
			m_mbench_comp_ins.push_back(comp_inst);
			--total_pairs;
		}
	}
	total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	while(total_pairs) {
		DynamicInstructionInfo linfo1 = 
			DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,true, SubsecondTime::Zero(), 
			m_mbench_src_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);
		DynamicInstructionInfo linfo2 = 
			DynamicInstructionInfo::createMemoryInfo(
			m_mbench_dest_inst_addr,true, SubsecondTime::Zero(), 
			m_mbench_dest_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);

		m_mbench_src_dyn_ins_info.push_back(linfo1);
		m_mbench_dest_dyn_ins_info.push_back(linfo2);
		m_mbench_src_addr += m_microbench_opsize;
		m_mbench_dest_addr += m_microbench_opsize;
		--total_pairs;
	}
}
void MemoryManager::create_microbench_copy_instructions() {
	int total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	//Create
	if(m_mbench_src_ins.size() == 0) {
		while(total_pairs) {
			OperandList load_list;
			OperandList store_list;
			int reg	= m_mbench_regs[0];
			m_mbench_regs.erase(m_mbench_regs.begin());
			m_mbench_regs.push_back(reg);
			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
  		load_list.push_back(Operand(Operand::REG, reg, Operand::WRITE, 
																														"", true));
  		store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		store_list.push_back(Operand(Operand::REG, reg, Operand::READ, 
																														 "", true));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		Instruction *store_inst = new GenericInstruction(store_list);
  		load_inst->setAddress(m_mbench_src_inst_addr);
  		store_inst->setAddress(m_mbench_dest_inst_addr);
  		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  		load_inst->setAtomic(false);
  		load_inst->setDisassembly("");
  		store_inst->setAtomic(false);
  		store_inst->setDisassembly("");
			//Create uops for these instructions..
  		std::vector<const MicroOp *> *load_uops 
															= new std::vector<const MicroOp*>();
			std::vector<const MicroOp *> *store_uops 
															= new std::vector<const MicroOp*>();;
  		MicroOp *currentLMicroOp = new MicroOp();
  		MicroOp *currentSMicroOp = new MicroOp();
  		currentLMicroOp->makeLoad(
  			0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentSMicroOp->makeStore(
  			0
				,0
  		  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
  		  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
  		  , m_microbench_opsize //memop_load_size[0]
  		 );
  		currentLMicroOp->addDestinationRegister((xed_reg_enum_t)reg, ""); 
  		currentSMicroOp->addSourceRegister((xed_reg_enum_t)reg, "");
  		currentLMicroOp->setOperandSize(64); 
  		currentLMicroOp->setInstruction(load_inst);
  		currentLMicroOp->setFirst(true);
  		currentLMicroOp->setLast(true);
  		load_uops->push_back(currentLMicroOp);
  		load_inst->setMicroOps(load_uops);
  		currentSMicroOp->setOperandSize(64); 
  		currentSMicroOp->setInstruction(store_inst);
  		currentSMicroOp->setFirst(true);
  		currentSMicroOp->setLast(true);
  		store_uops->push_back(currentSMicroOp);
  		store_inst->setMicroOps(store_uops);
			m_mbench_src_ins.push_back(load_inst);
			m_mbench_dest_ins.push_back(store_inst);
			--total_pairs;
		}
	}
	total_pairs	= m_microbench_loopsize/m_microbench_opsize;
	while(total_pairs) {
		DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_src_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_src_addr, m_microbench_opsize, 
			Operand::READ, 0, HitWhere::UNKNOWN);
		DynamicInstructionInfo sinfo = DynamicInstructionInfo::createMemoryInfo(
			m_mbench_dest_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), m_mbench_dest_addr, m_microbench_opsize, 
			Operand::WRITE, 0, HitWhere::UNKNOWN);
		m_mbench_src_dyn_ins_info.push_back(linfo);
		m_mbench_dest_dyn_ins_info.push_back(sinfo);
		m_mbench_src_addr += m_microbench_opsize;
		m_mbench_dest_addr += m_microbench_opsize;
		--total_pairs;
	}
}

//#ifdef PIC_ENABLE_CHECKPOINT
UInt64 MemoryManager::getCheckpointInstructionsCount() {
	if(m_pic_on) {
		if(m_pic_use_vpic)
			return m_num_checkpoints; //a instruction per checkpoint
		else
			return m_num_checkpoints * (CHKPT_PAGE_SIZE/64);
	}
	else
		return m_num_checkpoints * 2 * (CHKPT_PAGE_SIZE/m_chkpt_opsize);
}

void
MemoryManager::startCheckpointing() {
	m_is_checkpointing = true; 
	m_last_checkpoint_time	= 
			getCore()->getPerformanceModel()->getElapsedTime();
	m_last_chkpt_ins_cnt =
			getCore()->getPerformanceModel()->getInstructionCount();
	m_last_checkpoint_page_addr 	= 0;
  //printf("[CHKPNT]: START\n");
}

bool 
MemoryManager::startNewInterval() {

	/*UInt64 cur_cycles = 
		SubsecondTime::divideRounded(
			getCore()->getPerformanceModel()->getElapsedTime(),
      getCore()->getDvfsDomain()->getPeriod());
	UInt64 prev_cycles = 
		SubsecondTime::divideRounded(
			m_last_checkpoint_time,
      getCore()->getDvfsDomain()->getPeriod());
	assert(cur_cycles >= prev_cycles);
	if((cur_cycles - prev_cycles) > CHKPT_INTERVAL)
		return true;*/
	long int cur_ins		= getCore()->getPerformanceModel()->getInstructionCount();
  long int chkpt_ins	= getCheckpointInstructionsCount();
	long int ins_done		= (cur_ins - chkpt_ins) - m_last_chkpt_ins_cnt;
	//assert(cur_ins >= (chkpt_ins + m_last_chkpt_ins_cnt));
	if( ins_done > (long int)m_chkpt_interval) {
		return true;
	}
	return false;
}

bool
MemoryManager::needs_checkpointing(IntPtr page_address) {
	return ((m_checkpointed_pages.find(page_address)
								== m_checkpointed_pages.end() ));
}

void
MemoryManager::inspect_page_access(IntPtr address) {
	//get page address
	IntPtr page_address	= address - (address % CHKPT_PAGE_SIZE);
	if(m_is_checkpointing) {
  	//printf("[CHKPNT]: INSPECT: %lu\n", page_address);
		//is this a page we CHECKPOINT TO? IGNORE IT THEN
		if((m_no_checkpointed_pages.find(page_address)
								!= m_no_checkpointed_pages.end())) {
			return;
		}
		else
			m_app_pages.insert(page_address); //Remeber app pages
		
  	//printf("[CHKPNT] INSPECT: %lu\n", page_address);
		//Do we have to start a new interval?
		if(startNewInterval()) {
			m_last_checkpoint_time	= 
				getCore()->getPerformanceModel()->getElapsedTime();
			m_last_chkpt_ins_cnt =
				getCore()->getPerformanceModel()->getInstructionCount() 
				- getCheckpointInstructionsCount() ;
			m_checkpointed_pages.clear();
  		//printf("[CHKPNT]: NEW_I\n");
			m_num_intervals++;
		}
		//Was this page accessed before?
		if(needs_checkpointing(page_address)) {
			//Need to issue loads/stores
			m_checkpointed_pages.insert(page_address);

			//Take a checkpoint
			take_checkpoint(page_address);
			m_num_checkpoints++;

			//TODO: Remove this
			//stopCheckpointing();
		}
	}
}
void MemoryManager::create_vpic_checkpoint_instructions(int num_pairs, 
	IntPtr l_address, IntPtr s_address) {
	assert((m_chkpt_l_dyn_ins_info.size() == 0) && 
									(m_chkpt_s_dyn_ins_info.size() == 0));
	if(m_chkpt_stores.size() == 0) {
		OperandList store_list;
  	store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  	Instruction *store_inst = new GenericInstruction(store_list);
  	store_inst->setAddress(m_chkpt_store_inst_addr);
  	store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
  	store_inst->setAtomic(false);
  	store_inst->setDisassembly("");
	
		std::vector<const MicroOp *> *store_uops 
														= new std::vector<const MicroOp*>();;
  	MicroOp *currentSMicroOp = new MicroOp();
  	currentSMicroOp->setInstructionPointer(
			Memory::make_access(m_chkpt_store_inst_addr));
  	currentSMicroOp->makeStore(
  		0
			,0
    	, XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
    	, "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    	, 8 //memop_load_size[0]
   	);

  	currentSMicroOp->setOperandSize(64); 
  	currentSMicroOp->setInstruction(store_inst);
  	currentSMicroOp->setFirst(true);
  	currentSMicroOp->setLast(true);

  	store_uops->push_back(currentSMicroOp);
  	store_inst->setMicroOps(store_uops);
		m_chkpt_stores.push_back(store_inst);
	}
	//Just before the instruction is looked in handleInstruction.. 
	//they do this
	DynamicInstructionInfo sinfo = DynamicInstructionInfo::
																	createMemoryInfo(
		m_chkpt_store_inst_addr,//ins address 
		true, //False if instruction will not be 
										//executed because of predication
		SubsecondTime::Zero(), s_address, 64, Operand::WRITE, 0, 
		HitWhere::UNKNOWN);
	m_chkpt_s_dyn_ins_info.push_back(sinfo);

	struct PicInsInfo pii;
	pii.op						= CacheCntlr::PIC_COPY;	
	pii.other_source 	= l_address;
	pii.other_source2	= 0;
	pii.is_vpic				= true;
	pii.count					= num_pairs;
	picInsInfoMap[s_address] = pii;
}
void MemoryManager::create_pic_checkpoint_instructions(int num_pairs, 
	IntPtr l_address, IntPtr s_address) {
	assert((m_chkpt_l_dyn_ins_info.size() == 0) && 
									(m_chkpt_s_dyn_ins_info.size() == 0));
	int total_pairs	= num_pairs;
	if(m_chkpt_stores.size() == 0) {
		while(total_pairs) {
			OperandList store_list;
   		store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
  		Instruction *store_inst = new GenericInstruction(store_list);
   		store_inst->setAddress(m_chkpt_store_inst_addr);
   		store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
   		store_inst->setAtomic(false);
   		store_inst->setDisassembly("");
	
			std::vector<const MicroOp *> *store_uops 
															= new std::vector<const MicroOp*>();;
   		MicroOp *currentSMicroOp = new MicroOp();
   		currentSMicroOp->setInstructionPointer(
				Memory::make_access(m_chkpt_store_inst_addr));
    	currentSMicroOp->makeStore(
    		0
				,0
      	, XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
      	, "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
      	, 8 //memop_load_size[0]
     	);

    	currentSMicroOp->setOperandSize(64); 
    	currentSMicroOp->setInstruction(store_inst);
    	currentSMicroOp->setFirst(true);
    	currentSMicroOp->setLast(true);

   		store_uops->push_back(currentSMicroOp);
   		store_inst->setMicroOps(store_uops);
			m_chkpt_stores.push_back(store_inst);
			--total_pairs;
		}
	}
	while(num_pairs) {
		//Just before the instruction is looked in handleInstruction.. 
		//they do this
		DynamicInstructionInfo sinfo = DynamicInstructionInfo::
																		createMemoryInfo(
			m_chkpt_store_inst_addr,//ins address 
			true, //False if instruction will not be 
											//executed because of predication
			SubsecondTime::Zero(), s_address, 64, Operand::WRITE, 0, 
			HitWhere::UNKNOWN);
		m_chkpt_s_dyn_ins_info.push_back(sinfo);

		struct PicInsInfo pii;
		pii.op										= CacheCntlr::PIC_COPY;	
		pii.other_source 					= l_address;
		pii.other_source2	= 0;
		pii.is_vpic								= false;
		pii.count									= 0;
		picInsInfoMap[s_address] 	= pii;
		
		l_address += 64;
		s_address += 64;
		--num_pairs;
	}
}

void MemoryManager::create_checkpoint_instructions(int num_pairs, 
	IntPtr l_address, IntPtr s_address) {
	int total_pairs	= num_pairs;
	assert((m_chkpt_l_dyn_ins_info.size() == 0) && 
									(m_chkpt_s_dyn_ins_info.size() == 0));
	if(m_chkpt_loads.size() == 0) {
		while(total_pairs) {
			OperandList load_list;
			OperandList store_list;

			int reg	= m_chkpt_regs[0];
			m_chkpt_regs.erase(m_chkpt_regs.begin());
			m_chkpt_regs.push_back(reg);

			load_list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));
   		load_list.push_back(Operand(Operand::REG, reg, Operand::WRITE, 
																																	"", true));
   		store_list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
   		store_list.push_back(Operand(Operand::REG, reg, Operand::READ, 
																																	"", true));
  		Instruction *load_inst = new GenericInstruction(load_list);
  		Instruction *store_inst = new GenericInstruction(store_list);
   		load_inst->setAddress(m_chkpt_load_inst_addr);
   		store_inst->setAddress(m_chkpt_store_inst_addr);
   		load_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
   		store_inst->setSize(4); //Possible sizes seen (L:1-9, S:1-8)
   		load_inst->setAtomic(false);
   		load_inst->setDisassembly("");
   		store_inst->setAtomic(false);
   		store_inst->setDisassembly("");
			//Create uops for these instructions..
   		std::vector<const MicroOp *> *load_uops 
																= new std::vector<const MicroOp*>();
			std::vector<const MicroOp *> *store_uops 
																= new std::vector<const MicroOp*>();;
    	MicroOp *currentLMicroOp = new MicroOp();
   		MicroOp *currentSMicroOp = new MicroOp();
   		//currentLMicroOp->setInstructionPointer(
					//Memory::make_access(m_chkpt_load_inst_addr));
   		//currentSMicroOp->setInstructionPointer(
					//Memory::make_access(m_chkpt_store_inst_addr));
    	currentLMicroOp->makeLoad(
    		0
    	  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
    	  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    	  , m_chkpt_opsize //memop_load_size[0]
    	 );
    	currentSMicroOp->makeStore(
    		0
				,0
    	  , XED_ICLASS_MOVQ //TODO: xed_decoded_inst_get_iclass(ins)
    	  , "" //xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
    	  , m_chkpt_opsize //memop_load_size[0]
    	 );
    	currentLMicroOp->addDestinationRegister((xed_reg_enum_t)reg, ""); 
																//String(xed_reg_enum_t2str(reg)
    	currentSMicroOp->addSourceRegister((xed_reg_enum_t)reg, "");
    	//currentMicroOp->addAddressRegister(reg, 
			//String(xed_reg_enum_t2str(reg))); //used in rob timer, ignore for now
			//xed_decoded_inst_get_operand_width, 8/16/32/64
    	currentLMicroOp->setOperandSize(64); 
    	currentLMicroOp->setInstruction(load_inst);
    	currentLMicroOp->setFirst(true);
    	currentLMicroOp->setLast(true);
   		load_uops->push_back(currentLMicroOp);
   		load_inst->setMicroOps(load_uops);

    	currentSMicroOp->setOperandSize(64); 
    	currentSMicroOp->setInstruction(store_inst);
    	currentSMicroOp->setFirst(true);
    	currentSMicroOp->setLast(true);
   		store_uops->push_back(currentSMicroOp);
   		store_inst->setMicroOps(store_uops);

			m_chkpt_loads.push_back(load_inst);
			m_chkpt_stores.push_back(store_inst);
			--total_pairs;
		}
	}
	while(num_pairs) {
		//Just before the instruction is looked in handleInstruction.. 
		//they do this
		DynamicInstructionInfo linfo = DynamicInstructionInfo::createMemoryInfo(
			m_chkpt_load_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), l_address, m_chkpt_opsize, Operand::READ, 0, 
			HitWhere::UNKNOWN);
		DynamicInstructionInfo sinfo = DynamicInstructionInfo::createMemoryInfo(
			m_chkpt_store_inst_addr,//ins address 
			true, //False if instruction will not be executed because of predication
			SubsecondTime::Zero(), s_address, m_chkpt_opsize, Operand::WRITE, 0, 
			HitWhere::UNKNOWN);
		m_chkpt_l_dyn_ins_info.push_back(linfo);
		m_chkpt_s_dyn_ins_info.push_back(sinfo);
		l_address += m_chkpt_opsize;
		s_address += m_chkpt_opsize;
		--num_pairs;
	}
}

void MemoryManager::take_checkpoint(IntPtr page_address) {
  UInt32 cache_block_size = getCacheBlockSize();
  IntPtr begin_addr = page_address;
  IntPtr end_addr = page_address + CHKPT_PAGE_SIZE;
  IntPtr begin_addr_aligned = begin_addr - (begin_addr % cache_block_size);
  IntPtr end_addr_aligned = end_addr - (end_addr % cache_block_size);
	IntPtr checkpoint_addr_aligned;
	//Where to checkpoint???
	if(m_last_checkpoint_page_addr == 0) {
		//offset by 2GB
		checkpoint_addr_aligned = begin_addr_aligned + 
													(CHKPT_PAGE_SIZE * 524288);
	}
	else {
		checkpoint_addr_aligned = m_last_checkpoint_page_addr + CHKPT_PAGE_SIZE;
 	}
	//Your destination should never be an app page
	assert((m_app_pages.find(checkpoint_addr_aligned)
								== m_app_pages.end()));

	m_last_checkpoint_page_addr = checkpoint_addr_aligned;
	m_no_checkpointed_pages.insert(m_last_checkpoint_page_addr);
	
  //printf("[CHKPNT]: Taking{%lu: %lu}\n",
		//begin_addr_aligned, checkpoint_addr_aligned); 
	assert(begin_addr == begin_addr_aligned);
	assert(end_addr == end_addr_aligned);
	if(m_pic_on) {
		if(m_pic_use_vpic) {
			create_vpic_checkpoint_instructions(CHKPT_PAGE_SIZE/64, 
																		begin_addr_aligned, 
																		checkpoint_addr_aligned);
		}
		else {
			create_pic_checkpoint_instructions(CHKPT_PAGE_SIZE/64, 
																		begin_addr_aligned, 
																		checkpoint_addr_aligned);
		}
		unsigned int num_chkpt_pics 	= m_chkpt_stores.size();
		while(num_chkpt_pics) {
  		getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																						(m_chkpt_s_dyn_ins_info[0], true);
  		getCore()->getPerformanceModel()->queueInstruction(
																						m_chkpt_stores[0], true);
			m_chkpt_stores.erase(m_chkpt_stores.begin());
			m_chkpt_s_dyn_ins_info.erase(m_chkpt_s_dyn_ins_info.begin());
			--num_chkpt_pics;
		}
		assert(m_chkpt_stores.size() == 0);
		assert(m_chkpt_s_dyn_ins_info.size() == 0);
	}
	else {
		create_checkpoint_instructions(CHKPT_PAGE_SIZE/m_chkpt_opsize, 
																		begin_addr_aligned, 
																		checkpoint_addr_aligned);
		unsigned int loads_8batches		= (m_chkpt_loads.size()/8);
		assert(m_chkpt_loads.size() == m_chkpt_l_dyn_ins_info.size());
		assert(m_chkpt_loads.size() == m_chkpt_s_dyn_ins_info.size());
	
		unsigned int num_chkpt_loads_cnt = 0;	
		unsigned int num_chkpt_stores_cnt = 0;	
		//Add them in respective places
		//8 loads and then 8 stores , max registers we have
		while(loads_8batches) {
			int counter = 8;
			while(counter) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																						(m_chkpt_l_dyn_ins_info[0], true);
				m_chkpt_l_dyn_ins_info.erase(m_chkpt_l_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_chkpt_loads[num_chkpt_loads_cnt], true);
				--counter;
				++num_chkpt_loads_cnt;
			}
			counter = 8;
			while(counter) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																						(m_chkpt_s_dyn_ins_info[0], true);
				m_chkpt_s_dyn_ins_info.erase(m_chkpt_s_dyn_ins_info.begin());
  			getCore()->getPerformanceModel()->queueInstruction(
															m_chkpt_stores[num_chkpt_stores_cnt], true);
				--counter;
				++num_chkpt_stores_cnt;
			}
			--loads_8batches;
		}
		//Still left?
		if(m_chkpt_l_dyn_ins_info.size()) {
			//loads first
			while(m_chkpt_l_dyn_ins_info.size()) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																						(m_chkpt_l_dyn_ins_info[0], true);
  			getCore()->getPerformanceModel()->queueInstruction(
																m_chkpt_loads[num_chkpt_loads_cnt], true);
				m_chkpt_l_dyn_ins_info.erase(m_chkpt_l_dyn_ins_info.begin());
				++num_chkpt_loads_cnt;
			}
			//stores next
			while(m_chkpt_s_dyn_ins_info.size()) {
  			getCore()->getPerformanceModel()->pushDynamicInstructionInfo
																						(m_chkpt_s_dyn_ins_info[0], true);
  			getCore()->getPerformanceModel()->queueInstruction(
																m_chkpt_stores[num_chkpt_stores_cnt], true);
				m_chkpt_s_dyn_ins_info.erase(m_chkpt_s_dyn_ins_info.begin());
				++num_chkpt_stores_cnt;
			}
		}
		assert(m_chkpt_l_dyn_ins_info.size() == 0);
		assert(m_chkpt_s_dyn_ins_info.size() == 0);
	}

  	/*m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
         Core::NONE, Core::READ,
         curr_addr_aligned, 0, NULL, 8,
         true, true);
  	m_cache_cntlrs[MemComponent::L1_DCACHE]->processMemOpFromCore(
         Core::NONE, Core::WRITE,
         checkpoint_addr_aligned, 0, NULL, 8,
         true, true);*/
}
//#endif

MemoryManager::~MemoryManager()
{
   UInt32 i;

   getNetwork()->unregisterCallback(SHARED_MEM_1);

   // Delete the Models

   if (m_itlb) delete m_itlb;
   if (m_dtlb) delete m_dtlb;
   if (m_stlb) delete m_stlb;

   for(i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      delete m_cache_perf_models[(MemComponent::component_t)i];
      m_cache_perf_models[(MemComponent::component_t)i] = NULL;
   }

   delete m_user_thread_sem;
   delete m_network_thread_sem;
   delete m_tag_directory_home_lookup;
   delete m_dram_controller_home_lookup;

   for(i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      delete m_cache_cntlrs[(MemComponent::component_t)i];
      m_cache_cntlrs[(MemComponent::component_t)i] = NULL;
   }

   if (m_nuca_cache)
      delete m_nuca_cache;
   if (m_dram_cache)
      delete m_dram_cache;
   if (m_dram_cntlr)
      delete m_dram_cntlr;
   if (m_dram_directory_cntlr)
      delete m_dram_directory_cntlr;
}

HitWhere::where_t
MemoryManager::coreInitiateMemoryAccess(
      MemComponent::component_t mem_component,
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      Core::MemModeled modeled)
{
   LOG_ASSERT_ERROR(mem_component <= m_last_level_cache,
      "Error: invalid mem_component (%d) for coreInitiateMemoryAccess", mem_component);

   if (mem_component == MemComponent::L1_ICACHE && m_itlb)
      accessTLB(m_itlb, address, true, modeled);
   else if (mem_component == MemComponent::L1_DCACHE && m_dtlb) {
      accessTLB(m_dtlb, address, false, modeled);
			if(m_pic_on && m_microbench_run) {
				if(picInsInfoMap.find(address) != picInsInfoMap.end()) {
					struct PicInsInfo pii = picInsInfoMap[address];
      		accessTLB(m_dtlb, pii.other_source, false, modeled);
				}
			}
	 }

   //CAP: Forward CAP operation to Cache Ctlr 
   if(m_cap_on) {
      if(capInsInfoMap.find(address) != capInsInfoMap.end()) {
				struct CAPInsInfo cii = capInsInfoMap[address];
				capInsInfoMap.erase(address);
        if(cii.op != CacheCntlr::CAP_NONE) 
   			  return m_cache_cntlrs[MemComponent::L3_CACHE]->processCAPSOpFromCore(cii.op, address);
			}
   }  
 
   
		//#ifdef PIC_ENABLE_CHECKPOINT
		if(m_chkpt_run) {
			if ((m_enabled) && ( mem_op_type == Core::WRITE) && 
					(mem_component == MemComponent::L1_DCACHE))
				inspect_page_access(address);
			if(m_enabled 
					//&& (m_last_checkpoint_time == SubsecondTime::Zero()) 
					&& (m_last_chkpt_ins_cnt == 0) 
					&& !isCheckpointing())
				startCheckpointing();
		}
		//#endif

		if(m_pic_on) {
			//Check if this is a PIC OPERATION
			if(picInsInfoVec.size()) {
				IntPtr pic_addr = (((picInsInfoVec[0].first) /64) * 64);
				if((pic_addr == address)) {
					struct PicInsInfo pii =  picInsInfoVec[0].second;
					picInsInfoVec.erase(picInsInfoVec.begin());
					if(!pii.is_vpic) {
						assert(pii.count == 0);
   					return m_cache_cntlrs[mem_component]->processPicSOpFromCore(
						pii.op, pii.other_source, address);
					}
					else {
						assert(pii.count != 0);
						if(pii.other_source2) {
							if(pii.op == CacheCntlr::PIC_CLMULT)
return m_cache_cntlrs[mem_component]->processPicVOpFromCoreCLMULT
(pii.op, pii.other_source, pii.other_source2, address, m_microbench_opsize);
							else if(pii.op == CacheCntlr::PIC_LOGICAL)
return m_cache_cntlrs[mem_component]->processPicVOpFromCoreLOGICAL(CacheCntlr::PIC_LOGICAL, 1024, 1088, 1152, 1);
						}
						else
   						return m_cache_cntlrs[mem_component]->processPicVOpFromCore(
							pii.op, pii.other_source, address, pii.count);
					}
				}
			}
			if(picInsInfoMap.find(address) != picInsInfoMap.end()) {
				struct PicInsInfo pii = picInsInfoMap[address];
				picInsInfoMap.erase(address);
				if(!pii.is_vpic) {
					assert(pii.count == 0);
   				return m_cache_cntlrs[mem_component]->processPicSOpFromCore(
					pii.op, pii.other_source, address);
				}
				else {
					assert(pii.count != 0);
					//TODO: For checkpointing I want the second source to come first
					if(m_microbench_run) {
						if(pii.other_source2) {
							assert(0);
						}
						else {
   						return m_cache_cntlrs[mem_component]->processPicVOpFromCore(
								pii.op, address, pii.other_source,
								pii.count);
							}
					}
					else
   					return m_cache_cntlrs[mem_component]->processPicVOpFromCore(
							pii.op, pii.other_source, address, 
							pii.count);
				}
			}
		}

   if(m_cap_on) mem_component = MemComponent::L3_CACHE;

   return m_cache_cntlrs[mem_component]->processMemOpFromCore(
         lock_signal,
         mem_op_type,
         address, offset,
         data_buf, data_length,
         modeled == Core::MEM_MODELED_NONE || modeled == Core::MEM_MODELED_COUNT ? false : true,
         modeled == Core::MEM_MODELED_NONE ? false : true);
}

void
MemoryManager::handleMsgFromNetwork(NetPacket& packet)
{
MYLOG("begin");
   core_id_t sender = packet.sender;
   PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg = PrL1PrL2DramDirectoryMSI::ShmemMsg::getShmemMsg((Byte*) packet.data);
   SubsecondTime msg_time = packet.time;

   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_SIM_THREAD, msg_time);
   shmem_msg->getPerf()->updatePacket(packet);

   MemComponent::component_t receiver_mem_component = shmem_msg->getReceiverMemComponent();
   MemComponent::component_t sender_mem_component = shmem_msg->getSenderMemComponent();

   if (m_enabled)
   {
      LOG_PRINT("Got Shmem Msg: type(%i), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), sender(%i), receiver(%i)",
            shmem_msg->getMsgType(), shmem_msg->getAddress(), sender_mem_component, receiver_mem_component, sender, packet.receiver);
   }

   switch (receiver_mem_component)
   {
      case MemComponent::L2_CACHE: /* PrL1PrL2DramDirectoryMSI::DramCntlr sends to L2 and doesn't know about our other levels */
      case MemComponent::LAST_LEVEL_CACHE:
         switch(sender_mem_component)
         {
            case MemComponent::TAG_DIR:
               m_cache_cntlrs[m_last_level_cache]->handleMsgFromDramDirectory(sender, shmem_msg);
               break;

            default:
               LOG_PRINT_ERROR("Unrecognized sender component(%u)",
                     sender_mem_component);
               break;
         }
         break;

      case MemComponent::TAG_DIR:
         switch(sender_mem_component)
         {
            LOG_ASSERT_ERROR(m_tag_directory_present, "Tag directory NOT present");

            case MemComponent::LAST_LEVEL_CACHE:
               m_dram_directory_cntlr->handleMsgFromL2Cache(sender, shmem_msg);
               break;

            case MemComponent::DRAM:
               m_dram_directory_cntlr->handleMsgFromDRAM(sender, shmem_msg);
               break;

            default:
               LOG_PRINT_ERROR("Unrecognized sender component(%u)",
                     sender_mem_component);
               break;
         }
         break;

      case MemComponent::DRAM:
         switch(sender_mem_component)
         {
            LOG_ASSERT_ERROR(m_dram_cntlr_present, "Dram Cntlr NOT present");

            case MemComponent::TAG_DIR:
            {
               DramCntlrInterface* dram_interface = m_dram_cache ? (DramCntlrInterface*)m_dram_cache : (DramCntlrInterface*)m_dram_cntlr;
               dram_interface->handleMsgFromTagDirectory(sender, shmem_msg);
               break;
            }

            default:
               LOG_PRINT_ERROR("Unrecognized sender component(%u)",
                     sender_mem_component);
               break;
         }
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized receiver component(%u)",
               receiver_mem_component);
         break;
   }

   // Delete the allocated Shared Memory Message
   // First delete 'data_buf' if it is present
   // LOG_PRINT("Finished handling Shmem Msg");

   if (shmem_msg->getDataLength() > 0)
   {
      assert(shmem_msg->getDataBuf());
      delete [] shmem_msg->getDataBuf();
   }
   delete shmem_msg;
MYLOG("end");
}
void
MemoryManager::sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf, UInt32 data_length, HitWhere::where_t where, ShmemPerf *perf, ShmemPerfModel::Thread_t thread_num , IntPtr other_pic_address, IntPtr other_pic_address2)
{
MYLOG("send msg %u %ul%u > %ul%u", msg_type, requester, sender_mem_component, receiver, receiver_mem_component);
   assert((data_buf == NULL) == (data_length == 0));
   PrL1PrL2DramDirectoryMSI::ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length, perf);
   shmem_msg.setWhere(where);

	 if(m_pic_on) {
		shmem_msg.m_other_address		= other_pic_address;
		shmem_msg.m_other_address2	= other_pic_address2;
	 }

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(thread_num);
   perf->updateTime(msg_time);

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), receiver);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         m_core_id_master, receiver,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf, UInt32 data_length, ShmemPerf *perf, ShmemPerfModel::Thread_t thread_num)
{
MYLOG("bcast msg");
   assert((data_buf == NULL) == (data_length == 0));
   PrL1PrL2DramDirectoryMSI::ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length, perf);

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(thread_num);
   perf->updateTime(msg_time);

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), NetPacket::BROADCAST);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         m_core_id_master, NetPacket::BROADCAST,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::accessTLB(TLB * tlb, IntPtr address, bool isIfetch, Core::MemModeled modeled)
{
   bool hit = tlb->lookup(address, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
   if (hit == false
       && !(modeled == Core::MEM_MODELED_NONE || modeled == Core::MEM_MODELED_COUNT)
       && m_tlb_miss_penalty.getLatency() != SubsecondTime::Zero()
   )
   {
      if (m_tlb_miss_parallel)
      {
         incrElapsedTime(m_tlb_miss_penalty.getLatency(), ShmemPerfModel::_USER_THREAD);
      }
      else
      {
         Instruction *i = new TLBMissInstruction(m_tlb_miss_penalty.getLatency(), isIfetch);
         getCore()->getPerformanceModel()->queueDynamicInstruction(i);
      }
   }
}

SubsecondTime
MemoryManager::getCost(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type)
{
   if (mem_component == MemComponent::INVALID_MEM_COMPONENT)
      return SubsecondTime::Zero();

   return m_cache_perf_models[mem_component]->getLatency(access_type);
}

void
MemoryManager::incrElapsedTime(SubsecondTime latency, ShmemPerfModel::Thread_t thread_num)
{
   MYLOG("cycles += %s", itostr(latency).c_str());
   getShmemPerfModel()->incrElapsedTime(latency, thread_num);
}

void
MemoryManager::incrElapsedTime(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type, ShmemPerfModel::Thread_t thread_num)
{
   incrElapsedTime(getCost(mem_component, access_type), thread_num);
}

void
MemoryManager::enableModels()
{
   m_enabled = true;

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      m_cache_cntlrs[(MemComponent::component_t)i]->enable();
      m_cache_perf_models[(MemComponent::component_t)i]->enable();
   }

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->enable();
}

void
MemoryManager::disableModels()
{
   m_enabled = false;

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      m_cache_cntlrs[(MemComponent::component_t)i]->disable();
      m_cache_perf_models[(MemComponent::component_t)i]->disable();
   }

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->disable();
}

}
