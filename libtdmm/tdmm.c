#include "tdmm.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>

typedef struct block_hdr block_hdr;
struct block_hdr {
	size_t size;
	int free;
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

static size_t round4(size_t n)
{
	size_t r = n % 4;
	if (r == 0) return n;
	return n + (4 - r);
}

static size_t map_need(size_t payload)
{
	size_t min_map = payload + sizeof(block_hdr);
	size_t base = 64 * 1024;
	size_t ask = (min_map > base) ? min_map : base;
	size_t remainder = ask % g_page;
	if (remainder) ask += (g_page - remainder);
	return ask;
}

static void list_insert(block_hdr *b)
{
	if (!g_head) {
		g_head = b;
		return;
	}

	block_hdr *cur = g_head;
	if ((char *)b < (char *)g_head) {
		b->next = g_head;
		g_head->prev = b;
		g_head = b;
		return;
	}

	while (cur->next && (char *)cur->next < (char *)b) cur = cur->next;
	b->next = cur->next;
	b->prev = cur;
	if (cur->next) cur->next->prev = b;
	cur->next = b;
}

static block_hdr *new_region(size_t need_payload)
{
	size_t ask = map_need(need_payload);
	void *p = mmap(NULL, ask, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) return NULL;

	block_hdr *b = (block_hdr *)p;
	b->size = ask - sizeof(block_hdr);
	b->free = 1;
	b->next = NULL;
	b->prev = NULL;
	g_mapped += ask;
	list_insert(b);
	return b;
}

static block_hdr *pick_block(size_t need)
{
	block_hdr *cur = g_head;
	block_hdr *pick = NULL;

	if (g_strat == FIRST_FIT) {
		while (cur) {
			g_steps++;
			if (cur->free && cur->size >= need) return cur;
			cur = cur->next;
		}
		return NULL;
	}

	if (g_strat == BEST_FIT) {
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
		cur = cur->next;}
	
	return pick;
}

static void split_if_needed(block_hdr *b, size_t need)
{
	size_t min_left = sizeof(block_hdr) + 4;
	if (b->size < need + min_left) return;

	char *new_pos = (char *)b + sizeof(block_hdr) + need;
	block_hdr *nxt = (block_hdr *)new_pos;
	nxt->size = b->size - need - sizeof(block_hdr);
	nxt->free = 1;
	nxt->next = b->next;
	nxt->prev = b;
	if (b->next) b->next->prev = nxt;
	b->next = nxt;
	b->size = need;
}

static int right_next_to(block_hdr *a, block_hdr *b)
{
	char *end_a = (char *)a + sizeof(block_hdr) + a->size;
	return end_a == (char *)b;}

static void merge_one(block_hdr *a, block_hdr *b)
{
	a->size += sizeof(block_hdr) + b->size;
	a->next = b->next;
	if (b->next) b->next->prev = a;
}

void t_init(alloc_strat_e strat) {
	g_strat = strat;
	g_head = NULL;
	g_page = (size_t)sysconf(_SC_PAGESIZE);
	if (g_page == 0) g_page = 4096;
	g_mapped = 0;
	g_live = 0;
	g_peak = 0;
	g_steps = 0;
}

void *t_malloc(size_t size)
{
	if (size == 0) return NULL;
	size_t need = round4(size);

	block_hdr *b = pick_block(need);
	if (!b) {
		if (!new_region(need)) return NULL;
		b = pick_block(need);
		if (!b) return NULL;
	}

	split_if_needed(b, need);
	b->free = 0;
	g_live += b->size;
	if (g_live > g_peak) g_peak = g_live;
	return (void *)((char *)b + sizeof(block_hdr));
}

void t_free(void *ptr)
{
	if (!ptr) return;

	block_hdr *cur = g_head;
	while (cur) {
		void *payload = (void *)((char *)cur + sizeof(block_hdr));
		if (payload == ptr) break;
		cur = cur->next;}

	if (!cur) {
		fprintf(stderr, "bad free pointer\n");
		return;}
	if (cur->free) return;

	cur->free = 1;
	if (g_live >= cur->size) g_live -= cur->size;
	else g_live = 0;

	if (cur->prev && cur->prev->free && right_next_to(cur->prev, cur)) {
		merge_one(cur->prev, cur);
		cur = cur->prev;
	}
	if (cur->next && cur->next->free && right_next_to(cur, cur->next)) {
		merge_one(cur, cur->next);
	}
}

size_t t_total_mapped(void)
{
	return g_mapped;
}

size_t t_live_aligned(void)
{
	return g_live;
}

size_t t_peak_aligned(void)
{
	return g_peak;
}

size_t t_search_steps(void)
{
	return g_steps;
}
