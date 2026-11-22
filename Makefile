# Convenience wrapper. The shell scripts are the source of truth; they also
# work on machines that have no make.
#
#   make            build the host (POSIX) binaries
#   make test       unit tests
#   make scenario   unit tests + end-to-end IPC scenario
#   make demo       run the demo story, artifacts in demo/
#   make qnx        cross-build for QNX 8.0 (needs the SDP environment)
#   make clean

.PHONY: all host test scenario demo qnx clean

all: host

host:
	./tools/build-host.sh

test:
	./tools/test.sh

scenario:
	./tools/test.sh all

demo:
	./tools/demo.sh

qnx:
	./qnx/build-qnx.sh

clean:
	./tools/build-host.sh clean
	rm -rf build-qnx run demo
