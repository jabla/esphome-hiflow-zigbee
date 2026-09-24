# Host tests for the HiFlow C port — no ESP32 / ESP-IDF / BLE, no hardware.
#
# Two independent suites live in this directory:
#   test_hiflow_frame.c  frame layout + key derivation + AES (OpenSSL backend)
#   test_pb.c            nanopb decode/encode of the protobuf messages
#   test_clock.c         wall clock, uptime handling and European summer time
#   test_proto.c         request payloads, paged replies and reassembly
#   test_session.c       the session state machine against a simulated inverter
#
#   make              build both test binaries
#   make test         build + run both suites (exit 0 only if all pass)
#   make test-frame   run only the frame/crypto suite
#   make test-pb      run only the protobuf suite
#   make test-clock   run only the clock suite
#   make test-proto   run only the payload/paging/reassembly suite
#   make test-session run only the session suite (-v on the binary: trace log)
#   make clean        remove build products
#
# Expected values come from the ground-truth test/vectors.json, reshaped into
# C headers at build time; the wire bytes themselves are read at run time.

CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
STD     ?= -std=c11

ROOT    := $(abspath $(CURDIR)/../..)
CORE    := $(ROOT)/src/hiflow_core
NANOPB  := $(ROOT)/src/hiflow_pb/nanopb
GEN     := $(ROOT)/src/hiflow_pb/generated
VECTORS := $(ROOT)/test/vectors.json

# ---- frame / crypto suite (OpenSSL host backend) --------------------
CRYPTO_BACKEND ?= OPENSSL
FRAME_CFLAGS   := $(STD) -DHIFLOW_CRYPTO_BACKEND_$(CRYPTO_BACKEND)
FRAME_INCS     := -I$(CORE) -I.
FRAME_LIBS     := -lcrypto
FRAME_SRC      := test_hiflow_frame.c $(CORE)/hiflow_frame.c $(CORE)/hiflow_crypto_openssl.c
FRAME_BIN      := test_hiflow_frame

# ---- protobuf suite (vendored nanopb) -------------------------------
PB_CFLAGS := $(STD)
PB_INCS   := -I$(NANOPB) -I$(GEN)
PB_SRC    := test_pb.c $(NANOPB)/pb_common.c $(NANOPB)/pb_decode.c $(NANOPB)/pb_encode.c \
             $(GEN)/RealDataNew.pb.c $(GEN)/CommandPB.pb.c $(GEN)/CommCmdPB.pb.c
PB_BIN    := test_pb

# ---- proto suite (payloads, paging, reassembly; nanopb, no crypto) --
PROTO_SRC := test_proto.c $(CORE)/hiflow_proto.c $(CORE)/hiflow_clock.c \
             $(NANOPB)/pb_common.c $(NANOPB)/pb_decode.c $(NANOPB)/pb_encode.c \
             $(GEN)/RealDataNew.pb.c $(GEN)/CommCmdPB.pb.c $(GEN)/APPInfomationData.pb.c
PROTO_BIN := test_proto

# ---- session suite (state machine against a simulated inverter) -----
SESSION_SRC := test_session.c fake_inverter.c \
               $(CORE)/hiflow_session.c $(CORE)/hiflow_proto.c $(CORE)/hiflow_clock.c \
               $(CORE)/hiflow_frame.c $(CORE)/hiflow_crypto_openssl.c $(CORE)/hiflow_appinfo.c \
               $(NANOPB)/pb_common.c $(NANOPB)/pb_decode.c $(NANOPB)/pb_encode.c \
               $(GEN)/RealDataNew.pb.c $(GEN)/CommCmdPB.pb.c $(GEN)/APPInfomationData.pb.c
SESSION_BIN := test_session

# ---- clock suite (no crypto, no protobuf) ---------------------------
CLOCK_SRC := test_clock.c $(CORE)/hiflow_clock.c
CLOCK_BIN := test_clock

.PHONY: all test test-frame test-pb test-clock test-proto test-session clean
all: $(FRAME_BIN) $(PB_BIN) $(CLOCK_BIN) $(PROTO_BIN) $(SESSION_BIN)

$(FRAME_BIN): $(FRAME_SRC) vectors.h
	$(CC) $(CFLAGS) $(FRAME_CFLAGS) $(FRAME_INCS) -o $@ $(FRAME_SRC) $(FRAME_LIBS)

$(PB_BIN): $(PB_SRC) vectors_pb.h
	$(CC) $(CFLAGS) $(PB_CFLAGS) $(PB_INCS) -o $@ $(PB_SRC)

$(CLOCK_BIN): $(CLOCK_SRC)
	$(CC) $(CFLAGS) $(STD) -I$(CORE) -o $@ $(CLOCK_SRC)

$(PROTO_BIN): $(PROTO_SRC) vectors_proto.h
	$(CC) $(CFLAGS) $(STD) -I$(CORE) -I$(NANOPB) -I$(GEN) -I. -o $@ $(PROTO_SRC) -lm

vectors_proto.h: $(VECTORS) gen_proto_header.py
	$(PYTHON) gen_proto_header.py $(VECTORS) $@

$(SESSION_BIN): $(SESSION_SRC) vectors_proto.h
	$(CC) $(CFLAGS) $(STD) -DHIFLOW_CRYPTO_BACKEND_$(CRYPTO_BACKEND) \
	    -I$(CORE) -I$(NANOPB) -I$(GEN) -I. -o $@ $(SESSION_SRC) $(FRAME_LIBS) -lm

vectors.h: $(VECTORS) $(ROOT)/test/emit_vectors_h.py
	$(PYTHON) $(ROOT)/test/emit_vectors_h.py $(VECTORS) $@

vectors_pb.h: $(VECTORS) gen_vectors_header.py
	$(PYTHON) gen_vectors_header.py $(VECTORS) $@

test-frame: $(FRAME_BIN)
	@echo "=== frame / crypto suite ==="
	@./$(FRAME_BIN)

test-pb: $(PB_BIN)
	@echo "=== protobuf / nanopb suite ==="
	@./$(PB_BIN) $(VECTORS)

test-clock: $(CLOCK_BIN)
	@echo "=== clock / timezone suite ==="
	@./$(CLOCK_BIN)

test-proto: $(PROTO_BIN)
	@echo "=== payload / paging / reassembly suite ==="
	@./$(PROTO_BIN)

test-session: $(SESSION_BIN)
	@echo "=== session state machine suite ==="
	@./$(SESSION_BIN)

test: test-frame test-pb test-clock test-proto test-session

clean:
	rm -f $(FRAME_BIN) $(PB_BIN) $(CLOCK_BIN) $(PROTO_BIN) $(SESSION_BIN) \
	      vectors.h vectors_pb.h vectors_proto.h
