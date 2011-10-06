###############################################################################
# Qemu Simulation Framework (qsim)                                            #
# Qsim is a modified version of the Qemu emulator (www.qemu.org), coupled     #
# a C++ API, for the use of computer architecture researchers.                #
#                                                                             #
# This work is licensed under the terms of the GNU GPL, version 2. See the    #
# COPYING file in the top-level directory.                                    #
###############################################################################
CXXFLAGS = -O2 -g -Idistorm/
LDFLAGS = -ldl -lqsim
PREFIX = /usr/local

all: libqsim.so qsim-fastforwarder

statesaver.o: statesaver.cpp statesaver.h qsim.h
	$(CXX) $(CXXFLAGS) -I./ -c -o statesaver.o statesaver.cpp

qsim-fastforwarder: fastforwarder.cpp statesaver.o statesaver.h libqsim.so
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -I ./ -L ./ -pthread \
               -o qsim-fastforwarder fastforwarder.cpp statesaver.o

libqsim.so: qsim.cpp qsim.h qsim-vm.h mgzd.h qsim-regs.h
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $<

install: libqsim.so qsim-fastforwarder
	cp libqsim.so $(PREFIX)/lib/
	cp qsim.h qsim-vm.h mgzd.h qsim-regs.h $(PREFIX)/include/
	cp qsim-fastforwarder $(PREFIX)/bin/
	/sbin/ldconfig

uninstall: $(PREFIX)/lib/libqsim.so
	rm -f $(PREFIX)/lib/libqsim.so $(PREFIX)/include/qsim.h         \
              $(PREFIX)/include/qsim-vm.h $(PREFIX)/include/qsim-regs.h \
              $(PREFIX)/bin/qsim-fastforwarder

clean:
	rm -f *~ \#*\# libqsim.so *.o test qtm qsim-fastforwarder