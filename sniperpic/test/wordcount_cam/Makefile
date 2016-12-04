TARGET = wordcount_cam
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
WC_OBJS_SER_CAM = wordcount_cam.o
PROGS = wordcount_cam
FILE = $(INPUT_DIR)/word_10MB.txt
#FILE = $(INPUT_DIR)/word_simple.txt

.PHONY: default clean
all: $(PROGS)

$(TARGET): $(WC_OBJS_SER_CAM)
	$(CC) $(CFLAGS) -o $@ $(WC_OBJS_SER_CAM) $(LIBS)

run:
	../../run-sniper -n 1 -c ../pic_configs/sim_cur --no-cache-warming --roi -- ./wordcount_cam $(FILE)
	#../../run-sniper -n 1 -c ../pic_configs/sim_cur --no-cache-warming --roi -s markers:stats -- ./wordcount_cam $(FILE)

clean:
	rm -f $(PROGS) *.o *.a *~ *.tmp *.bak *.log sim.out sim.info sim.stats.sqlite3 sim.cfg sim.scripts.py power.*

