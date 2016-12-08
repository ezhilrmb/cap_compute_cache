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



int getnextline(char* output, int max_len, char* file)
{
	int i=0;
	while(i<max_len-1)
	{
		if( file[i] == '\0')
		{
			if(i==0)
				return -1;
			else
				return i;
		}
		if( file[i] == '\r')
			return (i+2);

		if( file[i] == '\n' )
			return (i+1);

		output[i] = file[i];
		i++;
	}
	file+=i;
	return i;
}

void cap_cache_init(Byte* cap_file)
{
   
  assert(cap_file);

  //CAP: Read the file line/line and load the cache inputs
  SimRoiStart();
  SimNamedMarker((unsigned long)cap_file,"cprg");
  SimRoiEnd();


}


int main(int argc, char *argv[]) 
{   
   int fd_cap;
   Byte* fdata_cap;
   struct stat finfo_cap;
   char * InputMatchFile, *CacheProgramFile;

   //CAP: If the input text file or the cache program image file is not specified, then exit
   if (argv[1] == NULL || argv[2] == NULL)
   {
      printf("USAGE: %s <Input match filename> <Cache Program Image file>\n", argv[0]);
      exit(1);
   }
   InputMatchFile = argv[1];
   CacheProgramFile = argv[2];
     
   struct timeval starttime,endtime;
   srand( (unsigned)time( NULL ) );

   printf("CAP: Program the Cache with STEs...\n");

   // Read in the Cache Program file
   CHECK_ERROR((fd_cap = open(CacheProgramFile,O_RDONLY)) < 0);
   // Get the file length
   CHECK_ERROR(fstat(fd_cap, &finfo_cap) < 0);
   // Memory map the file
   CHECK_ERROR((fdata_cap= mmap(0, finfo_cap.st_size + 1,
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_cap, 0)) == NULL);

   printf("CAP: Cache pgm file ptr :0x%p, content: %d", fdata_cap, *(fdata_cap+3));
	 gettimeofday(&starttime,0);
   cap_cache_init(fdata_cap);
	 gettimeofday(&endtime,0);
   printf("CAP: Cache STE Programming Completed %ld\n",(endtime.tv_sec - starttime.tv_sec));


   CHECK_ERROR(munmap(fdata_cap, finfo_cap.st_size + 1) < 0);
   CHECK_ERROR(close(fd_cap) < 0);
   return 0;
}
