//#include "addr_bank_mapping.h"
#include "cache_cntlr.h"
#include "memory_manager.h"
#include "core_manager.h"
#include "subsecond_time.h"
#include "config.hpp"
#include "shmem_perf.h"
#include "log.h"
#include "mem_component.h"
namespace ParametricDramDirectoryMSI
{
//#ifdef PIC_ENABLE_OPERATIONS
	static int L1_BANKS = 4;					//sub-caches
	static int L2_BANKS = 8;					//sub-caches
	static int L3_BANKS = 128;					//sub-caches
	static int NUCA_CACHE_BANKS = 64;	//sub-caches
	static int L1_SETS = 64;
	static int L2_SETS = 512;
	static int L3_SETS = 8192;
	static int NUCA_CACHE_SETS = 2048;
	static int L1_WAYS = 8;
	static int L2_WAYS = 8;
	static int L3_WAYS = 16;
	static int NUCA_CACHE_WAYS = 16;

	int getBankAllWays(MemComponent::component_t comp, int set, int way){
		switch(comp) {
			case MemComponent::L1_DCACHE:
				return set/(L1_SETS/L1_BANKS);	//16 sets in one sub-cache
			case MemComponent::L2_CACHE:
				return set/(L2_SETS/L2_BANKS);	//64 sets in one sub-cache
			case MemComponent::L3_CACHE:
				return (set/(L3_SETS/L3_BANKS));	//64 sets in one sub-cache
			case MemComponent::NUCA_CACHE:
				return (set/(NUCA_CACHE_SETS/NUCA_CACHE_BANKS));	//16 sets in one sub-cache
			default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
	}
	int getBankMoreSets(MemComponent::component_t comp, int set, int way){
		switch(comp) {
			case MemComponent::L1_DCACHE:
				return way/(L1_WAYS/L1_BANKS);	//two ways of all sets in one bank
			case MemComponent::L2_CACHE:
				return way/(L2_WAYS/L2_BANKS);	//one way of all sets in one bank
			case MemComponent::L3_CACHE:
				return ((set/((L3_SETS * L3_WAYS)/L3_BANKS)) + way); //one way of 2048 sets in one bank
			case MemComponent::NUCA_CACHE:
				return ((set/((NUCA_CACHE_SETS * NUCA_CACHE_WAYS)/NUCA_CACHE_BANKS)) + way); //one way of 512 sets in one bank
			default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
	}
	bool validBank(MemComponent::component_t comp, int bank) {
		switch(comp) {
			case MemComponent::L1_DCACHE:
				return ((bank >= 0 )&& (bank < L1_BANKS));
			case MemComponent::L2_CACHE:
				return ((bank >= 0 )&& (bank < L2_BANKS));
			case MemComponent::L3_CACHE:
				return ((bank >= 0 )&& (bank < L3_BANKS));
			case MemComponent::NUCA_CACHE:
				return ((bank >= 0 )&& (bank < L3_BANKS));
			default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
	}
	bool validSet(MemComponent::component_t comp, int set) {
		switch(comp) {
			case MemComponent::L1_DCACHE:
				return ((set >= 0 )&& (set < L1_SETS));
			case MemComponent::L2_CACHE:
				return ((set >= 0 )&& (set < L2_SETS));
			case MemComponent::L3_CACHE:
				return ((set >= 0 )&& (set < L3_SETS));
			case MemComponent::NUCA_CACHE:
				return ((set >= 0 )&& (set < NUCA_CACHE_SETS));
			default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
	}
	bool validWay(MemComponent::component_t comp, int way) {
		switch(comp) {
			case MemComponent::L1_DCACHE:
				return ((way >= 0 )&& (way < L1_WAYS));
			case MemComponent::L2_CACHE:
				return ((way >= 0 )&& (way < L2_WAYS));
			case MemComponent::L3_CACHE:
				return ((way >= 0 )&& (way < L3_WAYS));
			case MemComponent::NUCA_CACHE:
				return ((way >= 0 )&& (way < NUCA_CACHE_WAYS));
			default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
	}

	bool inSameBank(MemComponent::component_t comp, 
	int set1, int way1, int set2, int way2,  
	CacheCntlr:: pic_map_policy_t policy ) {
		assert(validSet(comp, set1)); assert(validSet(comp, set2));
		assert(validWay(comp, way1)); assert(validWay(comp, way2));
		int bank1 = -1;
		int bank2 = -1;
		switch(policy){
      case CacheCntlr::PIC_ALL_WAYS_ONE_BANK:
				bank1 = getBankAllWays(comp, set1, way1);
				bank2 = getBankAllWays(comp, set2, way2);
				//printf("\nALL: l%d: (%d,%d), (%d,%d) -> (%d, %d)", 
					//comp, set1, way1, set2, way2, bank1, bank2);
				break;
      case CacheCntlr::PIC_MORE_SETS_ONE_BANK: 
				bank1 = getBankMoreSets(comp, set1, way1);
				bank2 = getBankMoreSets(comp, set2, way2);
				//printf("\nSETS: l%d: (%d,%d), (%d,%d) -> (%d, %d)", 
					//comp, set1, way1, set2, way2, bank1, bank2);
				break;
      default:
      	LOG_ASSERT_ERROR(false, "Where are you doing a PIC");
		}
		assert(validBank(comp, bank1));
		assert(validBank(comp, bank2));
		return (bank1 == bank2);
	}
//#endif
}
