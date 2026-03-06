#include "tdmm.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

typedef struct block_hdr block_hdr;
struct block_hdr {
	size_t size;
	int free;
	int buddy;
	uintptr_t region_base;
	block_hdr *next;
	block_hdr *prev;
};

static alloc_strat_e g_strat = FIRST_FIT;
static block_hdr *g_head = NULL;
static size_t g_page = 0;
static size_t g_mapped = 0;
static size_t g_live = 0;
static size_t g_peak = 0;
static size_t g_steps = 0;
static size_t g_mix_turn = 0;

static size_t round4(size_t n){
	size_t r = n % 4;
	if (!r) return n;
	return n + (4-r);
}

static size_t next_pow2(size_t x){
	size_t p = 1;
	while (p < x && p < ((size_t)-1 >> 1)) {
		p <<= 1;
	}
	return p;
}

static size_t map_need(size_t payload)
{
	size_t min_map = payload + sizeof(block_hdr);
	size_t base = 64 * 1024;
	size_t ask = (min_map > base) ? min_map : base;
	size_t rem = ask % g_page;
	if (rem) ask += (g_page - rem);
	return ask;
}

static void list_insert(block_hdr *b){
	if (!g_head) {
		g_head = b;
		return;
	}

	if ((uintptr_t)b < (uintptr_t)g_head) {
		b->next = g_head;
		g_head->prev = b;
		g_head = b;
		return;
	}

	block_hdr *walk = g_head;
	while (walk->next && (uintptr_t)walk->next < (uintptr_t)b) walk = walk->next;
	b->next = walk->next;
	b->prev = walk;
	if (walk->next) walk->next->prev = b;
	walk->next = b;
}

static block_hdr *new_region_regular(size_t need_payload)
{
	size_t ask = map_need(need_payload);
	void *p = mmap(NULL, ask, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) return NULL;

	block_hdr *b = (block_hdr *)p;
	b->size = ask - sizeof(block_hdr);
	b->free = 1;
	b->buddy = 0;
	b->region_base = (uintptr_t)p;
	b->next = NULL;
	b->prev = NULL;
	g_mapped += ask;
	list_insert(b);
	return b;
}

static block_hdr *new_region_buddy(size_t need_payload)
{
	size_t need_total = need_payload + sizeof(block_hdr);
	size_t base = 64 * 1024;
	size_t ask = next_pow2((need_total > base) ? need_total : base);
	void *p = mmap(NULL, ask, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) return NULL;

	block_hdr *b = (block_hdr *)p;
	b->size = ask - sizeof(block_hdr);
	b->free = 1;
	b->buddy = 1;
	b->region_base = (uintptr_t)p;
	b->next = NULL;
	b->prev = NULL;
	g_mapped += ask;
	list_insert(b);
	return b;
}

static block_hdr *pick_block(size_t need, alloc_strat_e how){
	block_hdr *cur = g_head;
	block_hdr *pick = NULL;

	if (how == FIRST_FIT) {
		while (cur) {
			g_steps++;
			if (cur->free && cur->size >= need) return cur;
			cur = cur->next;
		}
		return NULL;
	}

	if (how == BEST_FIT) {
		while (cur) {
			g_steps++;
			if (cur->free && cur->size >= need) {
				if (!pick || cur->size < pick->size) pick = cur;
			}
			cur = cur->next;
		}
		return pick;
	}

	while (cur) {
		g_steps++;
		if (cur->free && cur->size >= need) {
			if (!pick || cur->size > pick->size) pick = cur;
		}
		cur = cur->next;
	}

	return pick;
}

static void split_if_needed(block_hdr *b, size_t need){
	size_t min_left = sizeof(block_hdr) + 4;
	if (b->size < need + min_left) return;

	char *new_pos = (char *)b + sizeof(block_hdr) + need;
	block_hdr *nxt = (block_hdr *)new_pos;
	nxt->size = b->size - need - sizeof(block_hdr);
	nxt->free = 1;
	nxt->buddy = 0;
	nxt->region_base = b->region_base;
	nxt->next = b->next;
	nxt->prev = b;
	if (b->next) b->next->prev = nxt;
	b->next = nxt;
	b->size = need;
}

static block_hdr *find_block_by_addr(uintptr_t addr)
{
	block_hdr *cur = g_head;
	while (cur) {
		if ((uintptr_t)cur == addr) return cur;
		cur = cur->next;
	}
	return NULL;
}

static void unlink_block(block_hdr *b){
	if (b->prev) b->prev->next = b->next;
	else g_head = b->next;
	if (b->next) b->next->prev = b->prev;
}

static block_hdr *buddy_pick(size_t need_payload){
	size_t need_total = need_payload + sizeof(block_hdr);
	size_t target = next_pow2(need_total);
	block_hdr *cur = g_head;
	block_hdr *pick = NULL;

	while (cur) {
		g_steps++;
		if (cur->free && cur->buddy) {
			size_t total = cur->size + sizeof(block_hdr);
			if (total >= target) {
				if (!pick || total < (pick->size + sizeof(block_hdr))) pick = cur;
			}
		}
		cur = cur->next;
	}
	return pick;
}

static void buddy_split(block_hdr *b, size_t need_payload){
	size_t need_total = next_pow2(need_payload + sizeof(block_hdr));

	while (1) {
		size_t total = b->size + sizeof(block_hdr);
		if (total / 2 < need_total) break;
		if (total / 2 < sizeof(block_hdr) + 4) break;

		size_t half = total / 2;
		block_hdr *nxt = (block_hdr *)((char *)b + half);
		nxt->size = half - sizeof(block_hdr);
		nxt->free = 1;
		nxt->buddy = 1;
		nxt->region_base = b->region_base;
		nxt->next = b->next;
		nxt->prev = b;
		if (b->next) b->next->prev = nxt;
		b->next = nxt;

		b->size = half - sizeof(block_hdr);
		b->buddy = 1;
	}
}

static void *alloc_regular(size_t need, alloc_strat_e how){
	block_hdr *b = pick_block(need, how);
	if (!b) {
		if (!new_region_regular(need)) return NULL;
		b = pick_block(need, how);
		if (!b) return NULL;
	}

	split_if_needed(b, need);
	b->free = 0;
	g_live += b->size;
	if (g_live > g_peak) g_peak = g_live;
	return (void *)((char *)b + sizeof(block_hdr));
}

static void *alloc_buddy(size_t need){
	block_hdr *b = buddy_pick(need);
	if (!b) {
		if (!new_region_buddy(need)) return NULL;
		b = buddy_pick(need);
		if (!b) return NULL;
	}

	buddy_split(b, need);
	b->free = 0;
	g_live += b->size;
	if (g_live > g_peak) g_peak = g_live;
	return (void *)((char *)b + sizeof(block_hdr));
}

static int right_next_to(block_hdr *a, block_hdr *b){
	uintptr_t end_a = (uintptr_t)a + sizeof(block_hdr) + a->size;
	return end_a == (uintptr_t)b;
}

static void merge_one(block_hdr *a, block_hdr *b){
	a->size += sizeof(block_hdr) + b->size;
	a->next = b->next;
	if (b->next) b->next->prev = a;
}

static void free_buddy(block_hdr *cur){
	cur->free = 1;
	if (g_live >= cur->size) g_live -= cur->size;
	else g_live = 0;

	while (1) {
		size_t total = cur->size + sizeof(block_hdr);
		uintptr_t my_off = (uintptr_t)cur - cur->region_base;
		uintptr_t buddy_off = my_off ^ total;
		uintptr_t buddy_addr = cur->region_base + buddy_off;
		block_hdr *bud = find_block_by_addr(buddy_addr);

		if (!bud) break;
		if (!bud->free) break;
		if (!bud->buddy) break;
		if (bud->region_base != cur->region_base) break;
		if (bud->size != cur->size) break;

		block_hdr *left = ((uintptr_t)bud < (uintptr_t)cur) ? bud : cur;
		block_hdr *right = (left == bud) ? cur : bud;
		unlink_block(right);
		left->size = (2 * total) - sizeof(block_hdr);
		left->free = 1;
		left->buddy = 1;
		cur = left;
	}
}

void t_init(alloc_strat_e strat){
	g_strat = strat;
	g_head = NULL;
	g_page = (size_t)sysconf(_SC_PAGESIZE);
	if (g_page == 0) g_page = 4096;
	g_mapped = 0;
	g_live = 0;
	g_peak = 0;
	g_steps = 0;
	g_mix_turn = 0;
}

void *t_malloc(size_t size){
	if (size == 0) return NULL;
	size_t need = round4(size);

	if (g_strat == BUDDY_FIT) {
		return alloc_buddy(need);
	}

	if (g_strat == MIXED_FIT) {
		alloc_strat_e how;
		if (g_mix_turn % 3 == 0) how = FIRST_FIT;
		else if (g_mix_turn % 3 == 1) how = BEST_FIT;
		else how = WORST_FIT;
		g_mix_turn++;
		return alloc_regular(need, how);
	}

	return alloc_regular(need, g_strat);
}

void t_free(void *ptr){
	if (!ptr) return;

	block_hdr *cur = g_head;
	while (cur) {
		void *payload = (void *)((char *)cur + sizeof(block_hdr));
		if (payload == ptr) break;
		cur = cur->next;
	}

	if (!cur) {
		fprintf(stderr, "bad free pointer\n");
		return;
	}
	if (cur->free) return;

	if (cur->buddy) {
		free_buddy(cur);
		return;
	}

	cur->free = 1;
	if (g_live >= cur->size) g_live -= cur->size;
	else g_live = 0;

	if (cur->prev && cur->prev->free && !cur->prev->buddy && right_next_to(cur->prev, cur)) {
		merge_one(cur->prev, cur);
		cur = cur->prev;
	}

	if (cur->next && cur->next->free && !cur->next->buddy && right_next_to(cur, cur->next)) {
		merge_one(cur, cur->next);
	}
}

size_t t_total_mapped(void){
	return g_mapped;
}

size_t t_live_aligned(void){
	return g_live;
}

size_t t_peak_aligned(void){
	return g_peak;
}

size_t t_search_steps(void){
	return g_steps;
}

size_t t_overhead_bytes(void){
	block_hdr *cur = g_head;
	size_t count = 0;
	while (cur) {
		count++;
		cur = cur->next;
	}
	return count * sizeof(block_hdr);
}
