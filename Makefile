FUSE_ARGS=$(shell pkg-config fuse --cflags --libs 2>/dev/null)
ifeq (${FUSE_ARGS},)
$(error libfuse-dev not found)
endif

default: execfs

# Version info. Set this here or via the command line for a release. Otherwise
# you just get the git commit ID.
ifndef VERSION
VERSION="git-$(shell git rev-parse HEAD)"
endif

ifeq (${V},1)
Q:=
else
Q:=@
endif

ifeq (${DEBUG},1)
WERROR:=
else
WERROR:=-Werror
endif

### EXECFS TARGETS ###

execfs: main.o config.o fileops.o log.o
	@echo " [LD] $@"
	${Q}gcc -Wall ${WERROR} -o $@ $^ ${FUSE_ARGS}

main.o: entry.h config.h fileops.h log.h globals.h
config.o: entry.h config.h
fileops.o: entry.h fileops.h globals.h
log.o: log.h

%.o: %.c
	@echo " [CC] $@"
	${Q}gcc -DVERSION=\"${VERSION}\" -Wall ${WERROR} -c -o $@ $< ${FUSE_ARGS}

### TOOLS TARGETS ###

open: tools/open.o
	@echo " [LD] $@"
	${Q}gcc -Wall ${WERROR} -o $@ $^

block: tools/block.o
	@echo " [LD] $@"
	${Q}gcc -Wall ${WERROR} -o $@ $^

### TEST TARGETS ###

.PHONY: tests
TESTS=$(patsubst tests/%.config,%,$(wildcard tests/test-*.config))
tests: ${TESTS}

test-%: tests/test-%.sh tests/test-%.config execfs tests/test.sh
	@echo " [TEST] $@"
	${Q}PATH=.:${PATH} ./tests/test.sh $< $(word 2,$^)

.PHONY: default clean
clean:
	@echo " [CLEAN] execfs open block *.o"
	${Q}rm -f execfs open block *.o
