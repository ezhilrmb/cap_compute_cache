#include "dram_directory_cntlr.h"
#include "log.h"
#include "memory_manager.h"
#include "stats.h"
#include "nuca_cache.h"
#include "shmem_perf.h"
#include "coherency_protocol.h"
#include "config.hpp"

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stdout); printf("[%s] %d%cdd %-25s@%3u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getMemoryManager()->getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#else
#  define MYLOG(...) {}
#endif

namespace PrL1PrL2DramDirectoryMSI
{

char DStateString(DirectoryState::dstate_t state) {
   switch(state)
   {
      case DirectoryState::UNCACHED:  return 'U';
      case DirectoryState::SHARED:    return 'S';
      case DirectoryState::EXCLUSIVE: return 'E';
      case DirectoryState::OWNED:     return 'O';
      case DirectoryState::MODIFIED:  return 'M';
      default:                        return '?';
   }
}

DramDirectoryCntlr::DramDirectoryCntlr(core_id_t core_id,
      MemoryManagerBase* memory_manager,
      AddressHomeLookup* dram_controller_home_lookup,
      NucaCache* nuca_cache,
      UInt32 dram_directory_total_entries,
      UInt32 dram_directory_associativity,
      UInt32 cache_block_size,
      UInt32 dram_directory_max_num_sharers,
      UInt32 dram_directory_max_hw_sharers,
      String dram_directory_type_str,
      ComponentLatency dram_directory_cache_access_time,
      ShmemPerfModel* shmem_perf_model):
   m_memory_manager(memory_manager),
   m_dram_controller_home_lookup(dram_controller_home_lookup),
   m_nuca_cache(nuca_cache),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_shmem_perf_model(shmem_perf_model),
   forward(0),
   forward_failed(0)
{
   m_dram_directory_cache = new DramDirectoryCache(
         core_id,
         dram_directory_type_str,
         dram_directory_total_entries,
         dram_directory_associativity,
         cache_block_size,
         dram_directory_max_hw_sharers,
         dram_directory_max_num_sharers,
         dram_directory_cache_access_time,
         m_shmem_perf_model);
   m_dram_directory_req_queue_list = new ReqQueueList();
   for(DirectoryState::dstate_t state = DirectoryState::dstate_t(0); state < DirectoryState::NUM_DIRECTORY_STATES; state = DirectoryState::dstate_t(int(state)+1))
   {
      if (state != DirectoryState::UNCACHED)
      {
         evict[state] = 0;
         registerStatsMetric("directory", core_id, String("evict-")+DStateString(state), &evict[state]);
      }
   }
   registerStatsMetric("directory", core_id, "forward", &forward);
   registerStatsMetric("directory", core_id, "forward-failed", &forward_failed);

   String protocol = Sim()->getCfg()->getString("caching_protocol/variant");
   if (protocol == "msi")
   {
      m_protocol = CoherencyProtocol::MSI;
   }
   else if (protocol == "mesi")
   {
      m_protocol = CoherencyProtocol::MESI;
   }
   else if (protocol == "mesif")
   {
      m_protocol = CoherencyProtocol::MESIF;
   }
   else
   {
      LOG_PRINT_ERROR("Invalid coherency protocol %s, must be msi, mesi or mesif", protocol.c_str());
   }
}

DramDirectoryCntlr::~DramDirectoryCntlr()
{
   delete m_dram_directory_cache;
   delete m_dram_directory_req_queue_list;
}

void
DramDirectoryCntlr::handleMsgFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);
   IntPtr address = shmem_msg->getAddress();

   MYLOG("begin for address %lx, %d in queue", address, m_dram_directory_req_queue_list->size(address));

   // Look up line state in the tag directory
   // This is just for modeling the TD lookup time (this is the only place where we call getDirectoryEntry with modeled==true),
   // elsewhere we assume outstanding requests are stored in a fast MSHR-like structure
	 //#ifdef PIC_ENABLE_OPERATIONS
    if(shmem_msg_type == ShmemMsg::VPIC_COPY_REQ) {
      ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);
      m_dram_directory_req_queue_list->enqueue(address, shmem_req);
			//TODO: Might not be true in multi-cores
      assert(m_dram_directory_req_queue_list->size(address) == 1);
      assert(m_dram_directory_req_queue_list->size(
			 					shmem_req->getShmemMsg()->m_other_address) == 0);
      processPicCopyFromL2Cache(shmem_req);
			return;
		}
		else if((shmem_msg_type == ShmemMsg::VPIC_CMP_REQ) ||
						(shmem_msg_type == ShmemMsg::VPIC_SEARCH_REQ)) {
      // Add request onto a queue
      ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);
      m_dram_directory_req_queue_list->enqueue(address, shmem_req);
			//TODO: Might not be true in multi-cores
      assert(m_dram_directory_req_queue_list->size(address) == 1);
      assert(m_dram_directory_req_queue_list->size(
			 					shmem_req->getShmemMsg()->m_other_address) == 0);
      processPicCmpFromL2Cache(shmem_req);
			return;
		}
	 //#endif
   m_dram_directory_cache->getDirectoryEntry(address, true);
   updateShmemPerf(shmem_msg, ShmemPerf::TD_ACCESS);

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
      {
         MYLOG("E REQ<%u @ %lx", sender, address);
         // Add request onto a queue
         ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);
         m_dram_directory_req_queue_list->enqueue(address, shmem_req);
         MYLOG("ENqueued E REQ for address %lx", address );
         if (m_dram_directory_req_queue_list->size(address) == 1)
         {
            processExReqFromL2Cache(shmem_req);
         }
         else
         {
            MYLOG("E REQ (%lx) not handled yet because of outstanding request in the queue", address);
         }
         break;
      }
      case ShmemMsg::SH_REQ:
      {
         MYLOG("S REQ<%u @ %lx", sender, address);

         // Add request onto a queue
         ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);

         m_dram_directory_req_queue_list->enqueue(address, shmem_req);
         MYLOG("ENqueued S REQ for address %lx", address );
         if (m_dram_directory_req_queue_list->size(address) == 1)
         {
            processShReqFromL2Cache(shmem_req);
         }
         else
         {
            MYLOG("S REQ (%lx) not handled because of outstanding request in the queue", address);
         }
         break;
      }

      case ShmemMsg::INV_REP:
         MYLOG("INV REP<%u @ %lx", sender, address);
         processInvRepFromL2Cache(sender, shmem_msg);
         break;
			
			//#ifdef PIC_ENABLE_OPERATIONS
      case ShmemMsg::FLUSH_DATA_REP:
			//#endif
      case ShmemMsg::FLUSH_REP:
         MYLOG("FLUSH REP<%u @ %lx", sender, address);
         processFlushRepFromL2Cache(sender, shmem_msg);
         break;

      case ShmemMsg::WB_REP:
         MYLOG("WB REP<%u @ %lx", sender, address);
         processWbRepFromL2Cache(sender, shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized Shmem Msg Type: %u", shmem_msg_type);
         break;

      case ShmemMsg::UPGRADE_REQ:
         MYLOG("UPGR REQ<%u @ %lx", sender, address);

         // Add request onto a queue
         ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);
         m_dram_directory_req_queue_list->enqueue(address, shmem_req);
         MYLOG("ENqueued  UPGRADE REQ for address %lx",  address );

         if (m_dram_directory_req_queue_list->size(address) == 1)
         {
            processUpgradeReqFromL2Cache(shmem_req);
         }
         else
         {
            MYLOG("UPGRADE REQ (%lx) not handled because of outstanding request in the queue", address);
         }

         break;

   }
MYLOG("done for %lx", address);
}

void
DramDirectoryCntlr::handleMsgFromDRAM(core_id_t sender, ShmemMsg* shmem_msg)
{
   MYLOG("Start");
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();

   switch (shmem_msg_type)
   {
      case ShmemMsg::DRAM_READ_REP:
         processDRAMReply(sender, shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized Shmem Msg Type: %u", shmem_msg_type);
         break;
   }
   MYLOG("End");
}

void
DramDirectoryCntlr::processNextReqFromL2Cache(IntPtr address)
{
   MYLOG("Start processNextReqFromL2Cache(%lx): %d in Queue", address, m_dram_directory_req_queue_list->size(address) );

   MYLOG("about to dequeue request for address %lx", address );
	 //#ifdef PIC_ENABLE_OPERATIONS
		//IntPtr the_other_address	= 0;
	 	if((picSrc1ToSrc2.find(address) != picSrc1ToSrc2.end()) 
			|| (picSrc2ToSrc1.find(address) != picSrc2ToSrc1.end())) {
				ShmemReq* shmem_req;
				if (picSrc1ToSrc2.find(address) != picSrc1ToSrc2.end()) {
   				assert(
						m_dram_directory_req_queue_list->size(
													picSrc1ToSrc2[address]) >= 1);
   					shmem_req = m_dram_directory_req_queue_list->front
							(picSrc1ToSrc2[address]);
				}
				else {
   				assert(
						m_dram_directory_req_queue_list->size(address) >= 1);
   					shmem_req = m_dram_directory_req_queue_list->front(address);
						//the_other_address = picSrc2ToSrc1[address];
				}

      	if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::VPIC_COPY_REQ)
					processPicCopyFromL2Cache(shmem_req);
				else {
      		assert((shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::VPIC_CMP_REQ) ||
						(shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::VPIC_SEARCH_REQ));
					processPicCmpFromL2Cache(shmem_req);
				}
				return;
		}
	 //#endif
   assert(m_dram_directory_req_queue_list->size(address) >= 1);
		
   ShmemReq* completed_shmem_req = 
			m_dram_directory_req_queue_list->dequeue(address);
   delete completed_shmem_req;

   if (! m_dram_directory_req_queue_list->empty(address)) {
      MYLOG("A new shmem req for address(%lx) found", address);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update the Shared Mem Cycle Counts appropriately
      getShmemPerfModel()->setElapsedTime(
													ShmemPerfModel::_SIM_THREAD, shmem_req->getTime());

      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ) {
         MYLOG("A new EX_REQ for address(%lx) found", address);
         processExReqFromL2Cache(shmem_req);
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ) {
         MYLOG("A new SH_REQ for address(%lx) found", address);
         processShReqFromL2Cache(shmem_req);
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::UPGRADE_REQ)
      {
         MYLOG("A new UPGRADE_REQ for address(%lx) found", address);
         processUpgradeReqFromL2Cache(shmem_req);
      }
      else
         LOG_PRINT_ERROR("Unrecognized Request(%u)", shmem_req->getShmemMsg()->getMsgType());
   }
   MYLOG("End processNextReqFromL2Cache(%lx)", address);
}

DirectoryEntry*
DramDirectoryCntlr::processDirectoryEntryAllocationReq(ShmemReq* shmem_req)
{
	 IntPtr other_address = 0;
	 IntPtr other_address2 = 0;
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   other_address = shmem_req->getShmemMsg()->m_other_address;
   other_address2 = shmem_req->getShmemMsg()->m_other_address2;
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   MYLOG("Start @ %lx", address);
   std::vector<DirectoryEntry*> replacement_candidate_list;
   m_dram_directory_cache->getReplacementCandidates(address, replacement_candidate_list);

   std::vector<DirectoryEntry*>::iterator it;
   std::vector<DirectoryEntry*>::iterator replacement_candidate = replacement_candidate_list.end();
   for (it = replacement_candidate_list.begin(); 
				it != replacement_candidate_list.end(); it++) {

      if (((replacement_candidate == replacement_candidate_list.end()) ||
         ((*replacement_candidate)->getNumSharers() > (*it)->getNumSharers()))
          && (m_dram_directory_req_queue_list->size((*it)->getAddress()) == 0)
	 //#ifdef PIC_ENABLE_OPERATIONS
					&& ((*it)->getAddress() != other_address)
					&& ((*it)->getAddress() != other_address2)
	 //#endif
         )
      {
         replacement_candidate = it;
      }
   }
	

   LOG_ASSERT_ERROR(replacement_candidate != replacement_candidate_list.end(),
         "Cannot find a directory entry to be replaced with a non-zero request list (see Redmine #175)");

   DirectoryState::dstate_t curr_dstate = (*replacement_candidate)->getDirectoryBlockInfo()->getDState();
   evict[curr_dstate]++;

   IntPtr replaced_address = (*replacement_candidate)->getAddress();

   // We get the entry with the lowest number of sharers
   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->replaceDirectoryEntry
																						(replaced_address, address, true);

   ShmemMsg nullify_msg(ShmemMsg::NULLIFY_REQ, MemComponent::TAG_DIR, MemComponent::TAG_DIR, requester, replaced_address, NULL, 0, NULL);

   ShmemReq* nullify_req = new ShmemReq(&nullify_msg, msg_time);

   m_dram_directory_req_queue_list->enqueue(replaced_address, nullify_req);
   MYLOG("ENqueued NULLIFY request for address %lx", replaced_address );

   assert(m_dram_directory_req_queue_list->size(replaced_address) == 1);
   processNullifyReq(nullify_req);

   MYLOG("End @ %lx", address);

   return directory_entry;
}

void
DramDirectoryCntlr::processNullifyReq(ShmemReq* shmem_req)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   // NOTE: no ShmemPerf accounting for nullify requests as they may happen *after* core requests are completed

   MYLOG("Start @ %lx", address);
   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);
   DirectoryBlockInfo* directory_block_info = 
			directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE:
      case DirectoryState::MODIFIED:
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN,
               NULL,
               ShmemPerfModel::_SIM_THREAD);
         break;

      case DirectoryState::SHARED:

         {
            std::pair<bool, std::vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();
            if (sharers_list_pair.first == true)
            {
               // Broadcast Invalidation Request to all cores
               // (irrespective of whether they are sharers or not)
               getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     address,
                     NULL, 0,
                     NULL,
                     ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               // Send Invalidation Request to only a specific set of sharers
               for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
               {
                  getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                        MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                        requester /* requester */,
                        sharers_list_pair.second[i] /* receiver */,
                        address,
                        NULL, 0,
                        HitWhere::UNKNOWN,
                        NULL,
                        ShmemPerfModel::_SIM_THREAD);
               }
            }
         }
         break;

      case DirectoryState::UNCACHED:

         {
						#ifdef NUCA_INCLUSIVE
							if(nucaEvictions.find(address) != nucaEvictions.end()) {
								nucaEvictions.erase(address);
								//printf("\nNUCA-COMPLETE(%lx)", address);
							}
						#endif

            m_dram_directory_cache->invalidateDirectoryEntry(address);
            processNextReqFromL2Cache(address);
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }

   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processExReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   MYLOG("Start @ %lx", address);
   updateShmemPerf(shmem_req);

   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   if (directory_entry == NULL)
   {
      directory_entry = processDirectoryEntryAllocationReq(shmem_req);
   }

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE: // Cache may have done a silent upgrade to MODIFIED, so send a FLUSH (dirty data) rather than an INV (data clean)
      case DirectoryState::MODIFIED:
      {
         assert(cached_data_buf == NULL);
         MYLOG("Send FLUSH_REQ>%d for %lx", directory_entry->getOwner(), address )
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN, shmem_req->getShmemMsg()->getPerf(), ShmemPerfModel::_SIM_THREAD);
         break;
      }

      case DirectoryState::SHARED:
      {
         assert(cached_data_buf == NULL);
         std::pair<bool, std::vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();
         MYLOG("\nExReq: Send INV_REQ>%d for %lx", directory_entry->getOwner(), address );
         if (sharers_list_pair.first == true)
         {
            // Broadcast Invalidation Request to all cores
            // (irrespective of whether they are sharers or not)
            getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  requester /* requester */,
                  address,
                  NULL, 0,
                  NULL, // No ShmemPerf on broadcast
                  ShmemPerfModel::_SIM_THREAD);
         }
         else
         {
            // Send Invalidation Request to only a specific set of sharers
         MYLOG("\nExReq: Send INV_REQ>%d for %lx", directory_entry->getOwner(), address );
            for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
            {
               MYLOG("Send INV_REQ>%d for %lx", sharers_list_pair.second[i], address )
                        getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                              MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                              requester /* requester */,
                              sharers_list_pair.second[i] /* receiver */,
                              address,
                              NULL, 0,
                              HitWhere::UNKNOWN,
                              i == 0 ? shmem_req->getShmemMsg()->getPerf() : NULL,
                              ShmemPerfModel::_SIM_THREAD);
            }
         }
         break;
      }

      case DirectoryState::UNCACHED:
      {
         // Modifiy the directory entry contents
         MYLOG("\nExReq: Retrieve> %lx", address );
         bool add_result = directory_entry->addSharer(requester, m_dram_directory_cache->getMaxHwSharers());
         assert(add_result == true);
         directory_entry->setOwner(requester);
         directory_block_info->setDState(DirectoryState::MODIFIED);

         retrieveDataAndSendToL2Cache(ShmemMsg::EX_REP, requester, address, cached_data_buf, shmem_req->getShmemMsg());
         break;
      }

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processShReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   MYLOG("Start @ %lx", address);
   updateShmemPerf(shmem_req);

   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   if (directory_entry == NULL)
   {
      directory_entry = processDirectoryEntryAllocationReq(shmem_req);
   }

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE:
      {
         assert (requester != directory_entry->getOwner());
         MYLOG("WB_REQ>%d for %lx", directory_entry->getOwner(), address  )
                  assert(cached_data_buf == NULL);
         getMemoryManager()->sendMsg(ShmemMsg::WB_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN, shmem_req->getShmemMsg()->getPerf(), ShmemPerfModel::_SIM_THREAD);
         break;
      }

      case DirectoryState::MODIFIED:
      {
         MYLOG("WB_REQ>%d for %lx", directory_entry->getOwner(), address  )
         assert(cached_data_buf == NULL);
         getMemoryManager()->sendMsg(ShmemMsg::WB_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN, shmem_req->getShmemMsg()->getPerf(), ShmemPerfModel::_SIM_THREAD);
         break;
      }

      case DirectoryState::SHARED:
      {
         if (directory_entry->hasSharer(requester))
         {
            MYLOG("got a WB/INV REP from the forwarder and now handling the original SH REQ");
            ++forward;
            if (cached_data_buf == NULL)
            {
               // Forwarder evicted the data while we requested it. Will have to get it from DRAM anyway.
               ++forward_failed;
            }
            retrieveDataAndSendToL2Cache(ShmemMsg::SH_REP, requester, address, cached_data_buf, shmem_req->getShmemMsg());
         }
         else
         {
            bool add_result = directory_entry->addSharer(requester, m_dram_directory_cache->getMaxHwSharers());
            if (add_result == false)
            {
               core_id_t sharer_id = directory_entry->getOneSharer();
               // Send a message to another sharer to invalidate that
               MYLOG("INV_REQ>%d for %lx because I could not add sharer", directory_entry->getOwner(), address  )
               getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     sharer_id /* receiver */,
                     address,
                     NULL, 0,
                     HitWhere::UNKNOWN, shmem_req->getShmemMsg()->getPerf(), ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               MYLOG("SHARED state, retrieve data and send")
               retrieveDataAndSendToL2Cache(ShmemMsg::SH_REP, requester, address, cached_data_buf, shmem_req->getShmemMsg());
            }
         }
         break;
      }

      case DirectoryState::UNCACHED:
      {
         MYLOG("was UNCACHED, is now EXCLUSIVE")
         // Modifiy the directory entry contents
         bool add_result = directory_entry->addSharer(requester, m_dram_directory_cache->getMaxHwSharers());
         assert(add_result == true);

         directory_entry->setOwner(requester);

         if (m_protocol == CoherencyProtocol::MSI)
         {
            directory_block_info->setDState(DirectoryState::SHARED);
            retrieveDataAndSendToL2Cache(ShmemMsg::SH_REP, requester, address, cached_data_buf, shmem_req->getShmemMsg());
         }
         else
         {
            directory_block_info->setDState(DirectoryState::EXCLUSIVE);
            retrieveDataAndSendToL2Cache(ShmemMsg::EX_REP, requester, address, cached_data_buf, shmem_req->getShmemMsg());
         }

         break;
      }

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::retrieveDataAndSendToL2Cache(ShmemMsg::msg_t reply_msg_type,
      core_id_t receiver, IntPtr address, Byte* cached_data_buf, ShmemMsg *orig_shmem_msg)
{
   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry != NULL);

   MYLOG("Start @ %lx", address);
   if (cached_data_buf != NULL)
   {
      // Forwarder state moves to the last cache receiving a copy,
      // assuming this one is the least likely to evict it early.
      directory_entry->setForwarder(receiver);

      // I already have the data I need cached
      MYLOG("Already have the data that I need cached");
      getMemoryManager()->sendMsg(reply_msg_type,
            MemComponent::TAG_DIR, MemComponent::L2_CACHE,
            receiver /* requester */,
            receiver /* receiver */,
            address,
            cached_data_buf, getCacheBlockSize(),
            HitWhere::CACHE_REMOTE /* cached_data_buf was filled by a WB_REQ or FLUSH_REQ */,
            orig_shmem_msg->getPerf(),
            ShmemPerfModel::_SIM_THREAD);

      // Process Next Request
      processNextReqFromL2Cache(address);
   }
   else
   {
      if (m_nuca_cache)
      {
         SubsecondTime nuca_latency;
         HitWhere::where_t hit_where;
         Byte nuca_data_buf[getCacheBlockSize()];
         boost::tie(nuca_latency, hit_where) = m_nuca_cache->read(address, 
					nuca_data_buf, 
					getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), 
					orig_shmem_msg->getPerf(), true);

         getShmemPerfModel()->incrElapsedTime(nuca_latency, ShmemPerfModel::_SIM_THREAD);

         if (hit_where != HitWhere::MISS)
         {
            getMemoryManager()->sendMsg(reply_msg_type,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  receiver /* requester */,
                  receiver /* receiver */,
                  address,
                  nuca_data_buf, getCacheBlockSize(),
                  HitWhere::NUCA_CACHE,
                  orig_shmem_msg->getPerf(),
                  ShmemPerfModel::_SIM_THREAD);

            // Process Next Request
            processNextReqFromL2Cache(address);

            return;
         }
      }

      assert(m_dram_directory_req_queue_list->size(address) > 0);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // MESIF protocol: get data from a sharing cache

      if (m_protocol == CoherencyProtocol::MESIF
          && shmem_req->getForwardingFrom() == INVALID_CORE_ID // If forwarding already failed once, don't try again
          && directory_entry->getDirectoryBlockInfo()->getDState() == DirectoryState::SHARED
          && directory_entry->getForwarder() != INVALID_CORE_ID)
      {
         core_id_t forwarder = directory_entry->getForwarder();
         shmem_req->setForwardingFrom(forwarder);

         // Send WB_REQ to forwarder to have it send us the data
         getMemoryManager()->sendMsg(ShmemMsg::WB_REQ,
            MemComponent::TAG_DIR, MemComponent::L2_CACHE,
            receiver /* requester */,
            forwarder /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN, shmem_req->getShmemMsg()->getPerf(), ShmemPerfModel::_SIM_THREAD);
         return;
      }

      // Get the data from DRAM
      // This could be directly forwarded to the cache or passed
      // through the Dram Directory Controller

      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::UPGRADE_REQ)
      {
         MYLOG("Have to get data from DRAM while doing an UPGRADE_REQ: lost data somehow\n");
      }

      // Remember that this request is waiting for data, and should not be woken up by voluntary invalidates
      shmem_req->setWaitForData(true);

      core_id_t dram_node = m_dram_controller_home_lookup->getHome(address);

      MYLOG("Sending request to DRAM for the data");
      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_READ_REQ,
            MemComponent::TAG_DIR, MemComponent::DRAM,
            receiver /* requester */,
            dram_node /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN,
            orig_shmem_msg->getPerf(),
            ShmemPerfModel::_SIM_THREAD);
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processDRAMReply(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   // Data received from DRAM
   //   Which node to reply to?
   MYLOG("Start @ %lx", address);
	 ShmemReq* shmem_req = NULL;
	 //#ifdef PIC_ENABLE_OPERATIONS
	 	if(
			(picSrc1ToSrc2.find(address) != picSrc1ToSrc2.end()) 
			|| (picSrc2ToSrc1.find(address) != picSrc2ToSrc1.end())) {
			if (picSrc1ToSrc2.find(address) != picSrc1ToSrc2.end()) {
   			assert(
					m_dram_directory_req_queue_list->size(picSrc1ToSrc2[address]) >= 1);
   				shmem_req = m_dram_directory_req_queue_list->front
																			(picSrc1ToSrc2[address]);
			}
			else {
   			assert(m_dram_directory_req_queue_list->size(address) >= 1);
   			shmem_req = m_dram_directory_req_queue_list->front(address);
			}
   		sendDataToNUCA(address, shmem_req->getShmemMsg()->getRequester(), 
			shmem_msg->getDataBuf(), 
			getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), 
						false);
   		// Process Next Request
  		processNextReqFromL2Cache(address);
			return;
		}
		else {
   		assert(m_dram_directory_req_queue_list->size(address) >= 1);
   		shmem_req = m_dram_directory_req_queue_list->front(address);
		} 
	 assert(shmem_req);
   updateShmemPerf(shmem_req);

   //   Which reply type to use?
   ShmemMsg::msg_t reply_msg_type;
   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);
   DirectoryBlockInfo* directory_block_info = 
			directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();
   updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);

   switch(shmem_req->getShmemMsg()->getMsgType())
   {
      case ShmemMsg::SH_REQ:
         assert(curr_dstate == DirectoryState::SHARED 
						|| curr_dstate == DirectoryState::EXCLUSIVE);
         if (curr_dstate == DirectoryState::EXCLUSIVE) {
            reply_msg_type = ShmemMsg::EX_REP;
         }
         else {
            reply_msg_type = ShmemMsg::SH_REP;
         }
         break;
      case ShmemMsg::EX_REQ:
         reply_msg_type = ShmemMsg::EX_REP;
         assert(curr_dstate == DirectoryState::MODIFIED);
         break;
      case ShmemMsg::UPGRADE_REQ: 
			{
         // if we had to get the data from DRAM, 
				 //nobody has it anymore: send EX_REP
         reply_msg_type = ShmemMsg::EX_REP;
         break;
      }
      default:
        	LOG_PRINT_ERROR("Unsupported request type: %u", 
						shmem_req->getShmemMsg()->getMsgType());
   }
   //   Which HitWhere to report?
   HitWhere::where_t hit_where = shmem_msg->getWhere();
   if (hit_where == HitWhere::DRAM)
      hit_where = (sender == shmem_msg->getRequester()) ? 
									HitWhere::DRAM_LOCAL : HitWhere::DRAM_REMOTE;
   //   Send reply
   MYLOG("MSG DRAM>%d for %lx", shmem_req->getShmemMsg()->getRequester(), 
		address )
   getMemoryManager()->sendMsg(reply_msg_type,
         MemComponent::TAG_DIR, MemComponent::L2_CACHE,
         shmem_req->getShmemMsg()->getRequester() /* requester */,
         shmem_req->getShmemMsg()->getRequester() /* receiver */,
         address,
         shmem_msg->getDataBuf(), getCacheBlockSize(),
         hit_where,
         shmem_req->getShmemMsg()->getPerf(),
         ShmemPerfModel::_SIM_THREAD);

   // Keep a copy in NUCA
   sendDataToNUCA(address, shmem_req->getShmemMsg()->getRequester(), 
		shmem_msg->getDataBuf(), 
		getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), false);

   // Process Next Request
   processNextReqFromL2Cache(address);
   MYLOG("End @ %lx", address);

}

void
DramDirectoryCntlr::processInvRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   MYLOG("Start @ %lx", address);

   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = 
			directory_entry->getDirectoryBlockInfo();

   LOG_ASSERT_ERROR(directory_block_info->getDState() == DirectoryState::SHARED || directory_block_info->getDState() == DirectoryState::EXCLUSIVE, "Ooops (%lx)", address);
   assert(directory_block_info->getDState() == DirectoryState::SHARED || directory_block_info->getDState() == DirectoryState::EXCLUSIVE);

   directory_entry->removeSharer(sender);
   if (directory_entry->getForwarder() == sender)
   {
      directory_entry->setForwarder(INVALID_CORE_ID);
   }
   if (directory_entry->getNumSharers() == 0)
   {
      directory_block_info->setDState(DirectoryState::UNCACHED);
   }

   if (m_dram_directory_req_queue_list->size(address) > 0)
   {
      MYLOG("More requests outstanding for address %lx", address);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update Times in the Shmem Perf Model and the Shmem Req
      shmem_req->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), ShmemPerfModel::_SIM_THREAD);

      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ)
      {
         // An ShmemMsg::EX_REQ caused the invalidation
         if (directory_block_info->getDState() == DirectoryState::UNCACHED)
         {
            updateShmemPerf(shmem_req, ShmemPerf::INV_IMBALANCE);
            processExReqFromL2Cache(shmem_req);
         }
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::UPGRADE_REQ)
      {
         MYLOG("as part of UPGR: got INV_REP from %d, %d sharers left", sender,directory_entry->getNumSharers() )

         // An ShmemMsg::UPGRADE_REQ caused the invalidation: additional action is only required
         // when all SHARED copies have been invalided
         if (directory_entry->getNumSharers() == 1)
         {
            core_id_t requester = shmem_req->getShmemMsg()->getRequester();
            if (directory_entry->hasSharer(requester))
            {
               // all Shared copies have been invalidated, UPGRADE_REQ can be completed now.
               MYLOG("UPGRADE_REQ: all OTHER shared copies have been invalided: complete upgrade req.\n");
               updateShmemPerf(shmem_req, ShmemPerf::INV_IMBALANCE);
               processUpgradeReqFromL2Cache(shmem_req);
            }
         }
         else if  (directory_entry->getNumSharers() == 0)
         {
            MYLOG("UPGRADE_REQ: all shared copies have been invalided, requester has no copy anymore.\n");

            directory_block_info->setDState(DirectoryState::UNCACHED);

            updateShmemPerf(shmem_req, ShmemPerf::INV_IMBALANCE);
            processUpgradeReqFromL2Cache(shmem_req);
         }
         else
         {
            MYLOG("could not do complete UPGRADE_REQ because there are still multiple sharers");
         }
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ)
      {
         if (shmem_req->getWaitForData())
         {
            // This is a voluntary invalidate (probably part of an upgrade or eviction),
            // the next request should only be woken up once its data arrives from DRAM.
         }
         else if (shmem_req->isForwarding() && sender != shmem_req->getForwardingFrom())
         {
            // This is a voluntary invalidate (probably part of an upgrade or eviction),
            // the next request should only be woken up once its data arrives from the forwarder.
         }
         else
         {
            // A ShmemMsg::SH_REQ caused the invalidation
            updateShmemPerf(shmem_req, ShmemPerf::INV_IMBALANCE);
            processShReqFromL2Cache(shmem_req);
         }
      }
      else // shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::NULLIFY_REQ
      {
         if (directory_block_info->getDState() == DirectoryState::UNCACHED)
         {
            updateShmemPerf(shmem_req, ShmemPerf::INV_IMBALANCE);
            processNullifyReq(shmem_req);
         }
      }
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processUpgradeReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf)
{
   ShmemMsg* shmem_msg = shmem_req->getShmemMsg();

   IntPtr address = shmem_msg->getAddress();
   core_id_t requester = shmem_msg->getRequester();
   updateShmemPerf(shmem_req);

   MYLOG("processUpgradeReqFromL2Cache for address: %lx, requester= %d", address, requester);
   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   if (directory_entry == NULL)
   {
      directory_entry = processDirectoryEntryAllocationReq(shmem_req);
   }
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   std::pair<bool, std::vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();

   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   MYLOG("state=%d :: ", curr_dstate);
   MYLOG("owner=%d" , directory_entry->getOwner());
   if (!sharers_list_pair.first)
   {
      for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
      {
         MYLOG("sharer: %d",sharers_list_pair.second[i]);
      }
   }

   updateShmemPerf(shmem_msg, ShmemPerf::TD_ACCESS);

   switch (curr_dstate)
   {
      case DirectoryState::EXCLUSIVE:
      case DirectoryState::MODIFIED:
      {
         if (sharers_list_pair.second[0] == requester)
         {
            MYLOG("upgrade request immediately finished, sending UPGRADE_REP to %d", requester);
            assert (directory_entry->getOwner() == requester);

            getMemoryManager()->sendMsg(ShmemMsg::UPGRADE_REP,
                        MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                        requester /* requester */,
                        requester /* receiver */,
                        address,
                        NULL, 0,
                        HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);

            directory_block_info->setDState(DirectoryState::MODIFIED);

            processNextReqFromL2Cache(address);
         }
         else
         {
            // Send FLUSH_REQ to the current owner
            MYLOG("FLUSH REQ (UPGR)>%u @ %lx",sharers_list_pair.second[0] , address);
            getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  requester /* requester */,
                  directory_entry->getOwner() /* receiver */,
                  address,
                  NULL, 0,
                  HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
         }

         break;
      }
      case DirectoryState::SHARED:
      {
         if ((sharers_list_pair.second.size() == 1) && (sharers_list_pair.second[0] == requester))
         {
            // Let the requester know it can take ownership
            MYLOG("sending UPGRADE_REP>%d", requester )
            directory_entry->setOwner(requester);
            directory_block_info->setDState(DirectoryState::MODIFIED);

            getMemoryManager()->sendMsg(ShmemMsg::UPGRADE_REP,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  requester /* requester */,
                  requester /* receiver */,
                  address,
                  NULL, 0,
                  HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);

            processNextReqFromL2Cache(address);
         }
         else
         {
            bool requesterHasCopy = directory_entry->hasSharer(requester);
            if (!requesterHasCopy)
            {
               MYLOG("UPGRADE_REQ: %lu sharer(s), but the requester isn't holding a copy!?", sharers_list_pair.second.size());
            }

            // send inv_req to all sharers
            if (sharers_list_pair.first == true)
            {
               // Broadcast Invalidation Request to all cores
               // (irrespective of whether they are sharers or not)
               getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     address,
                     NULL, 0,
                     NULL, // No ShmemPerf on broadcast
                     ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               // Send Invalidation Request to only a specific set of sharers
               for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
               {
                  if (sharers_list_pair.second[i] != requester)
                  {
                     MYLOG("INV REQ (UPGR)>%u @ %lx",sharers_list_pair.second[i] , shmem_msg->getAddress());
                     // avoid having to fetch the data from DRAM, so ask at least one core to FLUSH instead of INV
                     ShmemMsg::msg_t msg_type = (!requesterHasCopy && i==0) ? ShmemMsg::FLUSH_REQ : ShmemMsg::INV_REQ;
                     //ShmemMsg::msg_t msg_type = ShmemMsg::INV_REQ;
                     getMemoryManager()->sendMsg( msg_type, //ShmemMsg::INV_REQ,
                           MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                           requester /* requester */,
                           sharers_list_pair.second[i] /* receiver */,
                           address,
                           NULL, 0,
                           HitWhere::UNKNOWN,
                           i == 0 ? shmem_msg->getPerf() : NULL,
                           ShmemPerfModel::_SIM_THREAD);
                  }
               }
            }
         }
         break;
      }
      case DirectoryState::UNCACHED:
      {
         MYLOG("%lx is UNCACHED", address);
         assert (sharers_list_pair.second.size() == 0);

         // Modifiy the directory entry contents
         bool add_result = directory_entry->addSharer(requester, m_dram_directory_cache->getMaxHwSharers());
         assert(add_result == true);
         directory_entry->setOwner(requester);
         directory_block_info->setDState(DirectoryState::MODIFIED);

         if (cached_data_buf == NULL)
         {
            // maybe the data is stored in the msg already?
            cached_data_buf = shmem_msg->getDataBuf();
         }
         retrieveDataAndSendToL2Cache(ShmemMsg::EX_REP, requester, address, cached_data_buf, shmem_msg);

         break;
      }
      default:
      {
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
      }
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processFlushRepFromL2Cache(core_id_t sender, 
	ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   SubsecondTime now = getShmemPerfModel()->getElapsedTime
				(ShmemPerfModel::_SIM_THREAD);

   MYLOG("Start @ %lx", address);

   DirectoryEntry* directory_entry = 
			m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = 
			directory_entry->getDirectoryBlockInfo();

   assert(directory_entry->hasSharer(sender));

	 //#ifdef PIC_ENABLE_OPERATIONS
   if(shmem_msg->getMsgType() != ShmemMsg::FLUSH_DATA_REP )
	 //#endif
	 {
   		directory_entry->removeSharer(sender);
   		directory_entry->setForwarder(INVALID_CORE_ID);
   		directory_entry->setOwner(INVALID_CORE_ID);

   		// could be that this is a FLUSH to force a core with S-state to to 
	 		// write back clean data
   		// to avoid a memory access
   		if (directory_entry->getNumSharers() == 0) {
      	directory_block_info->setDState(DirectoryState::UNCACHED);
   		}
   		else {
      	assert(directory_block_info->getDState() == DirectoryState::SHARED);
   		}
		}

   if (m_dram_directory_req_queue_list->size(address) != 0) {
	 	//#ifdef PIC_ENABLE_OPERATIONS
		//TODO: Fail in multicores
   	if(shmem_msg->getMsgType() == ShmemMsg::FLUSH_DATA_REP )
			assert(0);
	 	//#endif
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update times
      shmem_req->updateTime(now);
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), 
				ShmemPerfModel::_SIM_THREAD);
      shmem_req->getShmemMsg()->getPerf()->updateTime(now);
      updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);
      // An involuntary/voluntary Flush
      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ) {
         processExReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ) {
         // Write Data to Dram
         sendDataToDram(address, shmem_msg->getRequester(), 
																							shmem_msg->getDataBuf(), now);
         processShReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == 
																										ShmemMsg::UPGRADE_REQ) {
         MYLOG("as part of UPGR: got FLUSH_REP from %d, %d sharers left", 
						sender,directory_entry->getNumSharers() );
         // there should be only one M copy that is written back
         if (directory_entry->getNumSharers() == 0 ) {
            // The only M copy has been invalidated (WB), 
						// UPGRADE_REQ can be completed now.
            directory_block_info->setDState(DirectoryState::UNCACHED);
            processUpgradeReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
         }
         else
         {
            // This is part of a UPGRADE REQ where the requester did 
						// not have a copy anymore
            // One of the sharers whas FLUSHed instead of INV to 
						// get the data. This is it's reply
            // store the data that was FLUSHed
            shmem_req->getShmemMsg()->setDataBuf(shmem_msg->getDataBuf());
            // Nothing else to do, there are still S copies.
         }
      }
      else // shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::NULLIFY_REQ
      {
         // Write Data To Dram
				#ifdef NUCA_INCLUSIVE
				//if(nucaEvictions.find(address) != nucaEvictions.end())
					//printf("\nNUCA-NOWRITE(%lx)", address);
				#endif
         sendDataToDram(address, shmem_msg->getRequester(), 
						shmem_msg->getDataBuf(), now);
         processNullifyReq(shmem_req);
      }
   }
   else
   {
      // This was just an eviction
      // Write Data to Dram
      sendDataToDram(address, shmem_msg->getRequester(), 
					shmem_msg->getDataBuf(), now);
   }

   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::processWbRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   SubsecondTime now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   MYLOG("Start @ %lx", address);

   DirectoryEntry* directory_entry = 
		m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   //assert(directory_block_info->getDState() == DirectoryState::MODIFIED);
   assert(directory_entry->hasSharer(sender));

   directory_entry->setOwner(INVALID_CORE_ID);
   directory_block_info->setDState(DirectoryState::SHARED);

   if (m_dram_directory_req_queue_list->size(address) != 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update Time
      shmem_req->updateTime(now);
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), ShmemPerfModel::_SIM_THREAD);

      shmem_req->getShmemMsg()->getPerf()->updateTime(now);
      updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);

      LOG_ASSERT_ERROR(shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ,
            "Address(0x%x), Req(%u)",
            address, shmem_req->getShmemMsg()->getMsgType());
      processShReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
   }
   else
   {
      LOG_PRINT_ERROR("Should not reach here");
   }
   MYLOG("End @ %lx", address);
}

void
DramDirectoryCntlr::sendDataToNUCA(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now,bool count)
{
   if (m_nuca_cache)
   {
      bool eviction = false;
      bool evict_no_wb = false;
      IntPtr evict_address;
      Byte evict_buf[getCacheBlockSize()];

			IntPtr other_address	= 0;
			IntPtr other_address2	= 0;
			//#ifdef PIC_ENABLE_OPERATIONS
   			if(m_dram_directory_req_queue_list->size(address) >= 1) {
   				ShmemReq* shmem_req_check = 
						m_dram_directory_req_queue_list->front(address);
					other_address = shmem_req_check->getShmemMsg()->m_other_address;
					other_address2 = shmem_req_check->getShmemMsg()->m_other_address2;
				}
			//#endif
      m_nuca_cache->write(
         address, data_buf,
         eviction, evict_address, evict_buf,
         getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD)
				 , count
				 , evict_no_wb
				 ,other_address
				 ,other_address2
      );

			//Should we evict to make inclusive??
			#ifdef NUCA_INCLUSIVE
				DirectoryEntry *de	= m_dram_directory_cache->testDirectoryEntry
															(evict_address, true);
				if(evict_no_wb && de) {
					if(de->getDirectoryBlockInfo()->getDState() 
														!= DirectoryState::UNCACHED) {
	 					processDirectoryEntryEvictionReq(evict_address, m_core_id);
					}
				}
			#endif
			
      if (eviction)
      {
         // Write data to Dram
         core_id_t dram_node = 
					m_dram_controller_home_lookup->getHome(evict_address);

         getMemoryManager()->sendMsg(
							 PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_WRITE_REQ,
               MemComponent::TAG_DIR, MemComponent::DRAM,
               m_core_id /* requester */,
               dram_node /* receiver */,
               evict_address,
               evict_buf, getCacheBlockSize(),
               HitWhere::UNKNOWN,
               NULL,
               ShmemPerfModel::_SIM_THREAD);
      }
   }
   MYLOG("End");
}

void
DramDirectoryCntlr::sendDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   MYLOG("Start @ %lx", address);

   if (m_nuca_cache 
		#ifdef NUCA_INCLUSIVE
			&& (nucaEvictions.find(address) == nucaEvictions.end())
		#endif
	 )
	 {
      // If we have a NUCA cache: write it there, 
			// it will be written to DRAM on eviction
      sendDataToNUCA(address, requester, data_buf, now, true);
   }
   else {
      // Write data to Dram
      core_id_t dram_node = m_dram_controller_home_lookup->getHome(address);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_WRITE_REQ,
            MemComponent::TAG_DIR, MemComponent::DRAM,
            requester /* requester */,
            dram_node /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN,
            NULL,
            ShmemPerfModel::_SIM_THREAD);

      // DRAM latency is ignored on write
   }
   MYLOG("End @ %lx", address);
}

//#ifdef PIC_ENABLE_OPERATIONS
void
DramDirectoryCntlr::processPicCmpFromL2Cache(ShmemReq* shmem_req, 
		Byte* cached_data_buf) {
	 //have we seen this before?
	bool load1_completed = false, load2_completed = false;
  bool pic_begin	= true;
	if(picSrc1ToSrc2.find(shmem_req->getShmemMsg()->getAddress())
		!= picSrc1ToSrc2.end()) {
		load1_completed	= true;
		pic_begin = false;
	}
	if(picSrc2ToSrc1.find(shmem_req->getShmemMsg()->getAddress())
		!= picSrc2ToSrc1.end()) {
		load2_completed	= true;
		pic_begin = false;
	}
	assert(!(load1_completed && load2_completed));
	IntPtr load1_address = 0, load2_address = 0;
  core_id_t requester 				= shmem_req->getShmemMsg()->getRequester();
	//We haven't seen this before
	if(!load1_completed && !load2_completed) {
		assert(pic_begin);
   	load2_address 			= shmem_req->getShmemMsg()->getAddress();
   	load1_address 			= shmem_req->getShmemMsg()->m_other_address;
		picSrc1ToSrc2[load1_address]		= load2_address;
		picSrc2ToSrc1[load2_address]  	= load1_address;
		//Change the request fields.. hacky!!!
	 	shmem_req->getShmemMsg()->m_other_address = load2_address;
	 	shmem_req->getShmemMsg()->setAddress(load1_address);
   	updateShmemPerf(shmem_req);
		DirectoryEntry *directory_entry	
			= m_dram_directory_cache->testDirectoryEntry (load1_address, true);
		if(directory_entry) { 
   		DirectoryBlockInfo* directory_block_info 
			= directory_entry->getDirectoryBlockInfo();
   		DirectoryState::dstate_t curr_dstate = 
				directory_block_info->getDState();
   		updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);
   		switch (curr_dstate) {
      	case DirectoryState::EXCLUSIVE:
      	case DirectoryState::MODIFIED:
      	case DirectoryState::SHARED:
				{
					//TODO: Will break in multicore
         	assert ((requester == directory_entry->getOwner())
         					|| (directory_entry->hasSharer(requester)));
				 	load1_completed = true;
         	break;
      	}
      	case DirectoryState::UNCACHED:
      	{
        	load1_completed = 
						picRetrieveData(requester, load1_address, load2_address, 
						cached_data_buf, shmem_req->getShmemMsg(), false);
         	break;
      	}
      	default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   		}
			if(!load1_completed)
				return;	//done for now
		}
		else {
    	load1_completed = 
				picRetrieveData(requester, load1_address, load2_address, 
				cached_data_buf, shmem_req->getShmemMsg(), false);
		}
	}
  if (load1_completed) {
		if(pic_begin) {
			assert(load1_address && load2_address); 
		}
		else {
			assert(!load1_address && !load2_address);
   		load1_address 				= shmem_req->getShmemMsg()->getAddress();
			assert(picSrc1ToSrc2.find(load1_address) != picSrc1ToSrc2.end());
   		load2_address 			= picSrc1ToSrc2[load1_address]; 
			assert(picSrc2ToSrc1.find(load2_address) != picSrc2ToSrc1.end());
		}
	 	shmem_req->getShmemMsg()->m_other_address = load1_address;
	 	shmem_req->getShmemMsg()->setAddress(load2_address);
   	updateShmemPerf(shmem_req);

		//Process store request
   	DirectoryEntry* directory_entry = 
			m_dram_directory_cache->testDirectoryEntry(load2_address, true);
   	if (directory_entry) {
   		DirectoryBlockInfo* directory_block_info = 
				directory_entry->getDirectoryBlockInfo();
   		DirectoryState::dstate_t curr_dstate = 
				directory_block_info->getDState();
   		updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);

   		switch (curr_dstate) {
      	case DirectoryState::EXCLUSIVE: 
      	case DirectoryState::MODIFIED:
      	case DirectoryState::SHARED:
      	{
					//TODO: Will break in multicore
         	assert ((requester == directory_entry->getOwner())
         					|| (directory_entry->hasSharer(requester)));
				 	load2_completed = true;
         	break;
      	}
      	case DirectoryState::UNCACHED:
      	{
        	load2_completed = 
						picRetrieveData(requester, load2_address, load2_address, 
						cached_data_buf, shmem_req->getShmemMsg(), false);
         break;
      	}
      	default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   		}
		}
		else {
    	load2_completed = 
				picRetrieveData(requester, load2_address, load2_address, 
				cached_data_buf, shmem_req->getShmemMsg(), false);
		}
	}
  if(load2_completed) {
  	load2_address 				= shmem_req->getShmemMsg()->getAddress();
		assert(picSrc2ToSrc1.find(load2_address) != picSrc2ToSrc1.end());
   	load1_address 			= picSrc2ToSrc1[load2_address]; 
		assert(picSrc1ToSrc2.find(load1_address) != picSrc1ToSrc2.end());
		picSrc1ToSrc2.erase(load1_address);
		picSrc2ToSrc1.erase(load2_address);
		assert(picSrc1ToSrc2.find(load1_address) == picSrc1ToSrc2.end());
		assert(picSrc2ToSrc1.find(load2_address) == picSrc2ToSrc1.end());
		core_id_t receiver	= shmem_req->getShmemMsg()->getRequester();
		ShmemPerf *perf			= shmem_req->getShmemMsg()->getPerf();	
   	ShmemReq* completed_shmem_req = 
			m_dram_directory_req_queue_list->dequeue(load2_address);

		assert(receiver == 
			completed_shmem_req->getShmemMsg()->getRequester());
		assert(perf == 
			completed_shmem_req->getShmemMsg()->getPerf());
   
		assert(m_dram_directory_req_queue_list->size(load2_address) == 0);
		assert(m_dram_directory_req_queue_list->size(load1_address) == 0);

		SubsecondTime nuca_latency;
		HitWhere::where_t hit_where;

			boost::tie(nuca_latency, hit_where) =
				m_nuca_cache->picOp(load1_address, load2_address,
				getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD),
				perf, false, 
				shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::VPIC_CMP_REQ?
				ParametricDramDirectoryMSI::CacheCntlr::PIC_CMP :
				ParametricDramDirectoryMSI::CacheCntlr::PIC_SEARCH
			);
			
		//TODO: Need to manage hit_where correctly
   	hit_where = shmem_req->getShmemMsg()->getWhere();
   	/*if (hit_where == HitWhere::DRAM)
      	hit_where = (sender == shmem_msg->getRequester()) ? 
									HitWhere::DRAM_LOCAL : HitWhere::DRAM_REMOTE;*/
		hit_where	= HitWhere::DRAM_LOCAL;
   	delete completed_shmem_req;

   		getMemoryManager()->sendMsg(
				shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::VPIC_CMP_REQ ? 
				ShmemMsg::VPIC_CMP_REP :
				ShmemMsg::VPIC_SEARCH_REP,
        MemComponent::TAG_DIR, MemComponent::L2_CACHE,
        receiver /* requester */,
        receiver /* receiver */,
        load2_address,
        NULL,0 ,
        hit_where, perf, ShmemPerfModel::_SIM_THREAD,
				load1_address);
	}
}
void
DramDirectoryCntlr::processPicCopyFromL2Cache(ShmemReq* shmem_req, 
		Byte* cached_data_buf) {
	 //have we seen this before?
   bool load_completed = false, store_completed = false;
   bool pic_begin	= true;
	 if(picSrc1ToSrc2.find(shmem_req->getShmemMsg()->getAddress())
			!= picSrc1ToSrc2.end()) {
			load_completed	= true;
			pic_begin = false;
		}
	 if(picSrc2ToSrc1.find(shmem_req->getShmemMsg()->getAddress())
			!= picSrc2ToSrc1.end()) {
			store_completed	= true;
			pic_begin = false;
		}
	 assert(!(load_completed && store_completed));
	 IntPtr store_address = 0, load_address = 0;
   core_id_t requester 				= shmem_req->getShmemMsg()->getRequester();
	 //We haven't seen this before
	 if(!load_completed && !store_completed) {
			assert(pic_begin);
   		store_address 			= shmem_req->getShmemMsg()->getAddress();
   		load_address 				= shmem_req->getShmemMsg()->m_other_address;
			picSrc1ToSrc2[load_address]		= store_address;
			picSrc2ToSrc1[store_address]  = load_address;
			//Change the request fields.. hacky!!!
	 		shmem_req->getShmemMsg()->m_other_address = store_address;
	 		shmem_req->getShmemMsg()->setAddress(load_address);
   		updateShmemPerf(shmem_req);
			DirectoryEntry *directory_entry	
				= m_dram_directory_cache->testDirectoryEntry (load_address, true);

			if(directory_entry) { 
   			DirectoryBlockInfo* directory_block_info
					= directory_entry->getDirectoryBlockInfo();
   			DirectoryState::dstate_t curr_dstate = 
										directory_block_info->getDState();
   			updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);
   			switch (curr_dstate) {
      		case DirectoryState::EXCLUSIVE:
      		case DirectoryState::MODIFIED:
      		case DirectoryState::SHARED:
					{
				 		//TODO: Will break in multicore
         		assert ((requester == directory_entry->getOwner())
         					|| (directory_entry->hasSharer(requester)));
				 		load_completed = true; //begin store processing immed
         		break;
      		}
      		case DirectoryState::UNCACHED:
					{
      			load_completed = 
							picRetrieveData(requester, load_address, store_address, 
							cached_data_buf, shmem_req->getShmemMsg(), false);
         		break;
					}
      		default:
         	LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         	break;
   			}
			}
			else {
      	load_completed = 
					picRetrieveData(requester, load_address, store_address, 
					cached_data_buf, shmem_req->getShmemMsg(), false);
			}
			if(!load_completed)
				return;	//done for now
	 }
	if (load_completed) {
		if(pic_begin) {
			//instantaneous load complete
			assert(load_address && store_address); 
		}
		else {
			//we had to take pains to complete load too ;(
			assert(!load_address && !store_address);
   		load_address 				= shmem_req->getShmemMsg()->getAddress();
			assert(picSrc1ToSrc2.find(load_address) != picSrc1ToSrc2.end());
   		store_address 			= picSrc1ToSrc2[load_address]; 
			assert(picSrc2ToSrc1.find(store_address) != picSrc2ToSrc1.end());
		}
		//Hack the req again....
	 	shmem_req->getShmemMsg()->m_other_address = load_address;
	 	shmem_req->getShmemMsg()->setAddress(store_address);
   	updateShmemPerf(shmem_req);

		//Process store request
   	DirectoryEntry* directory_entry = 
			m_dram_directory_cache->testDirectoryEntry(store_address, true);
   	if (directory_entry) {
   		DirectoryBlockInfo* directory_block_info = 
				directory_entry->getDirectoryBlockInfo();
   		DirectoryState::dstate_t curr_dstate = 
				directory_block_info->getDState();
   		updateShmemPerf(shmem_req, ShmemPerf::TD_ACCESS);
   		switch (curr_dstate) {
      	case DirectoryState::EXCLUSIVE: 
      	case DirectoryState::MODIFIED:
      	case DirectoryState::SHARED:
      	{
				 //TODO: Will break in multicore
         assert ((requester == directory_entry->getOwner())
         					|| (directory_entry->hasSharer(requester)));
				 store_completed = true; //begin store processing immed
         break;
      	}
      	case DirectoryState::UNCACHED:
				{
    			store_completed	= 
						picRetrieveData(requester, store_address, store_address, 
						cached_data_buf, shmem_req->getShmemMsg(), true);
         break;
				}
      	default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
			}
		}
		else {
    	store_completed	= 
				picRetrieveData(requester, store_address, store_address, 
					cached_data_buf, shmem_req->getShmemMsg(), true);
   	}
	}
  if(store_completed) {
  	store_address 				= shmem_req->getShmemMsg()->getAddress();
		assert(picSrc2ToSrc1.find(store_address) != picSrc2ToSrc1.end());
   	load_address 			= picSrc2ToSrc1[store_address]; 
		assert(picSrc1ToSrc2.find(load_address) != picSrc1ToSrc2.end());
		picSrc1ToSrc2.erase(load_address);
		picSrc2ToSrc1.erase(store_address);
		assert(picSrc1ToSrc2.find(load_address) == picSrc1ToSrc2.end());
		assert(picSrc2ToSrc1.find(store_address) == picSrc2ToSrc1.end());
		core_id_t receiver	= shmem_req->getShmemMsg()->getRequester();
		ShmemPerf *perf			= shmem_req->getShmemMsg()->getPerf();	
   	ShmemReq* completed_shmem_req = 
			m_dram_directory_req_queue_list->dequeue(store_address);

		assert(receiver == 
			completed_shmem_req->getShmemMsg()->getRequester());
		assert(perf == 
			completed_shmem_req->getShmemMsg()->getPerf());
   
		assert(m_dram_directory_req_queue_list->size(store_address) == 0);
		assert(m_dram_directory_req_queue_list->size(load_address) == 0);

		SubsecondTime nuca_latency;
		HitWhere::where_t hit_where;
		boost::tie(nuca_latency, hit_where) =
				 m_nuca_cache->picOp(load_address, store_address,
				 getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD),
					perf, true, ParametricDramDirectoryMSI::CacheCntlr::PIC_COPY);
			
		//TODO: Need to manage hit_where correctly
   	hit_where = shmem_req->getShmemMsg()->getWhere();
   	/*if (hit_where == HitWhere::DRAM)
      	hit_where = (sender == shmem_msg->getRequester()) ? 
									HitWhere::DRAM_LOCAL : HitWhere::DRAM_REMOTE;*/
		hit_where	= HitWhere::DRAM_LOCAL;
   	delete completed_shmem_req;

   	getMemoryManager()->sendMsg(ShmemMsg::VPIC_COPY_REP,
         MemComponent::TAG_DIR, MemComponent::L2_CACHE,
         receiver /* requester */,
         receiver /* receiver */,
         store_address,
         NULL,0 ,
         hit_where, perf, ShmemPerfModel::_SIM_THREAD,
				 load_address);
	 }
}
bool
DramDirectoryCntlr::picRetrieveData( core_id_t receiver, 
IntPtr address1, IntPtr address2, Byte* cached_data_buf, 
ShmemMsg *orig_shmem_msg, bool is_dest_copy)
{
   assert(cached_data_buf == NULL);
   if (m_nuca_cache) {
   	SubsecondTime nuca_latency;
    HitWhere::where_t hit_where;
		//Just count tag checks
    boost::tie(nuca_latency, hit_where) = 
			m_nuca_cache->picPeek( address1, 
			getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), 
			orig_shmem_msg->getPerf());

    getShmemPerfModel()->incrElapsedTime(nuca_latency, 
					ShmemPerfModel::_SIM_THREAD);
   	if(hit_where != HitWhere::MISS)
			return true;
   }
		//if dest_copy & avoiding DRAM, just install a line
	if(Sim()->getCfg()->getBool("general/pic_avoid_dram")) {
		if(is_dest_copy) {
  		sendDataToNUCA(address1, receiver, NULL, 
			getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), false);
			return true;
		}
	}
  assert(m_dram_directory_req_queue_list->size(address2) == 1);
  ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address2);
  shmem_req->setWaitForData(true);
  core_id_t dram_node = m_dram_controller_home_lookup->getHome(address1);
  getMemoryManager()->sendMsg(
		PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_READ_REQ,
    MemComponent::TAG_DIR, MemComponent::DRAM,
    receiver /* requester */,
    dram_node /* receiver */,
    address1,
    NULL, 0,
    HitWhere::UNKNOWN,
    orig_shmem_msg->getPerf(),
    ShmemPerfModel::_SIM_THREAD,
		address2);
	return false;
}
//#endif
#ifdef NUCA_INCLUSIVE
void DramDirectoryCntlr::processDirectoryEntryEvictionReq(IntPtr address, 
core_id_t receiver)
{
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime
														(ShmemPerfModel::_SIM_THREAD);
   IntPtr replaced_address = address; 
	 m_dram_directory_cache->replaceDirectoryEntry
												(replaced_address, INVALID_ADDRESS, true);

   ShmemMsg nullify_msg
		(ShmemMsg::NULLIFY_REQ, MemComponent::TAG_DIR, 
			MemComponent::TAG_DIR, receiver, replaced_address, NULL, 0, NULL);

   ShmemReq* nullify_req = new ShmemReq(&nullify_msg, msg_time);

   m_dram_directory_req_queue_list->enqueue(replaced_address, nullify_req);
   assert(m_dram_directory_req_queue_list->size(replaced_address) == 1);
	 assert(nucaEvictions.find(address) == nucaEvictions.end());
	 nucaEvictions.insert(address);
   //printf("\nNUCA-EVICT(%lx)", replaced_address );
   processNullifyReq(nullify_req);
   MYLOG("End @ %lx", address);
}
#endif
}
