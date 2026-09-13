#pragma once
#include "threadmanager.h"
#include "trie-storage.h"

void parallel_scanner_init(parallel_scanner* scanner, t_bucket_store* store, struct node* parent,
                           int max_depth, int num_threads);
void parallel_scanner_set_ignores(parallel_scanner* scanner, const char** dirs, int dir_count,
                                  const char** files, int file_count);
void parallel_scanner_start(parallel_scanner* scanner, const char* root_path);
void parallel_scanner_start_multi(parallel_scanner* scanner, const char** roots, int root_count);
/* Multi-root scan with per-root max depths (depth < 0 = scanner default).
 * Per-root ignore dirs are set beforehand via parallel_scanner_set_root_ignores. */
void parallel_scanner_start_multi_depth(parallel_scanner* scanner, const char** roots,
                                        const int* depths, int root_count);
void parallel_scanner_set_root_ignores(parallel_scanner* scanner, int root_idx,
                                       const char** dirs, int dir_count);
void parallel_scanner_wait(parallel_scanner* scanner);
void parallel_scanner_stop(parallel_scanner* scanner);
void parallel_scanner_set_dir_timeout(parallel_scanner* scanner, int timeout_ms);
int scan_queue_push(scan_queue* q, const char* path, int depth, int root_idx);
