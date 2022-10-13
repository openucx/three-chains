#
# Copyright (C) 2020-2022 Arm Ltd. All rights reserved.
#
UCX_PATH ?= ${HOME}/opt
LLVM_PATH ?= ${HOME}/llvm

BASE_CFLAGS = -Wall -Wextra -pedantic -std=c11
BASE_CXXFLAGS = -Wall -Wextra -pedantic -std=c++17

# Debugging flags: -O0 -g3 -fno-omit-frame-pointer -rdynamic
# Don't use -fsanitize=address on dynamic libs, manually add it for *.cpp.

# Compile with -O3 produces large ifunc messages, but we have caching
IFUNC_BC_CFLAGS = -O3
IFUNC_SO_CFLAGS = -O3
IFUNC_EXE_CXXFLAGS = -O3 -Wl,-O1

UCX_CFLAGS = -I${UCX_PATH}/include
UCX_LDFLAGS = -L${UCX_PATH}/lib -Wl,-rpath,${UCX_PATH}/lib -lucp -lucs

EXE_SOURCES = $(wildcard *.cpp)
EXE_TARGETS = $(EXE_SOURCES:.cpp=.x)

LIB_SOURCES = $(wildcard *.c)
LIB_TARGETS = $(LIB_SOURCES:.c=.so)

# $(1) = CFLAGS; $(2) = LDFLAGS
define build_ifunc_so =
	gcc ${BASE_CFLAGS} ${IFUNC_SO_CFLAGS} $(1) -mcmodel=tiny -fPIC -fno-plt -S $< $(2)
	./patch_asm.py $(basename $<) < $(basename $<).s > $(basename $<).patched.s
	gcc ${BASE_CFLAGS} ${IFUNC_SO_CFLAGS} $(1) -mcmodel=tiny -fPIC -fno-plt -shared $(basename $<).patched.s -o $@ $(2)
endef

# $(1) = CFLAGS; $(2) = LDFLAGS; $(3) = Target triple
# Use '-S' instead of '-c' to get human-readable IR (*.ll)
# To get rid of 'lib.so.3', use `sed -e 's/\.so\../\.so/g'` or `sed -n -e '/\.so$/p'`
# But some libraries do not have the 'lib.so' symlink! Better keep everything!
define build_ifunc_bc =
	${LLVM_PATH}/bin/clang ${BASE_CFLAGS} ${IFUNC_BC_CFLAGS} $(1) -c -emit-llvm --target=$(3) $< -o $(basename $@).$(3).bc
	gcc ${BASE_CFLAGS} ${IFUNC_BC_CFLAGS} $(1) $< -shared -fPIC -o /dev/null -Wl,--trace $(2) | sed -n -e '/\.so/p' | sed 's/.*(\(.*\))/\1/' | xargs basename -a | sort | uniq > $(basename $@).deps
endef

all: $(EXE_TARGETS) $(LIB_TARGETS)

%.x: %.cpp
	g++ $< ${BASE_CXXFLAGS} ${IFUNC_EXE_CXXFLAGS} ${UCX_CFLAGS} ${UCX_LDFLAGS} -o $@

b.so: b.c
	$(call build_ifunc_so,,)
	$(call build_ifunc_bc,,,aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,,,x86_64-unknown-linux-gnu)

p.so: p.c
	$(call build_ifunc_so,${UCX_CFLAGS},${UCX_LDFLAGS})
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},x86_64-unknown-linux-gnu)

hello.so: hello.c
	$(call build_ifunc_so,,)
	$(call build_ifunc_bc,,,aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,,,x86_64-unknown-linux-gnu)

chaser.so: chaser.c
	$(call build_ifunc_so,${UCX_CFLAGS},${UCX_LDFLAGS})
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},x86_64-unknown-linux-gnu)

stop.so: stop.c
	$(call build_ifunc_so,${UCX_CFLAGS},${UCX_LDFLAGS})
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},x86_64-unknown-linux-gnu)

result.so: result.c
	$(call build_ifunc_so,${UCX_CFLAGS},${UCX_LDFLAGS})
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,${UCX_CFLAGS},${UCX_LDFLAGS},x86_64-unknown-linux-gnu)

pure.so: pure.c
	$(call build_ifunc_so,,)
	$(call build_ifunc_bc,,,aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,,,x86_64-unknown-linux-gnu)

multi.so: multi.c
	$(call build_ifunc_so,,-lcrypto -lz -pthread)
	$(call build_ifunc_bc,,-lcrypto -lz -pthread,aarch64-unknown-linux-gnu)
	$(call build_ifunc_bc,,-lcrypto -lz -pthread,x86_64-unknown-linux-gnu)

clean:
	rm -f *.so *.s *.bc *.ll *.deps *.x
