IMAGE ?= shopping-sessions
DEV ?= official/dev
OUT ?= out
OFFICIAL := official/starter-duckdb
GENERATOR := official/generator/target/release/generate
RUNNER ?= build/shopping-sessions
PLATFORM := linux/amd64
export IMAGE

.PHONY: build docker official generator dev fixtures run-dev check-dev check-csv
build:
	scripts/build.sh

docker:
	docker build --platform $(PLATFORM) -t $(IMAGE) .

official:
	scripts/fetch-official.sh

generator:
	cargo build --release --locked --manifest-path official/generator/Cargo.toml

dev:
	$(GENERATOR) --seed 1 --events 1000000 --clicks 250000 --output $(DEV)

fixtures:
	python3 $(OFFICIAL)/scripts/run_fixtures.py $(OFFICIAL)/fixtures -- $(RUNNER)

run-dev:
	$(RUNNER) $(DEV) $(OUT)

check-dev:
	python3 $(OFFICIAL)/scripts/compare.py $(OUT) official/dev-answer

check-csv:
	mkdir -p build
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Isrc tests/csv_parser.cpp -o build/check-csv
	build/check-csv
