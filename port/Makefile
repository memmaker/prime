# macOS build (run from the repo root: make -f port/Makefile)
SRCS := $(filter-out src/NEUI.cpp src/NCUI.cpp src/CrtUI.cpp,$(wildcard src/*.cpp))
UI ?= XUI
O := port/obj
$(shell mkdir -p $(O))
OBJS := $(patsubst src/%.cpp,$(O)/%.o,$(SRCS)) $(O)/$(UI).o $(O)/Lore.o
BREW := /opt/homebrew
override CXXFLAGS += -MMD -O -g -w -std=gnu++98 -DNDEBUG -Iport -I$(BREW)/include -I/opt/X11/include -DNOGUI
LIBS := -L$(BREW)/lib -lsigsegv
ifeq ($(UI),NCUI)
LIBS += -lncurses -lpanel
else
LIBS += -L/opt/X11/lib -lX11 -lXft -lfontconfig
override CXXFLAGS += -DPRIME_X11 -I/opt/X11/include/freetype2
endif

BIN ?= prime
$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

$(O)/%.o: src/%.cpp
	$(CXX) -c $(CXXFLAGS) $< -o $@
$(O)/XUI.o: port/XUI.cpp port/*.h
	$(CXX) -c $(CXXFLAGS) -Isrc $< -o $@

$(O)/Lore.o: src/gen/Lore.cpp
	$(CXX) -c $(CXXFLAGS) -Isrc $< -o $@

# data tables: m4 + tablemk (built from src/tablemk: bison, flex, fpc)
$(O)/tablemk: src/tablemk/tablemaker.y src/tablemk/tablemaker.l src/tablemk/tablemaker.pas
	cd $(O) && bison -d ../../src/tablemk/tablemaker.y -o tablemaker.tab.c \
	  && flex -o lex.yy.c ../../src/tablemk/tablemaker.l && cc -c tablemaker.tab.c lex.yy.c \
	  && echo 'int yywrap(void){return 1;}' > fl.c && cc -c fl.c && ar rcs libfl.a fl.o \
	  && fpc ../../src/tablemk/tablemaker.pas -otablemk -FE. -Fl. -Fl$$(xcrun --show-sdk-path)/usr/lib \
	     -k-L$$(xcrun --show-sdk-path)/usr/lib >/dev/null
$(O)/ency2c: src/ency/encyclopedia2c.cpp
	$(CXX) -w $< -o $@
lore: $(O)/ency2c
	$(O)/ency2c src/Lore.txt && mv src/Lore.h src/Lore.cpp src/gen/

src/gen/%.cpp src/gen/%.h: ;
gen: $(O)/tablemk
	m4 < src/Monsters.txt | $(O)/tablemk src/gen/
	m4 < src/Attacks.txt | $(O)/tablemk src/gen/
	m4 < src/Flavor.txt | $(O)/tablemk src/gen/
	$(O)/tablemk src/gen/ < src/Items.txt

-include $(O)/*.d

clean:
	rm -f $(O)/*.o prime
