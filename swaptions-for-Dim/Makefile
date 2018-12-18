MCXX = g++
DEF =
INCLUDE =

EXEC = swaptions-serial 

OBJS= CumNormalInv.o MaxFunction.o RanUnif.o nr_routines.o icdf.o \
	HJM_SimPath_Forward_Blocking.o HJM.o HJM_Swaption_Blocking.o  \
	HJM_Securities.o

KEEPFILES = -K
CXXFLAGS  = -O3 -Wno-deprecated -Wall #--verbose $(KEEPFILES)
LIBS:=$(LIBS) -lm

ifdef version
  ifeq "$(version)" "pthreads" 
    DEF := $(DEF) -DENABLE_THREADS
    CXXFLAGS := $(CXXFLAGS) -pthread
    EXEC=swaptions-pthreads
  endif

  ifeq "$(version)" "ompss"
    #MCXX=/gpfs/home/bsc18/bsc18186/apps/mcxx/bin/mcxx
    MCXX=mcxx
    DEF := $(DEF) -DENABLE_OMPSS
    CXXFLAGS := $(CXXFLAGS) --ompss
    EXEC=swaptions-ompss
  endif
  ifeq "$(version)" "ompss-centr"
    #MCXX=/gpfs/home/bsc18/bsc18186/apps/mcxx/bin/mcxx
    MCXX=/home/kchronaki/ompss/buildmcxxCentralized/bin/mcxx
    DEF := $(DEF) -DENABLE_OMPSS
    CXXFLAGS := $(CXXFLAGS) --ompss
    EXEC=swaptions-ompss
  endif


  ifeq "$(version)" "omp"
    MCXX=g++
    DEF := $(DEF) -DENABLE_OMP
    CXXFLAGS := $(CXXFLAGS) -fopenmp 
    EXEC=swaptions-omp
  endif
  #  ifeq "$(version)" "tbb"
#    DEF := $(DEF) -DENABLE_THREADS -DTBB_VERSION
#    LIBS := $(LIBS) -ltbb
#  endif
endif

all: $(EXEC)

$(EXEC): $(OBJS)
	$(MCXX) $(CXXFLAGS) $(LDFLAGS) $(DEF) $(OBJS) $(INCLUDE) $(LIBS) -o $(EXEC)

.cpp.o:
	$(MCXX) $(CXXFLAGS) $(DEF) -c $*.cpp -o $*.o

.c.o:
	$(MCXX) $(CXXFLAGS) $(DEF) -c $*.c -o $*.o

clean:
	rm -f $(OBJS) $(EXEC)

install: $(EXEC)
	cp -f $(EXEC) ../bin/$(EXEC)
	
