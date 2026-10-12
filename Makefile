build/account_factory.wasm: account_factory.cpp
	mkdir -p build
	cdt-cpp -abigen -O2 -o build/account_factory.wasm account_factory.cpp

# XPR Network: plain names and eosio.proton::newaccres
xpr: build/xpr/account_factory.wasm

build/xpr/account_factory.wasm: account_factory.cpp
	mkdir -p build/xpr
	cdt-cpp -abigen -O2 -DXPR_NETWORK -o build/xpr/account_factory.wasm account_factory.cpp

clean:
	rm -rf build

.PHONY: xpr clean
