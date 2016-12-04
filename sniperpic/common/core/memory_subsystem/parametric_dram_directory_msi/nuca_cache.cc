#include "nuca_cache.h"
#include "memory_manager_base.h"
#include "pr_l1_cache_block_info.h"
#include "config.hpp"
#include "stats.h"
#include "queue_model.h"
#include "shmem_perf.h"

NucaCache::NucaCache(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, AddressHomeLookup* home_lookup, UInt32 cache_block_size, ParametricDramDirectoryMSI::CacheParameters& parameters)
   : m_core_id(memory_manager->getCore()->getId())
   , m_memory_manager(memory_manager)
   , m_shmem_perf_model(shmem_perf_model)
   , m_home_lookup(home_lookup)
   , m_cache_block_size(cache_block_size)
   , m_data_access_time(parameters.data_access_time)
   , m_tags_access_time(parameters.tags_access_time)
   , m_data_array_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/nuca/bandwidth"))
   , m_queue_model(NULL)
   , m_reads(0)
   , m_writes(0)
   , m_read_misses(0)
   , m_write_misses(0)
{
   m_cache = new Cache("nuca-cache",
      "perf_model/nuca/cache",
      m_core_id,
      parameters.num_sets,
      parameters.associativity,
      m_cache_block_size,
      parameters.replacement_policy,
      CacheBase::PR_L1_CACHE,
      CacheBase::parseAddressHash(parameters.hash_function),
      NULL, /* FaultinjectionManager */
      home_lookup
   );

   if (Sim()->getCfg()->getBool("perf_model/nuca/queue_model/enabled"))
   {
      String queue_model_type = Sim()->getCfg()->getString("perf_model/nuca/queue_model/type");
      m_queue_model = QueueModel::create("nuca-cache-queue", m_core_id, queue_model_type, m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size)); // bytes to bits
   }

   registerStatsMetric("nuca-cache", m_core_id, "reads", &m_reads);
   registerStatsMetric("nuca-cache", m_core_id, "writes", &m_writes);
   registerStatsMetric("nuca-cache", m_core_id, "read-misses", &m_read_misses);
   registerStatsMetric("nuca-cache", m_core_id, "write-misses", &m_write_misses);
   registerStatsMetric("nuca-cache", m_core_id, "dirty_evicts", &m_dirty_evicts);
	 if(Sim()->getCfg()->getBool("general/pic_on")) {

			if(Sim()->getCfg()->getBool("general/microbench_run")) {
				m_microbench_loopsize	= Sim()->getCfg()->getInt("general/microbench_loopsize");
				m_microbench_type	= Sim()->getCfg()->getInt("general/microbench_type");
				m_distinct_search_keys = 
					Sim()->getCfg()->getInt("general/microbench_totalsize")/
					m_microbench_loopsize;
				m_microbench_outer_loops = 
											Sim()->getCfg()->getInt("general/microbench_outer_loops");
			}
			else {
				m_microbench_loopsize = 0;
				m_microbench_type = -1;
			}

   	for(ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t start = 
			ParametricDramDirectoryMSI::CacheCntlr::PIC_COPY; 
			start < ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_OPS; 
			start = ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t(int(start)+1)) {
   		const char * op_str = 
				ParametricDramDirectoryMSI::picOpString(start);
    		registerStatsMetric("nuca-cache", m_core_id, 
					String("pic_ops_")+op_str, &pic_ops[(int)start]);

    	for(ParametricDramDirectoryMSI::CacheCntlr::pic_map_policy_t map_start = 
			ParametricDramDirectoryMSI::CacheCntlr::PIC_ALL_WAYS_ONE_BANK; 
			map_start < ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_MAP_POLICY; 
		  map_start = ParametricDramDirectoryMSI::CacheCntlr::pic_map_policy_t
																													(int(map_start)+1)) {
   				const char * map_str = ParametricDramDirectoryMSI::
																										picMapString(map_start);
    			registerStatsMetric("nuca-cache", m_core_id, 
					String("pic_ops_in_bank") + String("_") + op_str 
																		+ String("_") + map_str, 
									&pic_ops_in_bank[(int)start][(int)map_start]);
			 }
		}
   registerStatsMetric("nuca-cache", m_core_id, "pic_key_writes", &pic_key_writes);
	}
}

NucaCache::~NucaCache()
{
   delete m_cache;
   if (m_queue_model)
      delete m_queue_model;
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NucaCache::read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf , bool count)
{
   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time.getLatency();
   perf->updateTime(now + latency, ShmemPerf::NUCA_TAGS);

   if (block_info)
   {
      m_cache->accessSingleLine(address, Cache::LOAD, data_buf, m_cache_block_size, now + latency, true);

      latency += accessDataArray(Cache::LOAD, now + latency, perf);
      hit_where = HitWhere::NUCA_CACHE;
   }
   else
   {
      if (count) ++m_read_misses;
   }
   if (count)  ++m_reads;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, hit_where);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NucaCache::write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now, bool count, 
bool &evict_no_wb, IntPtr other_address, IntPtr other_address2)
{
   HitWhere::where_t hit_where = HitWhere::MISS;

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time.getLatency();

   if (block_info)
   {
      block_info->setCState(CacheState::MODIFIED);
      m_cache->accessSingleLine(address, Cache::STORE, data_buf, m_cache_block_size, now + latency, true);

      latency += accessDataArray(Cache::STORE, now + latency, NULL);
      hit_where = HitWhere::NUCA_CACHE;
   }
   else
   {
      PrL1CacheBlockInfo evict_block_info;
      m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         now + latency, NULL, other_address, other_address2);
			evict_no_wb = eviction;
      if (eviction)
      {
				 //STAT_FIX: Count eviction only if dirty
         if (evict_block_info.getCState() != CacheState::MODIFIED)
         {
            // Unless data is dirty, don't have caller write it back
            eviction = false;
         }
				 else
					++m_dirty_evicts;
      }
      if (count)  ++m_write_misses;
   }
   if (count)  ++m_writes;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, hit_where);
}

SubsecondTime
NucaCache::accessDataArray(Cache::access_t access, SubsecondTime t_start, ShmemPerf *perf)
{
   perf->updateTime(t_start);

   // Compute Queue Delay
   SubsecondTime queue_delay;
   if (m_queue_model)
   {
      SubsecondTime processing_time = m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size); // bytes to bits

      queue_delay = processing_time + m_queue_model->computeQueueDelay(t_start, processing_time, m_core_id);

      perf->updateTime(t_start + processing_time, ShmemPerf::NUCA_BUS);
      perf->updateTime(t_start + queue_delay, ShmemPerf::NUCA_QUEUE);
   }
   else
   {
      queue_delay = SubsecondTime::Zero();
   }

   perf->updateTime(t_start + queue_delay + m_data_access_time.getLatency(), ShmemPerf::NUCA_DATA);

   return queue_delay + m_data_access_time.getLatency();
}
//#ifdef PIC_ENABLE_OPERATIONS
//This guy just models time
boost::tuple<SubsecondTime, HitWhere::where_t> NucaCache::picOp
(IntPtr address1, IntPtr address2, SubsecondTime now, 
	ShmemPerf *perf, bool is_copy, 
	ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t pic_opcode) {
	//Fastbit hack: For pic_or, we need to 27 cycles = 8*3 + 3
	//Equivalent to : 1tag + 3 data access 
	 SubsecondTime latency;
	 //Read first
   perf->updateTime(now);
   PrL1CacheBlockInfo* block_info1 = 
		(PrL1CacheBlockInfo*)m_cache->peekSingleLine(address1);
   latency = m_tags_access_time.getLatency();
   perf->updateTime(now + latency, ShmemPerf::NUCA_TAGS);
   assert(block_info1);	//You should have it please
   latency += accessDataArray(Cache::LOAD, now + latency, perf);

	 //Write next
	if(is_copy) {
   	PrL1CacheBlockInfo* block_info2 = 
			(PrL1CacheBlockInfo*)m_cache->peekSingleLine(address2);
		if(m_microbench_type != 5) { //Fastbit hack
   		latency += m_tags_access_time.getLatency();
		}
   	assert(block_info2);
		//TODO: Why was this commented?
  	block_info2->setCState(CacheState::MODIFIED); //Assert this

		if(m_microbench_type == 5) {//another data, Fastbit hack
   		latency += accessDataArray(Cache::LOAD, now + latency, NULL);
		}

   	latency += accessDataArray(Cache::STORE, now + latency, NULL);
	}
	else {
   	PrL1CacheBlockInfo* block_info2 = 
			(PrL1CacheBlockInfo*)m_cache->peekSingleLine(address2);
   	latency += m_tags_access_time.getLatency();
   	perf->updateTime(now + latency, ShmemPerf::NUCA_TAGS);
   	assert(block_info2);
   	latency += accessDataArray(Cache::LOAD, now + latency, NULL);
	}
	picUpdateCounters(pic_opcode, address1, address2);
  return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, 
			HitWhere::NUCA_CACHE);
}
boost::tuple<SubsecondTime, HitWhere::where_t>
NucaCache::picPeek(IntPtr address, SubsecondTime now, ShmemPerf *perf)
{
   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);
   PrL1CacheBlockInfo* block_info = 
			(PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time.getLatency();
   perf->updateTime(now + latency, ShmemPerf::NUCA_TAGS);

   if (block_info){
      hit_where = HitWhere::NUCA_CACHE;
   }
   else {
      ++m_read_misses;
			++m_reads;
   }
   return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, hit_where);
}
void NucaCache::picUpdateCounters(
	ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t pic_opcode, 
  IntPtr ca_address1,IntPtr ca_address2) {
    pic_ops[(int)pic_opcode]++;

		if(m_microbench_loopsize 
			&& (pic_opcode == ParametricDramDirectoryMSI::CacheCntlr::PIC_SEARCH)) {
			//Every 512 byte would need a key write
			int factor	= (m_microbench_loopsize < 512) ? (512/m_microbench_loopsize) : 1;
			pic_key_writes = m_distinct_search_keys /factor;
			pic_key_writes = pic_key_writes * m_microbench_outer_loops;
		}

		UInt32 set1, way1;
		UInt32 set2, way2;
		m_cache->peekSingleLine(ca_address1, &set1, &way1);
		m_cache->peekSingleLine(ca_address2, &set2, &way2);

    for(ParametricDramDirectoryMSI::CacheCntlr::pic_map_policy_t map_start = 
			ParametricDramDirectoryMSI::CacheCntlr::PIC_ALL_WAYS_ONE_BANK; 
			map_start < ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_MAP_POLICY; 
			map_start = ParametricDramDirectoryMSI::CacheCntlr::pic_map_policy_t
			(int(map_start)+1)) {
			if(inSameBank(MemComponent::NUCA_CACHE, set1, way1, set2, way2, 
				map_start))
    		pic_ops_in_bank[(int)pic_opcode][(int)map_start]++;
		}
}
//#endif
