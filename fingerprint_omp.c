/*
 * Load-Balanced Parallel Fingerprint Identification on Multicore CPUs
 * C11 + OpenMP. Academic demonstration of a simple, aligned-minutiae matcher.
 *
 * Build: gcc -O2 -std=c11 -fopenmp -Wall -Wextra fingerprint_omp.c -o fingerprint_omp
 * Demo:  ./fingerprint_omp --synthetic 6000 --threads 4 --schedule dynamic --early 1
 * Suite: ./fingerprint_omp --benchmark --synthetic 6000
 * Real:  ./fingerprint_omp --manifest manifest.txt --query-index 100 --threads 4
 *
 * NIST .xyt input: one line per minutia: x y theta quality
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <omp.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MAX_MINUTIAE 128
#define PATH_SIZE 1024

#ifdef _WIN32
static LARGE_INTEGER performance_frequency;

static int initialize_timer(void) {
    return QueryPerformanceFrequency(&performance_frequency) != 0;
}

static double wall_time(void) {
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) {
        fputs("QueryPerformanceCounter failed\n", stderr);
        abort();
    }
    return (double)counter.QuadPart / (double)performance_frequency.QuadPart;
}
#else
static int initialize_timer(void) {
    return 1;
}

static double wall_time(void) {
    return omp_get_wtime();
}
#endif

typedef struct { short x, y, angle, quality; } Minutia;
typedef struct { int count; int identity; Minutia pts[MAX_MINUTIAE]; } Fingerprint;
typedef struct { double busy; long comparisons; } ThreadStat;
typedef struct {
    int index, identity, score;
    long checked;
    double seconds;
    int active_threads;
    ThreadStat *stats;
} Result;
typedef struct {
    int synthetic, query_index, threads, repeats, early, threshold, chunk, benchmark;
    const char *manifest, *query_file, *schedule, *csv_path, *thread_csv_path;
} Options;

static uint32_t seed_state = 0x43ad75e1u;
static uint32_t rand32(void) {
    uint32_t x = seed_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    seed_state = x;
    return x;
}

static int better(int score, int index, int best_score, int best_index) {
    return score > best_score || (score == best_score && (best_index < 0 || index < best_index));
}

/* Greedy one-to-one minutiae matching; aligned positions and angles only.
 * Score is between 0 and 100, and identical nonempty templates score 100.
 * This is deliberately not an MCC/Bozorth3-grade biometric matcher.
 */
static int match_score(const Fingerprint *q, const Fingerprint *f) {
    if (q->count == 0 || f->count == 0) return 0;
    unsigned char used[MAX_MINUTIAE] = {0};
    int matched = 0;
    for (int i = 0; i < q->count; ++i) {
        int best = -1, best_d2 = 101;  /* squared distance <= 10^2 */
        for (int j = 0; j < f->count; ++j) {
            if (used[j]) continue;
            int dx = (int)q->pts[i].x - f->pts[j].x;
            int dy = (int)q->pts[i].y - f->pts[j].y;
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            if (d2 > 100 || d2 >= best_d2) continue;
            int da = abs((int)q->pts[i].angle - f->pts[j].angle) % 360;
            if (da > 180) da = 360 - da;
            if (da <= 20) { best = j; best_d2 = (int)d2; }
        }
        if (best >= 0) { used[best] = 1; ++matched; }
    }
    int den = q->count > f->count ? q->count : f->count;
    return (100 * matched) / den;
}

static int load_xyt(const char *path, Fingerprint *fp, int identity) {
    FILE *file = fopen(path, "r");
    if (!file) { perror(path); return 0; }
    fp->count = 0; fp->identity = identity;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        int x, y, angle, quality = 0;
        if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line)) continue;
        if (sscanf(line, "%d %d %d %d", &x, &y, &angle, &quality) < 3) continue;
        if (fp->count >= MAX_MINUTIAE) break;
        if (x < -32768 || x > 32767 || y < -32768 || y > 32767) continue;
        angle = (angle % 360 + 360) % 360;
        fp->pts[fp->count++] = (Minutia){(short)x, (short)y, (short)angle, (short)quality};
    }
    fclose(file);
    return fp->count > 0;
}

static int load_manifest(const char *manifest, Fingerprint **out) {
    FILE *file = fopen(manifest, "r");
    if (!file) { perror(manifest); return 0; }
    size_t capacity = 256;
    Fingerprint *db = malloc(capacity * sizeof(*db));
    if (!db) { fclose(file); return 0; }
    char line[PATH_SIZE];
    int n = 0;
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        while (len && (line[len-1]=='\n' || line[len-1]=='\r' || isspace((unsigned char)line[len-1]))) line[--len] = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;
        if ((size_t)n == capacity) {
            capacity *= 2;
            Fingerprint *tmp = realloc(db, capacity * sizeof(*db));
            if (!tmp) { free(db); fclose(file); return 0; }
            db = tmp;
        }
        if (!load_xyt(p, &db[n], n)) {
            fprintf(stderr, "Skipping missing/empty xyt: %s\n", p);
            continue;
        }
        ++n;
    }
    fclose(file);
    *out = db;
    return n;
}

static Fingerprint *make_synthetic(int n) {
    /* Heavy templates concentrated in the first quarter to expose static imbalance. */
    Fingerprint *db = malloc((size_t)n * sizeof(*db));
    if (!db) return NULL;
    for (int i = 0; i < n; ++i) {
        Fingerprint *f = &db[i];
        f->identity = i;
        f->count = i < n / 4 ? 80 : 20 + (int)(rand32() % 16);
        for (int j = 0; j < f->count; ++j) {
            f->pts[j] = (Minutia){
                (short)(rand32()%512), (short)(rand32()%512),
                (short)(rand32()%360), (short)(60 + rand32()%41)
            };
        }
    }
    return db;
}

/* Copy pointers so enlarged databases reuse original identities/templates. */
static const Fingerprint **make_gallery(const Fingerprint *base, int base_size, int n) {
    const Fingerprint **ptrs = malloc((size_t)n * sizeof(*ptrs));
    if (!ptrs) return NULL;
    for (int i = 0; i < n; ++i) ptrs[i] = &base[i % base_size];
    return ptrs;
}

static Result run_sequential(const Fingerprint **db, int n, const Fingerprint *query) {
    Result r = {.index = -1, .identity = -1, .score = -1, .checked = 0, .active_threads = 1, .stats = NULL};
    double start = wall_time();
    for (int i = 0; i < n; ++i) {
        int s = match_score(query, db[i]);
        ++r.checked;
        if (better(s, i, r.score, r.index)) {
            r.index = i; r.score = s; r.identity = db[i]->identity;
        }
    }
    r.seconds = wall_time() - start;
    return r;
}

static omp_sched_t get_schedule(const char *name) {
    if (strcmp(name, "dynamic") == 0) return omp_sched_dynamic;
    if (strcmp(name, "guided") == 0) return omp_sched_guided;
    return omp_sched_static;
}

static Result run_parallel(const Fingerprint **db, int n, const Fingerprint *query,
                           int threads, const char *schedule, int chunk, int early, int threshold) {
    Result r = {.index = -1, .identity = -1, .score = -1, .checked = 0, .active_threads = 0};
    r.stats = calloc((size_t)threads, sizeof(*r.stats));
    if (!r.stats) { fprintf(stderr, "Out of memory\n"); exit(1); }

    int stop = 0;
    int winning_index = -1;
    int winning_score = -1;
    int global_best_index = -1;
    int global_best_score = -1;
    long total_checked = 0;
    int actual_threads = 0;

    /* static,chunk=0 means contiguous block partitioning. */
    omp_set_schedule(get_schedule(schedule), strcmp(schedule,"static") == 0 ? 0 : chunk);
    double start = wall_time();
    #pragma omp parallel num_threads(threads) reduction(+:total_checked)
    {
        int tid = omp_get_thread_num();
        #pragma omp single
        actual_threads = omp_get_num_threads();
        int local_best_score = -1, local_best_index = -1;
        double local_busy = 0;
        long local_checked = 0;

        #pragma omp for schedule(runtime)
        for (int i = 0; i < n; ++i) {
            int should_stop;
            #pragma omp atomic read
            should_stop = stop;
            if (early && should_stop) continue;

            double t0 = wall_time();
            int score = match_score(query, db[i]);
            local_busy += wall_time() - t0;
            ++local_checked;

            if (better(score, i, local_best_score, local_best_index)) {
                local_best_score = score;
                local_best_index = i;
            }
            if (early && score >= threshold) {
                #pragma omp critical(winner)
                {
                    int already_stopped;
                    #pragma omp atomic read
                    already_stopped = stop;
                    if (!already_stopped) {
                        #pragma omp atomic write
                        stop = 1;
                        winning_index = i;
                        winning_score = score;
                    }
                }
                /* Works when OMP_CANCELLATION=true; atomic flag is the fallback. */
                #pragma omp cancel for
            }
            #pragma omp cancellation point for
        }

        r.stats[tid].busy = local_busy;
        r.stats[tid].comparisons = local_checked;
        total_checked += local_checked;

        #pragma omp critical(best_match)
        if (local_best_index >= 0 && better(local_best_score, local_best_index,
                                             global_best_score, global_best_index)) {
            global_best_score = local_best_score;
            global_best_index = local_best_index;
        }
    }
    r.seconds = wall_time() - start;
    r.checked = total_checked;
    r.active_threads = actual_threads;
    /* When stopping, return first threshold-satisfying candidate, NOT global maximum. */
    r.index = winning_index >= 0 ? winning_index : global_best_index;
    if (winning_index >= 0) {
        r.identity = db[r.index]->identity;
        r.score = winning_score;
    } else if (r.index >= 0) {
        r.identity = db[r.index]->identity;
        r.score = global_best_score;
    }
    return r;
}

static void print_help(const char *app) {
    printf("Usage: %s [options]\n\n", app);
    printf("Input (one required; default --synthetic 6000):\n");
    printf("  --synthetic N          Generate N demonstration templates\n");
    printf("  --manifest FILE        Text file listing paths to NIST .xyt files\n");
    printf("  --query-index I        Gallery query index (default 1500, wrapped if needed)\n");
    printf("  --query-file FILE      Separate .xyt query instead of gallery template\n\n");
    printf("Settings:\n");
    printf("  --threads N            Thread count (default 4)\n");
    printf("  --schedule NAME        static, dynamic, guided (default dynamic)\n");
    printf("  --chunk N              Chunk size for dynamic/guided (default 8)\n");
    printf("  --early 0|1            Enable stop when a score reaches threshold\n");
    printf("  --threshold 0..100     Early-stop threshold (default 100)\n");
    printf("  --repeats N            Repeat each configuration (default 5)\n");
    printf("  --benchmark            All sizes 6000/30000/60000, threads 1/2/4/8/16,\n");
    printf("                         static/dynamic/guided, early on/off\n");
    printf("  --csv FILE             Results CSV (default results.csv)\n");
    printf("  --thread-csv FILE      Per-thread CSV (default thread_results.csv)\n");
    printf("\nFor early stop with --threshold below 100, the match need not be the\n");
    printf("same as the full-search highest-scoring match.\n");
}

static int parse_positive(const char *s, int minimum, int maximum) {
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < minimum || n > maximum) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (int)n;
}

static Options get_options(int argc, char **argv) {
    Options o = {.synthetic=6000, .query_index=1500,
        .threads=4, .repeats=5, .early=0, .threshold=100, .chunk=8,
        .benchmark=0, .manifest=NULL, .query_file=NULL, .schedule="dynamic",
        .csv_path="results.csv", .thread_csv_path="thread_results.csv"};
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a,"--help") || !strcmp(a,"-h")) { print_help(argv[0]); exit(0); }
        if (!strcmp(a,"--benchmark")) { o.benchmark = 1; continue; }
        if (i+1 >= argc) { fprintf(stderr, "Missing value for %s\n", a); exit(2); }
        const char *v = argv[++i];
        if (!strcmp(a,"--synthetic")) { o.synthetic = parse_positive(v,1,1000000); o.manifest = NULL; }
        else if (!strcmp(a,"--manifest")) { o.manifest=v; o.synthetic=0; }
        else if (!strcmp(a,"--query-file")) o.query_file=v;
        else if (!strcmp(a,"--query-index")) o.query_index=parse_positive(v,0,1000000);
        else if (!strcmp(a,"--threads")) o.threads=parse_positive(v,1,256);
        else if (!strcmp(a,"--repeats")) o.repeats=parse_positive(v,1,100);
        else if (!strcmp(a,"--early")) o.early=parse_positive(v,0,1);
        else if (!strcmp(a,"--threshold")) o.threshold=parse_positive(v,0,100);
        else if (!strcmp(a,"--chunk")) o.chunk=parse_positive(v,1,10000);
        else if (!strcmp(a,"--schedule")) o.schedule=v;
        else if (!strcmp(a,"--csv")) o.csv_path=v;
        else if (!strcmp(a,"--thread-csv")) o.thread_csv_path=v;
        else { fprintf(stderr,"Unknown option: %s\n", a); exit(2); }
    }
    if (strcmp(o.schedule,"static") && strcmp(o.schedule,"dynamic") && strcmp(o.schedule,"guided")) {
        fprintf(stderr,"Schedule must be static, dynamic or guided\n"); exit(2);
    }
    return o;
}

static FILE *open_csv(const char *path, const char *header) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "%s\n", header);
    return f;
}

static void record_result(FILE *csv, FILE *thread_csv, int n, int threads,
                          const char *schedule, int early, int threshold, int run,
                          const Result *seq, double seq_mean, const Result *parallel) {
    int same_identity = seq->identity == parallel->identity;
    int same_best = seq->index == parallel->index && seq->score == parallel->score;
    int accepted = parallel->score >= threshold;
    int reference_accepted = seq->score >= threshold;
    double end_to_end_speedup = seq_mean / parallel->seconds;
    double speedup = early ? 0.0 : end_to_end_speedup;
    double efficiency = early ? 0.0 : speedup / parallel->active_threads;
    fprintf(csv,"%d,%d,%d,%s,%d,%d,%d,%.15f,%.15f,",
        n,threads,parallel->active_threads,schedule,early,threshold,run,
        parallel->seconds,seq_mean);
    if (early) fprintf(csv,",");
    else fprintf(csv,"%.12f,%.12f",speedup,efficiency);
    fprintf(csv,",%.12f,%ld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        end_to_end_speedup,parallel->checked,parallel->index,parallel->identity,parallel->score,
        seq->index,seq->identity,seq->score,same_identity,same_best,accepted,reference_accepted);
    for (int tid = 0; tid < parallel->active_threads; ++tid) {
        double busy = parallel->stats[tid].busy;
        double idle_estimate = fmax(0.0, parallel->seconds - busy);
        fprintf(thread_csv,"%d,%d,%d,%s,%d,%d,%d,%ld,%.15f,%.15f,%.15f\n",
            n,threads,parallel->active_threads,schedule,early,run,tid,parallel->stats[tid].comparisons,
            busy,idle_estimate,parallel->seconds);
    }
    printf("N=%-6d T=%-2d %-7s early=%d run=%d time=%10.7fs end-to-end=%7.2fx "
           "checked=%-6ld id=%-6d score=%-3d accepted=%s same_ID=%s same_best=%s\n",
           n,threads,schedule,early,run,parallel->seconds,end_to_end_speedup,parallel->checked,
           parallel->identity,parallel->score,accepted?"YES":"NO",
           same_identity?"YES":"NO",same_best?"YES":"NO");
}

int main(int argc, char **argv) {
    Options o = get_options(argc, argv);
    if (!initialize_timer()) {
        fputs("Could not initialize a reliable high-resolution timer\n", stderr);
        return 1;
    }
    omp_set_dynamic(0);
    Fingerprint *base = NULL;
    int base_size = 0;
    if (o.manifest) {
        base_size = load_manifest(o.manifest, &base);
    } else {
        base_size = o.synthetic;
        base = make_synthetic(base_size);
    }
    if (!base || base_size < 1) {
        fprintf(stderr,"No templates loaded. Check input/manifest.\n"); free(base); return 1;
    }
    Fingerprint external_query;
    if (o.query_file && !load_xyt(o.query_file, &external_query, -1)) {
        fprintf(stderr,"Could not read query %s\n", o.query_file); free(base); return 1;
    }
    FILE *csv = open_csv(o.csv_path,
        "db_size,threads,actual_threads,schedule,early_stop,threshold,run,parallel_seconds,sequential_seconds,"
        "speedup,efficiency,end_to_end_speedup,checked,match_index,match_identity,match_score,reference_index,"
        "reference_identity,reference_score,same_identity,same_exact_best,accepted,reference_accepted");
    FILE *tcsv = open_csv(o.thread_csv_path,
        "db_size,threads,actual_threads,schedule,early_stop,run,thread_id,comparisons,busy_seconds,"
        "idle_plus_overhead_est_seconds,parallel_seconds");

    const int sizes[] = {6000,30000,60000};
    const int thread_counts[] = {1,2,4,8,16};
    const char *schedules[] = {"static","dynamic","guided"};
    int size_count = o.benchmark ? 3 : 1;
    for (int si = 0; si < size_count; ++si) {
        int n = o.benchmark ? sizes[si] : base_size;
        const Fingerprint **db = make_gallery(base, base_size, n);
        if (!db) { fprintf(stderr,"Cannot allocate gallery\n"); fclose(csv); fclose(tcsv); free(base); return 1; }
        /* The benchmark gallery can be smaller than the loaded base. */
        int query_index = o.query_index % (base_size < n ? base_size : n);
        const Fingerprint *query = o.query_file ? &external_query : db[query_index];
        if (!o.query_file) printf("\nQuery: gallery index %d, identity %d\n", query_index, query->identity);
        else printf("\nQuery: external .xyt file %s\n",o.query_file);
        printf("Database size: %d; base templates: %d; repeats: %d\n",n,base_size,o.repeats);
        Result seq = run_sequential(db,n,query);
        if (!(seq.seconds > 0.0) || !isfinite(seq.seconds)) {
            fprintf(stderr,"Invalid sequential timing for database size %d\n", n);
            free(db); fclose(csv); fclose(tcsv); free(base); return 1;
        }
        double seq_total = seq.seconds;
        for (int r = 1; r < o.repeats; ++r) {
            double sample_seconds = run_sequential(db,n,query).seconds;
            if (!(sample_seconds > 0.0) || !isfinite(sample_seconds)) {
                fprintf(stderr,"Invalid sequential timing for database size %d\n", n);
                free(db); fclose(csv); fclose(tcsv); free(base); return 1;
            }
            seq_total += sample_seconds;
        }
        double seq_mean = seq_total / o.repeats;
        printf("SEQUENTIAL: %.9fs  index=%d identity=%d score=%d (all %d checked)\n",
               seq_mean,seq.index,seq.identity,seq.score,n);

        int t_count = o.benchmark ? 5 : 1;
        int s_count = o.benchmark ? 3 : 1;
        int e_count = o.benchmark ? 2 : 1;
        for (int ti = 0; ti < t_count; ++ti) {
            int threads = o.benchmark ? thread_counts[ti] : o.threads;
            for (int sj = 0; sj < s_count; ++sj) {
                const char *schedule = o.benchmark ? schedules[sj] : o.schedule;
                for (int ei = 0; ei < e_count; ++ei) {
                    int early = o.benchmark ? ei : o.early;
                    for (int run = 1; run <= o.repeats; ++run) {
                        Result p = run_parallel(db,n,query,threads,schedule,o.chunk,early,o.threshold);
                        if (!(p.seconds > 0.0) || !isfinite(p.seconds) ||
                            p.active_threads < 1 || p.checked < 1 || p.checked > n ||
                            (early && ((p.score >= o.threshold && p.index < 0) ||
                                       (p.score < o.threshold && p.checked != n)))) {
                            fprintf(stderr,"Invalid parallel result or timing: N=%d T=%d %s early=%d run=%d\n",
                                    n,threads,schedule,early,run);
                            free(p.stats); free(db); fclose(csv); fclose(tcsv); free(base); return 1;
                        }
                        record_result(csv,tcsv,n,threads,schedule,early,o.threshold,run,
                                      &seq,seq_mean,&p);
                        free(p.stats);
                    }
                    fflush(csv); fflush(tcsv);
                }
            }
        }
        free(db);
    }
    fclose(csv); fclose(tcsv); free(base);
    printf("\nSaved %s and %s\n",o.csv_path,o.thread_csv_path);
    return 0;
}
