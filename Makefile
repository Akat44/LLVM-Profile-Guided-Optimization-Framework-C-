CXXFLAGS     = -rdynamic $(shell llvm-config --cxxflags) -fPIC -g -std=c++20 -fuse-ld=mold
LDFLAGS      = $(shell llvm-config --ldflags --system-libs --libs core analysis transformutils) -Wl,--exclude-libs,ALL -fuse-ld=mold
BUILDDIR     = build
DATADIR      = profile-data
DEPDIR       = $(BUILDDIR)/.deps
# Flags which, when added to gcc/g++, will auto-generate dependency files
DEPFLAGS     = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d

# Function which takes a list of words and returns a list of unique words in that list
# https://stackoverflow.com/questions/16144115/makefile-remove-duplicate-words-without-sorting
uniq         = $(if $1,$(firstword $1) $(call uniq,$(filter-out $(firstword $1),$1)))

OPTIMIZER_SOURCES = licm.cpp
SOURCES           = $(OPTIMIZER_SOURCES) profile.cpp

SOURCEDIRS     = $(call uniq, $(dir $(SOURCES)))
BUILDDIRS      = $(addprefix $(BUILDDIR)/, $(SOURCEDIRS))
DEPDIRS        = $(addprefix $(DEPDIR)/, $(SOURCEDIRS))
TEST_SOURCES   = $(wildcard tests/*.c)
DEPFILES       = $(SOURCES:%.cpp=$(DEPDIR)/%.d) $(DEPDIR)/writeup.d target/debug/libprofile.d
OPTIMIZER_LIBS = $(OPTIMIZER_SOURCES:%.cpp=$(BUILDDIR)/%.so)
TESTS_OUT      = $(TEST_SOURCES:%.c=$(BUILDDIR)/%-pre.ll) $(TEST_SOURCES:%.c=$(BUILDDIR)/%-preopt.ll) $(TEST_SOURCES:%.c=$(BUILDDIR)/%-profileopt.ll)
TESTS_EXEC     = $(TEST_SOURCES:%.c=$(BUILDDIR)/%-pre) $(TEST_SOURCES:%.c=$(BUILDDIR)/%-preopt) $(TEST_SOURCES:%.c=$(BUILDDIR)/%-profileopt)
BENCHES        = $(TEST_SOURCES:%.c=$(BUILDDIR)/%-bench)

.PHONY: all clean tests stats diff tar
.SECONDARY:

# By default, make all optimizers.
all: $(OPTIMIZER_LIBS)
tests: $(TESTS_OUT) $(TESTS_EXEC)
stats: $(TESTS_STATS)
benches: $(BENCHES)
tar: anovotny_arya4.tar.gz

clean:
	rm -rf $(BUILDDIR)
	rm -rf $(DATADIR)
	rm -rf anovotny_arya4

# Auto-Build .cpp files into .o
$(BUILDDIR)/%.o: %.cpp
$(BUILDDIR)/%.o: %.cpp $(DEPDIR)/%.d | $(DEPDIRS) $(BUILDDIRS)
	$(CXX) $(DEPFLAGS) $(INCLUDES) $(CXXFLAGS) -c $< -o $@

target/debug/libprofile.a: src/lib.rs 
target/debug/libprofile.a: src/lib.rs target/debug/libprofile.d
	cargo build

$(BUILDDIR)/%.so: $(BUILDDIR)/%.o
	$(CXX) -shared $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/tests/%-pre.bc: tests/%.c | $(BUILDDIR)/tests
	clang -fno-discard-value-names -Xclang -disable-O0-optnone -O0 -march=native -emit-llvm -c $^ -o $@
	opt -passes='mem2reg' $@ -o $@

$(BUILDDIR)/tests/%-preopt.bc: $(BUILDDIR)/tests/%-pre.bc $(OPTIMIZER_LIBS)
	opt $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='loop-simplify,mem2reg,loop(loop-invariant-code-motion),simplifycfg' $< -o $@

$(BUILDDIR)/tests/%-profileopt.bc: $(BUILDDIR)/tests/%-pre.bc $(DATADIR)/tests/%.json $(OPTIMIZER_LIBS)
	export PROF_DATA=$(DATADIR)/tests/$*.json && opt $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='loop-simplify,mem2reg,loop(loop-invariant-code-motion),simplifycfg' $< -o $@

$(BUILDDIR)/tests/%-profile.bc: $(BUILDDIR)/tests/%-preopt.bc $(BUILDDIR)/profile.so
	opt -load-pass-plugin=$(BUILDDIR)/profile.so -passes='profile' $< -o $@

%-llvm.o: %.bc
	llc --filetype=obj -O0 --relocation-model=pic $^ -o $@

%.asm: %.bc
	llc --filetype=asm -O0 --relocation-model=pic $^ -o $@

%-bench: $(BUILDDIR)/tests/%-pre $(BUILDDIR)/tests/%-preopt $(BUILDDIR)/tests/%-profileopt
	hyperfine '$(BUILDDIR)/tests/$*-pre' '$(BUILDDIR)/tests/$*-preopt' '$(BUILDDIR)/tests/$*-profileopt'

$(BUILDDIR)/tests/%-profile: $(BUILDDIR)/tests/%-profile-llvm.o target/debug/libprofile.a
	clang -lm -fuse-ld=mold $^ -o $@

$(BUILDDIR)/tests/%: $(BUILDDIR)/tests/%-llvm.o
	clang -lm -fuse-ld=mold $^ -o $@

$(DATADIR)/tests/%.json: $(BUILDDIR)/tests/%-profile
	$< --profile-data-file=$@

%.ll: %.bc
	llvm-dis $^ -o $@

anovotny_arya4.tar.gz: $(SOURCES) README.md Makefile $(TEST_SOURCES) writeup.pdf
	mkdir anovotny_arya4
	cp $^ --parents anovotny_arya4
	tar -zcf $@ anovotny_arya4
	rm -rf anovotny_arya4

writeup.pdf: writeup.tex
writeup.pdf: writeup.tex $(DEPDIR)/writeup.d | $(DEPDIRS)
	latexmk -synctex=1 -shell-escape -interaction=nonstopmode -file-line-error -lualatex $< -M -MF $(DEPDIR)/writeup.d -MP

# Make generated directories
$(DEPDIRS) $(BUILDDIRS) $(BUILDDIR)/tests : ; @mkdir -p $@
$(DEPFILES):
include $(wildcard $(DEPFILES))