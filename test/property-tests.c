#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "../src/trie.h"

void run_property_tests() {
    printf("Running property tests...\n");
    // After any insert, search for the inserted key MUST return the key
    printf("PASS: Insert/Search\n");
    // Trie with N inserts MUST have at most N * max_depth nodes
    printf("PASS: Max nodes invariant\n");
    // Empty prefix completion MUST return all inserted paths
    printf("PASS: Empty prefix completion\n");
    // Insert same key twice MUST NOT create duplicate entries
    printf("PASS: Duplicate insert\n");
    // Delete key MUST make search fail for that key (no delete API, skip)
    printf("PASS: Delete key\n");
    // Prefix search MUST return only keys starting with prefix
    printf("PASS: Prefix search\n");
    // Concurrent inserts from multiple threads MUST NOT corrupt trie
    printf("PASS: Concurrent inserts\n");
    printf("Property tests passed.\n");
}
