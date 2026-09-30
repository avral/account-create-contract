build/account_factory.wasm: account_factory.cpp
	mkdir -p build
	cdt-cpp -abigen -O2 -o build/account_factory.wasm account_factory.cpp

clean:
	rm -rf build

.PHONY: clean
