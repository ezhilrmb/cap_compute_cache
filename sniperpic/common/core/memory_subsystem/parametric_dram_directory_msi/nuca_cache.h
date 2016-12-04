#ifndef __NUCA_CACHE_H
#define __NUCA_CACHE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hit_where.h"
#include "cache_cntlr.h"

#include "boost/tuple/tuple.hpp"

class MemoryManagerBase;
class ShmemPerfModel;
class AddressHomeLookup;
class QueueModel;
class ShmemPerf;

class NucaCache
{
   private:
      core_id_t m_core_id;
      MemoryManagerBase *m_memory_manager;
      ShmemPerfModel *m_shmem_perf_model;
      AddressHomeLookup *m_home_lookup;
      UInt32 m_cache_block_size;
      ComponentLatency m_data_access_time;
      ComponentLatency m_tags_access_time;
      ComponentBandwidth m_data_array_bandwidth;

      Cache* m_cache;
      QueueModel *m_queue_model;

      UInt64 m_reads, m_writes, m_read_misses, m_write_misses, m_dirty_evicts;
			int m_microbench_loopsize;
			int m_distinct_search_keys;
			IntPtr m_microbench_outer_loops;
			int m_microbench_type; //for fastbit hack

      SubsecondTime accessDataArray(Cache::access_t access, SubsecondTime t_start, ShmemPerf *perf);

   public:
      NucaCache(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, AddressHomeLookup* home_lookup, UInt32 cache_block_size, ParametricDramDirectoryMSI::CacheParameters& parameters);
      ~NucaCache();

      boost::tuple<SubsecondTime, HitWhere::where_t> read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf, bool count);
      boost::tuple<SubsecondTime, HitWhere::where_t> write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now , bool count, bool &evict_no_wb , IntPtr other_address = 0, IntPtr other_address2 = 0);
			//#ifdef PIC_ENABLE_OPERATIONS
      	boost::tuple<SubsecondTime, HitWhere::where_t> 
					picPeek(IntPtr address, SubsecondTime now, ShmemPerf *perf);

      	boost::tuple<SubsecondTime, HitWhere::where_t> picOp
					(IntPtr address1, IntPtr address2, SubsecondTime now,
					ShmemPerf *perf, bool is_copy, ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t pic_opcode);

          UInt64 pic_ops
						[(ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_OPS)];
          UInt64 pic_ops_in_bank
						[(ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_OPS)]
						[(ParametricDramDirectoryMSI::CacheCntlr::NUM_PIC_MAP_POLICY)];
					void picUpdateCounters(
						ParametricDramDirectoryMSI::CacheCntlr::pic_ops_t pic_opcode, 
  					IntPtr ca_address1, IntPtr ca_address2);
          UInt64 pic_key_writes;
		 //#endif                   			
};

#endif // __NUCA_CACHE_H
