#include "enki/vector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct enki_vector {
    enki_allocator allocator;
    void** items;
    size_t len;
    size_t capacity;
};

static void* system_alloc(void* ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void* system_realloc(void* ctx, void* ptr, size_t size)
{
    (void)ctx;
    return realloc(ptr, size);
}

static void system_free(void* ctx, void* ptr)
{
    (void)ctx;
    free(ptr);
}

static bool allocator_is_valid(enki_allocator allocator)
{
    return allocator.alloc != NULL && allocator.free != NULL;
}

static bool item_bytes(size_t capacity, size_t* bytes)
{
    if (capacity > (SIZE_MAX / sizeof(void*))) {
        return false;
    }

    *bytes = capacity * sizeof(void*);
    return true;
}

static enki_status resize_storage(enki_vector* vector, size_t capacity)
{
    size_t bytes = 0;

    if (!item_bytes(capacity, &bytes)) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->allocator.realloc != NULL) {
        void* resized =
            vector->allocator.realloc(vector->allocator.ctx, (void*)vector->items, bytes);
        if (resized == NULL) {
            return ENKI_ERR_ALLOC;
        }

        vector->items = (void**)resized;
        vector->capacity = capacity;
        return ENKI_OK;
    }

    void** next = (void**)vector->allocator.alloc(vector->allocator.ctx, bytes);
    if (next == NULL) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->items != NULL && vector->len > 0) {
        memcpy((void*)next, (const void*)vector->items, vector->len * sizeof(void*));
    }

    vector->allocator.free(vector->allocator.ctx, (void*)vector->items);
    vector->items = next;
    vector->capacity = capacity;
    return ENKI_OK;
}

enki_allocator enki_allocator_system(void)
{
    return (enki_allocator){
        .ctx = NULL,
        .alloc = system_alloc,
        .realloc = system_realloc,
        .free = system_free,
    };
}

enki_vector* enki_vector_create(enki_allocator allocator)
{
    if (!allocator_is_valid(allocator)) {
        return NULL;
    }

    enki_vector* vector = allocator.alloc(allocator.ctx, sizeof(*vector));
    if (vector == NULL) {
        return NULL;
    }

    vector->allocator = allocator;
    vector->items = NULL;
    vector->len = 0;
    vector->capacity = 0;

    return vector;
}

void enki_vector_destroy(enki_vector* vector)
{
    if (vector == NULL) {
        return;
    }

    enki_allocator allocator = vector->allocator;
    allocator.free(allocator.ctx, (void*)vector->items);
    allocator.free(allocator.ctx, vector);
}

enki_status enki_vector_push(enki_vector* vector, void* item)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (vector->len == vector->capacity) {
        size_t next_capacity = 4;
        if (vector->capacity > 0) {
            if (vector->capacity > (SIZE_MAX / 2)) {
                return ENKI_ERR_ALLOC;
            }
            next_capacity = vector->capacity * 2;
        }

        enki_status status = enki_vector_reserve(vector, next_capacity);
        if (status != ENKI_OK) {
            return status;
        }
    }

    vector->items[vector->len] = item;
    vector->len += 1;
    return ENKI_OK;
}

void* enki_vector_pop(enki_vector* vector)
{
    if (vector == NULL || vector->len == 0) {
        return NULL;
    }

    vector->len -= 1;
    void* item = vector->items[vector->len];
    vector->items[vector->len] = NULL;
    return item;
}

void* enki_vector_get(const enki_vector* vector, size_t index)
{
    if (vector == NULL || index >= vector->len) {
        return NULL;
    }

    return vector->items[index];
}

enki_status enki_vector_set(enki_vector* vector, size_t index, void* item)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (index >= vector->len) {
        return ENKI_ERR_BOUNDS;
    }

    vector->items[index] = item;
    return ENKI_OK;
}

size_t enki_vector_len(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->len;
}

size_t enki_vector_capacity(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->capacity;
}

enki_status enki_vector_reserve(enki_vector* vector, size_t capacity)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (capacity <= vector->capacity) {
        return ENKI_OK;
    }

    return resize_storage(vector, capacity);
}

enki_status enki_vector_shrink(enki_vector* vector)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (vector->len == vector->capacity) {
        return ENKI_OK;
    }

    if (vector->len == 0) {
        vector->allocator.free(vector->allocator.ctx, (void*)vector->items);
        vector->items = NULL;
        vector->capacity = 0;
        return ENKI_OK;
    }

    return resize_storage(vector, vector->len);
}
