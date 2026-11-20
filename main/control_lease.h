#pragma once

#include <stdbool.h>

enum {
    CONTROL_LEASE_TOKEN_LENGTH = 32,
    CONTROL_LEASE_MIN_TTL_S = 30,
    CONTROL_LEASE_MAX_TTL_S = 1800,
};

typedef enum {
    CONTROL_LEASE_OK = 0,
    CONTROL_LEASE_BUSY,
    CONTROL_LEASE_INVALID,
    CONTROL_LEASE_DENIED,
} control_lease_result_t;

typedef struct {
    bool active;
    unsigned remaining_s;
} control_lease_status_t;

control_lease_status_t control_lease_status(void);
control_lease_result_t control_lease_acquire(unsigned ttl_s, char token[CONTROL_LEASE_TOKEN_LENGTH + 1]);
control_lease_result_t control_lease_renew(const char *token, unsigned ttl_s);
control_lease_result_t control_lease_release(const char *token);

// Pass NULL only when the caller omitted the token. Hold the permit until the
// operation has finished, then call end exactly once for each successful begin.
// An expired lease stays active through an admitted mutation, with zero time
// remaining. Expiration does not admit a competing writer during that operation.
control_lease_result_t control_lease_begin_mutation(const char *token);
void control_lease_end_mutation(void);
