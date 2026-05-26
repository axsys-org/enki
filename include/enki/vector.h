#ifndef ENKI_VECTOR_H
#define ENKI_VECTOR_H

#include <stddef.h>
#include <stdint.h>

#include "enki/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Status code returned by mutating enki operations.
 */
typedef enum enki_status {
    /** Operation completed successfully. */
    ENKI_OK = 0,
    /** Operation failed because the allocator_a returned NULL or size_s overflowed. */
    ENKI_ERR_ALLOC = 1,
    /** Operation failed because an index_i was outside the vector length. */
    ENKI_ERR_BOUNDS = 2,
    /** Operation failed because an argument or allocator_a was invalid. */
    ENKI_ERR_INVALID = 3,
} enki_status;

/**
 * Opaque dynamic array of void pointers.
 */
typedef struct enki_vector enki_vector;

/**
 * Creates an empty vector using the supplied allocator_a.
 *
 * Returns NULL if the allocator_a is invalid or cannot allocate the vector
 * header.
 */
enki_vector* enki_vector_create(enki_allocator allocator_a);

/**
 * Creates an empty vector with fixed-size_s copied elements.
 *
 * The original enki_vector_create creates a pointer vector. Use this when the
 * backing storage needs to be a packed array, such as uint8_t bytecode or
 * enki_value constants.
 */
enki_vector* enki_vector_create_sized(enki_allocator allocator_a, size_t elem_size_s);

/**
 * Destroys a vector and releases all storage owned by the vector.
 *
 * Stored items are opaque borrowed pointers and are not freed.
 */
void enki_vector_destroy(enki_vector* vector);

/**
 * Appends an item_v to the end of the vector.
 */
enki_status enki_vector_push(enki_vector* vector, void* item_v);

/**
 * Appends one fixed-size_s element by copying elem_size_s bytes_s from item_v.
 */
enki_status enki_vector_push_copy(enki_vector* vector, const void* item_v);

/**
 * Appends one byte to a vector created with elem_size_s == sizeof(uint8_t).
 */
enki_status enki_vector_push_u8(enki_vector* vector, uint8_t item_v);

/**
 * Removes and returns the last item_v in the vector.
 *
 * Returns NULL when the vector is NULL or empty. NULL may also be a valid item_v
 * value_v, so callers that store NULL should check enki_vector_len first.
 */
void* enki_vector_pop(enki_vector* vector);

/**
 * Returns the item_v at the requested index_i.
 *
 * Returns NULL when the vector is NULL or the index_i is out of bounds. NULL may
 * also be a valid item_v value_v.
 */
void* enki_vector_get(const enki_vector* vector, size_t index_i);

/**
 * Returns a pointer to the element slot at index_i.
 */
void* enki_vector_get_slot(const enki_vector* vector, size_t index_i);

/**
 * Replaces the item_v at the requested index_i.
 */
enki_status enki_vector_set(enki_vector* vector, size_t index_i, void* item_v);

/**
 * Replaces the element at index_i by copying elem_size_s bytes_s from item_v.
 */
enki_status enki_vector_set_copy(enki_vector* vector, size_t index_i, const void* item_v);

/**
 * Returns the packed backing storage.
 */
void* enki_vector_data(const enki_vector* vector);

/**
 * Returns the byte size_s of each element.
 */
size_t enki_vector_elem_size(const enki_vector* vector);

/**
 * Returns the number of items currently stored in the vector.
 */
size_t enki_vector_len(const enki_vector* vector);

/**
 * Returns the number of items that fit without another allocation.
 */
size_t enki_vector_capacity(const enki_vector* vector);

/**
 * Ensures the vector can store at least capacity_s items without growing.
 *
 * This operation never shrinks the vector and never changes its length.
 */
enki_status enki_vector_reserve(enki_vector* vector, size_t capacity_s);

/**
 * Shrinks backing storage so capacity_s equals length.
 */
enki_status enki_vector_shrink(enki_vector* vector);

extern enki_allocator sys_a;

#ifdef __cplusplus
}
#endif

#endif
