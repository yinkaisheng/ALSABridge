CXX = g++
CXXFLAGS = -fPIC -std=c++17 $(FLAGG) $(FLAGD) $(FLAGV)
LDFLAGS = -lasound -pthread

ifeq ($(vlog), 1)
FLAGV = -DUTIL_VLOG
else
FLAGV =
endif

ifeq ($(release), 1)
FLAGG = -O2
else
FLAGG = -g
endif

ifeq ($(dll), 1)
FLAGD = -DBUILD_DLL
LINKARG = -shared -fPIC
OUT = libalsadevice.so
else
FLAGD =
LINKARG =
OUT = alsatest
endif

all: alsadevice

alsadevice: main.o alsadevice.o
	$(CXX) -o $@ $^ $(FLAGG) $(LDFLAGS)

# Shared library object; -DBUILD_DLL exports the C API (do not reuse alsadevice.o)
SO_CXXFLAGS = -fPIC -std=c++17 -DBUILD_DLL $(FLAGG) $(FLAGV)

alsadevice_so.o: alsadevice.cpp alsadevice.h dllexport.h util.h
	$(CXX) -c alsadevice.cpp -o $@ $(SO_CXXFLAGS)

libalsadevice.so: alsadevice_so.o
	$(CXX) -shared -fPIC -o $@ $^ $(LDFLAGS)

buildso: libalsadevice.so

%.o: %.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS)

clean:
	rm -f alsadevice.o alsadevice_so.o main.o alsadevice

cleanso:
	rm -f libalsadevice.so alsadevice_so.o

.PHONY: all clean cleanso buildso
