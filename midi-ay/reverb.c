/* reverb.c */

#include <stdlib.h>

#include "reverb.h"

/* Macros for common operations */
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define CLAMP(x,min,max) MAX(min, MIN(max, x))

reverb_t *reverb_create(size_t max_delay_length)
{
    reverb_t *rev;

    rev = malloc(sizeof(*rev));
    if (rev == NULL)
        return NULL;

    rev->buffer = calloc(max_delay_length, sizeof(reverb_sample_t));
    if (rev->buffer == NULL) {
        free(rev);
        return NULL;
    }

    rev->max_size = max_delay_length;
    rev->size = max_delay_length;
    rev->pos  = 0;

    return rev;
}

void reverb_destroy(reverb_t *rev)
{
    if (rev == NULL)
        return;

    free(rev->buffer);
    free(rev);
}

reverb_sample_t reverb_process(reverb_t *rev, reverb_sample_t sample)
{
    int32_t         delayed;
    int32_t         mixed;
    reverb_sample_t output;

    if (rev == NULL || rev->size == 0)
        return sample;

    delayed = rev->buffer[rev->pos];
    mixed   = (int32_t) sample + (delayed >> 1);
    output  = CLAMP(mixed, INT16_MIN, INT16_MAX);

    rev->buffer[rev->pos] = sample;
    rev->pos = (rev->pos + 1) % rev->size;

    return output;
}

void reverb_set_delay(reverb_t *rev, size_t delay_length)
{
    if (rev == NULL || delay_length == 0)
        return;

    if (delay_length > rev->max_size)
        delay_length = rev->max_size;

    rev->size = delay_length;
    rev->pos = 0;
}

