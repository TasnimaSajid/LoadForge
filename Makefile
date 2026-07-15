# ============================================================================
#  LoadForge v3 Makefile
# ============================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -g -Iinclude -std=c11 \
           -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS := -lpthread -ljson-c -lm

SRC     := src
BIN     := bin
INC     := include

.PHONY: all clean backends backends-mixed lb stop test demo add-backend ctl

all: $(BIN)/lb $(BIN)/backend $(BIN)/client $(BIN)/lf-ctl
	@chmod +x *.sh scripts/*.sh tests/*.sh 2>/dev/null || true
	@echo "[+] Build complete"

$(BIN):
	mkdir -p $(BIN)

$(BIN)/lb: $(SRC)/lb.c $(SRC)/config.c $(INC)/config.h | $(BIN)
	$(CC) $(CFLAGS) $(SRC)/lb.c $(SRC)/config.c -o $@ $(LDFLAGS)

$(BIN)/backend: $(SRC)/backend.c | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ -lpthread -lm

$(BIN)/client: $(SRC)/client.c $(SRC)/config.c $(INC)/config.h | $(BIN)
	$(CC) $(CFLAGS) $(SRC)/client.c $(SRC)/config.c -o $@ -lpthread -ljson-c

$(BIN)/lf-ctl: $(SRC)/lf_ctl.c $(INC)/config.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ 

clean:
	rm -rf $(BIN)

backends: $(BIN)/backend
	@echo "[+] Starting 3 backends on 127.0.0.1:9001-9003"
	@$(BIN)/backend 127.0.0.1 9001 1 > /tmp/lf_b1.log 2>&1 &
	@$(BIN)/backend 127.0.0.1 9002 2 > /tmp/lf_b2.log 2>&1 &
	@$(BIN)/backend 127.0.0.1 9003 3 > /tmp/lf_b3.log 2>&1 &
	@sleep 0.5
	@echo "[+] Backends ready"

backends-mixed: $(BIN)/backend
	@echo "[+] Starting 3 backends — Backend 2 uses CPU simulation"
	@$(BIN)/backend 127.0.0.1 9001 1         > /tmp/lf_b1.log 2>&1 &
	@$(BIN)/backend 127.0.0.1 9002 2 cpu     > /tmp/lf_b2.log 2>&1 &
	@$(BIN)/backend 127.0.0.1 9003 3         > /tmp/lf_b3.log 2>&1 &
	@sleep 0.5
	@echo "[+] Backends ready (Backend 2 is CPU-intensive)"

lb: $(BIN)/lb
	$(BIN)/lb config/config.json

stop:
	-@pkill -f "$(BIN)/backend" 2>/dev/null || true
	-@pkill -f "$(BIN)/lb"      2>/dev/null || true
	@echo "[+] Stopped"

test: all
	@bash tests/run_tests.sh

demo: all
	@bash run_demo.sh

add-backend:
	@chmod +x scripts/add_backend.sh
	@scripts/add_backend.sh $(IP) $(PORT) $(SIM)

ctl: $(BIN)/lf-ctl
	@$(BIN)/lf-ctl $(or $(CMD),status)
