TARGET = string_match_pic
CC = gcc

OSTYPE = $(shell uname)

ifeq ($(OSTYPE),CYGWIN_NT-5.1)
ARCH = -D_CYGWIN_
endif

ifeq ($(OSTYPE),Linux)
ARCH = -D_LINUX_
endif

ifeq ($(OSTYPE),SunOS)
ARCH =  -D_SOLARIS_
endif

ifeq ($(OSTYPE),Darwin)
ARCH = -D_DARWIN_
endif

PATH_OUT = ./$(OSTYPE)/

CFLAGS = -Wall $(ARCH) -O3 -I $(SNIPER_ROOT)/include
LIBS = -lpthread -lm -lrt
PROGS = string_match_pic
STR_MATCH_PIC_OBJS = string_match_pic.o
FILE = $(INPUT_DIR)/key_file_50MB.txt

.PHONY: default clean
all: $(PROGS)

$(TARGET): $(STR_MATCH_PIC_OBJS)
	$(CC) $(CFLAGS) -o $@ $(STR_MATCH_PIC_OBJS) $(LIBS)

run:
	../../run-sniper -n 1 -c ../pic_configs/sim_cur_pic_l1 --no-cache-warming --roi -- ./string_match_pic $(FILE) 16

clean:
	rm -f $(PROGS) *.o *.a *~ *.tmp *.bak *.log sim.out sim.info sim.stats.sqlite3 sim.cfg sim.scripts.py power.*

