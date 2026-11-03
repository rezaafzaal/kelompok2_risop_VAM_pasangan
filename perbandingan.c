/*
 * Perbandingan 5 metode solusi awal (IBFS) Transportation Problem:
 *   1. VAM           (port dari VAM.c)
 *   2. RBSM          (port dari RBSM.cpp)
 *   3. SAM           (port dari SAM.c)
 *   4. CSM           (dari paper Bunyamin dkk., ETASR 2026)
 *   5. VAM Pasangan  (VAM Pasangan + kapasitas, bobot 3 dan 2)
 *
 * Biaya optimal dihitung dengan min-cost flow (successive shortest path),
 * dipakai untuk menghitung gap (%) tiap metode.
 *
 * Format file soal (sama dengan dataset96):
 *   m n
 *   c11 ... c1n
 *   ...
 *   cm1 ... cmn
 *   s1 ... sm
 *   d1 ... dn
 *
 * Kompilasi : gcc -O2 -o perbandingan perbandingan.c -lm
 * Jalankan  : ./perbandingan <folder_dataset> [file_csv_output]
 *   contoh  : ./perbandingan dataset96 hasil.csv
 *
 * Mode trace (cetak langkah demi langkah satu soal):
 *   ./perbandingan --trace <file_soal> [SOAL|OPT|VAM|RBSM|SAM|CSM|VP|ALL]
 *   contoh  : ./perbandingan --trace dataset96/CIDI01.txt VAM
 *
 * Runtime tiap metode diukur dengan timer resolusi tinggi. Karena satu kali
 * jalan bisa < 1 mikrodetik, tiap metode diulang sampai total >= 5 ms lalu
 * dirata-rata (waktu baca file tidak ikut dihitung).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static double now_us(void) {
    static LARGE_INTEGER freq; LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
#endif

#define MAXN 105
#define NMETHOD 5
#define TIME_WINDOW_US 5000.0

static int TRACE = 0;   /* 1 = cetak langkah demi langkah */

typedef struct {
    char name[128];
    int m, n;          /* ukuran setelah diseimbangkan */
    int m0, n0;        /* ukuran asli */
    int dummy;         /* 0 = seimbang, 1 = baris dummy, 2 = kolom dummy */
    int c[MAXN][MAXN];
    int s[MAXN];
    int d[MAXN];
} Problem;

typedef struct {
    long long cost;
    int feasible;      /* 1 = semua supply & demand terpenuhi */
} Result;

static const char *METHOD_NAME[NMETHOD] = {"VAM", "RBSM", "SAM", "CSM", "VAM-Pasangan"};

/* ------------------------------------------------------------------ */
/* Utilitas                                                            */
/* ------------------------------------------------------------------ */

static int read_problem(const char *path, Problem *P) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int i, j;
    if (fscanf(f, "%d %d", &P->m, &P->n) != 2) { fclose(f); return 0; }
    if (P->m < 1 || P->n < 1 || P->m >= MAXN - 1 || P->n >= MAXN - 1) { fclose(f); return 0; }
    for (i = 0; i < P->m; i++)
        for (j = 0; j < P->n; j++)
            if (fscanf(f, "%d", &P->c[i][j]) != 1) { fclose(f); return 0; }
    for (i = 0; i < P->m; i++) if (fscanf(f, "%d", &P->s[i]) != 1) { fclose(f); return 0; }
    for (j = 0; j < P->n; j++) if (fscanf(f, "%d", &P->d[j]) != 1) { fclose(f); return 0; }
    fclose(f);

    P->m0 = P->m; P->n0 = P->n; P->dummy = 0;
    long long ts = 0, td = 0;
    for (i = 0; i < P->m; i++) ts += P->s[i];
    for (j = 0; j < P->n; j++) td += P->d[j];
    if (ts < td) {            /* tambah baris dummy biaya 0 */
        for (j = 0; j < P->n; j++) P->c[P->m][j] = 0;
        P->s[P->m] = (int)(td - ts);
        P->m++; P->dummy = 1;
    } else if (ts > td) {     /* tambah kolom dummy biaya 0 */
        for (i = 0; i < P->m; i++) P->c[i][P->n] = 0;
        P->d[P->n] = (int)(ts - td);
        P->n++; P->dummy = 2;
    }
    return 1;
}

/* hitung biaya dan cek kelayakan alokasi x */
static Result evaluate(const Problem *P, int x[MAXN][MAXN]) {
    Result r; int i, j;
    r.cost = 0; r.feasible = 1;
    for (i = 0; i < P->m; i++) {
        long long t = 0;
        for (j = 0; j < P->n; j++) {
            if (x[i][j] < 0) r.feasible = 0;
            t += x[i][j];
            r.cost += (long long)x[i][j] * P->c[i][j];
        }
        if (t != P->s[i]) r.feasible = 0;
    }
    for (j = 0; j < P->n; j++) {
        long long t = 0;
        for (i = 0; i < P->m; i++) t += x[i][j];
        if (t != P->d[j]) r.feasible = 0;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Utilitas trace                                                      */
/* ------------------------------------------------------------------ */

static const char *rl(const Problem *P, int i) {     /* label baris */
    static char b[8][16]; static int k; k = (k + 1) & 7;
    if (P->dummy == 1 && i == P->m - 1) snprintf(b[k], 16, "Sdum"); else snprintf(b[k], 16, "S%d", i + 1);
    return b[k];
}

static const char *cl(const Problem *P, int j) {     /* label kolom */
    static char b[8][16]; static int k; k = (k + 1) & 7;
    if (P->dummy == 2 && j == P->n - 1) snprintf(b[k], 16, "Ddum"); else snprintf(b[k], 16, "D%d", j + 1);
    return b[k];
}

/* tabel biaya[alokasi], plus supply, total alokasi baris, status */
static void tr_alloc(const Problem *P, int x[MAXN][MAXN], int show_status) {
    int i, j;
    printf("  %-5s", "");
    for (j = 0; j < P->n; j++) printf(" %10s", cl(P, j));
    printf(" %8s %8s%s\n", "Supply", "Alokasi", show_status ? "  Status" : "");
    for (i = 0; i < P->m; i++) {
        long long rs = 0;
        printf("  %-5s", rl(P, i));
        for (j = 0; j < P->n; j++) {
            char a[16] = "";
            if (x[i][j]) snprintf(a, sizeof(a), "%d", x[i][j]);
            printf(" %4d[%4s]", P->c[i][j], a);
            rs += x[i][j];
        }
        printf(" %8d %8lld", P->s[i], rs);
        if (show_status) printf("  %s", rs == P->s[i] ? "pas" : (rs < P->s[i] ? "kurang" : "LEBIH"));
        printf("\n");
    }
    printf("  %-5s", "Dmd");
    for (j = 0; j < P->n; j++) printf(" %10d", P->d[j]);
    printf("\n");
}

/* tabel akhir + rincian biaya */
static void tr_final(const Problem *P, int x[MAXN][MAXN]) {
    int i, j, first = 1; long long tot = 0;
    printf("Tabel alokasi akhir  (format sel: biaya[alokasi])\n");
    tr_alloc(P, x, 1);
    printf("Biaya = ");
    for (i = 0; i < P->m; i++) for (j = 0; j < P->n; j++) if (x[i][j]) {
        printf("%s%dx%d", first ? "" : " + ", P->c[i][j], x[i][j]); first = 0;
        tot += (long long)P->c[i][j] * x[i][j];
    }
    printf(" = %lld\n", tot);
}

/* ------------------------------------------------------------------ */
/* Optimal: min-cost flow (successive shortest path, SPFA)             */
/* ------------------------------------------------------------------ */

#define MAXV (2 * MAXN + 2)
#define MAXE (2 * (MAXN * MAXN + 2 * MAXN))
static int eto[MAXE], enext[MAXE], ehead[MAXV], ecnt;
static long long ecap[MAXE], ecost[MAXE];

static void add_edge(int u, int v, long long cap, long long cost) {
    eto[ecnt] = v; ecap[ecnt] = cap; ecost[ecnt] = cost; enext[ecnt] = ehead[u]; ehead[u] = ecnt++;
    eto[ecnt] = u; ecap[ecnt] = 0;   ecost[ecnt] = -cost; enext[ecnt] = ehead[v]; ehead[v] = ecnt++;
}

static int tr_aug_it;   /* nomor augmentasi untuk trace */

static long long optimal_cost(const Problem *P) {
    int V = P->m + P->n + 2, S = V - 2, T = V - 1, i, j;
    long long INF = (long long)4e18, total = 0;
    static long long dist[MAXV]; static int inq[MAXV], pe[MAXV], q[MAXV * 64];
    memset(ehead, -1, sizeof(ehead)); ecnt = 0;
    for (i = 0; i < P->m; i++) add_edge(S, i, P->s[i], 0);
    for (j = 0; j < P->n; j++) add_edge(P->m + j, T, P->d[j], 0);
    for (i = 0; i < P->m; i++)
        for (j = 0; j < P->n; j++) add_edge(i, P->m + j, (long long)4e15, P->c[i][j]);
    while (1) {
        int h = 0, t = 0, v;
        for (v = 0; v < V; v++) { dist[v] = INF; inq[v] = 0; pe[v] = -1; }
        dist[S] = 0; q[t++] = S; inq[S] = 1;
        while (h != t) {
            int u = q[h++]; if (h == MAXV * 64) h = 0;
            inq[u] = 0;
            for (int e = ehead[u]; e != -1; e = enext[e])
                if (ecap[e] > 0 && dist[u] + ecost[e] < dist[eto[e]]) {
                    dist[eto[e]] = dist[u] + ecost[e]; pe[eto[e]] = e;
                    if (!inq[eto[e]]) { inq[eto[e]] = 1; q[t++] = eto[e]; if (t == MAXV * 64) t = 0; }
                }
        }
        if (dist[T] == INF) break;
        long long f = INF;
        for (v = T; v != S; v = eto[pe[v] ^ 1]) if (ecap[pe[v]] < f) f = ecap[pe[v]];
        for (v = T; v != S; v = eto[pe[v] ^ 1]) { ecap[pe[v]] -= f; ecap[pe[v] ^ 1] += f; }
        total += f * dist[T];
        if (TRACE) {
            static int path[MAXV];
            int np = 0, k;
            for (v = T; ; v = eto[pe[v] ^ 1]) { path[np++] = v; if (v == S) break; }
            printf("Augmentasi %d: jalur ", ++tr_aug_it);
            for (k = np - 1; k >= 0; k--) {
                int u = path[k];
                if (u == S) printf("src");
                else if (u == T) printf(" -> snk");
                else printf(" -> %s", u < P->m ? rl(P, u) : cl(P, u - P->m));
            }
            printf("\n  aliran = %lld unit, biaya per unit jalur = %lld, total biaya sementara = %lld\n",
                   f, dist[T], total);
        }
    }
    return total;
}

/* trace optimal: jalankan MCF lalu ambil alokasi dari aliran sisi S_i -> D_j */
static void tr_optimal(const Problem *P) {
    static int x[MAXN][MAXN]; int i, j;
    printf("Model: src -> S_i (kapasitas supply), S_i -> D_j (biaya c_ij), D_j -> snk (kapasitas demand).\n");
    printf("Tiap augmentasi mengirim aliran lewat jalur termurah di graf residual (SPFA).\n");
    printf("Sisi balik (mis. D2 -> S1) berarti membatalkan sebagian alokasi sebelumnya.\n\n");
    tr_aug_it = 0;
    optimal_cost(P);
    for (i = 0; i < P->m; i++)
        for (j = 0; j < P->n; j++)
            x[i][j] = (int)ecap[2 * (P->m + P->n) + 2 * (i * P->n + j) + 1];
    printf("\n");
    tr_final(P, x);
}

/* ------------------------------------------------------------------ */
/* 1. VAM  (logika sama dengan VAM.c)                                  */
/*    - penalti = biaya termurah ke-2 - termurah (1 sel tersisa:       */
/*      INT_MAX - termurah)                                            */
/*    - penalti maksimum pertama (indeks terkecil) di baris & kolom    */
/*    - seri baris vs kolom: min cost lebih kecil; masih seri: total   */
/*      biaya garis lebih kecil (baris jika sama)                      */
/* ------------------------------------------------------------------ */

static void vam_diff(const Problem *P, int rdone[], int cdone[], int j, int is_row, long long res[4]) {
    int i, len = is_row ? P->n : P->m;
    long long min1 = INT_MAX, min2 = INT_MAX, min_p = -1, total = 0;
    for (i = 0; i < len; i++) {
        if (is_row ? cdone[i] : rdone[i]) continue;
        long long c = is_row ? P->c[j][i] : P->c[i][j];
        total += c;
        if (c < min1) { min2 = min1; min1 = c; min_p = i; }
        else if (c < min2) min2 = c;
    }
    res[0] = min2 - min1; res[1] = min1; res[2] = min_p; res[3] = total;
}

static void vam_max_penalty(const Problem *P, int rdone[], int cdone[], int is_row, long long res[5]) {
    int i, len1 = is_row ? P->m : P->n;
    long long pc = -1, pm = -1, mc = -1, md = LLONG_MIN, mxc = 0, r2[4];
    for (i = 0; i < len1; i++) {
        if (is_row ? rdone[i] : cdone[i]) continue;
        vam_diff(P, rdone, cdone, i, is_row, r2);
        if (r2[0] > md) { md = r2[0]; pm = i; mc = r2[1]; pc = r2[2]; mxc = r2[3]; }
    }
    if (is_row) { res[0] = pm; res[1] = pc; } else { res[0] = pc; res[1] = pm; }
    res[2] = mc; res[3] = md; res[4] = mxc;
}

static Result run_vam(const Problem *P) {
    static int x[MAXN][MAXN]; int s[MAXN], d[MAXN], rdone[MAXN] = {0}, cdone[MAXN] = {0}, i;
    memset(x, 0, sizeof(x));
    long long left = 0;
    for (i = 0; i < P->m; i++) { s[i] = P->s[i]; left += s[i]; }
    for (i = 0; i < P->n; i++) d[i] = P->d[i];
    int guard = 0, it = 0;
    if (TRACE) printf("Penalti = biaya termurah ke-2 - termurah pada baris/kolom yang masih aktif\n"
                      "(M = tinggal 1 sel, penalti dianggap sangat besar).\n\n");
    while (left > 0 && guard++ < 100000) {
        long long r1[5], r2[5], *res;
        vam_max_penalty(P, rdone, cdone, 1, r1);
        vam_max_penalty(P, rdone, cdone, 0, r2);
        if (r1[3] == r2[3]) {
            if (r1[2] < r2[2]) res = r1;
            else if (r1[2] > r2[2]) res = r2;
            else res = (r1[4] > r2[4]) ? r2 : r1;
        } else res = (r1[3] > r2[3]) ? r1 : r2;
        int r = (int)res[0], c = (int)res[1];
        if (r < 0 || c < 0) break;
        int q = d[c] <= s[r] ? d[c] : s[r];
        if (TRACE) {
            long long t[4]; int k;
            printf("Iterasi %d\n  Penalti baris :", ++it);
            for (k = 0; k < P->m; k++) if (!rdone[k]) {
                vam_diff(P, rdone, cdone, k, 1, t);
                if (t[0] + t[1] == INT_MAX) printf(" %s=M", rl(P, k)); else printf(" %s=%lld", rl(P, k), t[0]);
            }
            printf("\n  Penalti kolom :");
            for (k = 0; k < P->n; k++) if (!cdone[k]) {
                vam_diff(P, rdone, cdone, k, 0, t);
                if (t[0] + t[1] == INT_MAX) printf(" %s=M", cl(P, k)); else printf(" %s=%lld", cl(P, k), t[0]);
            }
            printf("\n  Penalti terbesar ");
            if (res[3] + res[2] == INT_MAX) printf("M"); else printf("%lld", res[3]);
            printf(" di %s%s -> sel termurah (%s,%s) biaya %d\n",
                   res == r1 ? rl(P, r) : cl(P, c), r1[3] == r2[3] ? " (seri baris-kolom)" : "",
                   rl(P, r), cl(P, c), P->c[r][c]);
            printf("  Alokasi x(%s,%s) = min(sisa supply %d, sisa demand %d) = %d",
                   rl(P, r), cl(P, c), s[r], d[c], q);
        }
        d[c] -= q; if (d[c] == 0) cdone[c] = 1;
        s[r] -= q; if (s[r] == 0) rdone[r] = 1;
        x[r][c] += q; left -= q;
        if (TRACE) {
            if (rdone[r]) printf("; %s habis", rl(P, r));
            if (cdone[c]) printf("; %s terpenuhi", cl(P, c));
            printf("\n\n");
        }
    }
    if (TRACE) tr_final(P, x);
    return evaluate(P, x);
}

/* ------------------------------------------------------------------ */
/* 2. RBSM  (logika sama dengan RBSM.cpp)                              */
/* ------------------------------------------------------------------ */

static int rb_row, rb_col, rb_mat[MAXN][MAXN], rb_s[MAXN], rb_TC[MAXN], rb_sup[MAXN][MAXN],
           rb_sumRow[MAXN], rb_status[MAXN];

static int rb_updateStatus(int *satisfied) {
    int i, nNeed = 0, nExc = 0;
    for (i = 0; i < rb_row; i++) {
        if (rb_sumRow[i] == rb_s[i]) { if (rb_status[i] != 1) (*satisfied)--; rb_status[i] = 1; }
        else if (rb_sumRow[i] < rb_s[i]) { rb_status[i] = 2; nNeed = 1; }
        else { rb_status[i] = 3; nExc = 1; }
    }
    if (!nNeed || !nExc) return 0;
    return 1;
}

static void rb_findDonatur(int *don, int *rec, int *dcol, float *rflc, float *rslc) {
    int i, j, k, needSup[MAXN], nNeed = 0;
    for (i = 0; i < rb_row; i++) if (rb_status[i] == 2) needSup[nNeed++] = i;
    int temp, smallestIndex = 0, flag = 0;
    int localSmallest[MAXN], localDon[MAXN], localRec[MAXN];
    for (j = 0; j < rb_col; j++) { localDon[j] = 0; localRec[j] = 0; }
    for (j = 0; j < rb_col; j++) {
        localSmallest[j] = INT_MAX;
        for (i = 0; i < rb_row; i++) {
            if (rb_status[i] == 3 && rb_sup[i][j]) {
                for (k = 0; k < nNeed; k++) {
                    temp = rb_mat[i][j] - rb_mat[needSup[k]][j];
                    if (temp < 0) temp *= -1;
                    if (localSmallest[j] > temp) { localSmallest[j] = temp; localDon[j] = i; localRec[j] = needSup[k]; }
                    else if (localSmallest[j] == temp) {
                        if (rb_TC[needSup[k]] > rb_TC[localRec[j]]) { localDon[j] = i; localRec[j] = needSup[k]; }
                    }
                }
            }
        }
        if (localSmallest[smallestIndex] > localSmallest[j] || smallestIndex == j) { smallestIndex = j; flag = 0; }
        else if (localSmallest[smallestIndex] == localSmallest[j]) flag = 1;
    }
    *rflc = (float)rb_s[localDon[smallestIndex]] * rb_mat[localDon[smallestIndex]][smallestIndex];
    *rslc = (float)rb_s[localRec[smallestIndex]] * rb_mat[localRec[smallestIndex]][smallestIndex];
    if (!flag) { *don = localDon[smallestIndex]; *rec = localRec[smallestIndex]; *dcol = smallestIndex; }
    else {
        int cap1, cap2, max1, max2, p1, p2;
        for (j = smallestIndex + 1; j < rb_col; j++) {
            if (localSmallest[smallestIndex] == localSmallest[j]) {
                cap1 = rb_sup[localDon[smallestIndex]][smallestIndex];
                cap2 = rb_sup[localDon[j]][j];
                if (*rflc > *rslc) p1 = localDon[smallestIndex];
                else if (*rflc < *rslc) p1 = localRec[smallestIndex];
                else if (localDon[smallestIndex] < localRec[smallestIndex]) p1 = localDon[smallestIndex];
                else p1 = localRec[smallestIndex];
                max1 = rb_s[p1] - rb_sumRow[p1]; if (max1 < 0) max1 *= -1;
                if (*rflc > *rslc) p2 = localDon[j];
                else if (*rflc < *rslc) p2 = localRec[j];
                else if (localDon[j] < localRec[j]) p2 = localDon[j];
                else p2 = localRec[j];
                max2 = rb_s[p2] - rb_sumRow[p2]; if (max2 < 0) max1 *= -1;   /* sama persis dengan RBSM.cpp (kemungkinan typo max2) */
                if (cap1 < max1) max1 = cap1;
                if (cap2 < max2) max2 = cap2;
                if (max1 < max2) { *don = localDon[smallestIndex]; *rec = localRec[smallestIndex]; *dcol = smallestIndex; }
                else {
                    int k1 = max1 * rb_mat[localDon[smallestIndex]][smallestIndex];
                    int k2 = max2 * rb_mat[localDon[j]][j];
                    if (k1 <= k2) { *don = localDon[smallestIndex]; *rec = localRec[smallestIndex]; *dcol = smallestIndex; }
                    else { *don = localDon[j]; *rec = localRec[j]; *dcol = j; smallestIndex = j; }
                }
            }
        }
    }
}

static void rb_donate(int don, int rec, int dcol, float rflc, float rslc) {
    int cap = rb_sup[don][dcol], prio;
    if (rflc > rslc) prio = don;
    else if (rflc < rslc) prio = rec;
    else if (don < rec) prio = don;
    else prio = rec;
    int maxDon = rb_s[prio] - rb_sumRow[prio]; if (maxDon < 0) maxDon *= -1;
    if (cap < maxDon) maxDon = cap;
    rb_sup[don][dcol] -= maxDon; rb_sumRow[don] -= maxDon;
    rb_sup[rec][dcol] += maxDon; rb_sumRow[rec] += maxDon;
}

static Result run_rbsm(const Problem *P) {
    int i, j;
    static int x[MAXN][MAXN];
    rb_row = P->m; rb_col = P->n;
    for (i = 0; i < rb_row; i++) {
        rb_TC[i] = 0;
        for (j = 0; j < rb_col; j++) { rb_mat[i][j] = P->c[i][j]; rb_sup[i][j] = 0; rb_TC[i] += rb_mat[i][j]; }
        rb_status[i] = 0; rb_sumRow[i] = 0; rb_s[i] = P->s[i];
    }
    /* minimumCell: demand kolom j ke baris termurah (seri: TC terbesar) */
    for (j = 0; j < rb_col; j++) {
        int mn = 0;
        for (i = 1; i < rb_row; i++) {
            if (rb_mat[mn][j] > rb_mat[i][j]) mn = i;
            else if (rb_mat[mn][j] == rb_mat[i][j] && rb_TC[i] > rb_TC[mn]) mn = i;
        }
        rb_sup[mn][j] = P->d[j];
    }
    for (i = 0; i < rb_row; i++) for (j = 0; j < rb_col; j++) rb_sumRow[i] += rb_sup[i][j];

    if (TRACE) {
        printf("TC (jumlah biaya baris):");
        for (i = 0; i < rb_row; i++) printf(" %s=%d", rl(P, i), rb_TC[i]);
        printf("\n\nLangkah 0: alokasi awal - demand tiap kolom ke sel termurah (seri: TC terbesar)\n");
        tr_alloc(P, rb_sup, 1);
        printf("\n");
    }
    int satisfied = rb_row, don, rec, dcol, guard = 0, step = 0; float rflc, rslc;
    if (rb_updateStatus(&satisfied)) {
        while (satisfied && guard++ < 10000) {
            don = -1; rec = -1;
            rb_findDonatur(&don, &rec, &dcol, &rflc, &rslc);
            int before = rb_sup[don][dcol], exc = rb_sumRow[don] - rb_s[don], need = rb_s[rec] - rb_sumRow[rec];
            rb_donate(don, rec, dcol, rflc, rslc);
            if (TRACE) {
                printf("Langkah %d: donatur %s (lebih %d) -> penerima %s (kurang %d) di kolom %s\n",
                       ++step, rl(P, don), exc, rl(P, rec), need, cl(P, dcol));
                printf("  selisih biaya |%d - %d| = %d (terkecil), dipindah %d unit\n",
                       rb_mat[don][dcol], rb_mat[rec][dcol], abs(rb_mat[don][dcol] - rb_mat[rec][dcol]),
                       before - rb_sup[don][dcol]);
                tr_alloc(P, rb_sup, 1);
                printf("\n");
            }
            rb_updateStatus(&satisfied);
        }
    } else if (TRACE) printf("Tidak ada pasangan baris lebih/kurang, alokasi awal dipakai.\n\n");
    for (i = 0; i < rb_row; i++) for (j = 0; j < rb_col; j++) x[i][j] = rb_sup[i][j];
    if (TRACE) tr_final(P, x);
    return evaluate(P, x);
}

/* ------------------------------------------------------------------ */
/* 3. SAM  (logika sama dengan SAM.c, batas 20 iterasi seperti aslinya)*/
/* ------------------------------------------------------------------ */

#define SAM_MAX_ITER 20

static Result run_sam(const Problem *P) {
    int m = P->m, n = P->n, p, q;
    static int alloc[MAXN][MAXN];
    double acs[MAXN];
    int arr[MAXN], satisfy[MAXN], tab[MAXN], flc[MAXN], slc[MAXN], diff[MAXN], konflik[MAXN];
    for (p = 0; p < m; p++) {
        double a = 0; for (q = 0; q < n; q++) a += P->c[p][q];
        acs[p] = (a / n) * P->s[p];               /* rata-rata biaya x supply */
    }
    for (p = 0; p < m; p++) for (q = 0; q < n; q++) alloc[p][q] = 0;
    for (p = 0; p < n; p++) {                     /* alokasi awal: termurah per kolom */
        int co = 0;
        for (q = 0; q < m; q++) {
            if (P->c[co][p] > P->c[q][p]) co = q;
            else if (P->c[co][p] == P->c[q][p] && acs[co] < acs[q]) co = q;
        }
        alloc[co][p] = P->d[p];
    }
    if (TRACE) {
        printf("ACS (rata-rata biaya baris x supply):");
        for (p = 0; p < m; p++) printf(" %s=%.2f", rl(P, p), acs[p]);
        printf("\n\nIterasi 0: alokasi awal - demand tiap kolom ke sel termurah (seri: ACS terbesar)\n");
        tr_alloc(P, alloc, 1);
        printf("\n");
    }
    int it = 0;
    while (1) {
        int nER = 0;
        for (p = 0; p < m; p++) {
            arr[p] = 0; satisfy[p] = 0; tab[p] = 0;
            for (q = 0; q < n; q++) tab[p] += alloc[p][q];
            if (tab[p] > P->s[p]) { arr[p] = 1; nER++; }
            else if (tab[p] == P->s[p]) satisfy[p] = 1;
        }
        if (nER == 0) { if (TRACE) printf("Tidak ada baris yang kelebihan (ER) -> selesai.\n\n"); break; }
        it++;
        if (it >= SAM_MAX_ITER) { if (TRACE) printf("Batas %d iterasi tercapai -> berhenti.\n\n", SAM_MAX_ITER); break; }
        for (q = 0; q < n; q++) {
            konflik[q] = 0; flc[q] = -1; slc[q] = -1; diff[q] = INT_MAX;
            for (p = 0; p < m; p++) if (alloc[p][q] > 0 && arr[p] == 1) { konflik[q] = 1; break; }
        }
        for (q = 0; q < n; q++) if (konflik[q]) {     /* FLC */
            int temp = 0, iter = 0;
            for (p = 0; p < m; p++) { iter++; if (satisfy[p] != 1 && alloc[p][q] > 0) { temp = P->c[p][q]; flc[q] = p; break; } }
            for (p = iter; p < m; p++)
                if (satisfy[p] != 1 && temp > P->c[p][q] && alloc[p][q] > 0) { temp = P->c[p][q]; flc[q] = p; }
        }
        for (q = 0; q < n; q++) if (konflik[q]) {     /* SLC */
            int temp = 0, iter = 0;
            for (p = 0; p < m; p++) { iter++; if (flc[q] != p && arr[p] != 1 && satisfy[p] != 1) { temp = P->c[p][q]; slc[q] = p; break; } }
            for (p = iter; p < m; p++)
                if (flc[q] != p && arr[p] != 1 && satisfy[p] != 1) {
                    if (temp > P->c[p][q]) { temp = P->c[p][q]; slc[q] = p; }
                    else if (temp == P->c[p][q] && q < m && acs[q] < acs[p]) slc[q] = p;  /* sama dengan SAM.c: indeks [q] (kemungkinan typo [p]); q>=m di SAM.c baca di luar array */
                }
        }
        for (q = 0; q < n; q++)
            if (konflik[q] && flc[q] >= 0 && slc[q] >= 0) diff[q] = P->c[slc[q]][q] - P->c[flc[q]][q];
            else konflik[q] = 0;
        int cs = -1;
        for (q = 0; q < n; q++) if (konflik[q]) { cs = q; break; }
        if (TRACE) {
            printf("Iterasi %d\n  Baris kelebihan (ER):", it);
            for (p = 0; p < m; p++) if (arr[p]) printf(" %s(%d>%d)", rl(P, p), tab[p], P->s[p]);
            printf("\n  Kolom konflik:\n");
            for (q = 0; q < n; q++) if (konflik[q])
                printf("    %s: FLC=%s(%d) SLC=%s(%d) diff=%d\n", cl(P, q), rl(P, flc[q]), P->c[flc[q]][q],
                       rl(P, slc[q]), P->c[slc[q]][q], diff[q]);
            if (cs < 0) printf("    (tidak ada kolom konflik yang valid -> berhenti)\n\n");
        }
        if (cs < 0) break;
        for (q = cs + 1; q < n; q++) if (konflik[q]) {
            if (diff[cs] == diff[q]) { if (alloc[flc[cs]][cs] < alloc[flc[q]][q]) cs = q; }
            else if (diff[cs] > diff[q]) cs = q;
        }
        int bs = slc[cs], bf = flc[cs], before = alloc[bf][cs];
        if (acs[bf] == acs[bs]) {
            if (tab[bf] - alloc[bf][cs] <= P->s[bf]) {
                alloc[bf][cs] -= (tab[bf] - P->s[bf]); alloc[bs][cs] += (tab[bf] - P->s[bf]);
            } else if (tab[bs] + alloc[bf][cs] >= P->s[bs]) {
                alloc[bs][cs] += (P->s[bs] - tab[bs]); alloc[bf][cs] -= (P->s[bs] - tab[bs]);
            } else { alloc[bs][cs] += alloc[bf][cs]; alloc[bf][cs] = 0; }
        } else if (nER >= 2 ? (acs[bf] > acs[bs]) : (acs[bf] < acs[bs])) {
            if (tab[bf] - alloc[bf][cs] <= P->s[bf]) {
                alloc[bf][cs] -= (tab[bf] - P->s[bf]); alloc[bs][cs] += (tab[bf] - P->s[bf]);
            } else { alloc[bs][cs] += alloc[bf][cs]; alloc[bf][cs] = 0; }
        } else {
            int sisa = P->s[bs] - tab[bs];
            if (sisa >= alloc[bf][cs]) { alloc[bs][cs] += alloc[bf][cs]; alloc[bf][cs] = 0; }
            else { alloc[bs][cs] = sisa; alloc[bf][cs] -= sisa; }
        }
        if (TRACE) {
            printf("  Dipilih kolom %s (diff terkecil): pindah %d unit dari %s ke %s\n",
                   cl(P, cs), before - alloc[bf][cs], rl(P, bf), rl(P, bs));
            tr_alloc(P, alloc, 1);
            printf("\n");
        }
    }
    if (TRACE) tr_final(P, alloc);
    return evaluate(P, alloc);
}

/* ------------------------------------------------------------------ */
/* 4. CSM  (Cost Supply Method, Bunyamin dkk. 2026)                     */
/*    TC_i = jumlah biaya baris i, CS_i = TC_i x S_i                   */
/*    Alokasi awal: demand ke FLC kolom (seri: TC terbesar, lalu       */
/*    indeks terkecil). Selama ada ER: untuk tiap sel ER beralokasi,   */
/*    penerima = baris NS termurah di kolom itu (seri: TC terbesar).   */
/*    Pilih diff terkecil; seri: kenaikan biaya (q x c penerima)       */
/*    terkecil; seri: biaya sel penerima terkecil.                      */
/*    q: jika supply donor = supply penerima, puaskan donor (FLC);      */
/*    selain itu puaskan baris dengan CS terbesar.                      */
/* ------------------------------------------------------------------ */

static Result run_csm(const Problem *P) {
    int m = P->m, n = P->n, i, j, k;
    static int x[MAXN][MAXN];
    long long TC[MAXN], CS[MAXN];
    for (i = 0; i < m; i++) {
        TC[i] = 0; for (j = 0; j < n; j++) { TC[i] += P->c[i][j]; x[i][j] = 0; }
        CS[i] = TC[i] * P->s[i];
    }
    for (j = 0; j < n; j++) {
        int b = 0;
        for (i = 1; i < m; i++)
            if (P->c[i][j] < P->c[b][j] || (P->c[i][j] == P->c[b][j] && TC[i] > TC[b])) b = i;
        x[b][j] = P->d[j];
    }
    if (TRACE) {
        printf("TC (jumlah biaya baris) dan CS = TC x supply:\n ");
        for (i = 0; i < m; i++) printf(" %s: TC=%lld CS=%lld;", rl(P, i), TC[i], CS[i]);
        printf("\n\nLangkah 0: alokasi awal - demand tiap kolom ke FLC (seri: TC terbesar)\n");
        tr_alloc(P, x, 1);
        printf("\n");
    }
    int guard = 0, step = 0;
    while (guard++ < 100000) {
        long long TA[MAXN]; int status[MAXN], anyER = 0;   /* 0=S, 1=NS, 2=ER */
        for (i = 0; i < m; i++) {
            TA[i] = 0; for (j = 0; j < n; j++) TA[i] += x[i][j];
            status[i] = TA[i] == P->s[i] ? 0 : (TA[i] < P->s[i] ? 1 : 2);
            if (status[i] == 2) anyER = 1;
        }
        if (!anyER) { if (TRACE) printf("Tidak ada baris yang kelebihan (ER) -> selesai.\n\n"); break; }
        int bd = -1, br = -1, bc = -1; long long bdiff = 0, binc = 0, bq = 0, bcost = 0;
        for (j = 0; j < n; j++) {
            int r = -1;                          /* SLC: baris NS termurah */
            for (k = 0; k < m; k++) if (status[k] == 1) {
                if (r < 0 || P->c[k][j] < P->c[r][j] || (P->c[k][j] == P->c[r][j] && TC[k] > TC[r])) r = k;
            }
            if (r < 0) continue;
            for (i = 0; i < m; i++) {
                if (status[i] != 2 || x[i][j] <= 0) continue;
                long long excess = TA[i] - P->s[i], need = P->s[r] - TA[r], q;
                if (P->s[i] == P->s[r] || CS[i] >= CS[r]) q = excess;   /* puaskan donor */
                else q = need;                                          /* puaskan penerima */
                if (q > x[i][j]) q = x[i][j];
                long long df = llabs((long long)P->c[i][j] - P->c[r][j]);
                long long inc = q * P->c[r][j];
                int better = 0;
                if (bd < 0 || df < bdiff) better = 1;
                else if (df == bdiff) {
                    if (inc < binc) better = 1;
                    else if (inc == binc && P->c[r][j] < bcost) better = 1;
                }
                if (better) { bd = i; br = r; bc = j; bdiff = df; binc = inc; bq = q; bcost = P->c[r][j]; }
            }
        }
        if (bd < 0 || bq <= 0) { if (TRACE) printf("Tidak ada perpindahan yang mungkin -> berhenti.\n\n"); break; }
        x[bd][bc] -= (int)bq; x[br][bc] += (int)bq;
        if (TRACE) {
            printf("Langkah %d: donor %s (ER, lebih %lld) -> penerima %s (NS, kurang %lld) di kolom %s\n",
                   ++step, rl(P, bd), TA[bd] - P->s[bd], rl(P, br), P->s[br] - TA[br], cl(P, bc));
            printf("  diff |%d - %d| = %lld (terkecil), pindah q = %lld, tambahan biaya q x c = %lld\n",
                   P->c[bd][bc], P->c[br][bc], bdiff, bq, binc);
            tr_alloc(P, x, 1);
            printf("\n");
        }
    }
    if (TRACE) tr_final(P, x);
    return evaluate(P, x);
}

/* ------------------------------------------------------------------ */
/* 5. VAM Pasangan + kapasitas                                          */
/*    skor garis = penalti + 3(rata-rata - FLC) + 2(kapasitas - FLC)    */
/*    kandidat   = sel termurah di barisnya ATAU di kolomnya            */
/*    skor sel   = skor baris + skor kolom                              */
/*    seri: biaya lebih murah, lalu alokasi lebih besar, lalu indeks    */
/* ------------------------------------------------------------------ */

#define WA 3.0
#define WC 2.0

static int cmp_pair(const void *a, const void *b) {
    const int *x = (const int *)a, *y = (const int *)b;
    if (x[0] != y[0]) return x[0] - y[0];
    return x[1] - y[1];
}

static double vp_line_score(const Problem *P, int s[], int d[], int ar[], int nar, int ac[], int nac,
                            int is_row, int k, double comp[3]) {
    int pairs[MAXN][2], np = 0, t;
    double need;
    if (is_row) { for (t = 0; t < nac; t++) { pairs[np][0] = P->c[k][ac[t]]; pairs[np][1] = d[ac[t]]; np++; } need = s[k]; }
    else        { for (t = 0; t < nar; t++) { pairs[np][0] = P->c[ar[t]][k]; pairs[np][1] = s[ar[t]]; np++; } need = d[k]; }
    qsort(pairs, np, sizeof(pairs[0]), cmp_pair);
    double flc = pairs[0][0], sum = 0;
    for (t = 0; t < np; t++) sum += pairs[t][0];
    double pen = np > 1 ? pairs[1][0] - flc : 0.0;
    double avg = sum / np - flc;
    double cap = 0.0;
    if (need > 0) {
        double rem = need, tt = 0.0;
        for (t = 0; t < np; t++) {
            if (rem <= 0) break;
            double u = rem < pairs[t][1] ? rem : pairs[t][1];
            tt += u * pairs[t][0]; rem -= u;
        }
        cap = tt / need - flc;
    }
    if (comp) { comp[0] = pen; comp[1] = avg; comp[2] = cap; }
    return pen + WA * avg + WC * cap;
}

static Result run_vam_pasangan(const Problem *P) {
    static int x[MAXN][MAXN];
    int s[MAXN], d[MAXN], ar[MAXN], ac[MAXN], nar = P->m, nac = P->n, i, j, a, b;
    double R[MAXN], C[MAXN];
    memset(x, 0, sizeof(x));
    for (i = 0; i < P->m; i++) { s[i] = P->s[i]; ar[i] = i; }
    for (j = 0; j < P->n; j++) { d[j] = P->d[j]; ac[j] = j; }
    int it = 0;
    if (TRACE) printf("Skor garis = pen + 3 x (rata-rata - FLC) + 2 x (biaya rata2 berbobot kapasitas - FLC)\n"
                      "Kandidat = sel termurah di barisnya atau di kolomnya; skor sel = skor baris + skor kolom.\n\n");
    while (nar > 0 && nac > 0) {
        if (TRACE && (nar == 1 || nac == 1)) {
            printf("Iterasi %d\n  Tinggal 1 %s aktif -> semua sisa dialokasikan ke %s itu:", ++it,
                   nar == 1 ? "baris" : "kolom", nar == 1 ? "baris" : "kolom");
            if (nar == 1) for (b = 0; b < nac; b++) printf(" x(%s,%s)=%d", rl(P, ar[0]), cl(P, ac[b]), d[ac[b]]);
            else for (a = 0; a < nar; a++) printf(" x(%s,%s)=%d", rl(P, ar[a]), cl(P, ac[0]), s[ar[a]]);
            printf("\n\n");
        }
        if (nar == 1) { i = ar[0]; for (b = 0; b < nac; b++) { j = ac[b]; x[i][j] += d[j]; s[i] -= d[j]; d[j] = 0; } break; }
        if (nac == 1) { j = ac[0]; for (a = 0; a < nar; a++) { i = ar[a]; x[i][j] += s[i]; d[j] -= s[i]; s[i] = 0; } break; }
        double cp[3];
        if (TRACE) printf("Iterasi %d\n", ++it);
        for (a = 0; a < nar; a++) {
            R[ar[a]] = vp_line_score(P, s, d, ar, nar, ac, nac, 1, ar[a], cp);
            if (TRACE) printf("  %-4s pen=%-4g avg=%-6.2f cap=%-6.2f skor=%.2f\n", rl(P, ar[a]), cp[0], cp[1], cp[2], R[ar[a]]);
        }
        for (b = 0; b < nac; b++) {
            C[ac[b]] = vp_line_score(P, s, d, ar, nar, ac, nac, 0, ac[b], cp);
            if (TRACE) printf("  %-4s pen=%-4g avg=%-6.2f cap=%-6.2f skor=%.2f\n", cl(P, ac[b]), cp[0], cp[1], cp[2], C[ac[b]]);
        }
        /* kandidat: termurah di baris atau di kolom */
        static int cand[MAXN][MAXN];
        for (a = 0; a < nar; a++) for (b = 0; b < nac; b++) cand[ar[a]][ac[b]] = 0;
        for (a = 0; a < nar; a++) {
            int mc = INT_MAX; i = ar[a];
            for (b = 0; b < nac; b++) if (P->c[i][ac[b]] < mc) mc = P->c[i][ac[b]];
            for (b = 0; b < nac; b++) if (P->c[i][ac[b]] == mc) cand[i][ac[b]] = 1;
        }
        for (b = 0; b < nac; b++) {
            int mc = INT_MAX; j = ac[b];
            for (a = 0; a < nar; a++) if (P->c[ar[a]][j] < mc) mc = P->c[ar[a]][j];
            for (a = 0; a < nar; a++) if (P->c[ar[a]][j] == mc) cand[ar[a]][j] = 1;
        }
        int bi = -1, bj = -1; double bs = 0;
        /* urut i lalu j naik; ambil yang pertama untuk skor maksimum */
        for (i = 0; i < P->m; i++) for (j = 0; j < P->n; j++) {
            int okr = 0, okc = 0;
            for (a = 0; a < nar; a++) if (ar[a] == i) okr = 1;
            for (b = 0; b < nac; b++) if (ac[b] == j) okc = 1;
            if (!okr || !okc || !cand[i][j]) continue;
            double sc = R[i] + C[j];
            int better = 0;
            if (bi < 0 || sc > bs) better = 1;
            else if (sc == bs) {
                if (P->c[i][j] < P->c[bi][bj]) better = 1;
                else if (P->c[i][j] == P->c[bi][bj]) {
                    int q1 = s[i] < d[j] ? s[i] : d[j], q0 = s[bi] < d[bj] ? s[bi] : d[bj];
                    if (q1 > q0) better = 1;
                }
            }
            if (better) { bi = i; bj = j; bs = sc; }
        }
        int q = s[bi] < d[bj] ? s[bi] : d[bj];
        if (TRACE) {
            printf("  Kandidat:");
            for (a = 0; a < nar; a++) for (b = 0; b < nac; b++)
                if (cand[ar[a]][ac[b]]) printf(" (%s,%s)=%.2f", rl(P, ar[a]), cl(P, ac[b]), R[ar[a]] + C[ac[b]]);
            printf("\n  Dipilih (%s,%s) skor %.2f biaya %d -> alokasi min(%d, %d) = %d",
                   rl(P, bi), cl(P, bj), bs, P->c[bi][bj], s[bi], d[bj], q);
        }
        x[bi][bj] += q; s[bi] -= q; d[bj] -= q;
        int remr = s[bi] == 0, remc = d[bj] == 0;
        if (TRACE) {
            if (remr) printf("; %s habis", rl(P, bi));
            if (remc) printf("; %s terpenuhi", cl(P, bj));
            printf("\n\n");
        }
        if (remr) { for (a = 0; a < nar; a++) if (ar[a] == bi) { ar[a] = ar[--nar]; break; } }
        if (remc) { for (b = 0; b < nac; b++) if (ac[b] == bj) { ac[b] = ac[--nac]; break; } }
        /* jaga urutan indeks tetap naik */
        for (a = 1; a < nar; a++) for (b = a; b > 0 && ar[b - 1] > ar[b]; b--) { int t = ar[b]; ar[b] = ar[b - 1]; ar[b - 1] = t; }
        for (a = 1; a < nac; a++) for (b = a; b > 0 && ac[b - 1] > ac[b]; b--) { int t = ac[b]; ac[b] = ac[b - 1]; ac[b - 1] = t; }
    }
    if (TRACE) tr_final(P, x);
    return evaluate(P, x);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}

typedef Result (*MethodFn)(const Problem *);
static Result run_optimal(const Problem *P) { Result r; r.cost = optimal_cost(P); r.feasible = 1; return r; }
static MethodFn METHOD_FN[NMETHOD] = {run_vam, run_rbsm, run_sam, run_csm, run_vam_pasangan};

/* rata-rata waktu 1x jalan (mikrodetik): ulangi sampai total >= TIME_WINDOW_US */
static double time_method(MethodFn fn, const Problem *P) {
    volatile long long sink = 0; int reps = 0; double t0 = now_us(), t;
    do { sink += fn(P).cost; reps++; t = now_us() - t0; } while (t < TIME_WINDOW_US);
    (void)sink;
    return t / reps;
}

static void str_upper(char *d, const char *s, size_t n) {
    size_t i; for (i = 0; i + 1 < n && s[i]; i++) d[i] = (char)toupper((unsigned char)s[i]); d[i] = 0;
}

static void tr_problem(const Problem *P) {
    int i, j;
    printf("Soal %s  (ukuran asli %dx%d, %s)\n", P->name, P->m0, P->n0,
           P->dummy == 0 ? "seimbang" : (P->dummy == 1 ? "tidak seimbang -> tambah baris dummy biaya 0"
                                                       : "tidak seimbang -> tambah kolom dummy biaya 0"));
    printf("\nMatriks biaya c_ij:\n  %-5s", "");
    for (j = 0; j < P->n; j++) printf(" %6s", cl(P, j));
    printf(" %8s\n", "Supply");
    long long ts = 0, td = 0;
    for (i = 0; i < P->m; i++) {
        printf("  %-5s", rl(P, i));
        for (j = 0; j < P->n; j++) printf(" %6d", P->c[i][j]);
        printf(" %8d\n", P->s[i]); ts += P->s[i];
    }
    printf("  %-5s", "Dmd");
    for (j = 0; j < P->n; j++) { printf(" %6d", P->d[j]); td += P->d[j]; }
    printf("\n\nTotal supply = %lld, total demand = %lld\n", ts, td);
}

static int trace_main(int argc, char **argv) {
    static Problem P; char up[32]; int k, all;
    if (argc < 3) {
        printf("Pemakaian: %s --trace <file_soal> [SOAL|OPT|VAM|RBSM|SAM|CSM|VP|ALL]\n", argv[0]);
        return 1;
    }
    if (!read_problem(argv[2], &P)) { printf("File '%s' gagal dibaca.\n", argv[2]); return 1; }
    const char *base = strrchr(argv[2], '/'), *b2 = strrchr(argv[2], '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : argv[2];
    strncpy(P.name, base, sizeof(P.name) - 1);
    if (strlen(P.name) > 4 && strcmp(P.name + strlen(P.name) - 4, ".txt") == 0) P.name[strlen(P.name) - 4] = 0;
    str_upper(up, argc > 3 ? argv[3] : "ALL", sizeof(up));
    if (strcmp(up, "VAM-PASANGAN") == 0 || strcmp(up, "VAMP") == 0) strcpy(up, "VP");
    all = strcmp(up, "ALL") == 0;

    TRACE = 0;
    long long opt = optimal_cost(&P);

    if (all || strcmp(up, "SOAL") == 0) { tr_problem(&P); printf("\n"); }
    if (all || strcmp(up, "OPT") == 0) {
        printf("==================== OPTIMAL (Min-Cost Flow) - %s ====================\n", P.name);
        TRACE = 1; tr_optimal(&P); TRACE = 0;
        printf("Runtime rata-rata (tanpa cetak trace) = %.3f us\n\n", time_method(run_optimal, &P));
    }
    const char *key[NMETHOD] = {"VAM", "RBSM", "SAM", "CSM", "VP"};
    int found = all || strcmp(up, "SOAL") == 0 || strcmp(up, "OPT") == 0;
    for (k = 0; k < NMETHOD; k++) {
        if (!all && strcmp(up, key[k]) != 0) continue;
        found = 1;
        printf("==================== %s - %s ====================\n", METHOD_NAME[k], P.name);
        TRACE = 1; Result r = METHOD_FN[k](&P); TRACE = 0;
        printf("\nHasil %s: biaya = %lld (%s) | optimal = %lld | gap = %.2f%%\n", METHOD_NAME[k], r.cost,
               r.feasible ? "layak" : "TIDAK LAYAK", opt, opt > 0 ? 100.0 * (r.cost - opt) / opt : 0.0);
        printf("Runtime rata-rata (tanpa cetak trace) = %.3f us\n\n", time_method(METHOD_FN[k], &P));
    }
    if (!found) { printf("Metode '%s' tidak dikenal. Pilih SOAL, OPT, VAM, RBSM, SAM, CSM, VP, atau ALL.\n", up); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--trace") == 0) return trace_main(argc, argv);
    const char *dir = argc > 1 ? argv[1] : "dataset96";
    const char *csvname = argc > 2 ? argv[2] : "hasil_perbandingan.csv";
    DIR *dp = opendir(dir);
    if (!dp) { printf("Folder '%s' tidak bisa dibuka.\n", dir); return 1; }
    char *files[2000]; int nf = 0; struct dirent *ent;
    while ((ent = readdir(dp)) && nf < 2000) {
        size_t L = strlen(ent->d_name);
        if (L > 4 && strcmp(ent->d_name + L - 4, ".txt") == 0) files[nf++] = strdup(ent->d_name);
    }
    closedir(dp);
    qsort(files, nf, sizeof(char *), cmp_str);

    FILE *csv = fopen(csvname, "w");
    if (csv) fprintf(csv, "soal,ukuran,seimbang,optimal,VAM,RBSM,SAM,CSM,VAM_Pasangan,gap_VAM,gap_RBSM,gap_SAM,gap_CSM,gap_VAM_Pasangan,"
                          "t_us_Optimal,t_us_VAM,t_us_RBSM,t_us_SAM,t_us_CSM,t_us_VAM_Pasangan\n");
    /* waktu per soal: indeks 0 = optimal (MCF), 1..5 = metode */
    static double tm[2000][NMETHOD + 1]; static char tname[2000][128], tsize[2000][16];

    double sumgap[NMETHOD] = {0}; int nopt[NMETHOD] = {0}, nbest[NMETHOD] = {0}, ninf[NMETHOD] = {0};
    int win[NMETHOD] = {0}, tie[NMETHOD] = {0}, lose[NMETHOD] = {0}, nprob = 0, k;

    printf("%-12s %-6s %10s %10s %10s %10s %10s %13s\n", "Soal", "Ukuran", "Optimal",
           "VAM", "RBSM", "SAM", "CSM", "VAM-Pasangan");
    printf("--------------------------------------------------------------------------------------------\n");
    for (int f = 0; f < nf; f++) {
        char path[1024]; static Problem P;
        snprintf(path, sizeof(path), "%s/%s", dir, files[f]);
        if (!read_problem(path, &P)) { printf("%-12s gagal dibaca\n", files[f]); continue; }
        strncpy(P.name, files[f], sizeof(P.name) - 1);
        P.name[strlen(P.name) - 4] = 0;

        long long opt = optimal_cost(&P);
        Result r[NMETHOD];
        r[0] = run_vam(&P); r[1] = run_rbsm(&P); r[2] = run_sam(&P); r[3] = run_csm(&P); r[4] = run_vam_pasangan(&P);
        tm[nprob][0] = time_method(run_optimal, &P);
        for (k = 0; k < NMETHOD; k++) tm[nprob][k + 1] = time_method(METHOD_FN[k], &P);

        char sz[16]; snprintf(sz, sizeof(sz), "%dx%d", P.m0, P.n0);
        strcpy(tname[nprob], P.name); strcpy(tsize[nprob], sz);
        printf("%-12s %-6s %10lld", P.name, sz, opt);
        long long best = LLONG_MAX;
        for (k = 0; k < NMETHOD; k++) if (r[k].feasible && r[k].cost < best) best = r[k].cost;
        for (k = 0; k < NMETHOD; k++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld%s", r[k].cost, r[k].feasible ? "" : "*");
            printf(k == 4 ? " %13s" : " %10s", buf);
        }
        printf("%s\n", P.dummy ? "  (dummy)" : "");

        if (csv) {
            fprintf(csv, "%s,%s,%s,%lld", P.name, sz, P.dummy ? "tidak" : "ya", opt);
            for (k = 0; k < NMETHOD; k++) fprintf(csv, ",%lld%s", r[k].cost, r[k].feasible ? "" : "*");
            for (k = 0; k < NMETHOD; k++)
                fprintf(csv, ",%.4f", opt > 0 ? 100.0 * (r[k].cost - opt) / opt : 0.0);
            for (k = 0; k <= NMETHOD; k++) fprintf(csv, ",%.3f", tm[nprob][k]);
            fprintf(csv, "\n");
        }
        nprob++;
        for (k = 0; k < NMETHOD; k++) {
            if (!r[k].feasible) ninf[k]++;
            if (r[k].feasible) sumgap[k] += opt > 0 ? 100.0 * (r[k].cost - opt) / opt : 0.0;
            if (r[k].feasible && r[k].cost == opt) nopt[k]++;
            if (r[k].feasible && r[k].cost == best) nbest[k]++;
            if (k < 4) {   /* VAM Pasangan dibanding metode k */
                long long a = r[4].feasible ? r[4].cost : LLONG_MAX, b = r[k].feasible ? r[k].cost : LLONG_MAX;
                if (a < b) win[k]++; else if (a == b) tie[k]++; else lose[k]++;
            }
        }
    }
    if (csv) fclose(csv);

    printf("\n* = solusi tidak layak (supply/demand tidak terpenuhi). (dummy) = soal tidak seimbang, ditambah dummy biaya 0.\n");
    printf("\nRINGKASAN (%d soal)\n", nprob);
    printf("--------------------------------------------------------------------------\n");
    printf("%-14s %16s %16s %16s %10s\n", "Metode", "Rata-rata gap*", "Soal optimal", "Terbaik/seri", "Tdk layak");
    for (k = 0; k < NMETHOD; k++)
        printf("%-14s %15.2f%% %8d (%5.1f%%) %8d (%5.1f%%) %10d\n", METHOD_NAME[k],
               nprob - ninf[k] > 0 ? sumgap[k] / (nprob - ninf[k]) : 0.0,
               nopt[k], 100.0 * nopt[k] / nprob, nbest[k], 100.0 * nbest[k] / nprob, ninf[k]);
    printf("* rata-rata gap dihitung dari solusi yang layak saja.\n");
    printf("\nVAM Pasangan dibanding metode lain (lebih murah / sama / lebih mahal):\n");
    printf("  (solusi tidak layak dianggap kalah)\n");
    for (k = 0; k < 4; k++)
        printf("  vs %-6s : %3d / %3d / %3d\n", METHOD_NAME[k], win[k], tie[k], lose[k]);

    /* ---------------- runtime ---------------- */
    const char *TNAME[NMETHOD + 1] = {"Optimal(MCF)", "VAM", "RBSM", "SAM", "CSM", "VAM-Pasangan"};
    printf("\nRUNTIME PER SOAL (mikrodetik, rata-rata 1x jalan)\n");
    printf("--------------------------------------------------------------------------------------------\n");
    printf("%-12s %-6s %12s %10s %10s %10s %10s %13s\n", "Soal", "Ukuran", TNAME[0],
           "VAM", "RBSM", "SAM", "CSM", "VAM-Pasangan");
    for (int f = 0; f < nprob; f++) {
        printf("%-12s %-6s %12.3f", tname[f], tsize[f], tm[f][0]);
        for (k = 1; k <= NMETHOD; k++) printf(k == NMETHOD ? " %13.3f" : " %10.3f", tm[f][k]);
        printf("\n");
    }

    int nfast[NMETHOD + 1] = {0};
    for (int f = 0; f < nprob; f++) {
        int bk = 1;
        for (k = 2; k <= NMETHOD; k++) if (tm[f][k] < tm[f][bk]) bk = k;
        nfast[bk]++;
    }
    static double col[2000];
    printf("\nRINGKASAN RUNTIME (%d soal; tiap metode diulang sampai >= %.0f ms lalu dirata-rata)\n",
           nprob, TIME_WINDOW_US / 1000.0);
    printf("-----------------------------------------------------------------------------------------\n");
    printf("%-14s %11s %11s %11s %10s %11s %12s\n", "Metode", "Total (ms)", "Rata2 (us)", "Median (us)",
           "Min (us)", "Maks (us)", "Tercepat**");
    for (k = 0; k <= NMETHOD; k++) {
        double tot = 0;
        for (int f = 0; f < nprob; f++) { col[f] = tm[f][k]; tot += col[f]; }
        qsort(col, nprob, sizeof(double), cmp_dbl);
        double med = nprob % 2 ? col[nprob / 2] : (col[nprob / 2 - 1] + col[nprob / 2]) / 2;
        printf("%-14s %11.3f %11.3f %11.3f %10.3f %11.3f", TNAME[k], tot / 1000.0, tot / nprob, med,
               col[0], col[nprob - 1]);
        if (k == 0) printf(" %12s\n", "-"); else printf(" %5d (%4.1f%%)\n", nfast[k], 100.0 * nfast[k] / nprob);
    }
    printf("** jumlah soal di mana metode tsb paling cepat di antara 5 metode heuristik.\n");
    printf("   Optimal(MCF) = waktu menghitung solusi optimal, sebagai pembanding.\n");
    printf("\nHasil per soal juga disimpan di %s\n", csvname);
    return 0;
}
