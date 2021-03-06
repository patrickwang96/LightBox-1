#ifndef LB_TYPE_H
#define LB_TYPE_H

#include "lb_config.h"

#include "../src/lb/core/enclave/cuckoo/cuckoo_hash.h"

#include <stdint.h>
#include <time.h>
#include <stdint.h>

#define TIME_ELAPSED_IN_US(s_sec, s_ns, e_sec, e_ns) \
	((e_sec - s_sec) * 1000000.0 + (e_ns - s_ns) / 1000.0)
#define TIME_ELAPSED_IN_NS(s_sec, s_ns, e_sec, e_ns) \
	((e_sec - s_sec) * 1000000000.0 + (e_ns - s_ns))

typedef struct flow_state {
	char raw[FLOW_STATE_SIZE];
} flow_state_t;

typedef struct cuckoo_hash_item lkup_entry_t;

// To be refactored: 
// 1) now shared by flow cache and store for quick prototyping
// 2) some testing field should be removed
// 3) some fields should be moved to lkup_entry
typedef struct state_entry {
	flow_state_t state;

	// for single lkup
	int within_enclave;

	// for testing
	uint32_t idx;

	// for LRU
	struct state_entry *prev;
	struct state_entry *next;

	// 128-bit AES-GCM mac, should be stored outside
	char mac[16];

	lkup_entry_t *lkup;

	/* Ephemeral for connection tracking */
	int is_client;

	/* These two variables collectivley make expiration decision */
	time_t last_access_time;
	int to_end;  // controlled by MB specific logic

	// count
	// int swap_count;
} state_entry_t;

#endif
