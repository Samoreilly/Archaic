#pragma once
#include "threadmanager.h"
#include "trie-storage.h"

void parallel_scanner_init(parallel_scanner* scanner, t_bucket_store* store, struct node* parent, int max_depth, int num_threads);
void parallel_scanner_start(parallel_scanner* scanner, const char* root_path);
void parallel_scanner_wait(parallel_scanner* scanner);
void parallel_scanner_stop(parallel_scanner* scanner);
void scan_queue_init(scan_queue* q);
int scan_queue_push(scan_queue* q, const char* path, int depth);
int scan_queue_pop(scan_queue* q, char* path_out, int* depth_out, size_t path_cap);
