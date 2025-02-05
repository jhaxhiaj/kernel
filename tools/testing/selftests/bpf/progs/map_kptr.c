// SPDX-License-Identifier: GPL-2.0
#include <vmlinux.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_helpers.h>

struct map_value {
	struct prog_test_ref_kfunc __kptr *unref_ptr;
	struct prog_test_ref_kfunc __kptr_ref *ref_ptr;
};

struct array_map {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, int);
	__type(value, struct map_value);
	__uint(max_entries, 1);
} array_map SEC(".maps");

struct hash_map {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, int);
	__type(value, struct map_value);
	__uint(max_entries, 1);
} hash_map SEC(".maps");

struct hash_malloc_map {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, int);
	__type(value, struct map_value);
	__uint(max_entries, 1);
	__uint(map_flags, BPF_F_NO_PREALLOC);
} hash_malloc_map SEC(".maps");

struct lru_hash_map {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__type(key, int);
	__type(value, struct map_value);
	__uint(max_entries, 1);
} lru_hash_map SEC(".maps");

#define DEFINE_MAP_OF_MAP(map_type, inner_map_type, name)       \
	struct {                                                \
		__uint(type, map_type);                         \
		__uint(max_entries, 1);                         \
		__uint(key_size, sizeof(int));                  \
		__uint(value_size, sizeof(int));                \
		__array(values, struct inner_map_type);         \
	} name SEC(".maps") = {                                 \
		.values = { [0] = &inner_map_type },            \
	}

DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_ARRAY_OF_MAPS, array_map, array_of_array_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_ARRAY_OF_MAPS, hash_map, array_of_hash_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_ARRAY_OF_MAPS, hash_malloc_map, array_of_hash_malloc_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_ARRAY_OF_MAPS, lru_hash_map, array_of_lru_hash_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_HASH_OF_MAPS, array_map, hash_of_array_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_HASH_OF_MAPS, hash_map, hash_of_hash_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_HASH_OF_MAPS, hash_malloc_map, hash_of_hash_malloc_maps);
DEFINE_MAP_OF_MAP(BPF_MAP_TYPE_HASH_OF_MAPS, lru_hash_map, hash_of_lru_hash_maps);

extern struct prog_test_ref_kfunc *bpf_kfunc_call_test_acquire(unsigned long *sp) __ksym;
extern struct prog_test_ref_kfunc *
bpf_kfunc_call_test_kptr_get(struct prog_test_ref_kfunc **p, int a, int b) __ksym;
extern void bpf_kfunc_call_test_release(struct prog_test_ref_kfunc *p) __ksym;

static void test_kptr_unref(struct map_value *v)
{
	struct prog_test_ref_kfunc *p;

	p = v->unref_ptr;
	/* store untrusted_ptr_or_null_ */
	v->unref_ptr = p;
	if (!p)
		return;
	if (p->a + p->b > 100)
		return;
	/* store untrusted_ptr_ */
	v->unref_ptr = p;
	/* store NULL */
	v->unref_ptr = NULL;
}

static void test_kptr_ref(struct map_value *v)
{
	struct prog_test_ref_kfunc *p;

	p = v->ref_ptr;
	/* store ptr_or_null_ */
	v->unref_ptr = p;
	if (!p)
		return;
	if (p->a + p->b > 100)
		return;
	/* store NULL */
	p = bpf_kptr_xchg(&v->ref_ptr, NULL);
	if (!p)
		return;
	if (p->a + p->b > 100) {
		bpf_kfunc_call_test_release(p);
		return;
	}
	/* store ptr_ */
	v->unref_ptr = p;
	bpf_kfunc_call_test_release(p);

	p = bpf_kfunc_call_test_acquire(&(unsigned long){0});
	if (!p)
		return;
	/* store ptr_ */
	p = bpf_kptr_xchg(&v->ref_ptr, p);
	if (!p)
		return;
	if (p->a + p->b > 100) {
		bpf_kfunc_call_test_release(p);
		return;
	}
	bpf_kfunc_call_test_release(p);
}

static void test_kptr_get(struct map_value *v)
{
	struct prog_test_ref_kfunc *p;

	p = bpf_kfunc_call_test_kptr_get(&v->ref_ptr, 0, 0);
	if (!p)
		return;
	if (p->a + p->b > 100) {
		bpf_kfunc_call_test_release(p);
		return;
	}
	bpf_kfunc_call_test_release(p);
}

static void test_kptr(struct map_value *v)
{
	test_kptr_unref(v);
	test_kptr_ref(v);
	test_kptr_get(v);
}

SEC("tc")
int test_map_kptr(struct __sk_buff *ctx)
{
	struct map_value *v;
	int i, key = 0;

#define TEST(map)					\
	v = bpf_map_lookup_elem(&map, &key);		\
	if (!v)						\
		return 0;				\
	test_kptr(v)

	TEST(array_map);
	TEST(hash_map);
	TEST(hash_malloc_map);
	TEST(lru_hash_map);

#undef TEST
	return 0;
}

SEC("tc")
int test_map_in_map_kptr(struct __sk_buff *ctx)
{
	struct map_value *v;
	int i, key = 0;
	void *map;

#define TEST(map_in_map)                                \
	map = bpf_map_lookup_elem(&map_in_map, &key);   \
	if (!map)                                       \
		return 0;                               \
	v = bpf_map_lookup_elem(map, &key);		\
	if (!v)						\
		return 0;				\
	test_kptr(v)

	TEST(array_of_array_maps);
	TEST(array_of_hash_maps);
	TEST(array_of_hash_malloc_maps);
	TEST(array_of_lru_hash_maps);
	TEST(hash_of_array_maps);
	TEST(hash_of_hash_maps);
	TEST(hash_of_hash_malloc_maps);
	TEST(hash_of_lru_hash_maps);

#undef TEST
	return 0;
}

char _license[] SEC("license") = "GPL";
