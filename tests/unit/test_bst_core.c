#include <criterion/criterion.h>

#include <stddef.h>
#include <stdint.h>

#define ENKI_BST_VALUE_T intptr_t
#define ENKI_BST_STATIC
#define ENKI_BST_IMPLEMENTATION
#include "enki/bst.h"

#define LEN(x) (sizeof(x) / sizeof((x)[0]))
#define V(x) ((intptr_t)(x))

static enki_allocator test_allocator(void)
{
    return *enki_allocator_system();
}

typedef struct item {
    enki_bst_key key;
    intptr_t value;
    int macro;
} item;

static enki_bst_tree make_tree(const item* xs, size_t n)
{
    enki_bst_tree t;
    size_t i;

    enki_bst_init(&t, test_allocator());

    for (i = 0; i < n; ++i) {
        enki_bst_node* inserted = enki_bst_tree_put(&t, xs[i].key, xs[i].value, xs[i].macro);
        cr_assert_not_null(inserted);
    }

    return t;
}

static enki_bst_tree make_balanced7(void)
{
    static const item xs[] = {
        {8, 80, 0}, {4, 40, 1}, {12, 120, 0}, {2, 20, 0}, {6, 60, 1}, {10, 100, 1}, {14, 140, 0},
    };

    return make_tree(xs, LEN(xs));
}

static int ptr_seen(enki_bst_node** nodes, size_t count, enki_bst_node* p)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (nodes[i] == p)
            return 1;
    }

    return 0;
}

static void collect_unique_nodes(enki_bst_node* root, enki_bst_node** nodes, size_t* count,
                                 size_t cap)
{
    if (!root)
        return;

    if (ptr_seen(nodes, *count, root))
        return;

    cr_assert_lt(*count, cap);

    nodes[(*count)++] = root;

    collect_unique_nodes(root->left, nodes, count, cap);
    collect_unique_nodes(root->right, nodes, count, cap);
}

/* useful for persistent trees, where old and new versions share subtrees */
static void free_unique_nodes2(enki_bst_node* a, enki_bst_node* b)
{
    enki_allocator allocator = test_allocator();
    enki_bst_node* nodes[128];
    size_t count = 0;
    size_t i;

    collect_unique_nodes(a, nodes, &count, LEN(nodes));
    collect_unique_nodes(b, nodes, &count, LEN(nodes));

    for (i = 0; i < count; ++i)
        allocator.free(allocator.ctx, nodes[i]);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

Test(enki_bst_lifecycle, init_sets_root_to_null)
{
    enki_bst_tree t;

    t.root = (enki_bst_node*)0x1;
    enki_bst_init(&t, test_allocator());

    cr_assert_null(t.root);
    cr_assert(!enki_bst_failed(&t));
}

Test(enki_bst_lifecycle, init_accepts_null_tree_pointer)
{
    enki_bst_init(NULL, test_allocator());
}

Test(enki_bst_lifecycle, new_node_initializes_all_fields)
{
    enki_bst_node* n = enki_bst_new_node(test_allocator(), 42, V(9001), 17);

    cr_assert_not_null(n);
    cr_assert_eq(n->key, 42);
    cr_assert_eq(n->value, V(9001));
    cr_assert_eq(n->macro, 1);
    cr_assert_null(n->left);
    cr_assert_null(n->right);

    enki_bst_free(test_allocator(), n);
}

Test(enki_bst_lifecycle, free_accepts_null)
{
    enki_bst_free(test_allocator(), NULL);
    enki_bst_free_array(test_allocator(), NULL);
    enki_bst_tree_free(NULL);
}

Test(enki_bst_lifecycle, tree_free_releases_tree_and_nulls_root)
{
    enki_bst_tree t;

    enki_bst_init(&t, test_allocator());

    cr_assert_not_null(enki_bst_tree_put(&t, 10, V(100), 0));
    cr_assert_not_null(enki_bst_tree_put(&t, 5, V(50), 1));
    cr_assert_not_null(enki_bst_tree_put(&t, 15, V(150), 0));

    cr_assert_not_null(t.root);

    enki_bst_tree_free(&t);

    cr_assert_null(t.root);
}

/* ------------------------------------------------------------------------- */
/* put / find / get                                                           */
/* ------------------------------------------------------------------------- */

Test(enki_bst_put_get, put_into_empty_tree_creates_root)
{
    enki_bst_tree t;
    enki_bst_node* n;

    enki_bst_init(&t, test_allocator());

    n = enki_bst_tree_put(&t, 10, V(100), 1);

    cr_assert_not_null(n);
    cr_assert_not_null(t.root);
    cr_assert_eq(n, t.root);
    cr_assert_eq(t.root->key, 10);
    cr_assert_eq(t.root->value, V(100));
    cr_assert_eq(t.root->macro, 1);
    cr_assert_null(t.root->left);
    cr_assert_null(t.root->right);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, inserts_left_and_right_subtrees)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_not_null(t.root);
    cr_assert_eq(t.root->key, 8);

    cr_assert_not_null(t.root->left);
    cr_assert_not_null(t.root->right);

    cr_assert_eq(t.root->left->key, 4);
    cr_assert_eq(t.root->right->key, 12);

    cr_assert_eq(t.root->left->left->key, 2);
    cr_assert_eq(t.root->left->right->key, 6);

    cr_assert_eq(t.root->right->left->key, 10);
    cr_assert_eq(t.root->right->right->key, 14);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, find_returns_matching_node)
{
    enki_bst_tree t = make_balanced7();

    enki_bst_node* n = enki_bst_find(t.root, 10);

    cr_assert_not_null(n);
    cr_assert_eq(n->key, 10);
    cr_assert_eq(n->value, V(100));
    cr_assert_eq(n->macro, 1);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, find_returns_null_for_missing_key)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_null(enki_bst_find(t.root, 999));
    cr_assert_null(enki_bst_find(NULL, 10));

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, get_fills_all_outputs)
{
    enki_bst_tree t = make_balanced7();

    int macro = -1;
    intptr_t value = V(-1);
    enki_bst_node* node = NULL;

    int ok = enki_bst_get(t.root, 6, &macro, &value, &node);

    cr_assert_eq(ok, 1);
    cr_assert_eq(macro, 1);
    cr_assert_eq(value, V(60));
    cr_assert_not_null(node);
    cr_assert_eq(node->key, 6);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, get_allows_null_outputs)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_eq(enki_bst_get(t.root, 6, NULL, NULL, NULL), 1);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, get_does_not_touch_outputs_on_miss)
{
    enki_bst_tree t = make_balanced7();

    int macro = 123;
    intptr_t value = V(456);
    enki_bst_node* node = (enki_bst_node*)0x1;

    int ok = enki_bst_get(t.root, 999, &macro, &value, &node);

    cr_assert_eq(ok, 0);
    cr_assert_eq(macro, 123);
    cr_assert_eq(value, V(456));
    cr_assert_eq(node, (enki_bst_node*)0x1);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, updating_existing_key_preserves_node_and_children)
{
    enki_bst_tree t = make_balanced7();

    enki_bst_node* old_root = t.root;
    enki_bst_node* old_left = t.root->left;
    enki_bst_node* old_right = t.root->right;
    size_t before_count = enki_bst_count(t.root);

    enki_bst_node* updated = enki_bst_put(t.allocator, &t.root, 8, V(888), 1);

    cr_assert_eq(updated, old_root);
    cr_assert_eq(t.root, old_root);
    cr_assert_eq(t.root->left, old_left);
    cr_assert_eq(t.root->right, old_right);

    cr_assert_eq(t.root->value, V(888));
    cr_assert_eq(t.root->macro, 1);
    cr_assert_eq(enki_bst_count(t.root), before_count);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, updating_leaf_preserves_count)
{
    enki_bst_tree t = make_balanced7();

    enki_bst_node* old_leaf = enki_bst_find(t.root, 2);
    size_t before_count = enki_bst_count(t.root);

    enki_bst_node* updated = enki_bst_put(t.allocator, &t.root, 2, V(222), 1);

    cr_assert_eq(updated, old_leaf);
    cr_assert_eq(enki_bst_count(t.root), before_count);
    cr_assert_eq(old_leaf->value, V(222));
    cr_assert_eq(old_leaf->macro, 1);

    enki_bst_tree_free(&t);
}

Test(enki_bst_put_get, put_rejects_null_root_pointer_argument)
{
    cr_assert_null(enki_bst_put(test_allocator(), NULL, 1, V(1), 0));
}

/* ------------------------------------------------------------------------- */
/* count                                                                      */
/* ------------------------------------------------------------------------- */

Test(enki_bst_count, empty_tree_has_count_zero)
{
    cr_assert_eq(enki_bst_count(NULL), 0);
}

Test(enki_bst_count, counts_nodes)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_eq(enki_bst_count(t.root), 7);

    enki_bst_tree_free(&t);
}

Test(enki_bst_count, duplicate_put_does_not_increase_count)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_eq(enki_bst_count(t.root), 7);
    cr_assert_not_null(enki_bst_put(t.allocator, &t.root, 6, V(600), 0));
    cr_assert_eq(enki_bst_count(t.root), 7);

    enki_bst_tree_free(&t);
}

/* ------------------------------------------------------------------------- */
/* walk                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct walk_capture {
    size_t keys[64];
    intptr_t values[64];
    int macros[64];
    size_t count;
} walk_capture;

static int capture_node(enki_bst_node* node, void* user)
{
    walk_capture* cap = (walk_capture*)user;

    cr_assert_lt(cap->count, LEN(cap->keys));

    cap->keys[cap->count] = node->key;
    cap->values[cap->count] = node->value;
    cap->macros[cap->count] = !!node->macro;
    cap->count++;

    return 0;
}

Test(enki_bst_walk, empty_tree_walks_nothing)
{
    walk_capture cap = {{0}, {0}, {0}, 0};

    cr_assert_eq(enki_bst_walk(NULL, capture_node, &cap), 0);
    cr_assert_eq(cap.count, 0);
}

Test(enki_bst_walk, inorder_walk_returns_sorted_keys)
{
    enki_bst_tree t = make_balanced7();

    static const size_t expected[] = {2, 4, 6, 8, 10, 12, 14};

    walk_capture cap = {{0}, {0}, {0}, 0};
    size_t i;

    cr_assert_eq(enki_bst_walk(t.root, capture_node, &cap), 0);
    cr_assert_eq(cap.count, LEN(expected));

    for (i = 0; i < LEN(expected); ++i)
        cr_assert_eq(cap.keys[i], expected[i]);

    enki_bst_tree_free(&t);
}

typedef struct stop_capture {
    walk_capture seen;
    size_t stop_key;
} stop_capture;

static int capture_until_key(enki_bst_node* node, void* user)
{
    stop_capture* cap = (stop_capture*)user;

    cr_assert_lt(cap->seen.count, LEN(cap->seen.keys));

    cap->seen.keys[cap->seen.count] = node->key;
    cap->seen.values[cap->seen.count] = node->value;
    cap->seen.macros[cap->seen.count] = !!node->macro;
    cap->seen.count++;

    return node->key == cap->stop_key;
}

Test(enki_bst_walk, callback_can_stop_walk_early)
{
    enki_bst_tree t = make_balanced7();

    static const size_t expected_prefix[] = {2, 4, 6, 8, 10};

    stop_capture cap;
    size_t i;

    cap.seen = (walk_capture){{0}, {0}, {0}, 0};
    cap.stop_key = 10;

    cr_assert_eq(enki_bst_walk(t.root, capture_until_key, &cap), 1);
    cr_assert_eq(cap.seen.count, LEN(expected_prefix));

    for (i = 0; i < LEN(expected_prefix); ++i)
        cr_assert_eq(cap.seen.keys[i], expected_prefix[i]);

    enki_bst_tree_free(&t);
}

Test(enki_bst_walk, null_callback_is_allowed)
{
    enki_bst_tree t = make_balanced7();

    cr_assert_eq(enki_bst_walk(t.root, NULL, NULL), 0);

    enki_bst_tree_free(&t);
}

/* ------------------------------------------------------------------------- */
/* walk_array                                                                 */
/* ------------------------------------------------------------------------- */

Test(enki_bst_walk_array, empty_tree_returns_null_array_and_zero_count)
{
    size_t count = 123;
    enki_bst_node** nodes = enki_bst_walk_array(test_allocator(), NULL, &count);

    cr_assert_null(nodes);
    cr_assert_eq(count, 0);
}

Test(enki_bst_walk_array, returns_inorder_node_array)
{
    enki_bst_tree t = make_balanced7();

    static const size_t expected[] = {2, 4, 6, 8, 10, 12, 14};

    size_t count = 0;
    enki_bst_node** nodes = enki_bst_tree_walk_array(&t, &count);
    size_t i;

    cr_assert_not_null(nodes);
    cr_assert_eq(count, LEN(expected));

    for (i = 0; i < count; ++i) {
        cr_assert_not_null(nodes[i]);
        cr_assert_eq(nodes[i]->key, expected[i]);
        cr_assert_eq(nodes[i], enki_bst_find(t.root, expected[i]));
    }

    enki_bst_free_array(t.allocator, nodes);
    enki_bst_tree_free(&t);
}

Test(enki_bst_walk_array, null_count_output_is_allowed)
{
    enki_bst_tree t = make_balanced7();

    enki_bst_node** nodes = enki_bst_tree_walk_array(&t, NULL);

    cr_assert_not_null(nodes);

    enki_bst_free_array(t.allocator, nodes);
    enki_bst_tree_free(&t);
}

/* ------------------------------------------------------------------------- */
/* fold                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct fold_record {
    enki_bst_key key;
    int macro;
    intptr_t value;
    intptr_t left;
    intptr_t right;
} fold_record;

typedef struct fold_capture {
    fold_record records[32];
    size_t count;
} fold_capture;

static intptr_t pack_subtree_size(enki_bst_key key, int macro, intptr_t value, intptr_t left,
                                  intptr_t right, void* user)
{
    fold_capture* cap = (fold_capture*)user;

    cr_assert_not_null(cap);
    cr_assert_lt(cap->count, LEN(cap->records));

    cap->records[cap->count].key = key;
    cap->records[cap->count].macro = !!macro;
    cap->records[cap->count].value = value;
    cap->records[cap->count].left = left;
    cap->records[cap->count].right = right;
    cap->count++;

    return 1 + left + right;
}

Test(enki_bst_fold_adt, empty_tree_returns_empty_value)
{
    fold_capture cap = {{{0, 0, 0, 0, 0}}, 0};

    intptr_t result = enki_bst_fold_adt(NULL, V(-7), NULL, &cap);

    cr_assert_eq(result, V(-7));
    cr_assert_eq(cap.count, 0);
}

Test(enki_bst_fold_adt, folds_bottom_up_and_passes_child_results)
{
    static const item xs[] = {
        {10, 100, 0},
        {5, 50, 1},
        {15, 150, 0},
        {3, 30, 1},
    };

    enki_bst_tree t = make_tree(xs, LEN(xs));
    fold_capture cap = {{{0, 0, 0, 0, 0}}, 0};

    intptr_t result = enki_bst_fold_adt(t.root, V(0), pack_subtree_size, &cap);

    cr_assert_eq(result, V(4));
    cr_assert_eq(cap.count, 4);

    /* postorder by tree shape: left subtree, right subtree, root */
    cr_assert_eq(cap.records[0].key, 3);
    cr_assert_eq(cap.records[0].left, V(0));
    cr_assert_eq(cap.records[0].right, V(0));

    cr_assert_eq(cap.records[1].key, 5);
    cr_assert_eq(cap.records[1].macro, 1);
    cr_assert_eq(cap.records[1].value, V(50));
    cr_assert_eq(cap.records[1].left, V(1));
    cr_assert_eq(cap.records[1].right, V(0));

    cr_assert_eq(cap.records[2].key, 15);
    cr_assert_eq(cap.records[2].left, V(0));
    cr_assert_eq(cap.records[2].right, V(0));

    cr_assert_eq(cap.records[3].key, 10);
    cr_assert_eq(cap.records[3].macro, 0);
    cr_assert_eq(cap.records[3].value, V(100));
    cr_assert_eq(cap.records[3].left, V(2));
    cr_assert_eq(cap.records[3].right, V(1));

    enki_bst_tree_free(&t);
}

/* ------------------------------------------------------------------------- */
/* persistent put                                                             */
/* ------------------------------------------------------------------------- */

Test(enki_bst_persistent, put_into_empty_tree_returns_singleton)
{
    enki_bst_node* root = enki_bst_put_persistent(test_allocator(), NULL, 42, V(420), 1);

    cr_assert_not_null(root);
    cr_assert_eq(root->key, 42);
    cr_assert_eq(root->value, V(420));
    cr_assert_eq(root->macro, 1);
    cr_assert_null(root->left);
    cr_assert_null(root->right);

    enki_bst_free(test_allocator(), root);
}

Test(enki_bst_persistent, insert_copies_search_path_and_preserves_original_tree)
{
    enki_bst_tree old_tree = make_balanced7();

    enki_bst_node* old_root = old_tree.root;
    enki_bst_node* old_left = old_root->left;
    enki_bst_node* old_right = old_root->right;
    enki_bst_node* old_left_left = old_left->left;
    enki_bst_node* old_left_right = old_left->right;

    enki_bst_node* new_root =
        enki_bst_put_persistent(old_tree.allocator, old_tree.root, 5, V(500), 1);

    cr_assert_not_null(new_root);

    cr_assert_neq(new_root, old_root);
    cr_assert_neq(new_root->left, old_left);
    cr_assert_neq(new_root->left->right, old_left_right);

    /* untouched subtrees are shared */
    cr_assert_eq(new_root->right, old_right);
    cr_assert_eq(new_root->left->left, old_left_left);

    /* original tree is unchanged */
    cr_assert_null(enki_bst_find(old_tree.root, 5));
    cr_assert_eq(enki_bst_count(old_tree.root), 7);

    /* new tree contains the inserted binding */
    cr_assert_not_null(enki_bst_find(new_root, 5));
    cr_assert_eq(enki_bst_find(new_root, 5)->value, V(500));
    cr_assert_eq(enki_bst_find(new_root, 5)->macro, 1);
    cr_assert_eq(enki_bst_count(new_root), 8);

    free_unique_nodes2(old_tree.root, new_root);
}

Test(enki_bst_persistent, update_existing_key_copies_node_and_shares_children)
{
    enki_bst_tree old_tree = make_balanced7();

    enki_bst_node* old_root = old_tree.root;
    enki_bst_node* old_left = old_root->left;
    enki_bst_node* old_right = old_root->right;

    enki_bst_node* new_root =
        enki_bst_put_persistent(old_tree.allocator, old_tree.root, 8, V(888), 1);

    cr_assert_not_null(new_root);

    cr_assert_neq(new_root, old_root);
    cr_assert_eq(new_root->left, old_left);
    cr_assert_eq(new_root->right, old_right);

    cr_assert_eq(old_root->value, V(80));
    cr_assert_eq(old_root->macro, 0);

    cr_assert_eq(new_root->value, V(888));
    cr_assert_eq(new_root->macro, 1);

    cr_assert_eq(enki_bst_count(old_tree.root), 7);
    cr_assert_eq(enki_bst_count(new_root), 7);

    free_unique_nodes2(old_tree.root, new_root);
}

Test(enki_bst_persistent, updating_leaf_preserves_original_leaf)
{
    enki_bst_tree old_tree = make_balanced7();

    enki_bst_node* old_leaf = enki_bst_find(old_tree.root, 2);
    enki_bst_node* new_root =
        enki_bst_put_persistent(old_tree.allocator, old_tree.root, 2, V(222), 1);
    enki_bst_node* new_leaf;

    cr_assert_not_null(old_leaf);
    cr_assert_not_null(new_root);

    new_leaf = enki_bst_find(new_root, 2);

    cr_assert_not_null(new_leaf);
    cr_assert_neq(new_leaf, old_leaf);

    cr_assert_eq(old_leaf->value, V(20));
    cr_assert_eq(old_leaf->macro, 0);

    cr_assert_eq(new_leaf->value, V(222));
    cr_assert_eq(new_leaf->macro, 1);

    free_unique_nodes2(old_tree.root, new_root);
}
