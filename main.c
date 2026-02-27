#include "tdmm.h"
#include <stdio.h>

typedef struct {
	void *p;
	size_t sz;
} slot;

static size_t round4_local(size_t n)
{
	size_t r = n % 4;
	if (r == 0) return n;
	return n + (4 - r);
}

static void run_case(alloc_strat_e strat, const char *name)
{
	slot s[256];
	size_t i = 0;
	size_t events = 0;
	size_t req_live = 0;
	size_t req_peak = 0;
	size_t aligned_live = 0;
	size_t aligned_peak = 0;
	size_t failed = 0;

	for (i = 0; i < 256; i++) {
		s[i].p = NULL;
		s[i].sz = 0;
	}

	t_init(strat);

	for (i = 0; i < 256; i++) {
		size_t need = 8 + ((i * 13) % 120);
		void *p = t_malloc(need);
		events++;
		if (!p) {
			failed++;
			continue;
		}
		s[i].p = p;
		s[i].sz = need;
		req_live += need;
		aligned_live += round4_local(need);
		if (req_live > req_peak) req_peak = req_live;
		if (aligned_live > aligned_peak) aligned_peak = aligned_live;
	}

	for (i = 0; i < 256; i += 2) {
		if (!s[i].p) continue;
		t_free(s[i].p);
		events++;
		req_live -= s[i].sz;
		aligned_live -= round4_local(s[i].sz);
		s[i].p = NULL;
		s[i].sz = 0;
	}

	for (i = 0; i < 128; i++) {
		size_t need = 16 + ((i * 37) % 200);
		void *p = t_malloc(need);
		events++;
		if (!p) {
			failed++;
			continue;
		}
		size_t idx = i * 2;
		s[idx].p = p;
		s[idx].sz = need;
		req_live += need;
		aligned_live += round4_local(need);
		if (req_live > req_peak) req_peak = req_live;
		if (aligned_live > aligned_peak) aligned_peak = aligned_live;
	}

	for (i = 0; i < 256; i++) {
		if (!s[i].p) continue;
		t_free(s[i].p);
		events++;
		req_live -= s[i].sz;
		aligned_live -= round4_local(s[i].sz);
		s[i].p = NULL;
		s[i].sz = 0;
	}

	size_t mapped = t_total_mapped();
	size_t peak_from_tdmm = t_peak_aligned();
	size_t search = t_search_steps();
	double util = 0.0;
	if (mapped > 0) util = (100.0 * (double)peak_from_tdmm) / (double)mapped;

	printf("\n%s\n", name);
	printf("events: %zu\n", events);
	printf("failed malloc: %zu\n", failed);
	printf("mapped bytes: %zu\n", mapped);
	printf("peak aligned bytes (tdmm): %zu\n", peak_from_tdmm);
	printf("peak requested bytes (test): %zu\n", req_peak);
	printf("peak aligned bytes (test): %zu\n", aligned_peak);
	printf("search steps: %zu\n", search);
	printf("utilization (peak_aligned / mapped): %.2f%%\n", util);
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;
	run_case(FIRST_FIT, "FIRST_FIT");
	run_case(BEST_FIT, "BEST_FIT");
	run_case(WORST_FIT, "WORST_FIT");
	return 0;
}
