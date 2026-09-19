/*
 * taucmp.c — compare the three candidate atmospheric-transmittance models,
 * and dry-run the two-distance discrimination experiment without hardware.
 *
 * The port has three ways to obtain transmittance, and they disagree:
 *
 *   1. the analytic ARM model   CalcFixRaw()           (thermometry.c)
 *   2. the tau_*.bin table      dyt_calib_tau_read_f()
 *   3. the MILI6_*.bin table    dyt_calib_tau_read_f()
 *
 * (2) and (3) are the same reader over different data; they are listed
 * separately because the data turns out to be structured differently
 * (see RE Docs 10 §3.4).
 *
 * Two modes:
 *   (default)  report the tau each model gives over a distance sweep, plus
 *              the row structure of each table.  This is what produced
 *              RE Docs 10 §3.4 and §8.3.
 *   --sim T    dry-run the experiment: propagate each model's tau through a
 *              Stefan-Boltzmann radiance inversion to get the *apparent*
 *              temperature a blackbody at T would show at each distance, and
 *              report whether the models are separable above a given noise
 *              floor.  This is a sensitivity calculation for planning, NOT a
 *              measurement prediction -- it uses the port's T^4 radiance
 *              convention, not the vendor's full inversion.
 *
 * usage: taucmp [-d DIR] [--air C] [--humi F] [--emiss F]
 *               [--sim T_C] [--noise K] [--dist a,b,c]
 * build: make build/taucmp
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calib.h"
#include "thermometry.h"

static const double k_dist[] = {0.25, 0.50, 1.00, 2.00, 3.00, 5.00, 10.0, 20.0, 50.0};
#define NDIST ((int)(sizeof k_dist / sizeof k_dist[0]))

static const char *g_dir   = "calib";
static float g_air         = 25.0f;
static float g_humi        = 0.5f;
static float g_emiss       = 0.95f;
static int   g_sim         = 0;
static float g_target      = 100.0f;
static float g_noise       = 0.2f;   /* assumed repeatability, kelvin */

static double g_simdist[3] = {0.50, 2.00, 10.0};
#define NSIM ((int)(sizeof g_simdist / sizeof g_simdist[0]))

static void print_dist_header(void)
{
    printf("  dist(m)  :");
    for (int i = 0; i < NDIST; i++) printf(" %6.2f", k_dist[i]);
    printf("\n");
}

/* ---------------------------------------------------------------- */

static void report_analytic(void)
{
    float tau, te, ite, bg;

    printf("== analytic CalcFixRaw tau_eff, ambient %.1f C, vs humidity ==\n", g_air);
    for (int h = 0; h <= 4; h++) {
        float hv = (float)h * 0.25f;
        printf("  humi=%.2f :", hv);
        for (int i = 0; i < NDIST; i++) {
            CalcFixRaw(g_air, hv, (float)k_dist[i], g_emiss, g_air,
                       &tau, &te, &ite, &bg);
            printf(" %6.4f", te);
        }
        printf("\n");
    }
    print_dist_header();
    printf("\n");
}

static int report_table(const char *file)
{
    dyt_calib_table_t t;
    char path[512];
    static const int temps[] = {0, 100, 300, 1000};

    snprintf(path, sizeof path, "%s/%s", g_dir, file);
    if (dyt_calib_load(&t, path) != 0) {
        fprintf(stderr, "taucmp: cannot load %s\n", path);
        return -1;
    }

    printf("== %s : tau at fixed target temperatures ==\n", file);
    for (unsigned k = 0; k < sizeof temps / sizeof temps[0]; k++) {
        printf("  T=%-5d  :", temps[k]);
        for (int i = 0; i < NDIST; i++) {
            float v;
            if (dyt_calib_tau_read_f(&t, (float)temps[k], (float)k_dist[i], &v) != 0) {
                printf("  err  ");
                continue;
            }
            printf(" %6.4f", v);
        }
        printf("\n");
    }
    print_dist_header();

    /* Row structure: distinct rows, periodicity, and where each row stops
     * varying with distance.  These numbers are what exposed the tau_*.bin
     * anomalies (RE Docs 10 §3.4). */
    int distinct = 0;
    for (int r = 0; r < t.rows; r++) {
        int seen = 0;
        for (int p = 0; p < r; p++) {
            if (memcmp(t.data + (size_t)r * t.cols, t.data + (size_t)p * t.cols,
                       (size_t)t.cols * sizeof(uint16_t)) == 0) { seen = 1; break; }
        }
        if (!seen) distinct++;
    }

    int periodic14 = 0;
    if (t.rows >= 28) {
        size_t span = (size_t)t.cols * 14;
        periodic14 = 1;
        for (size_t i = 0; i + span < (size_t)t.rows * t.cols; i++) {
            if (t.data[i] != t.data[i + span]) { periodic14 = 0; break; }
        }
    }

    printf("  rows=%d distinct_rows=%d periodic_period14=%s\n",
           t.rows, distinct, periodic14 ? "YES" : "no");

    {
        int flat_at[64], nflat = 0, varies = 0;
        int naxis = 0;
        const double *axis = dyt_calib_dist_axis(t.layout, &naxis);
        for (int r = 0; r < t.rows; r++) {
            const uint16_t *row = t.data + (size_t)r * t.cols;
            int i = t.cols - 1;
            while (i > 0 && row[i - 1] == row[i]) i--;
            if (i >= t.cols - 1) { varies++; continue; }
            int found = 0;
            for (int k = 0; k < nflat; k++) if (flat_at[k] == i) { found = 1; break; }
            if (!found && nflat < 64) flat_at[nflat++] = i;
        }
        printf("  rows varying to the last column: %d\n", varies);
        for (int k = 0; k < nflat; k++) {
            int n = 0;
            for (int r = 0; r < t.rows; r++) {
                const uint16_t *row = t.data + (size_t)r * t.cols;
                int i = t.cols - 1;
                while (i > 0 && row[i - 1] == row[i]) i--;
                if (i == flat_at[k]) n++;
            }
            double dm = (flat_at[k] < naxis) ? axis[flat_at[k]] : -1.0;
            printf("  rows flat from col%-3d (%5.2f m) to the end: %d\n",
                   flat_at[k], dm, n);
        }
    }
    printf("\n");
    dyt_calib_free(&t);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Dry run: tau -> apparent temperature, per model.                  */
/* ---------------------------------------------------------------- */

/* Radiance convention used by the port: L(T) proportional to (T_K)^4.
 * The proportionality constant cancels, so work in arbitrary units. */
static double radiance(double t_c)
{
    double k = t_c + 273.15;
    return k * k * k * k;
}

static double apparent(double tau, double t_target, double t_amb)
{
    double l = tau * radiance(t_target) + (1.0 - tau) * radiance(t_amb);
    return pow(l, 0.25) - 273.15;
}

static int dry_run(void)
{
    dyt_calib_table_t tau_h, mili6_h;
    char path[512];
    float junk0, junk1, junk2, junk3;
    double tau_an[NSIM];
    double tau_tb[NSIM];
    double tau_m6[NSIM];
    double app_an[NSIM], app_tb[NSIM], app_m6[NSIM];

    snprintf(path, sizeof path, "%s/tau_H.bin", g_dir);
    if (dyt_calib_load(&tau_h, path) != 0) { fprintf(stderr, "taucmp: cannot load %s\n", path); return -1; }
    snprintf(path, sizeof path, "%s/MILI6_H.bin", g_dir);
    if (dyt_calib_load(&mili6_h, path) != 0) { fprintf(stderr, "taucmp: cannot load %s\n", path); dyt_calib_free(&tau_h); return -1; }

    printf("== DRY RUN: apparent temperature of a %.0f C blackbody, ambient %.0f C, humi %.2f ==\n",
           g_target, g_air, g_humi);
    printf("   assumed repeatability %.2f K.  Radiance model: L ~ T^4 (port convention).\n", g_noise);
    printf("   This is a SENSITIVITY calculation, not a measurement prediction.\n\n");

    for (int i = 0; i < NSIM; i++) {
        double d = g_simdist[i];
        CalcFixRaw(g_air, g_humi, (float)d, g_emiss, g_air,
                   &junk0, &junk1, &junk2, &junk3);
        tau_an[i] = junk1;                       /* tau_eff is the 2nd out-param */
        dyt_calib_tau_read_f(&tau_h,   g_target, (float)d, &junk0);
        tau_tb[i] = junk0;
        dyt_calib_tau_read_f(&mili6_h, g_target, (float)d, &junk0);
        tau_m6[i] = junk0;
        app_an[i] = apparent(tau_an[i], g_target, g_air);
        app_tb[i] = apparent(tau_tb[i], g_target, g_air);
        app_m6[i] = apparent(tau_m6[i], g_target, g_air);
    }

    printf("  %-10s %10s %10s %10s\n", "distance", "analytic", "tau_H", "MILI6_H");
    for (int i = 0; i < NSIM; i++)
        printf("  %7.2f m  %9.3fC %9.3fC %9.3fC\n", g_simdist[i], app_an[i], app_tb[i], app_m6[i]);

    printf("\n  %-10s %10s %10s %10s\n", "model", "tau(2m)", "tau(10m)", "delta tau");
    printf("  %-10s %10.4f %10.4f %10.4f\n", "analytic", tau_an[1], tau_an[2], tau_an[1] - tau_an[2]);
    printf("  %-10s %10.4f %10.4f %10.4f\n", "tau_H",    tau_tb[1], tau_tb[2], tau_tb[1] - tau_tb[2]);
    printf("  %-10s %10.4f %10.4f %10.4f\n", "MILI6_H",  tau_m6[1], tau_m6[2], tau_m6[1] - tau_m6[2]);

    /* The discriminator: the apparent-temperature drop from 2 m to 10 m.
     * Absolute accuracy is irrelevant; only this delta matters. */
    double d_an = app_an[1] - app_an[2];
    double d_tb = app_tb[1] - app_tb[2];
    double d_m6 = app_m6[1] - app_m6[2];

    printf("\n  predicted apparent-temperature DROP from %.2f m to %.2f m:\n",
           g_simdist[1], g_simdist[2]);
    printf("    analytic  %+.3f K\n", d_an);
    printf("    tau_H     %+.3f K\n", d_tb);
    printf("    MILI6_H   %+.3f K\n", d_m6);

    double g1 = fabs(d_an - d_tb);
    double g2 = fabs(d_tb - d_m6);
    double g3 = fabs(d_an - d_m6);
    double sep = g1 < g2 ? g1 : g2;
    if (g3 < sep) sep = g3;

    printf("\n  closest pair of predictions differs by %.3f K; noise floor %.2f K\n", sep, g_noise);
    if (sep > 5.0 * g_noise)
        printf("  VERDICT: separable with wide margin (%.1fx noise). Experiment is worth running.\n", sep / g_noise);
    else if (sep > 2.0 * g_noise)
        printf("  VERDICT: separable (%.1fx noise). Experiment is viable.\n", sep / g_noise);
    else
        printf("  VERDICT: NOT separable at this noise floor (%.1fx). Need lower noise or a bigger span.\n", sep / g_noise);

    /* Sanity: the tau_*.bin plateau must show up as a near-zero drop. */
    if (fabs(d_tb) < 0.05)
        printf("  NOTE: tau_H predicts almost no change across %.2f m -> %.2f m, consistent\n"
               "        with its distance plateau (RE Docs 10 §3.4).\n",
               g_simdist[1], g_simdist[2]);

    dyt_calib_free(&tau_h);
    dyt_calib_free(&mili6_h);
    return 0;
}

/* ---------------------------------------------------------------- */

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [-d DIR] [--air C] [--humi F] [--emiss F]\n"
        "          [--sim T_C] [--noise K] [--dist a,b,c]\n\n"
        "  default     report tau over a distance sweep + table row structure\n"
        "  --sim T_C   dry-run the 2-distance experiment for a blackbody at T_C\n",
        p);
}

int main(int argc, char **argv)
{
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)            g_dir  = argv[++i];
        else if (!strcmp(argv[i], "--air") && i + 1 < argc)    g_air  = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--humi") && i + 1 < argc)   g_humi = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--emiss") && i + 1 < argc)  g_emiss = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--noise") && i + 1 < argc)  g_noise = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--sim")) {
            g_sim = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') g_target = strtof(argv[++i], NULL);
        }
        else if (!strcmp(argv[i], "--dist") && i + 1 < argc) {
            char *s = argv[++i];
            for (int k = 0; k < NSIM; k++) {
                g_simdist[k] = strtod(s, &s);
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "taucmp: unknown argument '%s'\n", argv[i]); usage(argv[0]); return 2; }
    }

    printf("taucmp: ambient=%.1f C  humi=%.2f  emiss=%.2f  tables=%s\n\n",
           g_air, g_humi, g_emiss, g_dir);

    if (g_sim)
        return dry_run() != 0;

    report_analytic();
    rc |= report_table("tau_H.bin");
    rc |= report_table("tau_L.bin");
    rc |= report_table("MILI6_H.bin");
    rc |= report_table("MILI6_L.bin");
    return rc ? 1 : 0;
}
