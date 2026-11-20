#include "control_lease.h"

#include <stdint.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define US_PER_SECOND INT64_C(1000000)

static portMUX_TYPE lease_lock = portMUX_INITIALIZER_UNLOCKED;
static char lease_token[CONTROL_LEASE_TOKEN_LENGTH + 1];
static int64_t lease_deadline;
static bool mutation_in_flight;

static bool valid_ttl(unsigned ttl_s)
{
    return ttl_s >= CONTROL_LEASE_MIN_TTL_S && ttl_s <= CONTROL_LEASE_MAX_TTL_S;
}

static void clear_lease(void)
{
    lease_deadline = 0;
    memset(lease_token, 0, sizeof(lease_token));
}

static void expire_lease(int64_t now)
{
    if (lease_deadline && now >= lease_deadline && !mutation_in_flight) {
        clear_lease();
    }
}

static bool token_matches(const char *token, int64_t now)
{
    if (!lease_deadline || now >= lease_deadline || !token ||
        strnlen(token, sizeof(lease_token)) != CONTROL_LEASE_TOKEN_LENGTH) {
        return false;
    }
    // Check every character of a correctly sized candidate.
    unsigned difference = 0;
    for (unsigned i = 0; i < CONTROL_LEASE_TOKEN_LENGTH; ++i) {
        difference |= (unsigned char)token[i] ^ (unsigned char)lease_token[i];
    }
    return difference == 0;
}

control_lease_status_t control_lease_status(void)
{
    portENTER_CRITICAL(&lease_lock);
    const int64_t now = esp_timer_get_time();
    expire_lease(now);
    const int64_t remaining = lease_deadline - now;
    const control_lease_status_t status = {
        .active = lease_deadline != 0,
        .remaining_s = remaining > 0 ? (remaining + US_PER_SECOND - 1) / US_PER_SECOND : 0,
    };
    portEXIT_CRITICAL(&lease_lock);
    return status;
}

control_lease_result_t control_lease_acquire(unsigned ttl_s, char token[CONTROL_LEASE_TOKEN_LENGTH + 1])
{
    if (!valid_ttl(ttl_s) || !token) return CONTROL_LEASE_INVALID;

    // Wi-Fi is running when the HTTP control endpoint calls this.
    uint8_t random[CONTROL_LEASE_TOKEN_LENGTH / 2];
    char generated[CONTROL_LEASE_TOKEN_LENGTH + 1];
    static const char hex[] = "0123456789abcdef";
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); ++i) {
        generated[2 * i] = hex[random[i] >> 4];
        generated[2 * i + 1] = hex[random[i] & 0x0f];
    }
    generated[CONTROL_LEASE_TOKEN_LENGTH] = '\0';

    portENTER_CRITICAL(&lease_lock);
    const int64_t now = esp_timer_get_time();
    expire_lease(now);
    control_lease_result_t result = CONTROL_LEASE_BUSY;
    if (!lease_deadline && !mutation_in_flight) {
        memcpy(lease_token, generated, sizeof(lease_token));
        memcpy(token, generated, sizeof(lease_token));
        lease_deadline = now + ttl_s * US_PER_SECOND;
        result = CONTROL_LEASE_OK;
    }
    portEXIT_CRITICAL(&lease_lock);
    return result;
}

control_lease_result_t control_lease_renew(const char *token, unsigned ttl_s)
{
    if (!valid_ttl(ttl_s)) return CONTROL_LEASE_INVALID;
    portENTER_CRITICAL(&lease_lock);
    const int64_t now = esp_timer_get_time();
    expire_lease(now);
    control_lease_result_t result = CONTROL_LEASE_DENIED;
    if (token_matches(token, now)) {
        lease_deadline = now + ttl_s * US_PER_SECOND;
        result = CONTROL_LEASE_OK;
    }
    portEXIT_CRITICAL(&lease_lock);
    return result;
}

control_lease_result_t control_lease_release(const char *token)
{
    portENTER_CRITICAL(&lease_lock);
    const int64_t now = esp_timer_get_time();
    expire_lease(now);
    control_lease_result_t result = CONTROL_LEASE_DENIED;
    if (token_matches(token, now)) {
        if (mutation_in_flight) {
            result = CONTROL_LEASE_BUSY;
        } else {
            clear_lease();
            result = CONTROL_LEASE_OK;
        }
    }
    portEXIT_CRITICAL(&lease_lock);
    return result;
}

control_lease_result_t control_lease_begin_mutation(const char *token)
{
    portENTER_CRITICAL(&lease_lock);
    const int64_t now = esp_timer_get_time();
    expire_lease(now);
    control_lease_result_t result = CONTROL_LEASE_DENIED;
    if (token ? token_matches(token, now) : !lease_deadline) {
        result = CONTROL_LEASE_BUSY;
        if (!mutation_in_flight) {
            mutation_in_flight = true;
            result = CONTROL_LEASE_OK;
        }
    }
    portEXIT_CRITICAL(&lease_lock);
    return result;
}

void control_lease_end_mutation(void)
{
    portENTER_CRITICAL(&lease_lock);
    mutation_in_flight = false;
    expire_lease(esp_timer_get_time());
    portEXIT_CRITICAL(&lease_lock);
}
