#include "lb_core_edge_u.h"

#include <lb_type.h>

void ocall_state_store_alloc(void **store_new)
{
	*store_new = malloc(sizeof(state_entry_t));
}

void ocall_state_store_free(void *item)
{
	free(item);
}
