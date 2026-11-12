all: tinyply-core

tinyply-core: tinyply.h tinyply.cpp polygon_subset.h polygon_subset.cpp example.cpp
	$(CXX) tinyply.cpp polygon_subset.cpp example.cpp -std=c++17 -o $@ -Wall -Wpedantic

ragged-export-tests: tinyply.h tinyply.cpp polygon_subset.h polygon_subset.cpp ragged_export_tests.cpp
	$(CXX) tinyply.cpp polygon_subset.cpp ragged_export_tests.cpp -std=c++17 -o $@ -Wall -Wpedantic

.PHONY: clean check
check: ragged-export-tests
	./ragged-export-tests

clean:
	rm -f tinyply-core ragged-export-tests
