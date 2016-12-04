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
* DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY BE LIABLE FOR ANY
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
#include <pthread.h>

#include "MapReduceScheduler.h"
#include "stddefines.h"
#include <sim_api.h>

#define DEFAULT_DISP_NUM 10
#define START_ARRAY_SIZE 2000

#define LEVEL_BREADTH 26
#define SINGLE_CAM_SIZE 1024
#define SINGLE_WORD_SIZE 16
#define WORDS_IN_CAM (SINGLE_CAM_SIZE/SINGLE_WORD_SIZE)

typedef struct {
   long flen;
   char *fdata;
} wc_data_t;

enum {
   IN_WORD,
   NOT_IN_WORD
};

unsigned int sniper_payload[2];

//This is where words actually go
typedef struct single_cam_data_t single_cam_data_t;
struct single_cam_data_t {
	unsigned int cam_id;
	char cam[SINGLE_CAM_SIZE];
	char* end; //end of last word
	single_cam_data_t *next;
};

//This is where counters go
typedef struct single_cam_cnt_t single_cam_cnt_t;
struct single_cam_cnt_t {
	int cam[WORDS_IN_CAM];
	struct single_cam_cnt_t *next;
};

single_cam_data_t words_hashtable[LEVEL_BREADTH][LEVEL_BREADTH];
single_cam_cnt_t 	count_hashtable[LEVEL_BREADTH][LEVEL_BREADTH];

//TODO: Stats gathering : all i need is this?
unsigned long cam32_searches, cam64_searches, cam128_searches, cam256_searches;
unsigned long cams_created;
int sim_markers_done = 0;

void wordcount_getword(void *args_in);
void wordcount_addword(char* word, int len) ;

/** wordcount_splitter()
 *  Memory map the file and divide file on a word border i.e. a space.
 */
void wordcount_splitter(void *data_in)
{
	int i, j;
  wc_data_t * data = (wc_data_t *)data_in; 
  map_args_t* out = (map_args_t*)malloc(sizeof(map_args_t));
  out->data = data->fdata;
  out->length = data->flen;
  
	cams_created = 0;
	for(i=0;i<LEVEL_BREADTH;i++) {
		for(j=0;j<LEVEL_BREADTH;j++) {
			bzero(words_hashtable[i][j].cam, SINGLE_CAM_SIZE);
			words_hashtable[i][j].cam_id 	= cams_created;
			words_hashtable[i][j].end = NULL;
			words_hashtable[i][j].next = NULL;
			bzero(count_hashtable[i][j].cam, WORDS_IN_CAM * sizeof(int));
			count_hashtable[i][j].next = NULL;
			++cams_created;
		}
	} 
	cam32_searches = cam64_searches = cam128_searches = cam256_searches = 0;
  wordcount_getword(out);
  free(out);
}

/** wordcount_map()
 * Go through the file and update the count for each unique word
 */
void wordcount_getword(void *args_in) 
{
   map_args_t* args = (map_args_t*)args_in;

   char *curr_start, curr_ltr;
   int state = NOT_IN_WORD;
   int i;
   assert(args);

   char *data = (char *)(args->data);
   curr_start = data;
   assert(data);

   //printf("args_len is %d\n", args->length);

   SimRoiStart();
   for (i = 0; i < args->length; i++)
   {
      curr_ltr = toupper(data[i]);
      switch (state)
      {
      case IN_WORD:
         data[i] = curr_ltr;
         if ((curr_ltr < 'A' || curr_ltr > 'Z') && curr_ltr != '\'')
         {
            data[i] = 0;
			wordcount_addword(curr_start, &data[i] - curr_start + 1);
            state = NOT_IN_WORD;
         }
      break;

      default:
      case NOT_IN_WORD:
         if (curr_ltr >= 'A' && curr_ltr <= 'Z')
         {
            curr_start = &data[i];
            data[i] = curr_ltr;
            state = IN_WORD;
         }
         break;
      }
   }

   // Add the last word
   if (state == IN_WORD)
   {
			data[args->length] = 0;
			//printf("\nthe word is %s\n\n",curr_start);
			wordcount_addword(curr_start, &data[i] - curr_start + 1);
   }
  SimRoiEnd();
}

/** dobsearch()
 *  Search for a specific word in the CAM array
 *  Return non-zero position if present, else -1 if not present
 */
int docamsearch(char* start, char *end, char *word)
{
   // CAM search to check if word is present
	int pos = -1;
  while (start < end) {
		pos++;
    int cmp = strcmp(start, word);
		if(cmp == 0)
			return pos;
		start += SINGLE_WORD_SIZE;	//length of word
  }
  return -1; //not present
}

/** wordcount_addword()
 * Add a new word to the array in the correct sorted postion
 */
void wordcount_addword(char* word, int len) 
{
	//1. Compare first two characters to decide which cam list to look at
	int i, j;
	char first 																= word[0];
	char second 															= (len > 2) ? word[1] : '\'';
	int level1_index 													= (first == '\'') ? 0 : (first - 'A');
	int level2_index 													= (second == '\'')? 0 : (second - 'A');
	short is_insert														=  0;
	int word_pos 															= -1;
	int linked_list_index 										= -1;
  single_cam_cnt_t* new_cnt_cam 						= NULL;
	single_cam_data_t * cur_data_cam, *prev_data_cam;
  single_cam_cnt_t* count_cam;

	assert((level1_index >=0) && (level1_index < LEVEL_BREADTH));
	assert((level2_index >=0) && (level2_index < LEVEL_BREADTH));

	cur_data_cam 	= &words_hashtable[level1_index][level2_index];
	prev_data_cam = cur_data_cam;
  count_cam 		= &count_hashtable[level1_index][level2_index];

	if(cur_data_cam->end == NULL) //empty entry 
		is_insert = 1;
	else {	//need a search/searches
		do {

			sniper_payload[0] = cur_data_cam->cam_id; 
			sniper_payload[1] = ((cur_data_cam->end - 
															cur_data_cam->cam)/SINGLE_WORD_SIZE);
			//printf("\nAPP(%u,%u)",sniper_payload[0], sniper_payload[1]);
			//Snipersim will ignore instructions pin (functional model) feeds it. Instead we will insert our own search+cmp operations	
			SimNamedMarker(sniper_payload,"igrb");	
			//SimMarker(1, sim_markers_done);
			word_pos 				= docamsearch(cur_data_cam->cam, cur_data_cam->end, word);
			//SimMarker(2, sim_markers_done);
			//Snipersim will stop ignoring pin (functional model) instructions now
			SimNamedMarker(0,"igre");	 
			++sim_markers_done;
			prev_data_cam 	= cur_data_cam;
			cur_data_cam 		= ((word_pos == -1) && (cur_data_cam->next)) ? 
												cur_data_cam->next : cur_data_cam; 	//didnt find it, move to next cam if present
			linked_list_index++;
		} while ((word_pos == -1) && (prev_data_cam != cur_data_cam));
	}

	if((word_pos == -1) || (is_insert == 1)) {		//Need insert

		//Do we need to allocate new CAMs?
		if((cur_data_cam->cam + SINGLE_CAM_SIZE) == cur_data_cam->end) {
   		single_cam_data_t* new_data_cam = (single_cam_data_t*)malloc(sizeof(single_cam_data_t));
			assert(cur_data_cam->next == NULL);
			new_data_cam->end 	= NULL;
			new_data_cam->next 	= NULL;
			new_data_cam->cam_id 	= cams_created;
			++cams_created;
			bzero(new_data_cam->cam, SINGLE_CAM_SIZE);
			cur_data_cam->next = new_data_cam;
			cur_data_cam = new_data_cam;

   		new_cnt_cam 										= (single_cam_cnt_t*)malloc(sizeof(single_cam_cnt_t));
			bzero(new_cnt_cam->cam, WORDS_IN_CAM * sizeof(int));
			new_cnt_cam->next = NULL;

		}

		//Write word to CAM
		if(cur_data_cam->end == NULL) cur_data_cam->end = cur_data_cam->cam;
		for(i=0, j=0; i<(SINGLE_WORD_SIZE-1); i++, j++) {
			if(j < len)
				*cur_data_cam->end = word[j];
			++(cur_data_cam->end);
		}
		*cur_data_cam->end = 0;
		if(cur_data_cam->end < (cur_data_cam->cam + SINGLE_CAM_SIZE))
			cur_data_cam->end++;

		//Calculate position of word in CAM
		word_pos = ((cur_data_cam->end - cur_data_cam->cam) / SINGLE_WORD_SIZE) - 1;
	}

	//Update the counter now
	assert((word_pos >= 0) && (word_pos < WORDS_IN_CAM));
	while(linked_list_index > 0) {
		count_cam = count_cam->next;
		--linked_list_index;
	}
	if(new_cnt_cam) {
		assert((count_cam->next == NULL) && (word_pos == 0));
		count_cam->next = new_cnt_cam;
		count_cam = new_cnt_cam;
	}
	count_cam->cam[word_pos]++;
}

int main(int argc, char *argv[]) {
   
   int fd;
   char * fdata;
   int disp_num;
   struct stat finfo;
   char * fname, * disp_num_str;

	 int i,j;
	 single_cam_data_t * cur_data_cam;
   single_cam_cnt_t* count_cam;
	 char *start;
	 int pos;

   // Make sure a filename is specified
   if (argv[1] == NULL)
   {
      printf("USAGE: %s <filename> [Top # of results to display]\n", argv[0]);
      exit(1);
   }
   
   fname = argv[1];
   disp_num_str = argv[2];

   printf("Wordcount: Running...\n");
   
   // Read in the file
   CHECK_ERROR((fd = open(fname, O_RDONLY)) < 0);
   // Get the file info (for file length)
   CHECK_ERROR(fstat(fd, &finfo) < 0);
   // Memory map the file
   CHECK_ERROR((fdata = mmap(0, finfo.st_size + 1, 
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) == NULL);
   
   // Get the number of results to display
   CHECK_ERROR((disp_num = (disp_num_str == NULL) ? 
      DEFAULT_DISP_NUM : atoi(disp_num_str)) <= 0);
   
   // Setup splitter args
   wc_data_t wc_data;
   wc_data.flen = finfo.st_size;
   wc_data.fdata = fdata;

   printf("Wordcount Serial: Running\n");
   
   wordcount_splitter(&wc_data);
   printf("Additional CAMs created: %lu, total CAMs:%lu\n", cams_created-(26*26), cams_created);
   printf("Number of CAM searches:%d\n", sim_markers_done);

		for(i=0;i<LEVEL_BREADTH;i++) {
			for(j=0;j<LEVEL_BREADTH;j++) {
				cur_data_cam 	= &words_hashtable[i][j];
				count_cam 		= &count_hashtable[i][j];
				while(cur_data_cam) {
					assert(count_cam);
					pos = 0;
					start = cur_data_cam->cam;
					while(start < cur_data_cam->end) {
      			dprintf("%s: %d\n", start, count_cam->cam[pos]);
						start += SINGLE_WORD_SIZE;
						pos++;
					}
					cur_data_cam = cur_data_cam->next;
					count_cam = count_cam->next;
				}
			}
		}

   CHECK_ERROR(munmap(fdata, finfo.st_size + 1) < 0);
   CHECK_ERROR(close(fd) < 0);

   return 0;
}
