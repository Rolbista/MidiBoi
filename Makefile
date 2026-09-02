# MidiBoi build helpers. All version pinning lives in sketch.yaml.
SKETCH      := MidiBoi.ino
BUILD_DIR   := .build
PROFILE     ?= leonardo
PORT        ?= $(shell arduino-cli board list --format json 2>/dev/null | \
                 python3 -c "import sys,json;b=json.load(sys.stdin).get('detected_ports',[]);\
print(next((p['port']['address'] for p in b if p.get('matching_boards')), ''))" 2>/dev/null)

.PHONY: build upload clean db monitor ports upgrade-check

build:                       ## Compile with the pinned profile
	arduino-cli compile --profile $(PROFILE) --build-path $(BUILD_DIR)

upload: build                ## Compile and flash (override with PORT=/dev/tty.xxx)
	@test -n "$(PORT)" || { echo "No board detected. Run 'make ports' and pass PORT=..."; exit 1; }
	arduino-cli upload --profile $(PROFILE) -p $(PORT) --input-dir $(BUILD_DIR)

upgrade-check:               ## Compile against the 'latest' profile without changing pins
	arduino-cli compile --profile latest --build-path $(BUILD_DIR)-latest

ports:                       ## List connected boards
	arduino-cli board list

BAUD ?= 115200
monitor:                     ## Open the serial monitor (BAUD=115200 by default)
	@test -n "$(PORT)" || { echo "No board detected. Run 'make ports' and pass PORT=..."; exit 1; }
	arduino-cli monitor -p $(PORT) -c baudrate=$(BAUD)

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-latest

db:                          ## Regenerate compile_commands.json + .clangd for the editor
	arduino-cli compile --profile $(PROFILE) --only-compilation-database --build-path $(BUILD_DIR)
	@./tools/gen-clangd.sh $(BUILD_DIR)
