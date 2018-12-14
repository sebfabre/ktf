/*
 * Copyright (c) 2011, 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 * self.c: Some simple self tests for KTF
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/slab_def.h>

#include "ktf.h"
#include "ktf_map.h"
#include "ktf_cov.h"
#include "ktf_syms.h"

#include "hybrid.h"

MODULE_LICENSE("GPL");

struct map_test_ctx {
	struct ktf_context k;
};

static struct map_test_ctx s_mctx[3];

/* Declare a simple handle with no contexts for simple (unparameterized) tests: */
KTF_INIT();

/* For tests that defines multiple test cases
 * (e.g. if the test scope requires application of each test on several devices or
 *  other abstract contexts, definable by the test module)
 */
static KTF_HANDLE_INIT(dual_handle);
static KTF_HANDLE_INIT(single_handle);
static KTF_HANDLE_INIT(no_handle);
static KTF_HANDLE_INIT_VERSION(wrongversion_handle, 0);

static struct map_test_ctx *to_mctx(struct ktf_context *ctx)
{
	return container_of(ctx, struct map_test_ctx, k);
}

struct myelem {
	struct ktf_map_elem foo;
	int freed;
	int order;
};

/* --- Simple insertion and removal test --- */

TEST(selftest, simplemap)
{
	int i;
	const int nelems = 3;
	struct map_test_ctx *mctx = to_mctx(ctx);
	struct ktf_map tm;
	struct myelem e[nelems];

	if (mctx)
		tlog(T_DEBUG, "ctx %s", mctx->k.elem.key);
	else
		tlog(T_DEBUG, "ctx <none>");

	ktf_map_init(&tm, NULL, NULL);
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[0].foo, "foo"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[1].foo, "bar"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[2].foo, "zax"));

	for (i = 0; i < nelems; i++) {
		EXPECT_LONG_EQ(i, ktf_map_size(&tm));
		EXPECT_INT_EQ(0, ktf_map_insert(&tm, &e[i].foo));
	}
	EXPECT_LONG_EQ(i, ktf_map_size(&tm));

	/* Should be sorted alphabetically so we get 'bar' back: */
	EXPECT_ADDR_EQ(&e[1].foo, ktf_map_find_first(&tm));

	for (i = 0; i < nelems; i++) {
		EXPECT_LONG_EQ(nelems - i, ktf_map_size(&tm));
		EXPECT_ADDR_EQ(&e[i].foo, ktf_map_remove(&tm, e[i].foo.key));
	}
	EXPECT_LONG_EQ(0, ktf_map_size(&tm));
}

/* --- Reference counting test --- */

/* should be called when refcount is 0. */
static void myelem_free(struct ktf_map_elem *elem)
{
	struct myelem *myelem = container_of(elem, struct myelem, foo);

	myelem->freed = 1;
}

TEST(selftest, mapref)
{
	int i;
	const int nelems = 3;
	struct myelem e[nelems], *ep;
	struct ktf_map tm;
	struct ktf_map_elem *elem;

	ktf_map_init(&tm, NULL, myelem_free);
	/* Init map elems with "foo" "bar" "zax" */
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[0].foo, "foo"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[1].foo, "bar"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[2].foo, "zax"));

	/* Insert elems and drop our refcounts (map still holds ref) */
	for (i = 0; i < nelems; i++) {
		EXPECT_INT_EQ(0, ktf_map_insert(&tm, &e[i].foo));
		ktf_map_elem_put(&e[i].foo);
	}

	/* This macro takes (and drops) refcount for each elem */
	ktf_map_for_each_entry(ep, &tm, foo)
		ep->freed = 0;

	for (i = 0; i < nelems; i++) {
		elem = ktf_map_remove(&tm, e[i].foo.key);
		EXPECT_INT_EQ(0, e[i].freed);
		/* free our ref, now free function should be called. */
		ktf_map_elem_put(elem);
		EXPECT_INT_EQ(1, e[i].freed);
	}

	ktf_map_delete_all(&tm);
	EXPECT_LONG_EQ(0, ktf_map_size(&tm));
}

/* --- Compare function test --- */

/* key comparison function */
static int myelem_cmp(const char *key1, const char *key2)
{
	int i1 = *((int *)key1);
	int i2 = *((int *)key2);

	if (i1 < i2)
		return -1;
	else if (i1 > i2)
		return 1;
	return 0;
}

TEST(selftest, mapcmpfunc)
{
	int i;
	const int nelems = 3;
	struct myelem e[nelems], *ep;
	struct ktf_map tm;

	ktf_map_init(&tm, myelem_cmp, NULL);
	/* Init map elems with keys "foo" "bar" "zax" */
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[0].foo, "foo"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[1].foo, "bar"));
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e[2].foo, "zax"));

	/* Insert elems with order values 3, 2, 1. Ensure we see order
	 * 1, 2, 3 on retrieval.
	 */
	for (i = 0; i < nelems; i++) {
		e[i].order = nelems - i;
		EXPECT_INT_EQ(0, ktf_map_elem_init(&e[i].foo,
						   (char *)&e[i].order));
		EXPECT_INT_EQ(0, ktf_map_insert(&tm, &e[i].foo));
	}
	i = 1;
	/* Ensure ordering via compare function is respected */
	ktf_map_for_each_entry(ep, &tm, foo)
		EXPECT_INT_EQ(ep->order, i++);

	ktf_map_delete_all(&tm);
	EXPECT_LONG_EQ(0, ktf_map_size(&tm));
}

/* --- Verify that key name is truncated at KTF_MAX_NAME length --- */

TEST(selftest, map_keyoverflow)
{
	struct myelem e;
	struct ktf_map tm;
	char jumbokey[KTF_MAX_NAME + 2];
	char jumbokey_truncated[KTF_MAX_NAME + 1];

	ktf_map_init(&tm, NULL, NULL);
	memset(jumbokey, 'x', KTF_MAX_NAME + 1);
	memset(jumbokey_truncated, 'x', KTF_MAX_NAME);
	jumbokey_truncated[KTF_MAX_NAME] = '\0';
	EXPECT_INT_EQ(0, ktf_map_elem_init(&e.foo, jumbokey));
	EXPECT_TRUE(strcmp(e.foo.key, jumbokey_truncated) == 0);
}

TEST(selftest, dummy)
{
	EXPECT_TRUE(true);
}

TEST(selftest, wrongversion)
{
	tlog(T_INFO, "This test should never have run - wrong version\n!!!");
	EXPECT_TRUE(false);
}

static void add_map_tests(void)
{
	ADD_TEST(dummy);
	ADD_TEST_TO(dual_handle, simplemap);
	ADD_TEST_TO(dual_handle, mapref);
	ADD_TEST_TO(dual_handle, mapcmpfunc);
	ADD_TEST(map_keyoverflow);

	/* This should fail */
	ADD_TEST_TO(wrongversion_handle, wrongversion);
}

static int probecount;
static int proberet;

KTF_ENTRY_PROBE(printk, printkhandler)
{
	probecount++;

	KTF_ENTRY_PROBE_RETURN(0);
}

TEST(selftest, probeentry)
{
	probecount = 0;
	ASSERT_INT_EQ(KTF_REGISTER_ENTRY_PROBE(printk, printkhandler), 0);
	tlog(T_INFO, "Testing kprobe entry...");
	ASSERT_INT_GT_GOTO(probecount, 0, done);

done:
	KTF_UNREGISTER_ENTRY_PROBE(printk, printkhandler);
}

static int override_failed;

noinline int myfunc(int i)
{
	override_failed = 1;
	return i;
}

KTF_OVERRIDE(myfunc, myfunc_override)
{
	KTF_SET_RETURN_VALUE(0);
	KTF_OVERRIDE_RETURN;
}

TEST(selftest, override)
{
	override_failed = 0;

	ASSERT_INT_EQ(KTF_REGISTER_OVERRIDE(myfunc, myfunc_override), 0);

	(void)myfunc(0);

	/* Verify override function runs instead. */
	ASSERT_TRUE_GOTO(override_failed == 0, done);

	/* Verify override function modifies return value. */
	ASSERT_INT_EQ_GOTO(myfunc(100), 0, done);
	ASSERT_TRUE_GOTO(override_failed == 0, done);
done:
	KTF_UNREGISTER_OVERRIDE(myfunc, myfunc_override);
}

noinline int probesum(int a, int b)
{
	tlog(T_INFO, "Adding %d + %d", a, b);
	return a + b;
}

KTF_RETURN_PROBE(probesum, probesumhandler)
{
	tlog(T_DEBUG, "return value before modifying %ld",
	     regs_return_value(regs));
	KTF_SET_RETURN_VALUE(-1);
	tlog(T_DEBUG, "return value after modifying %ld",
	     regs_return_value(regs));
	return 0;
}

KTF_RETURN_PROBE(printk, printkrethandler)
{
	proberet = KTF_RETURN_VALUE();

	return 0;
}

TEST(selftest, probereturn)
{
	char *teststr = "Testing kprobe return...";

	proberet = -1;
	ASSERT_INT_EQ_GOTO(KTF_REGISTER_RETURN_PROBE(printk, printkrethandler),
			   0, done);
	printk(KERN_INFO "%s", teststr);
	ASSERT_INT_EQ_GOTO(proberet, strlen(teststr), done);

	/* Now test modification of return value */
	ASSERT_INT_EQ_GOTO(probesum(1, 1), 2, done);
	ASSERT_INT_EQ_GOTO(KTF_REGISTER_RETURN_PROBE(probesum, probesumhandler),
			   0, done);
	ASSERT_INT_EQ_GOTO(probesum(1, 1), -1, done);
done:
	KTF_UNREGISTER_RETURN_PROBE(printk, printkrethandler);
	KTF_UNREGISTER_RETURN_PROBE(probesum, probesumhandler);
}

static void add_probe_tests(void)
{
	ADD_TEST(probeentry);
	ADD_TEST(probereturn);
	ADD_TEST(override);
}

noinline void cov_counted(void)
{
	tlog(T_INFO, "got called!");
}

noinline void *doalloc(struct kmem_cache *c, size_t sz)
{
	if (c)
		return kmem_cache_alloc(c, GFP_KERNEL);
	return kmalloc(sz, GFP_KERNEL);
}

TEST(selftest, cov)
{
	int foundp1 = 0, foundp2 = 0, foundp3 = 0, foundp4 = 0;
	struct ktf_cov_entry *e;
	struct ktf_cov_mem *m;
	char *p1 = NULL, *p2 = NULL, *p3 = NULL, *p4 = NULL;
	struct kmem_cache *c = NULL;
	int oldcount;

	c = kmem_cache_create("selftest_cov_cache",
			      32, 0,
			     SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	ASSERT_ADDR_NE(NULL, c);

	tlog(T_INFO, "Allocated cache %p : %s %u\n", c, c->name, c->object_size);
	ASSERT_INT_EQ(0, ktf_cov_enable((THIS_MODULE)->name, KTF_COV_OPT_MEM));

	e = ktf_cov_entry_find((unsigned long)cov_counted, 0);
	ASSERT_ADDR_NE_GOTO(e, NULL, done);
	oldcount = e->count;
	ktf_cov_entry_put(e);
	cov_counted();
	e = ktf_cov_entry_find((unsigned long)cov_counted, 0);
	ASSERT_ADDR_NE_GOTO(e, NULL, done);
	if (e) {
		ASSERT_INT_EQ(e->count, oldcount + 1);
		ktf_cov_entry_put(e);
	}

	/* Need to call a noinline fn to do allocs since this test function
	 * will be inlined; and to track allocations they need to come
	 * from this module.  Don't need to do the same for kfree since
	 * we check every kfree() to see if it is freeing a tracked allocation.
	 */
	p1 = doalloc(NULL, 8);
	ASSERT_ADDR_NE_GOTO(p1, NULL, done);
	p2 = doalloc(NULL, 16);
	ASSERT_ADDR_NE_GOTO(p2, NULL, done);
	p3 = doalloc(c, 0);
	ASSERT_ADDR_NE_GOTO(p3, NULL, done);
	p4 = doalloc(c, 0);
	ASSERT_ADDR_NE_GOTO(p4, NULL, done);

	ktf_for_each_cov_mem(m) {
		if (m->key.address == (unsigned long)p1)
			foundp1 = 1;
		if (m->key.address == (unsigned long)p2 && m->key.size == 16)
			foundp2 = 1;
		if (m->key.address == (unsigned long)p3 && m->key.size == 32)
			foundp3 = 1;
		if (m->key.address == (unsigned long)p4)
			foundp4 = 1;
	}
	ASSERT_INT_EQ_GOTO(foundp1, 1, done);
	ASSERT_INT_EQ_GOTO(foundp2, 1, done);
	ASSERT_INT_EQ_GOTO(foundp3, 1, done);
	ASSERT_INT_EQ_GOTO(foundp4, 1, done);
	kfree(p1);
	kmem_cache_free(c, p4);
	/* Didn't free p2/p3 - should still be on our cov_mem list */
	foundp1 = 0;
	foundp2 = 0;
	foundp3 = 0;
	foundp4 = 0;
	ktf_for_each_cov_mem(m) {
		if (m->key.address == (unsigned long)p1)
			foundp1 = 1;
		if (m->key.address == (unsigned long)p2)
			foundp2 = 1;
		if (m->key.address == (unsigned long)p3)
			foundp3 = 1;
		if (m->key.address == (unsigned long)p4)
			foundp4 = 1;
	}
	ASSERT_INT_EQ_GOTO(foundp2, 1, done);
	ASSERT_INT_EQ_GOTO(foundp3, 1, done);
	ASSERT_INT_EQ_GOTO(foundp1, 0, done);
	ASSERT_INT_EQ_GOTO(foundp4, 0, done);
done:
	kfree(p2);
	if (p3)
		kmem_cache_free(c, p3);
	ktf_cov_disable((THIS_MODULE)->name);
	kmem_cache_destroy(c);
}

static void add_cov_tests(void)
{
	ADD_TEST(cov);
}

KTF_THREAD(test_thread)
{
	/* ensure assertions can work in thread context */
	ASSERT_INT_EQ(1, 1);
}

#define NUM_TEST_THREADS 20

static struct ktf_thread test_threads[NUM_TEST_THREADS];

TEST(selftest, thread)
{
	int assertions, i;

	for (i = 0; i < NUM_TEST_THREADS; i++) {
		KTF_THREAD_INIT(test_thread, &test_threads[i]);
		KTF_THREAD_RUN(&test_threads[i]);
	}
	for (i = 0; i < NUM_TEST_THREADS; i++)
		KTF_THREAD_WAIT_COMPLETED(&test_threads[i]);

	assertions = (int)ktf_get_assertion_count();

	/* Verify assertion in thread */
	ASSERT_INT_EQ(assertions, NUM_TEST_THREADS);
}

static void add_thread_tests(void)
{
	ADD_TEST(thread);
}

static int selftest_module_var;

/*
 * Test that ktf_find_symbol works both for module symbols and
 * core kernel symbols:
 */
TEST(selftest, symbol)
{
	/* Verify finding kernel-internal symbol works. */
	ASSERT_ADDR_NE(ktf_find_symbol(NULL, "skbuff_head_cache"), NULL);

	/* Verify finding module symbols works, both when we specify the
	 * module name and when we don't.
	 */
	ASSERT_ADDR_EQ(ktf_find_symbol(NULL, "selftest_module_var"),
		       &selftest_module_var);

	ASSERT_ADDR_EQ(ktf_find_symbol("selftest", "selftest_module_var"),
		       &selftest_module_var);
}

static void add_symbol_tests(void)
{
	ADD_TEST(symbol);
}

static int __init selftest_init(void)
{
	int ret = ktf_context_add(&dual_handle, &s_mctx[0].k, "map1");

	if (ret)
		return ret;

	ret = ktf_context_add(&dual_handle, &s_mctx[1].k, "map2");
	if (ret)
		return ret;

	ret = ktf_context_add(&single_handle, &s_mctx[2].k, "map3");
	if (ret)
		return ret;

	ktf_resolve_symbols();

	add_map_tests();
	add_probe_tests();
	add_cov_tests();
	add_thread_tests();
	add_hybrid_tests();
	add_symbol_tests();
	tlog(T_INFO, "selftest: loaded\n");
	return 0;
}

static void __exit selftest_exit(void)
{
	ktf_context_remove_all(&single_handle);
	ktf_context_remove_all(&dual_handle);
	KTF_HANDLE_CLEANUP(single_handle);
	KTF_HANDLE_CLEANUP(dual_handle);
	KTF_HANDLE_CLEANUP(no_handle);
	KTF_CLEANUP();
	tlog(T_INFO, "selftest: unloaded\n");
	/* Nothing to do here */
}

module_init(selftest_init);
module_exit(selftest_exit);
