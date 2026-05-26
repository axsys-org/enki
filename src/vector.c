#include "enki/vector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "enki/interp.h"

struct enki_vector {
    enki_allocator allocator_a;
    uint8_t* data_b;
    size_t elem_size_s;
    size_t len_s;
    size_t capacity_s;
};

static void* system_alloc(void* ctx, size_t size_s)
{
    (void)ctx;
    return malloc(size_s);
}

static void* system_realloc(void* ctx, void* ptr, size_t size_s)
{
    (void)ctx;
    return realloc(ptr, size_s);
}

static void system_free(void* ctx, void* ptr)
{
    (void)ctx;
    free(ptr);
}

static bool allocator_is_valid(const enki_allocator* allocator_a)
{
    return allocator_a != NULL && allocator_a->alloc != NULL && allocator_a->free != NULL;
}

static bool item_bytes(size_t capacity_s, size_t elem_size_s, size_t* bytes_s)
{
    if (elem_size_s == 0 || capacity_s > (SIZE_MAX / elem_size_s)) {
        return false;
    }

    *bytes_s = capacity_s * elem_size_s;
    return true;
}

static enki_status resize_storage(enki_vector* vector, size_t capacity_s)
{
    size_t bytes_s = 0;

    if (!item_bytes(capacity_s, vector->elem_size_s, &bytes_s)) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->allocator_a.realloc != NULL) {
        void* resized =
            vector->allocator_a.realloc(vector->allocator_a.ctx, (void*)vector->data_b, bytes_s);
        if (resized == NULL) {
            return ENKI_ERR_ALLOC;
        }

        vector->data_b = (uint8_t*)resized;
        vector->capacity_s = capacity_s;
        return ENKI_OK;
    }

    uint8_t* next = (uint8_t*)vector->allocator_a.alloc(vector->allocator_a.ctx, bytes_s);
    if (next == NULL) {
        return ENKI_ERR_ALLOC;
    }

    if (vector->data_b != NULL && vector->len_s > 0) {
        memcpy((void*)next, (const void*)vector->data_b, vector->len_s * vector->elem_size_s);
    }

    vector->allocator_a.free(vector->allocator_a.ctx, (void*)vector->data_b);
    vector->data_b = next;
    vector->capacity_s = capacity_s;
    return ENKI_OK;
}

const enki_allocator* enki_allocator_system(void)
{
    static const enki_allocator system_a = {
        .ctx = NULL,
        .alloc = system_alloc,
        .realloc = system_realloc,
        .free = system_free,
    };
    return &system_a;
}

enki_vector* enki_vector_create(const enki_allocator* allocator_a)
{
    return enki_vector_create_sized(allocator_a, sizeof(void*));
}

enki_vector* enki_vector_create_sized(const enki_allocator* allocator_a, size_t elem_size_s)
{
    if (!allocator_is_valid(allocator_a)) {
        return NULL;
    }
    if (elem_size_s == 0) {
        return NULL;
    }

    enki_vector* vector = allocator_a->alloc(allocator_a->ctx, sizeof(*vector));
    if (vector == NULL) {
        return NULL;
    }

    vector->allocator_a = *allocator_a;
    vector->data_b = NULL;
    vector->elem_size_s = elem_size_s;
    vector->len_s = 0;
    vector->capacity_s = 0;

    return vector;
}

static void vector_throw_status(enki_interpreter* i, enki_status status)
{
    switch(status) {
        case ENKI_ERR_ALLOC:
            enki_interp_throw(i, ENKI_ERROR_OOM, 0);
        case ENKI_ERR_BOUNDS:
            enki_interp_throw(i, ENKI_ERROR_BOUNDS, 0);
        case ENKI_ERR_INVALID:
            enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
        case ENKI_OK:
            return;
    }
    enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
}

enki_vector* enki_vector_create_sized_or_throw(enki_interpreter* i, const enki_allocator* allocator_a, size_t elem_size_s)
{
    enki_vector* vector = enki_vector_create_sized(allocator_a, elem_size_s);
    if(vector == NULL) {
        enki_interp_throw(i, ENKI_ERROR_OOM, 0);
    }
    return vector;
}

void enki_vector_push_u8_or_throw(enki_interpreter* i, enki_vector* vector, uint8_t item_v)
{
    vector_throw_status(i, enki_vector_push_u8(vector, item_v));
}

void enki_vector_push_copy_or_throw(enki_interpreter* i, enki_vector* vector, const void* item_v)
{
    vector_throw_status(i, enki_vector_push_copy(vector, item_v));
}

void enki_vector_destroy(enki_vector* vector)
{
    if (vector == NULL) {
        return;
    }

    enki_allocator allocator_a = vector->allocator_a;
    allocator_a.free(allocator_a.ctx, (void*)vector->data_b);
    allocator_a.free(allocator_a.ctx, vector);
}

static enki_status ensure_push_capacity(enki_vector* vector)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (vector->len_s == vector->capacity_s) {
        size_t next_capacity_s = 4;
        if (vector->capacity_s > 0) {
            if (vector->capacity_s > (SIZE_MAX / 2)) {
                return ENKI_ERR_ALLOC;
            }
            next_capacity_s = vector->capacity_s * 2;
        }

        enki_status status = enki_vector_reserve(vector, next_capacity_s);
        if (status != ENKI_OK) {
            return status;
        }
    }

    return ENKI_OK;
}

enki_status enki_vector_push(enki_vector* vector, void* item_v)
{
    if (vector == NULL || vector->elem_size_s != sizeof(void*)) {
        return ENKI_ERR_INVALID;
    }

    enki_status status = ensure_push_capacity(vector);
    if (status != ENKI_OK) {
        return status;
    }

    memcpy((void*)(vector->data_b + vector->len_s * vector->elem_size_s), &item_v, sizeof(item_v));
    vector->len_s += 1;
    return ENKI_OK;
}

enki_status enki_vector_push_copy(enki_vector* vector, const void* item_v)
{
    if (item_v == NULL) {
        return ENKI_ERR_INVALID;
    }

    enki_status status = ensure_push_capacity(vector);
    if (status != ENKI_OK) {
        return status;
    }

    memcpy((void*)(vector->data_b + vector->len_s * vector->elem_size_s), item_v, vector->elem_size_s);
    vector->len_s += 1;
    return ENKI_OK;
}

enki_status enki_vector_push_u8(enki_vector* vector, uint8_t item_v)
{
    if (vector == NULL || vector->elem_size_s != sizeof(uint8_t)) {
        return ENKI_ERR_INVALID;
    }

    return enki_vector_push_copy(vector, &item_v);
}

static void* slot_at(const enki_vector* vector, size_t index_i)
{
    return (void*)(vector->data_b + index_i * vector->elem_size_s);
}

void* enki_vector_pop(enki_vector* vector)
{
    if (vector == NULL || vector->len_s == 0 || vector->elem_size_s != sizeof(void*)) {
        return NULL;
    }

    vector->len_s -= 1;
    void* item_v = NULL;
    memcpy(&item_v, slot_at(vector, vector->len_s), sizeof(item_v));
    memset(slot_at(vector, vector->len_s), 0, sizeof(item_v));
    return item_v;
}

void* enki_vector_get(const enki_vector* vector, size_t index_i)
{
    if (vector == NULL || index_i >= vector->len_s || vector->elem_size_s != sizeof(void*)) {
        return NULL;
    }

    void* item_v = NULL;
    memcpy(&item_v, slot_at(vector, index_i), sizeof(item_v));
    return item_v;
}

void* enki_vector_get_slot(const enki_vector* vector, size_t index_i)
{
    if (vector == NULL || index_i >= vector->len_s) {
        return NULL;
    }

    return slot_at(vector, index_i);
}

enki_status enki_vector_set(enki_vector* vector, size_t index_i, void* item_v)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (index_i >= vector->len_s) {
        return ENKI_ERR_BOUNDS;
    }

    if (vector->elem_size_s != sizeof(void*)) {
        return ENKI_ERR_INVALID;
    }

    memcpy(slot_at(vector, index_i), &item_v, sizeof(item_v));
    return ENKI_OK;
}

enki_status enki_vector_set_copy(enki_vector* vector, size_t index_i, const void* item_v)
{
    if (vector == NULL || item_v == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (index_i >= vector->len_s) {
        return ENKI_ERR_BOUNDS;
    }

    memcpy(slot_at(vector, index_i), item_v, vector->elem_size_s);
    return ENKI_OK;
}

void* enki_vector_data(const enki_vector* vector)
{
    if (vector == NULL) {
        return NULL;
    }

    return vector->data_b;
}

size_t enki_vector_elem_size(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->elem_size_s;
}

size_t enki_vector_len(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->len_s;
}

size_t enki_vector_capacity(const enki_vector* vector)
{
    if (vector == NULL) {
        return 0;
    }

    return vector->capacity_s;
}

enki_status enki_vector_reserve(enki_vector* vector, size_t capacity_s)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (capacity_s <= vector->capacity_s) {
        return ENKI_OK;
    }

    return resize_storage(vector, capacity_s);
}

enki_status enki_vector_shrink(enki_vector* vector)
{
    if (vector == NULL) {
        return ENKI_ERR_INVALID;
    }

    if (vector->len_s == vector->capacity_s) {
        return ENKI_OK;
    }

    if (vector->len_s == 0) {
        vector->allocator_a.free(vector->allocator_a.ctx, (void*)vector->data_b);
        vector->data_b = NULL;
        vector->capacity_s = 0;
        return ENKI_OK;
    }

    return resize_storage(vector, vector->len_s);
}
