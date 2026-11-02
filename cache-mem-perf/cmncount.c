/*
 * cmncount.c - count Arm CMN mesh PMU events around one command.
 *
 * The Asimov image has no perf tool, so this is the small part of
 * "perf stat" that the cache tests need: open a few mesh events through
 * perf_event_open, run one command, and append one CSV line per event.
 *
 *   cmncount [--pmu arm_cmn_0] [--nodes 88,160,...] [--events a,b]
 *            [--global c,d] [--out FILE] [--label TEXT] [--dry-run] -- cmd...
 *
 * --events are opened once per node id in --nodes (bynodeid=1, nodeid=id),
 * --global once, summed over every node of the event's type. Event names and
 * the config bit layout come from the PMU's sysfs directory, the same files
 * the perf tool reads. Rules of the arm-cmn driver (drivers/perf/arm-cmn.c):
 * the events are system-wide (pid -1) on the PMU's own CPU, no exclude bits,
 * and they cannot share a group with core events, so each one is opened alone
 * and the kernel reports enabled and running time; a line whose running time
 * is below its enabled time was multiplexed and is scaled.
 *
 * Counts cover the whole child process: program load, buffer fill, chain
 * build, warm-up, flushes and timed passes together, plus the idle kernel on
 * the other CPUs. For the OCM rows every buffer touch goes to the same home
 * node, so the per-node split is the point, not the absolute count; a control
 * row with a tiny DRAM buffer gives the background per node. A counter whose
 * enable fails, or whose running time is zero, gets a comment line, never a
 * numeric row.
 *
 * The command always runs. If the PMU directory does not exist, or the node
 * list or an event name is bad, the command runs uncounted (or with the
 * remaining events) and a '# cmncount:' comment line in the output says so.
 * Every run, counted or not, also appends two timing rows on CLOCK_MONOTONIC
 * (chip time, 1 ns resolution): 'child_elapsed_ns', the command's lifetime from
 * fork to exit, and 'helper_elapsed_ns', this program's own lifetime from the
 * entry of main to the point where every preceding record is flushed to the
 * output file, which also covers the sysfs reads, the counter open, enable,
 * disable, read and close, and the output. Counted and uncounted runs flush at
 * the same point, just before the clock is read; writing the helper row itself
 * is outside the interval. The helper row minus the child row is the helper's
 * in-process work; the exec and load of the helper before main are not measured.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_EV 64
#define NAME_MAX_LEN 64

struct ev {
    char name[NAME_MAX_LEN];
    long nodeid; /* -1: summed over all nodes */
    int fd;
    int err; /* errno from perf_event_open, 0 if open */
    int bad; /* 1: event text or format could not be read */
    uint64_t config[3];
};

static const char *pmu = "arm_cmn_0";
static char sysfs[256];

static int read_text(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
        buf[--r] = '\0';
    return (int)r;
}

/* format/<field> holds "config:16-26", "config1:0-63" or "config:31". */
static int set_field(struct ev *e, const char *field, uint64_t val)
{
    char path[1024], spec[64];
    snprintf(path, sizeof path, "%s/format/%.63s", sysfs, field);
    if (read_text(path, spec, sizeof spec) < 0) {
        fprintf(stderr, "cmncount: no format/%s in %s\n", field, sysfs);
        return -1;
    }
    int which;
    if (!strncmp(spec, "config2:", 8)) which = 2;
    else if (!strncmp(spec, "config1:", 8)) which = 1;
    else if (!strncmp(spec, "config:", 7)) which = 0;
    else {
        fprintf(stderr, "cmncount: bad format %s: %s\n", field, spec);
        return -1;
    }
    const char *bits = strchr(spec, ':') + 1;
    char *end;
    long lo = strtol(bits, &end, 10), hi = lo;
    if (*end == '-')
        hi = strtol(end + 1, &end, 10);
    if (*end != '\0' || lo < 0 || hi < lo || hi > 63) {
        fprintf(stderr, "cmncount: bad bit range %s: %s\n", field, spec);
        return -1;
    }
    uint64_t width = (uint64_t)(hi - lo + 1);
    uint64_t mask = width == 64 ? ~0ULL : ((1ULL << width) - 1);
    if (val & ~mask) {
        fprintf(stderr, "cmncount: value 0x%llx does not fit %s (%s)\n", (unsigned long long)val, field, spec);
        return -1;
    }
    e->config[which] |= (val & mask) << lo;
    return 0;
}

/* events/<name> holds "type=0x200,eventid=0x5,occupid=0x0". */
static int build_event(struct ev *e)
{
    char path[1024], desc[256];
    snprintf(path, sizeof path, "%s/events/%.63s", sysfs, e->name);
    if (read_text(path, desc, sizeof desc) < 0) {
        fprintf(stderr, "cmncount: unknown event %s (no %s)\n", e->name, path);
        return -1;
    }
    for (char *tok = strtok(desc, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            fprintf(stderr, "cmncount: bad event text %s: %s\n", e->name, tok);
            return -1;
        }
        *eq = '\0';
        if (set_field(e, tok, strtoull(eq + 1, NULL, 0)) < 0)
            return -1;
    }
    if (e->nodeid >= 0) {
        if (set_field(e, "bynodeid", 1) < 0 || set_field(e, "nodeid", (uint64_t)e->nodeid) < 0)
            return -1;
    }
    return 0;
}

static int parse_list(const char *s, char out[][NAME_MAX_LEN], int max)
{
    char buf[1024];
    int n = 0;
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        if (n == max || strlen(tok) >= NAME_MAX_LEN) {
            fprintf(stderr, "cmncount: too many or too long entries in %s\n", s);
            exit(2);
        }
        memcpy(out[n++], tok, strlen(tok) + 1);
    }
    return n;
}

static FILE *out;
static const char *label = "-";

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* fork, exec, wait; prints the child's lifetime as a row; returns the exit status to use */
static int run_child(char **cmd)
{
    double t0 = now_ns();
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 2 << 8; /* an encoded wait status, like the waitpid failure below */
    }
    if (pid == 0) {
        execvp(cmd[0], cmd);
        perror(cmd[0]);
        _exit(127);
    }
    int status = 0;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
        ;
    if (w < 0) {
        perror("waitpid");
        status = 2 << 8; /* report failure, still print what the counters hold */
    }
    double el = now_ns() - t0;
    fprintf(out, "%s,child_elapsed_ns,-,%.0f,%.0f,%.0f,%.0f\n", label, el, el, el, el);
    return status;
}

static double t_main; /* set at the entry of main */

/* prints this program's own lifetime as a row; call after the counters are read.
 * Every preceding record is flushed before the clock is read, on every path, so
 * counted and uncounted runs carry the same output work; the row itself is outside. */
static void helper_row(void)
{
    fflush(out);
    double el = now_ns() - t_main;
    fprintf(out, "%s,helper_elapsed_ns,-,%.0f,%.0f,%.0f,%.0f\n", label, el, el, el, el);
}

static int exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* Run the command without counters, after one comment line that says why. */
static int run_uncounted(char **cmd, const char *why, int dry)
{
    fprintf(out, "# cmncount: %s, %s ran uncounted\n", why, label);
    if (dry) {
        fflush(out);
        return 0;
    }
    int status = run_child(cmd);
    helper_row();
    fflush(out);
    return exit_code(status);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: cmncount [--pmu NAME] [--nodes ID,ID,..] [--events E,E,..] [--global E,E,..]\n"
            "                [--out FILE] [--label TEXT] [--dry-run] -- command [args..]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *nodes = "", *events = "", *globals = "", *outpath = NULL;
    int dry = 0, i;
    t_main = now_ns();
    for (i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--pmu") && more) pmu = argv[++i];
        else if (!strcmp(argv[i], "--nodes") && more) nodes = argv[++i];
        else if (!strcmp(argv[i], "--events") && more) events = argv[++i];
        else if (!strcmp(argv[i], "--global") && more) globals = argv[++i];
        else if (!strcmp(argv[i], "--out") && more) outpath = argv[++i];
        else if (!strcmp(argv[i], "--label") && more) label = argv[++i];
        else usage();
    }
    if (!dry && (i >= argc - 1))
        usage();
    char **cmd = argv + i + 1;

    out = stdout;
    if (outpath) {
        out = fopen(outpath, "ae");
        if (!out) {
            perror(outpath);
            return 2;
        }
        struct stat st;
        if (fstat(fileno(out), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0)
            fprintf(out, "label,event,nodeid,count,enabled_ns,running_ns,scaled_count\n");
    }

    char evn[MAX_EV][NAME_MAX_LEN], gln[MAX_EV][NAME_MAX_LEN], nd[MAX_EV][NAME_MAX_LEN];
    int nev = *events ? parse_list(events, evn, MAX_EV) : 0;
    int ngl = *globals ? parse_list(globals, gln, MAX_EV) : 0;
    int nnd = *nodes ? parse_list(nodes, nd, MAX_EV) : 0;
    if (nev && !nnd)
        return run_uncounted(cmd, "--events needs --nodes", dry);

    struct ev ev[MAX_EV];
    int n = 0;
    for (int a = 0; a < nev; a++)
        for (int b = 0; b < nnd; b++) {
            if (n == MAX_EV) { fprintf(stderr, "cmncount: too many counters\n"); return 2; }
            memset(&ev[n], 0, sizeof ev[n]);
            memcpy(ev[n].name, evn[a], NAME_MAX_LEN);
            char *end;
            ev[n].nodeid = strtol(nd[b], &end, 0);
            if (*end || ev[n].nodeid < 0) {
                char why[128];
                snprintf(why, sizeof why, "bad node id %.63s", nd[b]);
                return run_uncounted(cmd, why, dry);
            }
            n++;
        }
    for (int a = 0; a < ngl; a++) {
        if (n == MAX_EV) { fprintf(stderr, "cmncount: too many counters\n"); return 2; }
        memset(&ev[n], 0, sizeof ev[n]);
        memcpy(ev[n].name, gln[a], NAME_MAX_LEN);
        ev[n].nodeid = -1;
        n++;
    }
    snprintf(sysfs, sizeof sysfs, "/sys/bus/event_source/devices/%s", pmu);
    char text[64];
    char path[1024];
    snprintf(path, sizeof path, "%s/type", sysfs);
    if (read_text(path, text, sizeof text) < 0) {
        char why[128];
        snprintf(why, sizeof why, "%.63s absent", pmu);
        return run_uncounted(cmd, why, dry);
    }
    unsigned pmu_type = (unsigned)strtoul(text, NULL, 0);
    snprintf(path, sizeof path, "%s/cpumask", sysfs);
    int pmu_cpu = 0;
    if (read_text(path, text, sizeof text) >= 0)
        pmu_cpu = (int)strtol(text, NULL, 10); /* first cpu of "2" or "2-9" */

    if (n == 0)
        return run_uncounted(cmd, "no events requested, timing only", dry);
    int good = 0;
    for (int k = 0; k < n; k++) {
        ev[k].bad = build_event(&ev[k]) < 0;
        ev[k].fd = -1;
        good += !ev[k].bad;
    }
    if (!good)
        return run_uncounted(cmd, "no usable event", dry);

    if (dry) {
        fprintf(out, "# cmncount dry run: pmu %s type %u cpu %d\n", pmu, pmu_type, pmu_cpu);
        for (int k = 0; k < n; k++)
            if (ev[k].bad)
                fprintf(out, "# cmncount: %s unknown\n", ev[k].name);
            else
            fprintf(out, "%s,%s,%ld,config=0x%llx,config1=0x%llx,config2=0x%llx\n", label, ev[k].name, ev[k].nodeid,
                    (unsigned long long)ev[k].config[0], (unsigned long long)ev[k].config[1],
                    (unsigned long long)ev[k].config[2]);
        return 0;
    }

    for (int k = 0; k < n; k++) {
        struct perf_event_attr a;
        if (ev[k].bad)
            continue;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = pmu_type;
        a.config = ev[k].config[0];
        a.config1 = ev[k].config[1];
        a.config2 = ev[k].config[2];
        a.disabled = 1;
        a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        ev[k].fd = (int)syscall(__NR_perf_event_open, &a, -1, pmu_cpu, -1, PERF_FLAG_FD_CLOEXEC);
        ev[k].err = ev[k].fd < 0 ? errno : 0;
    }
    for (int k = 0; k < n; k++)
        if (ev[k].fd >= 0 && ioctl(ev[k].fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
            ev[k].err = errno;
            close(ev[k].fd);
            ev[k].fd = -1;
        }

    int status = run_child(cmd);
    for (int k = 0; k < n; k++)
        if (ev[k].fd >= 0)
            ioctl(ev[k].fd, PERF_EVENT_IOC_DISABLE, 0);

    for (int k = 0; k < n; k++) {
        char nid[24];
        if (ev[k].nodeid >= 0)
            snprintf(nid, sizeof nid, "%ld", ev[k].nodeid);
        else
            snprintf(nid, sizeof nid, "-");
        if (ev[k].bad) {
            fprintf(out, "# cmncount: %s unknown, not counted\n", ev[k].name);
            continue;
        }
        if (ev[k].fd < 0) {
            fprintf(out, "# cmncount: %s nodeid %s unavailable (%s)\n", ev[k].name, nid, strerror(ev[k].err));
            continue;
        }
        uint64_t v[3] = { 0, 0, 0 };
        if (read(ev[k].fd, v, sizeof v) != (ssize_t)sizeof v) {
            fprintf(out, "# cmncount: %s nodeid %s read failed (%s)\n", ev[k].name, nid, strerror(errno));
            continue;
        }
        if (v[2] == 0) {
            fprintf(out, "# cmncount: %s nodeid %s did not run (running_ns 0, enabled_ns %llu)\n", ev[k].name, nid,
                    (unsigned long long)v[1]);
            close(ev[k].fd);
            continue;
        }
        double scaled = (double)v[0] * (double)v[1] / (double)v[2];
        fprintf(out, "%s,%s,%s,%llu,%llu,%llu,%.0f\n", label, ev[k].name, nid, (unsigned long long)v[0],
                (unsigned long long)v[1], (unsigned long long)v[2], scaled);
        close(ev[k].fd);
    }
    helper_row();
    fflush(out);
    return exit_code(status);
}
