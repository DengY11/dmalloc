CC       := clang
CFLAGS   := -std=c11 -Wall -Wextra -O2 -I include
LDFLAGS  := -pthread

SRC_DIR  := src
TEST_DIR := tests
BUILD_DIR:= build

SRCS     := $(SRC_DIR)/page_heap.c $(SRC_DIR)/large_bucket.c $(SRC_DIR)/dmalloc.c

TESTS    := $(BUILD_DIR)/test_page_heap $(BUILD_DIR)/test_large_bucket $(BUILD_DIR)/test_dmalloc $(BUILD_DIR)/test_mt $(BUILD_DIR)/test_free_release
BENCH    := $(BUILD_DIR)/bench_alloc

.PHONY: all clean test run-tests

all: $(TESTS)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_page_heap: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/test_page_heap.c include/page_heap.h include/large_bucket.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/test_page_heap.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_large_bucket: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/test_large_bucket.c include/page_heap.h include/large_bucket.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/test_large_bucket.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_dmalloc: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/test_dmalloc.c include/dmalloc.h include/page_heap.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/test_dmalloc.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_mt: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/test_mt.c include/dmalloc.h include/page_heap.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/test_mt.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_free_release: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/test_free_release.c include/dmalloc.h include/page_heap.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/test_free_release.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/bench_alloc: $(BUILD_DIR) $(SRCS) $(TEST_DIR)/bench_alloc.c include/dmalloc.h include/page_heap.h
	$(CC) $(CFLAGS) $(SRCS) $(TEST_DIR)/bench_alloc.c -o $@ $(LDFLAGS)

test: $(TESTS)
	$(BUILD_DIR)/test_page_heap
	$(BUILD_DIR)/test_large_bucket
	$(BUILD_DIR)/test_dmalloc
	$(BUILD_DIR)/test_mt
	$(BUILD_DIR)/test_free_release

.PHONY: bench
bench: $(BENCH)
	$(BUILD_DIR)/bench_alloc

run-tests: test

clean:
	rm -rf $(BUILD_DIR)
