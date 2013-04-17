#ident "$Id: test-del-inorder.c 32975 2011-07-11 23:42:51Z leifwalsh $"
/* The goal of this test.  Make sure that inserts stay behind deletes. */


#include "test.h"
#include "includes.h"
#include <brt-cachetable-wrappers.h>
#include "brt-flusher.h"
#include "brt-flusher-internal.h"
#include "checkpoint.h"

static TOKUTXN const null_txn = 0;
static DB * const null_db = 0;

enum { NODESIZE = 1024, KSIZE=NODESIZE-100, TOKU_PSIZE=20 };

CACHETABLE ct;
BRT t;

BOOL checkpoint_called;
BOOL checkpoint_callback_called;
toku_pthread_t checkpoint_tid;


// callback functions for flush_some_child
static bool
dont_destroy_bn(void* UU(extra))
{
    return false;
}

static bool recursively_flush_should_not_happen(BRTNODE UU(child), void* UU(extra)) {
    assert(FALSE);
}

static int child_to_flush(struct brt_header* UU(h), BRTNODE parent, void* UU(extra)) {
    assert(parent->height == 1);
    assert(parent->n_children == 2);
    return 0;
}

static void dummy_update_status(BRTNODE UU(child), int UU(dirtied), void* UU(extra)) {
}


static void checkpoint_callback(void* UU(extra)) {
    usleep(1*1024*1024);
    checkpoint_callback_called = TRUE;
}


static void *do_checkpoint(void *arg) {
    // first verify that checkpointed_data is correct;
    if (verbose) printf("starting a checkpoint\n");
    int r = toku_checkpoint(ct, NULL, checkpoint_callback, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert_zero(r);
    if (verbose) printf("completed a checkpoint\n");
    return arg;
}


static void flusher_callback(int state, void* extra) {
    int desired_state = *(int *)extra;
    if (verbose) {
        printf("state %d\n", state);
    }
    if (state == desired_state) {
        checkpoint_called = TRUE;
        int r = toku_pthread_create(&checkpoint_tid, NULL, do_checkpoint, NULL); 
        assert_zero(r);
        while (!checkpoint_callback_called) {
            usleep(1*1024*1024);
        }
    }
}

static void
doit (int state) {
    BLOCKNUM node_root;
    BLOCKNUM node_leaves[2];

    int r;
    checkpoint_called = FALSE;
    checkpoint_callback_called = FALSE;

    toku_flusher_thread_set_callback(flusher_callback, &state);
    
    r = toku_brt_create_cachetable(&ct, 500*1024*1024, ZERO_LSN, NULL_LOGGER); assert(r==0);
    unlink("foo2.brt");
    unlink("bar2.brt");
    // note the basement node size is 5 times the node size
    // this is done to avoid rebalancing when writing a leaf
    // node to disk
    r = toku_open_brt("foo2.brt", 1, &t, NODESIZE, 5*NODESIZE, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    toku_testsetup_initialize();  // must precede any other toku_testsetup calls

    r = toku_testsetup_leaf(t, &node_leaves[0], 1, NULL, NULL);
    assert(r==0);

    r = toku_testsetup_leaf(t, &node_leaves[1], 1, NULL, NULL);
    assert(r==0);

    char* pivots[1];
    pivots[0] = toku_strdup("kkkkk");
    int pivot_len = 6;

    r = toku_testsetup_nonleaf(t, 1, &node_root, 2, node_leaves, pivots, &pivot_len);
    assert(r==0);

    r = toku_testsetup_root(t, node_root);
    assert(r==0);

    r = toku_testsetup_insert_to_leaf(
        t,
        node_leaves[0],
        "a",
        2,
        NULL,
        0
        );
    assert_zero(r);
    r = toku_testsetup_insert_to_leaf(
        t,
        node_leaves[1],
        "z",
        2,
        NULL,
        0
        );
    assert_zero(r);


    // at this point, we have inserted two leafentries,
    // one in each leaf node. A flush should invoke a merge
    struct flusher_advice fa;
    flusher_advice_init(
        &fa,
        child_to_flush,
        dont_destroy_bn,
        recursively_flush_should_not_happen,
        default_merge_child,
        dummy_update_status,
        default_pick_child_after_split,
        NULL
        );

    // hack to get merge going
    BRTNODE node = NULL;
    toku_pin_node_with_min_bfe(&node, node_leaves[0], t);
    BLB_SEQINSERT(node, node->n_children-1) = FALSE;
    toku_unpin_brtnode(t->h, node);
    toku_pin_node_with_min_bfe(&node, node_leaves[1], t);
    BLB_SEQINSERT(node, node->n_children-1) = FALSE;
    toku_unpin_brtnode(t->h, node);

    
    struct brtnode_fetch_extra bfe;
    fill_bfe_for_min_read(&bfe, t->h);
    toku_pin_brtnode_off_client_thread(
        t->h, 
        node_root,
        toku_cachetable_hash(t->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(node->n_children == 2);

    // do the flush
    flush_some_child(t->h, node, &fa);
    assert(checkpoint_callback_called);

    // now let's pin the root again and make sure it is has merged
    toku_pin_brtnode_off_client_thread(
        t->h, 
        node_root,
        toku_cachetable_hash(t->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    toku_unpin_brtnode(t->h, node);

    void *ret;
    r = toku_pthread_join(checkpoint_tid, &ret); 
    assert_zero(r);

    //
    // now the dictionary has been checkpointed
    // copy the file to something with a new name,
    // open it, and verify that the state of what is
    // checkpointed is what we expect
    //

    r = system("cp foo2.brt bar2.brt ");
    assert_zero(r);

    BRT c_brt;
    // note the basement node size is 5 times the node size
    // this is done to avoid rebalancing when writing a leaf
    // node to disk
    r = toku_open_brt("bar2.brt", 0, &c_brt, NODESIZE, 5*NODESIZE, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    //
    // now pin the root, verify that the state is what we expect
    //
    fill_bfe_for_full_read(&bfe, c_brt->h);
    toku_pin_brtnode_off_client_thread(
        c_brt->h, 
        node_root,
        toku_cachetable_hash(c_brt->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(!node->dirty);
    BLOCKNUM left_child, right_child;
    // cases where we expect the checkpoint to contain the merge
    if (state == ft_flush_after_merge || state == ft_flush_before_unpin_remove) {
        assert(node->n_children == 1);
        left_child = BP_BLOCKNUM(node,0);
    }
    else if (state == ft_flush_before_merge || state == ft_flush_before_pin_second_node_for_merge) {
        assert(node->n_children == 2);
        left_child = BP_BLOCKNUM(node,0);
        right_child = BP_BLOCKNUM(node,1);
    }
    else {
        assert(false);
    }
    toku_unpin_brtnode_off_client_thread(c_brt->h, node);

    // now let's verify the leaves are what we expect
    if (state == ft_flush_before_merge || state == ft_flush_before_pin_second_node_for_merge) {
        toku_pin_brtnode_off_client_thread(
            c_brt->h, 
            left_child,
            toku_cachetable_hash(c_brt->h->cf, left_child),
            &bfe,
            TRUE, 
            0,
            NULL,
            &node
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(toku_omt_size(BLB_BUFFER(node,0)) == 1);
        toku_unpin_brtnode_off_client_thread(c_brt->h, node);

        toku_pin_brtnode_off_client_thread(
            c_brt->h, 
            right_child,
            toku_cachetable_hash(c_brt->h->cf, right_child),
            &bfe,
            TRUE, 
            0,
            NULL,
            &node
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(toku_omt_size(BLB_BUFFER(node,0)) == 1);
        toku_unpin_brtnode_off_client_thread(c_brt->h, node);
    }
    else if (state == ft_flush_after_merge || state == ft_flush_before_unpin_remove) {
        toku_pin_brtnode_off_client_thread(
            c_brt->h, 
            left_child,
            toku_cachetable_hash(c_brt->h->cf, left_child),
            &bfe,
            TRUE, 
            0,
            NULL,
            &node
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(toku_omt_size(BLB_BUFFER(node,0)) == 2);
        toku_unpin_brtnode_off_client_thread(c_brt->h, node);
    }
    else {
        assert(FALSE);
    }


    DBT k;
    struct check_pair pair1 = {2, "a", 0, NULL, 0};
    r = toku_brt_lookup(c_brt, toku_fill_dbt(&k, "a", 2), lookup_checkf, &pair1);
    assert(r==0);
    struct check_pair pair2 = {2, "z", 0, NULL, 0};
    r = toku_brt_lookup(c_brt, toku_fill_dbt(&k, "z", 2), lookup_checkf, &pair2);
    assert(r==0);


    r = toku_close_brt_nolsn(t, 0);    assert(r==0);
    r = toku_close_brt_nolsn(c_brt, 0);    assert(r==0);
    r = toku_cachetable_close(&ct); assert(r==0);
    toku_free(pivots[0]);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    default_parse_args(argc, argv);
    doit(ft_flush_before_merge);
    doit(ft_flush_before_pin_second_node_for_merge);
    doit(ft_flush_before_unpin_remove);
    doit(ft_flush_after_merge);
    return 0;
}