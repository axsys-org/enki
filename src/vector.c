#include "enki/vector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct enki_vector {
    enki_allocator allocator;
    uint8_t* data;
    size_t elem_size;
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

static bool item_bytes(size_t capacity, size_t elem_size, size_t* bytes)
{
    if (elem_size == 0 || capacity > (SIZE_MAX / elem_size)) {
        return false;
    }

    *bytes = capacity * elem_size;
    return true;
}

static enki_status resize_storage(enki_vector* vector, size_t capacity)
{
    size_t bytes = 0;

    if (!item_bytes(capacity, vector->elem_size, &bytes)) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->allocator.realloc != NULL) {
        void* resized =
            vector->allocator.realloc(vector->allocator.ctx, (void*)vector->data, bytes);
        if (resized == NULL) {
            return ENKI_ERR_ALLOC;
        }

        vector->data = (uint8_t*)resized;
        vector->capacity = capacity;
        return ENKI_OK;
    }

    uint8_t* next = (uint8_t*)vector->allocator.alloc(vector->allocator.ctx, bytes);
    if (next == NULL) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->data != NULL && vector->len > 0) {
        memcpy((void*)next, (const void*)vector->data, vector->len * vector->elem_size);
    }

    vector->allocator.free(vector->allocator.ctx, (void*)vector->data);
    vector->data = next;
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
    return enki_vector_create_sized(allocator, sizeof(void*));
}

enki_vector* enki_vector_create_sized(enki_allocator allocator, size_t elem_size)
{
    if (!allocator_is_valid(allocator)) {
        return NULL;
    }
    if (elem_size == 0) {
        return NULL;
    }

    enki_vector* vector = allocator.alloc(allocator.ctx, sizeof(*vector));
    if (vector == NULL) {
        return NULL;
    }

    vector->allocator = allocator;
    vector->data = NULL;
    vector->elem_size = elem_size;
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
    allocator.free(allocator.ctx, (void*)vector->data);
    allocator.free(allocator.ctx, vector);
}

static enki_status ensure_push_capacity(enki_vector* vector)
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

    return ENKI_OK;
}

enki_status enki_vector_push(enki_vector* vector, void* item)
{
    if (vector == NULL || vector->elem_size != sizeof(void*)) {
        return ENKI_ERR_INVALID;
    }

    enki_status status = ensure_push_capacity(vector);
    if (status != ENKI_OK) {
        return status;
    }

    memcpy((void*)(vector->data + vector->len * vector->elem_size), &item, sizeof(item));
    vector->len += 1;
    return ENKI_OK;
}

enki_status enki_vector_push_copy(enki_vector* vector, const void* item)
{
    if (item == NULL) {
        return ENKI_ERR_INVALID;
    }

    enki_status status = ensure_push_capacity(vector);
    if (status != ENKI_OK) {
        return status;
    }

    memcpy((void*)(vector->data + vector->len * vector->elem_size), item, vector->elem_size);
    vector->len += 1;
    return ENKI_OK;
}

enki_status enki_vector_push_u8(enki_vector* vector, uint8_t item)
{
    if (vector == NULL || vector->elem_size != sizeof(uint8_t)) {
        return ENKI_ERR_INVALID;
    }

    return enki_vector_push_copy(vector, &item);
}

static void* slot_at(const enki_vector* vector, size_t index)
{
    return (void*)(vector->data + index * vector->elem_size);
}

void* enki_vector_pop(enki_vector* vector)
{
    if (vector == NULL || vector->len == 0 || vector->elem_size != sizeof(void*)) {
        return NULL;
    }

    vector->len -= 1;
    void* item = NULL;
    memcpy(&item, slot_at(vector, vector->len), sizeof(item));
    memset(slot_at(vector, vector->len), 0, sizeof(item));
    return item;
}

void* enki_vector_get(const enki_vector* vector, size_t index)
{
    if (vector == NULL || index >= vector->len || vector->elem_size != sizeof(void*)) {
        return NULL;
    }

    void* item = NULL;
    memcpy(&item, slot_at(vector, index), sizeof(item));
    return item;
}

void* enki_vector_get_slot(const enki_vector* vector, size_t index)
{
    if (vector == NULL || index >= vector->len) {
        return NULL;
    }

    return slot_at(vector, index);
}

enki_status enki_vector_set(enki_vector* vector, size_t index, void* item)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (index >= vector->len) {
        return ENKI_ERR_BOUNDS;
    }

    if (vector->elem_size != sizeof(void*)) {
        return ENKI_ERR_INVALID;
    }

    memcpy(slot_at(vector, index), &item, sizeof(item));
    return ENKI_OK;
}

enki_status enki_vector_set_copy(enki_vector* vector, size_t index, const void* item)
{
    if (vector == NULL || item == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (index >= vector->len) {
        return ENKI_ERR_BOUNDS;
    }

    memcpy(slot_at(vector, index), item, vector->elem_size);
    return ENKI_OK;
}

void* enki_vector_data(const enki_vector* vector)
{
    if (vector == NULL) {
        return NULL;
    }

    return vector->data;
}

size_t enki_vector_elem_size(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->elem_size;
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
        vector->allocator.free(vector->allocator.ctx, (void*)vector->data);
        vector->data = NULL;
        vector->capacity = 0;
        return ENKI_OK;
    }

    return resize_storage(vector, vector->len);
}
