CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -fopenmp
LDLIBS ?= -lm
PYTHON ?= python
EXEEXT :=
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif
TARGET := fingerprint_omp$(EXEEXT)

.PHONY: all clean test demo
all: $(TARGET)

$(TARGET): fingerprint_omp.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

test:
	CC="$(CC)" $(PYTHON) tests/test_smoke.py

demo: $(TARGET)
	./$(TARGET) --synthetic 6000 --query-index 1500 --threads 4 --schedule dynamic --early 1 --threshold 100 --repeats 5

clean:
	rm -f fingerprint_omp fingerprint_omp.exe
