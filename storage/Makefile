# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra -Werror
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD_DIR ?= build
CPPFLAGS += -I. -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
SOURCES = a2_storage.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c tests/storage_test.c

.PHONY: test clean
test: $(BUILD_DIR)/storage_test
	./$(BUILD_DIR)/storage_test

$(BUILD_DIR)/storage_test: $(SOURCES) a2_storage.h vendor/littlefs/lfs.h vendor/littlefs/lfs_util.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $(SOURCES) -o $@

.PHONY: test-flash
test-flash: $(BUILD_DIR)/is29_test
	./$(BUILD_DIR)/is29_test

$(BUILD_DIR)/is29_test: a2_storage.c a2_storage.h a2_is29gl032.c a2_is29gl032.h tests/is29_test.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) a2_storage.c a2_is29gl032.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c tests/is29_test.c -o $@

.PHONY: test-overlay test-catalog test-all
test-overlay: $(BUILD_DIR)/overlay_test
	./$(BUILD_DIR)/overlay_test

$(BUILD_DIR)/overlay_test: a2_storage.c a2_storage.h a2_disk_overlay.c a2_disk_overlay.h tests/overlay_test.c tests/storage_test.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) a2_storage.c a2_disk_overlay.c vendor/littlefs/lfs.c vendor/littlefs/lfs_util.c tests/overlay_test.c -o $@

test-catalog: $(BUILD_DIR)/dos_catalog_test
	./$(BUILD_DIR)/dos_catalog_test

$(BUILD_DIR)/dos_catalog_test: a2_dos_catalog.c a2_dos_catalog.h tests/dos_catalog_test.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) a2_dos_catalog.c tests/dos_catalog_test.c -o $@

test-all: test test-flash test-overlay test-catalog

clean:
	rm -rf build
