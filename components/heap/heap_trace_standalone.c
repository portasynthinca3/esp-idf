/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <sdkconfig.h>
#include <inttypes.h>

#define HEAP_TRACE_SRCFILE /* don't warn on inclusion here */
#include "esp_heap_trace.h"
#undef HEAP_TRACE_SRCFILE

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"
#include "sys/queue.h"


#define STACK_DEPTH CONFIG_HEAP_TRACING_STACK_DEPTH

#if CONFIG_HEAP_TRACING_STANDALONE

static portMUX_TYPE trace_mux = portMUX_INITIALIZER_UNLOCKED;
static bool tracing;
static heap_trace_mode_t mode;

/* Define struct: linked list of records */
TAILQ_HEAD(heap_trace_record_list_struct_t, heap_trace_record_t);
typedef struct heap_trace_record_list_struct_t heap_trace_record_list_t;

/* Linked List of Records */
typedef struct {

    /* Buffer used for records. */
    heap_trace_record_t *buffer;

    /* Linked list of recorded allocations */
    heap_trace_record_list_t list;

    /* Linked list of available record objects */
    heap_trace_record_list_t unused;

    /* capacity of 'buffer' */
    size_t capacity;

    /* Count of entries in 'list' */
    size_t count;

    /* During execution, we remember the maximum
       value of 'count'. This can help you
       choose the right size for your buffer capacity.*/
    size_t high_water_mark;

    /* Has the buffer overflowed and lost trace entries? */
    bool has_overflowed;
} records_t;

// Forward Defines
static void heap_trace_dump_base(bool internal_ram, bool psram);
static void record_deep_copy(heap_trace_record_t *r_dest, const heap_trace_record_t *r_src);
static void list_setup(void);
static void list_remove(heap_trace_record_t *r_remove);
static heap_trace_record_t* list_add(const heap_trace_record_t *r_append);
static heap_trace_record_t* list_pop_unused(void);
static heap_trace_record_t* list_find_address_reverse(void *p);

/* The actual records. */
static records_t records;

/* Actual number of allocations logged */
static size_t total_allocations;

/* Actual number of frees logged */
static size_t total_frees;

/* Used to speed up heap_trace_get */
static heap_trace_record_t* r_get;
static size_t r_get_idx;

#if CONFIG_HEAP_TRACE_HASH_MAP

typedef struct heap_trace_hashmap_entry_t {
    heap_trace_record_t* record; // associated record
} heap_trace_hashmap_entry_t;

static heap_trace_hashmap_entry_t hash_map[(size_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE]; // Buffer used for hashmap entries
static size_t total_hashmap_hits;
static size_t total_hashmap_miss;

static size_t hash_idx(void* p)
{
    static const uint32_t fnv_prime = 16777619UL; // expression 224 + 28 + 0x93 (32 bits size)
    return ((uint32_t)p * fnv_prime) % (uint32_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE;
}

static void map_add(const heap_trace_record_t *r_add)
{
    size_t idx = hash_idx(r_add->address);

    // linear search: find empty location
    for(size_t i = 0; i < CONFIG_HEAP_TRACE_HASH_MAP_MAX_LINEAR; i++) {
        size_t n = (i + idx) % (size_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE;
        if (hash_map[n].record == NULL) {
            hash_map[n].record = (heap_trace_record_t*) r_add;
            break;
        }
    }
}

static void map_remove(void *p)
{
    size_t idx = hash_idx(p);

    // linear search: find matching address
    for(size_t i = 0; i < CONFIG_HEAP_TRACE_HASH_MAP_MAX_LINEAR; i++) {
        size_t n = (i + idx) % (size_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE;
        if (hash_map[n].record != NULL && hash_map[n].record->address == p) {
            hash_map[n].record = NULL;
            break;
        }
    }
}

static heap_trace_record_t* map_find(void *p)
{
    size_t idx = hash_idx(p);

    // linear search: find matching address
    for(size_t i = 0; i < CONFIG_HEAP_TRACE_HASH_MAP_MAX_LINEAR; i++) {
        size_t n = (i + idx) % (size_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE;
        if (hash_map[n].record != NULL && hash_map[n].record->address == p) {
            total_hashmap_hits++;
            return hash_map[n].record;
        }
    }

    total_hashmap_miss++;
    return NULL;
}
#endif // CONFIG_HEAP_TRACE_HASH_MAP

esp_err_t heap_trace_init_standalone(heap_trace_record_t *record_buffer, size_t num_records)
{
    if (tracing) {
        return ESP_ERR_INVALID_STATE;
    }

    if (record_buffer == NULL || num_records == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    records.buffer = record_buffer;
    records.capacity = num_records;

    return ESP_OK;
}

static esp_err_t set_tracing(bool enable)
{
    if (tracing == enable) {
        return ESP_ERR_INVALID_STATE;
    }
    tracing = enable;
    return ESP_OK;
}

esp_err_t heap_trace_start(heap_trace_mode_t mode_param)
{
    if (records.buffer == NULL || records.capacity == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&trace_mux);

    set_tracing(false);
    mode = mode_param;

    // clear buffers
    memset(records.buffer, 0, sizeof(heap_trace_record_t) * records.capacity);

#if CONFIG_HEAP_TRACE_HASH_MAP
    memset(hash_map, 0, sizeof(hash_map));
    total_hashmap_hits = 0;
    total_hashmap_miss = 0;
#endif // CONFIG_HEAP_TRACE_HASH_MAP

    records.count = 0;
    records.has_overflowed = false;
    list_setup();

    total_allocations = 0;
    total_frees = 0;

    const esp_err_t ret_val = set_tracing(true);

    portEXIT_CRITICAL(&trace_mux);
    return ret_val;
}

esp_err_t heap_trace_stop(void)
{
    portENTER_CRITICAL(&trace_mux);
    const esp_err_t ret_val = set_tracing(false);
    portEXIT_CRITICAL(&trace_mux);
    return ret_val;
}

esp_err_t heap_trace_resume(void)
{
    portENTER_CRITICAL(&trace_mux);
    const esp_err_t ret_val = set_tracing(true);
    portEXIT_CRITICAL(&trace_mux);
    return ret_val;
}

size_t heap_trace_get_count(void)
{
    return records.count;
}

esp_err_t heap_trace_get(size_t index, heap_trace_record_t *r_out)
{
    if (r_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;

    portENTER_CRITICAL(&trace_mux);

    if (index >= records.count) {

        result = ESP_ERR_INVALID_ARG; /* out of range for 'count' */

    } else {

        // Perf: speed up sequential access
        if (r_get && r_get_idx == index - 1) {

            r_get = TAILQ_NEXT(r_get, tailq);
            r_get_idx = index;

        } else {

            // Iterate through through the linked list

            r_get = TAILQ_FIRST(&records.list);

            for (int i = 0; i < index; i++) {

                if (r_get == NULL) {
                    break;
                }

                r_get = TAILQ_NEXT(r_get, tailq);
                r_get_idx = i + 1;
            }
        }

        // We already checked that index < records.count,
        // This could be indicative of memory corruption.
        assert(r_get != NULL);
        memcpy(r_out, r_get, sizeof(heap_trace_record_t));
    }

    portEXIT_CRITICAL(&trace_mux);
    return result;
}

esp_err_t heap_trace_summary(heap_trace_summary_t *summary)
{
    if (summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&trace_mux);
    summary->mode = mode;
    summary->total_allocations = total_allocations;
    summary->total_frees = total_frees;
    summary->count = records.count;
    summary->capacity = records.capacity;
    summary->high_water_mark = records.high_water_mark;
    summary->has_overflowed = records.has_overflowed;
#if CONFIG_HEAP_TRACE_HASH_MAP
    summary->total_hashmap_hits = total_hashmap_hits;
    summary->total_hashmap_miss = total_hashmap_miss;
#endif // CONFIG_HEAP_TRACE_HASH_MAP
    portEXIT_CRITICAL(&trace_mux);

    return ESP_OK;
}

void heap_trace_dump(void) {
    heap_trace_dump_caps(MALLOC_CAP_INTERNAL | MALLOC_CAP_SPIRAM);
}

void heap_trace_dump_caps(const uint32_t caps) {
    heap_trace_dump_base(caps & MALLOC_CAP_INTERNAL, caps & MALLOC_CAP_SPIRAM);
}

static void heap_trace_dump_base(bool internal_ram, bool psram)
{
    portENTER_CRITICAL(&trace_mux);

    size_t delta_size = 0;
    size_t delta_allocs = 0;
    size_t start_count = records.count;

    esp_rom_printf("====== Heap Trace: %"PRIu32" records (%"PRIu32" capacity) ======\n",
        records.count, records.capacity);

    // Iterate through through the linked list

    heap_trace_record_t *r_cur = TAILQ_FIRST(&records.list);

    for (int i = 0; i < records.count; i++) {

        // check corruption
        if (r_cur == NULL) {
            esp_rom_printf("\nError: heap trace linked list is corrupt. expected more records.\n");
            break;
        }

        bool should_print = r_cur->address != NULL &&
            ((psram && internal_ram) ||
             (internal_ram && esp_ptr_internal(r_cur->address)) ||
             (psram && esp_ptr_external_ram(r_cur->address)));

        if (should_print) {

            const char* label = "";
            if (esp_ptr_internal(r_cur->address)) {
                label = ", Internal";
            }
            if (esp_ptr_external_ram(r_cur->address)) {
                label = ",    PSRAM";
            }

            esp_rom_printf("%6d bytes (@ %p%s) allocated CPU %d ccount 0x%08x caller ",
                   r_cur->size, r_cur->address, label, r_cur->ccount & 1, r_cur->ccount & ~3);

            for (int j = 0; j < STACK_DEPTH && r_cur->alloced_by[j] != 0; j++) {
                esp_rom_printf("%p%s", r_cur->alloced_by[j],
                       (j < STACK_DEPTH - 1) ? ":" : "");
            }

            if (mode != HEAP_TRACE_ALL || STACK_DEPTH == 0 || r_cur->freed_by[0] == NULL) {
                delta_size += r_cur->size;
                delta_allocs++;
                esp_rom_printf("\n");
            } else {
                esp_rom_printf("\nfreed by ");
                for (int j = 0; j < STACK_DEPTH; j++) {
                    esp_rom_printf("%p%s", r_cur->freed_by[j],
                           (j < STACK_DEPTH - 1) ? ":" : "\n");
                }
            }
        }

        r_cur = TAILQ_NEXT(r_cur, tailq);
    }

    esp_rom_printf("====== Heap Trace Summary ======\n");

    if (mode == HEAP_TRACE_ALL) {
        esp_rom_printf("Mode: Heap Trace All\n");
        esp_rom_printf("%"PRIu32" bytes alive in trace (%"PRIu32"/%"PRIu32" allocations)\n",
               delta_size, delta_allocs, heap_trace_get_count());
    } else {
        esp_rom_printf("Mode: Heap Trace Leaks\n");
        esp_rom_printf("%"PRIu32" bytes 'leaked' in trace (%"PRIu32" allocations)\n", delta_size, delta_allocs);
    }

    esp_rom_printf("records: %"PRIu32" (%"PRIu32" capacity, %"PRIu32" high water mark)\n",
        records.count, records.capacity, records.high_water_mark);

#if CONFIG_HEAP_TRACE_HASH_MAP
    esp_rom_printf("hashmap: %"PRIu32" capacity (%"PRIu32" hits, %"PRIu32" misses)\n",
        (size_t)CONFIG_HEAP_TRACE_HASH_MAP_SIZE, total_hashmap_hits, total_hashmap_miss);
#endif // CONFIG_HEAP_TRACE_HASH_MAP

    esp_rom_printf("total allocations: %"PRIu32"\n", total_allocations);
    esp_rom_printf("total frees: %"PRIu32"\n", total_frees);

    if (start_count != records.count) { // only a problem if trace isn't stopped before dumping
        esp_rom_printf("(NB: New entries were traced while dumping, so trace dump may have duplicate entries.)\n");
    }
    if (records.has_overflowed) {
        esp_rom_printf("(NB: Internal Buffer has overflowed, so trace data is incomplete.)\n");
    }
    esp_rom_printf("================================\n");

    portEXIT_CRITICAL(&trace_mux);
}

/* Add a new allocation to the heap trace records */
static IRAM_ATTR void record_allocation(const heap_trace_record_t *r_allocation)
{
    if (!tracing || r_allocation->address == NULL) {
        return;
    }

    portENTER_CRITICAL(&trace_mux);

    if (tracing) {

        // If buffer is full, pop off the oldest
        // record to make more space
        if (records.count == records.capacity) {

            records.has_overflowed = true;

            heap_trace_record_t *r_first = TAILQ_FIRST(&records.list);

            list_remove(r_first);
#if CONFIG_HEAP_TRACE_HASH_MAP
            map_remove(r_first->address);
        }
        // push onto end of list
        heap_trace_record_t *r_dest = list_add(r_allocation);
        // add to hashmap
        if (r_dest) {
            map_add(r_dest);
        }
#else
        }
        // push onto end of list
        list_add(r_allocation);
#endif // CONFIG_HEAP_TRACE_HASH_MAP

        total_allocations++;
    }

    portEXIT_CRITICAL(&trace_mux);
}

/* record a free event in the heap trace log

   For HEAP_TRACE_ALL, this means filling in the freed_by pointer.
   For HEAP_TRACE_LEAKS, this means removing the record from the log.

   callers is an array of  STACK_DEPTH function pointer from the call stack
   leading to the call of record_free.
*/
static IRAM_ATTR void record_free(void *p, void **callers)
{
    if (!tracing || p == NULL) {
        return;
    }

    portENTER_CRITICAL(&trace_mux);
    // return directly if records.count == 0. In case of hashmap being used
    // this prevents the hashmap to return an item that is no longer in the
    // records list.
    if (records.count == 0) {
        portEXIT_CRITICAL(&trace_mux);
        return;
    }

    if (tracing) {

        total_frees++;

#if CONFIG_HEAP_TRACE_HASH_MAP
        // check the hashmap
        heap_trace_record_t *r_found = map_find(p);

        // list search
        if(!r_found) {
            r_found = list_find_address_reverse(p);
        }
#else
        heap_trace_record_t *r_found = list_find_address_reverse(p);
#endif // CONFIG_HEAP_TRACE_HASH_MAP
        // search backwards for the allocation record matching this fre

        if (r_found) {
            if (mode == HEAP_TRACE_ALL) {

                // add 'freed_by' info to the record
                memcpy(r_found->freed_by, callers, sizeof(void *) * STACK_DEPTH);

            } else { // HEAP_TRACE_LEAKS

                // Leak trace mode, once an allocation is freed
                // we remove it from the list & hashmap
                list_remove(r_found);
#if CONFIG_HEAP_TRACE_HASH_MAP
                map_remove(p);
#endif // CONFIG_HEAP_TRACE_HASH_MAP
            }
        }
    }

    portEXIT_CRITICAL(&trace_mux);
}

// connect all records into a linked list of 'unused' records
static void list_setup(void)
{
    TAILQ_INIT(&records.list);
    TAILQ_INIT(&records.unused);

    for (int i = 0; i < records.capacity; i++) {

        heap_trace_record_t *r_cur = &records.buffer[i];

        TAILQ_INSERT_TAIL(&records.unused, r_cur, tailq);
    }
}

/* 1. removes record r_remove from records.list,
   2. places it into records.unused */
static IRAM_ATTR void list_remove(heap_trace_record_t *r_remove)
{
    assert(records.count > 0);

    // remove from records.list
    TAILQ_REMOVE(&records.list, r_remove, tailq);

    // set as unused
    r_remove->address = 0;
    r_remove->size = 0;

    // add to records.unused
    TAILQ_INSERT_HEAD(&records.unused, r_remove, tailq);

    // decrement
    records.count--;
}


// pop record from unused list
static IRAM_ATTR heap_trace_record_t* list_pop_unused(void)
{
    // no records left?
    if (records.count >= records.capacity) {
        return NULL;
    }

    // get from records.unused
    heap_trace_record_t *r_unused = TAILQ_FIRST(&records.unused);
    assert(r_unused->address == NULL);
    assert(r_unused->size == 0);

    // remove from records.unused
    TAILQ_REMOVE(&records.unused, r_unused, tailq);

    return r_unused;
}

// deep copy a record.
// Note: only copies the *allocation data*, not the next & prev ptrs
static IRAM_ATTR void record_deep_copy(heap_trace_record_t *r_dest, const heap_trace_record_t *r_src)
{
    r_dest->ccount  = r_src->ccount;
    r_dest->address = r_src->address;
    r_dest->size    = r_src->size;
    memcpy(r_dest->freed_by,   r_src->freed_by,   sizeof(void *) * STACK_DEPTH);
    memcpy(r_dest->alloced_by, r_src->alloced_by, sizeof(void *) * STACK_DEPTH);
}

// Append a record to records.list
// Note: This deep copies r_append
static IRAM_ATTR heap_trace_record_t* list_add(const heap_trace_record_t *r_append)
{
    if (records.count < records.capacity) {

        // get unused record
        heap_trace_record_t *r_dest = list_pop_unused();

        // we checked that there is capacity, so this
        // should never be null.
        assert(r_dest != NULL);

        // copy allocation data
        record_deep_copy(r_dest, r_append);

        // append to records.list
        TAILQ_INSERT_TAIL(&records.list, r_dest, tailq);

        // increment
        records.count++;

        // high water mark
        if (records.count > records.high_water_mark) {
            records.high_water_mark = records.count;
        }

        return r_dest;

    } else {
        records.has_overflowed = true;
        return NULL;
    }
}

// search records.list backwards for the allocation record matching this address
static IRAM_ATTR heap_trace_record_t* list_find_address_reverse(void *p)
{
    heap_trace_record_t *r_found = NULL;

    // Perf: We search backwards because new allocations are appended
    // to the end of the list and most allocations are short lived.
    heap_trace_record_t *r_cur = NULL;
    TAILQ_FOREACH_REVERSE(r_cur, &records.list, heap_trace_record_list_struct_t, tailq) {
        if (r_cur->address == p) {
            r_found = r_cur;
            break;
        }
    }

    return r_found;
}

#include "heap_trace.inc"

#endif // CONFIG_HEAP_TRACING_STANDALONE
