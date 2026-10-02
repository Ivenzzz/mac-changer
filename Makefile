CXX      ?= g++
WINDRES  ?= windres
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2
CPPFLAGS  = -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN
# -static: no libstdc++/libwinpthread DLLs next to the exe. -municode: wmain / wWinMain entry points.
LDFLAGS   = -static -municode
LDLIBS    = -lsetupapi -liphlpapi -lcomctl32 -luser32 -lgdi32 -ladvapi32

all: macchanger.exe macchanger-cli.exe

macaddr.o: src/macaddr.cpp src/macaddr.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

main_gui.o: src/main_gui.cpp src/macaddr.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

main_cli.o: src/main_cli.cpp src/macaddr.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# The manifest (requireAdministrator, common controls v6, DPI) goes only into the GUI exe.
res.o: app.rc app.manifest
	$(WINDRES) $< -o $@

macchanger.exe: main_gui.o macaddr.o res.o
	$(CXX) $(LDFLAGS) -mwindows $^ -o $@ $(LDLIBS)

macchanger-cli.exe: main_cli.o macaddr.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -f *.o *.exe

.PHONY: all clean
