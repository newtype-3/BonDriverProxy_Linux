.PHONY: all clean distclean dep depend server client driver util

include Makefile.in

SRCDIR = .
LDFLAGS =
LIBS = -ldl
SRCS = BonDriverProxy.cpp BonDriverProxyEx.cpp sample.cpp
SOSRCS = BonDriver_Proxy.cpp BonDriver_Splitter.cpp
ifneq ($(UNAME), Darwin)
	SOSRCS += BonDriver_LinuxPT.cpp BonDriver_DVB.cpp
endif

ifeq ($(UNAME), Darwin)
all: server serverex client sample util
else
all: server serverex client driver sample util
endif
server: BonDriverProxy
serverex: BonDriverProxyEx
client: BonDriver_Proxy.$(EXT)
driver: BonDriver_LinuxPT.$(EXT) BonDriver_DVB.$(EXT) BonDriver_Splitter.$(EXT)

BonDriverProxy: BonDriverProxy.o
	$(CXX) $(CXXFLAGS) -rdynamic -o $@ $^ $(LIBS)

BonDriverProxyEx: BonDriverProxyEx.o
	$(CXX) $(CXXFLAGS) -rdynamic -o $@ $^ $(LIBS)

BonDriver_Proxy.$(EXT): BonDriver_Proxy.$(EXT).o
	$(CXX) $(SOFLAGS) $(CXXFLAGS) -o $@ $^ $(LIBS)

BonDriver_LinuxPT.$(EXT): BonDriver_LinuxPT.$(EXT).o
	$(CXX) $(SOFLAGS) $(CXXFLAGS) -o $@ $^ $(LIBS)

BonDriver_DVB.$(EXT): BonDriver_DVB.$(EXT).o
	$(CXX) $(SOFLAGS) $(CXXFLAGS) -o $@ $^ $(LIBS)

BonDriver_Splitter.$(EXT): BonDriver_Splitter.$(EXT).o
	$(CXX) $(SOFLAGS) $(CXXFLAGS) -o $@ $^ $(LIBS)

sample: sample.o
	$(CXX) $(CXXFLAGS) -rdynamic -o $@ $^ $(LIBS)

ifdef B25
CPPFLAGS += -DHAVE_LIBARIBB25
LIBS += -laribb25
SRCS += B25Decoder.cpp
BonDriverProxy BonDriverProxyEx BonDriver_LinuxPT.$(EXT) BonDriver_DVB.$(EXT): B25Decoder.o
B25Decoder.o: B25Decoder.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(ADDCOMPILEFLAGS) -fPIC -c -o $@ $<
endif

util:
	@cd util; make

%.$(EXT).o: %.cpp .depend
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(ADDCOMPILEFLAGS) -fPIC -c -o $@ $<

%.o: %.cpp .depend
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(ADDCOMPILEFLAGS) -c -o $@ $<

clean:
	$(RM) *.o *.so *.dylib BonDriverProxy BonDriverProxyEx sample .depend
	$(RM) -r *.dSYM
	@cd util; make clean
distclean: clean

dep: .depend
depend: .depend

ifneq ($(wildcard .depend),)
include .depend
endif

.depend:
	@$(RM) .depend
	@$(foreach SRC, $(SRCS:%=$(SRCDIR)/%), $(CXX) -g0 -MT $(basename $(SRC)).o -MM $(SRC) >> .depend;)
	@$(foreach SRC, $(SOSRCS:%=$(SRCDIR)/%), $(CXX) -g0 -MT $(basename $(SRC)).$(EXT).o -MM $(SRC) >> .depend;)
