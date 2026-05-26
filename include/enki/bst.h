/*
    enki_bst.h - small unbalanced binary search tree, single-header style

    This is free and unencumbered software released into the public domain.
    Anyone may copy, modify, publish, use, compile, sell, or distribute this
    software, either in source code form or as a compiled binary, for any
    purpose, commercial or non-commercial, and by any means.

    In jurisdictions that do not recognize the public domain, this file is
    licensed under CC0 1.0 Universal. No warranty is provided.

    usage:
        #define ENKI_BST_IMPLEMENTATION
        #include "enki/bst.h"

    this is deliberately boring: no balancing, no ownership of values, no magic.
*/

#ifndef ENKI_BST_H
#define ENKI_BST_H

#include <stddef.h>

#include "enki/allocator.h"
#include "enki/value.h"

#ifndef ENKI_BST_API
#ifdef ENKI_BST_STATIC
#define ENKI_BST_API static
#else
#define ENKI_BST_API extern
#endif
#endif

#ifndef ENKI_BST_VALUE_T
#define ENKI_BST_VALUE_T enki_value
#endif

#ifndef ENKI_BST_KEY_T
#define ENKI_BST_KEY_T enki_value
#endif

#ifndef ENKI_BST_KEY_LESS
#define ENKI_BST_KEY_LESS(a, b) enki_nat_le_bool(a,b)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef ENKI_BST_KEY_T enki_bst_key;
typedef ENKI_BST_VALUE_T enki_bst_value;

typedef struct enki_bst_node enki_bst_node;
struct enki_bst_node {
    enki_bst_key key;
    enki_bst_value value;
    unsigned char macro;
    enki_bst_node* left;
    enki_bst_node* right;
};

typedef struct enki_bst_tree {
    enki_allocator allocator;
    enki_bst_node* root;
    int failed;
} enki_bst_tree;

/* return non-zero from the callback to stop the walk early */
typedef int (*enki_bst_walk_fn)(enki_bst_node* node, void* user);

/* used by enki_bst_fold_adt; pack receives already-folded left/right values */
typedef enki_bst_value (*enki_bst_pack_fn)(enki_bst_key key, int macro, enki_bst_value value,
                                           enki_bst_value left, enki_bst_value right, void* user);

ENKI_BST_API void enki_bst_init(enki_bst_tree* t, enki_allocator allocator);
ENKI_BST_API int enki_bst_failed(const enki_bst_tree* t);

ENKI_BST_API enki_bst_node* enki_bst_new_node(enki_allocator allocator, enki_bst_key key,
                                              enki_bst_value value, int macro);
ENKI_BST_API void enki_bst_free(enki_allocator allocator, enki_bst_node* root);
ENKI_BST_API void enki_bst_tree_free(enki_bst_tree* t);
ENKI_BST_API void enki_bst_free_array(enki_allocator allocator, void* p);

ENKI_BST_API enki_bst_node* enki_bst_find(enki_bst_node* root, enki_bst_key key);
ENKI_BST_API int enki_bst_get(enki_bst_node* root, enki_bst_key key, int* out_macro,
                              enki_bst_value* out_value, enki_bst_node** out_node);

/* destructive put: inserts or updates, then returns the touched node, or NULL on OOM */
ENKI_BST_API enki_bst_node* enki_bst_put(enki_allocator allocator, enki_bst_node** root,
                                         enki_bst_key key, enki_bst_value value, int macro);
ENKI_BST_API enki_bst_node* enki_bst_tree_put(enki_bst_tree* t, enki_bst_key key,
                                              enki_bst_value value, int macro);

/* functional-ish put: copies the search path and shares untouched subtrees.
   do not blindly enki_bst_free() multiple versions of a shared tree. */
ENKI_BST_API enki_bst_node* enki_bst_put_persistent(enki_allocator allocator, enki_bst_node* root,
                                                    enki_bst_key key, enki_bst_value value,
                                                    int macro);

/* inorder traversal: left, node, right; returns non-zero if stopped early */
ENKI_BST_API int enki_bst_walk(enki_bst_node* root, enki_bst_walk_fn fn, void* user);
ENKI_BST_API size_t enki_bst_count(enki_bst_node* root);

/* allocates an inorder array of node pointers; caller releases with enki_bst_free_array */
ENKI_BST_API enki_bst_node** enki_bst_walk_array(enki_allocator allocator, enki_bst_node* root,
                                                 size_t* out_count);
ENKI_BST_API enki_bst_node** enki_bst_tree_walk_array(const enki_bst_tree* t, size_t* out_count);

/* folds a tree into another value type, matching the haskell bst shape:
      empty => empty
      node  => pack(key, macro, value, fold(left), fold(right), user)
*/
ENKI_BST_API enki_bst_value enki_bst_fold_adt(enki_bst_node* root, enki_bst_value empty,
                                              enki_bst_pack_fn pack, void* user);

#if defined(ENKI_BST_IMPLEMENTATION)

#include <assert.h>

#ifndef ENKI_BST_ASSERT
#define ENKI_BST_ASSERT(x) assert(x)
#endif

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

static int enki_bst__allocator_ok(enki_allocator allocator)
{
    return allocator.alloc != 0 && allocator.free != 0;
}

ENKI_BST_API void enki_bst_init(enki_bst_tree* t, enki_allocator allocator)
{
    if (!t)
        return;

    t->allocator = allocator;
    t->root = 0;
    t->failed = !enki_bst__allocator_ok(allocator);
}

ENKI_BST_API int enki_bst_failed(const enki_bst_tree* t)
{
    return !t || t->failed;
}

ENKI_BST_API enki_bst_node* enki_bst_new_node(enki_allocator allocator, enki_bst_key key,
                                              enki_bst_value value, int macro)
{
    enki_bst_node* n;

    if (!enki_bst__allocator_ok(allocator))
        return 0;

    n = (enki_bst_node*)allocator.alloc(allocator.ctx, sizeof(*n));
    if (!n)
        return 0;

    n->key = key;
    n->value = value;
    n->macro = (unsigned char)!!macro;
    n->left = 0;
    n->right = 0;
    return n;
}

ENKI_BST_API void enki_bst_free(enki_allocator allocator, enki_bst_node* root)
{
    if (!root)
        return;

    enki_bst_free(allocator, root->left);
    enki_bst_free(allocator, root->right);

    if (allocator.free)
        allocator.free(allocator.ctx, root);
}

ENKI_BST_API void enki_bst_tree_free(enki_bst_tree* t)
{
    enki_allocator allocator;

    if (!t)
        return;

    allocator = t->allocator;
    enki_bst_free(allocator, t->root);
    t->root = 0;
    t->failed = !enki_bst__allocator_ok(allocator);
}

ENKI_BST_API void enki_bst_free_array(enki_allocator allocator, void* p)
{
    if (allocator.free)
        allocator.free(allocator.ctx, p);
}

ENKI_BST_API enki_bst_node* enki_bst_find(enki_bst_node* root, enki_bst_key key)
{
    while (root) {
        if (ENKI_BST_KEY_LESS(key, root->key))
            root = root->left;
        else if (ENKI_BST_KEY_LESS(root->key, key))
            root = root->right;
        else
            return root;
    }
    return 0;
}

ENKI_BST_API int enki_bst_get(enki_bst_node* root, enki_bst_key key, int* out_macro,
                              enki_bst_value* out_value, enki_bst_node** out_node)
{
    enki_bst_node* n = enki_bst_find(root, key);
    if (!n)
        return 0;

    if (out_macro)
        *out_macro = !!n->macro;
    if (out_value)
        *out_value = n->value;
    if (out_node)
        *out_node = n;
    return 1;
}

ENKI_BST_API enki_bst_node* enki_bst_put(enki_allocator allocator, enki_bst_node** root,
                                         enki_bst_key key, enki_bst_value value, int macro)
{
    enki_bst_node** p;

    if (!root)
        return 0;

    p = root;
    while (*p) {
        enki_bst_node* n = *p;
        if (ENKI_BST_KEY_LESS(key, n->key)) {
            p = &n->left;
        } else if (ENKI_BST_KEY_LESS(n->key, key)) {
            p = &n->right;
        } else {
            n->value = value;
            n->macro = (unsigned char)!!macro;
            return n;
        }
    }

    *p = enki_bst_new_node(allocator, key, value, macro);
    return *p;
}

ENKI_BST_API enki_bst_node* enki_bst_tree_put(enki_bst_tree* t, enki_bst_key key,
                                              enki_bst_value value, int macro)
{
    enki_bst_node* node;

    if (!t || t->failed)
        return 0;

    node = enki_bst_put(t->allocator, &t->root, key, value, macro);
    if (!node)
        t->failed = 1;

    return node;
}

static enki_bst_node* enki_bst__put_copy(enki_allocator allocator, enki_bst_node* root,
                                         enki_bst_key key, enki_bst_value value, int macro, int* ok)
{
    enki_bst_node* n;

    if (!root) {
        n = enki_bst_new_node(allocator, key, value, macro);
        if (!n)
            *ok = 0;
        return n;
    }

    n = enki_bst_new_node(allocator, root->key, root->value, root->macro);
    if (!n) {
        *ok = 0;
        return 0;
    }

    n->left = root->left;
    n->right = root->right;

    if (ENKI_BST_KEY_LESS(key, root->key)) {
        enki_bst_node* l = enki_bst__put_copy(allocator, root->left, key, value, macro, ok);
        if (!*ok) {
            allocator.free(allocator.ctx, n);
            return 0;
        }
        n->left = l;
    } else if (ENKI_BST_KEY_LESS(root->key, key)) {
        enki_bst_node* r = enki_bst__put_copy(allocator, root->right, key, value, macro, ok);
        if (!*ok) {
            allocator.free(allocator.ctx, n);
            return 0;
        }
        n->right = r;
    } else {
        n->value = value;
        n->macro = (unsigned char)!!macro;
    }

    return n;
}

ENKI_BST_API enki_bst_node* enki_bst_put_persistent(enki_allocator allocator, enki_bst_node* root,
                                                    enki_bst_key key, enki_bst_value value,
                                                    int macro)
{
    int ok = 1;

    if (!enki_bst__allocator_ok(allocator))
        return 0;

    return enki_bst__put_copy(allocator, root, key, value, macro, &ok);
}

ENKI_BST_API int enki_bst_walk(enki_bst_node* root, enki_bst_walk_fn fn, void* user)
{
    if (!root)
        return 0;

    if (enki_bst_walk(root->left, fn, user))
        return 1;
    if (fn && fn(root, user))
        return 1;
    if (enki_bst_walk(root->right, fn, user))
        return 1;
    return 0;
}

ENKI_BST_API size_t enki_bst_count(enki_bst_node* root)
{
    if (!root)
        return 0;

    return (size_t)1 + enki_bst_count(root->left) + enki_bst_count(root->right);
}

typedef struct enki_bst__array_ctx {
    enki_bst_node** p;
} enki_bst__array_ctx;

static int enki_bst__array_push(enki_bst_node* node, void* user)
{
    enki_bst__array_ctx* ctx = (enki_bst__array_ctx*)user;
    *ctx->p++ = node;
    return 0;
}

ENKI_BST_API enki_bst_node** enki_bst_walk_array(enki_allocator allocator, enki_bst_node* root,
                                                 size_t* out_count)
{
    size_t n = enki_bst_count(root);
    enki_bst_node** a;
    enki_bst__array_ctx ctx;

    if (out_count)
        *out_count = n;
    if (n == 0)
        return 0;

    if (!enki_bst__allocator_ok(allocator) || n > SIZE_MAX / sizeof(*a)) {
        if (out_count)
            *out_count = 0;
        return 0;
    }

    a = (enki_bst_node**)allocator.alloc(allocator.ctx, sizeof(*a) * n);
    if (!a) {
        if (out_count)
            *out_count = 0;
        return 0;
    }

    ctx.p = a;
    (void)enki_bst_walk(root, enki_bst__array_push, &ctx);
    return a;
}

ENKI_BST_API enki_bst_node** enki_bst_tree_walk_array(const enki_bst_tree* t, size_t* out_count)
{
    if (!t) {
        if (out_count)
            *out_count = 0;
        return 0;
    }

    return enki_bst_walk_array(t->allocator, t->root, out_count);
}

ENKI_BST_API enki_bst_value enki_bst_fold_adt(enki_bst_node* root, enki_bst_value empty,
                                              enki_bst_pack_fn pack, void* user)
{
    enki_bst_value left;
    enki_bst_value right;

    ENKI_BST_ASSERT(pack != 0 || root == 0);
    if (!root)
        return empty;

    left = enki_bst_fold_adt(root->left, empty, pack, user);
    right = enki_bst_fold_adt(root->right, empty, pack, user);
    return pack(root->key, !!root->macro, root->value, left, right, user);
}

#endif /* implementation */

#ifdef __cplusplus
}
#endif

#endif /* ENKI_BST_H */
