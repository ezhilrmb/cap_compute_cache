#pragma once

#include "mem_component.h"
#include "fixed_types.h"
#include "dynamic_instruction_info.h"

class ShmemPerf;

namespace PrL1PrL2DramDirectoryMSI
{
   class ShmemMsg
   {
      public:
         enum msg_t
         {
            INVALID_MSG_TYPE = 0,
            MIN_MSG_TYPE,
            // Cache > tag directory
            EX_REQ = MIN_MSG_TYPE, 	//1
            SH_REQ,									//2
            UPGRADE_REQ,						//3
            INV_REQ,								//4
            FLUSH_REQ,							//5
            WB_REQ,									//6
            // Tag directory > cache
            EX_REP,									//7
            SH_REP,									//8
            UPGRADE_REP,						//9
            INV_REP,								//10
            FLUSH_REP,							//11
            WB_REP,									//12
            NULLIFY_REQ,						//13
            // Tag directory > DRAM
            DRAM_READ_REQ,					//14
            DRAM_WRITE_REQ,					//15
            // DRAM > tag directory
            DRAM_READ_REP,					//16

						//#ifdef PIC_ENABLE_OPERATIONS
							VPIC_COPY_REQ,				//17
							VPIC_CMP_REQ,					//18
							VPIC_COPY_REP,				//19
							VPIC_CMP_REP,					//20
            	FLUSH_DATA_REP,				//21
							VPIC_SEARCH_REQ,				//17
							VPIC_SEARCH_REP,					//18
						//#endif

            MAX_MSG_TYPE = NULLIFY_REQ,
            NUM_MSG_TYPES = MAX_MSG_TYPE - MIN_MSG_TYPE + 1
         };

      private:
         msg_t m_msg_type;
         MemComponent::component_t m_sender_mem_component;
         MemComponent::component_t m_receiver_mem_component;
         core_id_t m_requester;
         HitWhere::where_t m_where;
         IntPtr m_address;
         Byte* m_data_buf;
         UInt32 m_data_length;
         ShmemPerf* m_perf;

      public:
				 //#ifdef PIC_ENABLE_OPERATIONS
					//Right now i dnt really care abt OOO
					IntPtr m_other_address;
					IntPtr m_other_address2;
				 //#endif

         ShmemMsg();
         ShmemMsg(msg_t msg_type,
               MemComponent::component_t sender_mem_component,
               MemComponent::component_t receiver_mem_component,
               core_id_t requester,
               IntPtr address,
               Byte* data_buf,
               UInt32 data_length,
               ShmemPerf* perf);
         ShmemMsg(ShmemMsg* shmem_msg);

         ~ShmemMsg();

         static ShmemMsg* getShmemMsg(Byte* msg_buf);
         Byte* makeMsgBuf();
         UInt32 getMsgLen();

         // Modeling
         UInt32 getModeledLength();

         msg_t getMsgType() { return m_msg_type; }
         MemComponent::component_t getSenderMemComponent() { return m_sender_mem_component; }
         MemComponent::component_t getReceiverMemComponent() { return m_receiver_mem_component; }
         core_id_t getRequester() { return m_requester; }
         IntPtr getAddress() { return m_address; }
         Byte* getDataBuf() { return m_data_buf; }
         UInt32 getDataLength() { return m_data_length; }
         HitWhere::where_t getWhere() { return m_where; }

         void setDataBuf(Byte* data_buf) { m_data_buf = data_buf; }
         void setWhere(HitWhere::where_t where) { m_where = where; }

         ShmemPerf* getPerf() { return m_perf; }
				 //#ifdef PIC_ENABLE_OPERATIONS
         void setAddress(IntPtr address) { m_address = address; }
				 //#endif

   };

}
