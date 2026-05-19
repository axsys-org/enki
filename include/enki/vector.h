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
    /** Operation failed because the allocator returned NULL or size overflowed. */
    ENKI_ERR_ALLOC = 1,
    /** Operation failed because an index was outside the vector length. */
    ENKI_ERR_BOUNDS = 2,
    /** Operation failed because an argument or allocator was invalid. */
    ENKI_ERR_INVALID = 3,
} enki_status;

/**
 * Opaque dynamic array of void pointers.
 */
typedef struct enki_vector enki_vector;

/**
 * Creates an empty vector using the supplied allocator.
 *
 * Returns NULL if the allocator is invalid or cannot allocate the vector
 * header.
 */
enki_vector* enki_vector_create(enki_allocator allocator);

/**
 * Creates an empty vector with fixed-size copied elements.
 *
 * The original enki_vector_create creates a pointer vector. Use this when the
 * backing storage needs to be a packed array, such as uint8_t bytecode or
 * enki_value constants.
 */
enki_vector* enki_vector_create_sized(enki_allocator allocator, size_t elem_size);

/**
 * Destroys a vector and releases all storage owned by the vector.
 *
 * Stored items are opaque borrowed pointers and are not freed.
 */
void enki_vector_destroy(enki_vector* vector);

/**
 * Appends an item to the end of the vector.
 */
enki_status enki_vector_push(enki_vector* vector, void* item);

/**
 * Appends one fixed-size element by copying elem_size bytes from item.
 */
enki_status enki_vector_push_copy(enki_vector* vector, const void* item);

/**
 * Appends one byte to a vector created with elem_size == sizeof(uint8_t).
 */
enki_status enki_vector_push_u8(enki_vector* vector, uint8_t item);

/**
 * Removes and returns the last item in the vector.
 *
 * Returns NULL when the vector is NULL or empty. NULL may also be a valid item
 * value, so callers that store NULL should check enki_vector_len first.
 */
void* enki_vector_pop(enki_vector* vector);

/**
 * Returns the item at the requested index.
 *
 * Returns NULL when the vector is NULL or the index is out of bounds. NULL may
 * also be a valid item value.
 */
void* enki_vector_get(const enki_vector* vector, size_t index);

/**
 * Returns a pointer to the element slot at index.
 */
void* enki_vector_get_slot(const enki_vector* vector, size_t index);

/**
 * Replaces the item at the requested index.
 */
enki_status enki_vector_set(enki_vector* vector, size_t index, void* item);

/**
 * Replaces the element at index by copying elem_size bytes from item.
 */
enki_status enki_vector_set_copy(enki_vector* vector, size_t index, const void* item);

/**
 * Returns the packed backing storage.
 */
void* enki_vector_data(const enki_vector* vector);

/**
 * Returns the byte size of each element.
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
 * Ensures the vector can store at least capacity items without growing.
 *
 * This operation never shrinks the vector and never changes its length.
 */
enki_status enki_vector_reserve(enki_vector* vector, size_t capacity);

/**
 * Shrinks backing storage so capacity equals length.
 */
enki_status enki_vector_shrink(enki_vector* vector);

#ifdef __cplusplus
}
#endif

#endif
