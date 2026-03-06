#include "tdmm.h"
#include <stdio.h>
#include <sys/time.h>

typedef struct {
	void *ptr;
	size_t req;
} rec_t;

static const char* name_of(alloc_strat_e p) {
	if (p == FIRST_FIT) return "FIRST_FIT";
	if (p == BEST_FIT)  return "BEST_FIT";
	if (p == WORST_FIT) return "WORST_FIT";
	if (p == BUDDY_FIT) return "BUDDY_FIT";
	return "MIXED_FIT";
}

static double t_us() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static void push_event(FILE *f, const char *nm, size_t ev)
{
	size_t live = t_live_aligned();
	size_t mapd = t_total_mapped();
	size_t oh = t_overhead_bytes();
	double util = 0.0;

	if (mapd != 0) util = (100.0 * (double)live) / (double)mapd;
	fprintf(f, "%s,%zu,%zu,%zu,%zu,%.6f\n", nm, ev, live, mapd, oh, util);
}

static void run_trace_for_policy(alloc_strat_e pol, FILE *trace, FILE *summary)
{
	rec_t a[256];
	size_t i, ev = 0, cnt = 0;
	size_t peak_live = 0, peak_oh = 0;
	double util_acc = 0.0, oh_acc = 0.0;
	const char *nm = name_of(pol);

	for (i=0;i<256;i++) { a[i].ptr = NULL; a[i].req = 0; }
	t_init(pol);

	for (i=0;i<256;i++) {
		size_t need = 8 + ((i * 13) % 120);
		a[i].ptr = t_malloc(need);
		a[i].req = need;
		ev++;
		push_event(trace, nm, ev);
		util_acc += t_total_mapped() ? (100.0 * (double)t_live_aligned()) / (double)t_total_mapped() : 0.0;
		oh_acc += (double)t_overhead_bytes();
		cnt++;
		if (t_live_aligned() > peak_live) peak_live = t_live_aligned();
		if (t_overhead_bytes() > peak_oh) peak_oh = t_overhead_bytes();
	}

	for (i=0;i<256;i+=2) {
		if (a[i].ptr) t_free(a[i].ptr);
		a[i].ptr = NULL; a[i].req = 0;
		ev++;
		push_event(trace, nm, ev);
		util_acc += t_total_mapped() ? (100.0 * (double)t_live_aligned()) / (double)t_total_mapped() : 0.0;
		oh_acc += (double)t_overhead_bytes();
		cnt++;
		if (t_live_aligned() > peak_live) peak_live = t_live_aligned();
		if (t_overhead_bytes() > peak_oh) peak_oh = t_overhead_bytes();
	}

	for (i = 0; i < 128; i++) {
		size_t need = 16 + ((i * 37) % 200);
		size_t at = i * 2;
		a[at].ptr = t_malloc(need);
		a[at].req = need;
		ev++;
		push_event(trace, nm, ev);
		util_acc += t_total_mapped() ? (100.0 * (double)t_live_aligned()) / (double)t_total_mapped() : 0.0;
		oh_acc += (double)t_overhead_bytes();
		cnt++;
		if (t_live_aligned() > peak_live) peak_live = t_live_aligned();
		if (t_overhead_bytes() > peak_oh) peak_oh = t_overhead_bytes();
	}

	for (i=0;i<256;i++) {
		if (!a[i].ptr) continue;
		t_free(a[i].ptr);
		a[i].ptr = NULL; a[i].req = 0;
		ev++;
		push_event(trace, nm, ev);
		util_acc += t_total_mapped() ? (100.0 * (double)t_live_aligned()) / (double)t_total_mapped() : 0.0;
		oh_acc += (double)t_overhead_bytes();
		cnt++;
		if (t_live_aligned() > peak_live) peak_live = t_live_aligned();
		if (t_overhead_bytes() > peak_oh) peak_oh = t_overhead_bytes();
	}

	fprintf(summary, "%s,%.6f,%zu,%.6f,%zu,%zu\n",
		nm,
		cnt ? util_acc / (double)cnt : 0.0,
		peak_live,
		cnt ? oh_acc / (double)cnt : 0.0,
		peak_oh,
		t_total_mapped());
}

static int reps_for(size_t sz)
{
	if (sz <= 1024) return 400;
	if (sz <= 65536) return 160;
	if (sz <= 1048576) return 60;
	return 15;
}

static void warmup_heap(void)
{
	rec_t b[96];
	size_t i;
	for (i=0;i<96;i++) { b[i].ptr = NULL; b[i].req = 0; }

	for (i = 0; i < 96; i++) {
		size_t need = 24 + ((i * 29) % 320);
		b[i].ptr = t_malloc(need);
		b[i].req = need;
	}

	for (i = 0; i < 96; i += 3) {
		if (!b[i].ptr) continue;
		t_free(b[i].ptr);
		b[i].ptr = NULL;
	}
}

static void run_speed_for_policy(alloc_strat_e pol, FILE *speed)
{
	const char *nm = name_of(pol);
	size_t sz = 1;

	while (sz <= 8 * 1024 * 1024) {
		int reps = reps_for(sz), i, ok = 0;
		double msum = 0.0, fsum = 0.0;

		t_init(pol);
		warmup_heap();

		for (i = 0; i < reps; i++) {
			double a, b, c;
			void *p;

			a = t_us();
			p = t_malloc(sz);
			b = t_us();
			if (!p) break;
			t_free(p);
			c = t_us();

			msum += (b - a);
			fsum += (c - b);
			ok++;
		}

		if (ok > 0) {
			fprintf(speed, "%s,%zu,%.6f,%.6f\n", nm, sz, msum / (double)ok, fsum / (double)ok);
		}

		if (sz == 8 * 1024 * 1024) break;
		sz *= 2;
	}
}

int main(int argc, char **argv)
{
	FILE *sumf, *tracef, *speedf;
	(void)argc; (void)argv;

	sumf = fopen("report_summary.csv", "w");
	tracef = fopen("utilization_trace.csv", "w");
	speedf = fopen("speed_by_size.csv", "w");

	if (!sumf || !tracef || !speedf) {
		fprintf(stderr, "could not open output files\n");
		if (sumf) fclose(sumf);
		if (tracef) fclose(tracef);
		if (speedf) fclose(speedf);
		return 1;
	}

	fprintf(sumf, "policy,avg_utilization_pct,peak_live_bytes,avg_overhead_bytes,peak_overhead_bytes,mapped_bytes\n");
	fprintf(tracef, "policy,event,live_bytes,mapped_bytes,overhead_bytes,utilization_pct\n");
	fprintf(speedf, "policy,size_bytes,tmalloc_avg_us,tfree_avg_us\n");

	run_trace_for_policy(FIRST_FIT, tracef, sumf);
	run_trace_for_policy(BEST_FIT, tracef, sumf);
	run_trace_for_policy(WORST_FIT, tracef, sumf);
	run_trace_for_policy(BUDDY_FIT, tracef, sumf);
	run_trace_for_policy(MIXED_FIT, tracef, sumf);

	run_speed_for_policy(FIRST_FIT, speedf);
	run_speed_for_policy(BEST_FIT, speedf);
	run_speed_for_policy(WORST_FIT, speedf);
	run_speed_for_policy(BUDDY_FIT, speedf);
	run_speed_for_policy(MIXED_FIT, speedf);

	fclose(sumf);
	fclose(tracef);
	fclose(speedf);
	printf("wrote report_summary.csv, utilization_trace.csv, and speed_by_size.csv\n");
	return 0;
}
