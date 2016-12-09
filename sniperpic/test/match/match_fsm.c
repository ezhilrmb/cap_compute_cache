/* Copyright (c) 2007, Stanford University
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of Stanford University nor the
*       names of its contributors may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY STANFORD UNIVERSITY ``AS IS'' AND ANY
* EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY  BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/ 

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>

//#include <readline/readline.h>
//#include <readline/history.h>

#include <pthread.h>
#include "MapReduceScheduler.h"
#include "stddefines.h"
#include <sim_api.h>
#include "../../common/misc/fixed_types.h"



void cap_cache_init(Byte* cap_file)
{
  assert(cap_file);

  //CAP: Read the file line/line and load the cache inputs
  SimRoiStart();
  SimNamedMarker((unsigned long)cap_file,"cprg");
  SimRoiEnd();
}

void cap_ss_init(Byte* cap_file)
{
  assert(cap_file);

  //CAP: Read the file line/line and load the ss inputs
  
  SimRoiStart();
  SimNamedMarker((unsigned long)cap_file,"ssprg");
  SimRoiEnd();
}

void cap_repSte_init(Byte* ste_file)
{
  assert(ste_file);

  //CAP: Read the file line/line and load the reporting STE inputs
  
  SimRoiStart();
  SimNamedMarker((unsigned long)ste_file,"repSte");
  SimRoiEnd();
}

void cap_inputMatch_init(Byte* inp_file)
{
  assert(inp_file);

  //CAP: Read the input file line/line and load the ss inputs
  
  SimRoiStart();
  SimNamedMarker((unsigned long)inp_file,"match");
  SimRoiEnd();
}

int main(int argc, char *argv[]) 
{   
   int fd_cap, fd_cap2, fd_cap3, fd_cap4;
   Byte * fdata_cap, * fdata_cap2, *fdata_cap3, *fdata_cap4;
   struct stat finfo_cap, finfo_cap2, finfo_cap3, finfo_cap4;
   char *InputMatchFile, *CacheProgramFile, *SSProgramFile, *RepSteProgFile;

   //CAP: If the input text file or the cache program image file is not specified, then exit
   if (argv[1] == NULL || argv[2] == NULL || argv[3] == NULL || argv[4] == NULL)
   {
      printf("USAGE: %s <Input match filename> <Cache Program Image file> <SS program Image file><Reporting STE file>\n", argv[0]);
      exit(1);
   }
   InputMatchFile = argv[1];
   CacheProgramFile = argv[2];
   SSProgramFile = argv[3];
   RepSteProgFile = argv[4];
     
   struct timeval starttime,endtime,starttime2,endtime2,starttime3,endtime3,starttime4,endtime4;
   srand( (unsigned)time( NULL ) );

   printf("CAP: Program the Cache with STEs...\n");

   // ******************************************
   // Program the cache
   // ******************************************
   // Read in the Cache Program file
   CHECK_ERROR((fd_cap = open(CacheProgramFile,O_RDONLY)) < 0);
   // Get the file length
   CHECK_ERROR(fstat(fd_cap, &finfo_cap) < 0);
   // Memory map the file
   CHECK_ERROR((fdata_cap= mmap(0, finfo_cap.st_size + 1,
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_cap, 0)) == NULL);

   printf("CAP: Cache pgm file ptr :0x%p, content: %d\n", fdata_cap, *(fdata_cap+3));
	 gettimeofday(&starttime,0);
   cap_cache_init(fdata_cap);
	 gettimeofday(&endtime,0);
   printf("CAP: Cache STE Programming Completed %ld\n",(endtime.tv_sec - starttime.tv_sec));

   CHECK_ERROR(munmap(fdata_cap, finfo_cap.st_size + 1) < 0);
   CHECK_ERROR(close(fd_cap) < 0);

   // ******************************************
   // Program the swizzle switch
   // ******************************************
   printf("CAP: Program the Swizzle Switch with STEs...\n");

   // Read in the Cache Program file
   CHECK_ERROR((fd_cap2 = open(SSProgramFile,O_RDONLY)) < 0);
   // Get the file length
   CHECK_ERROR(fstat(fd_cap2, &finfo_cap2) < 0);
   // Memory map the file
   CHECK_ERROR((fdata_cap2= mmap(0, finfo_cap2.st_size + 1,
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_cap2, 0)) == NULL);

   printf("CAP: Swizzle switch pgm file ptr :0x%p, content: %d\n", fdata_cap2, *(fdata_cap2+3));
	 gettimeofday(&starttime2,0);
   cap_ss_init(fdata_cap2);
	 gettimeofday(&endtime2,0);
   printf("CAP: Swizzle Switch STE Programming Completed %ld\n",(endtime2.tv_sec - starttime2.tv_sec));


   CHECK_ERROR(munmap(fdata_cap2, finfo_cap2.st_size + 1) < 0);
   CHECK_ERROR(close(fd_cap2) < 0);

   // ******************************************
   // Program the reporting ste config
   // ******************************************
   printf("CAP: Program the Reporting STE ...\n");

   // Read in the Rep STE Program file
   CHECK_ERROR((fd_cap3 = open(RepSteProgFile,O_RDONLY)) < 0);
   // Get the file length
   CHECK_ERROR(fstat(fd_cap3, &finfo_cap3) < 0);
   // Memory map the file
   CHECK_ERROR((fdata_cap3= mmap(0, finfo_cap3.st_size + 1,
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_cap3, 0)) == NULL);

   printf("CAP: Reporting STE file ptr :0x%p, content: %d\n", fdata_cap3, *(fdata_cap3+3));
	 gettimeofday(&starttime3,0);
   cap_repSte_init(fdata_cap3);
	 gettimeofday(&endtime3,0);
   printf("CAP: Reporting STE Programming Completed %ld\n",(endtime3.tv_sec - starttime3.tv_sec));


   CHECK_ERROR(munmap(fdata_cap3, finfo_cap3.st_size + 1) < 0);
   CHECK_ERROR(close(fd_cap3) < 0);

   // ******************************************
   // Finally start streaming in the inputs
   // ******************************************
   printf("CAP: Reading the input file stream...\n");

   // Read in the input match file
   CHECK_ERROR((fd_cap4 = open(InputMatchFile,O_RDONLY)) < 0);
   // Get the file length
   CHECK_ERROR(fstat(fd_cap4, &finfo_cap4) < 0);
   // Memory map the file
   CHECK_ERROR((fdata_cap4= mmap(0, finfo_cap4.st_size + 1,
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_cap4, 0)) == NULL);

   printf("CAP: Input stream file ptr :0x%p, content: %d\n", fdata_cap4, *(fdata_cap4+3));
	 gettimeofday(&starttime4,0);
   cap_inputMatch_init(fdata_cap4);
	 gettimeofday(&endtime4,0);
   printf("CAP: Input streaming Completed %ld\n",(endtime4.tv_sec - starttime4.tv_sec));


   CHECK_ERROR(munmap(fdata_cap4, finfo_cap4.st_size + 1) < 0);
   CHECK_ERROR(close(fd_cap4) < 0);

   return 0;
}
