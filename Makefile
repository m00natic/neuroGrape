
# files

EXE = grapeLog

OBJS = attack.o board.o book.o eval.o fen.o hash.o list.o main.o material.o \
       move.o move_check.o move_do.o move_evasion.o move_gen.o move_legal.o \
       option.o pawn.o piece.o posix.o protocol.o pst.o pv.o random.o recog.o \
       search.o search_full.o see.o sort.o square.o trans.o util.o value.o \
       vector.o probe.o

# rules

all: $(EXE) .depend

clean:
	$(RM) *.o .depend gmon.out $(EXE) $(EXE).intel *~

# general

CXX      = gcc
CXXFLAGS = -pipe
LDFLAGS  = -lm -ldl -lpthread

# C++

CXXFLAGS += -O3 -fno-exceptions -fno-rtti -Wall

# optimisation

CXXFLAGS +=  -fstrict-aliasing
CXXFLAGS += -fomit-frame-pointer
# CXXFLAGS += -march=athlon-xp # SELECT ME
# CXXFLAGS += -march=pentium4 

# profiling

#CXXFLAGS += -fprofile-generate
#LDFLAGS  += -fprofile-generate
#CXXFLAGS += -fprofile-use


# strip

LDFLAGS += -s

# dependencies

$(EXE): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) -lstdc++

.depend:
	$(CXX) -MM $(OBJS:.o=.cpp) > $@

include .depend

