#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sensors.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define FAN_LEVEL0 0x00
#define FAN_LEVEL1 0x01
#define FAN_LEVEL2 0x02
#define FAN_LEVEL3 0x03
#define FAN_LEVEL4 0x04
#define FAN_LEVEL5 0x05
#define FAN_LEVEL6 0x06
#define FAN_LEVEL7 0x07
#define FAN_UNTHROTTLE 0x40
#define FAN_AUTO 0x80

#define CONFIG "/etc/fand.conf"
#define NLEVELS 9

struct level {
    int num;
    int min;
    int max;
};

int mib_temp[5];
int mib_flvl[2] = {CTL_HW, HW_FANLEVEL};

static void usage(void)
{
    fprintf(stderr, "usage: fand [-c config] [-d] [-h]\n");
}

/*
 * Returns substring of where all leading and trailing spaces
 * are removed. It is callers responsibility to free memory.
 */
static char *trim(char *s)
{
    char *p = s;

    while (isspace(*p)) {
        p++;
    }

    size_t n = strlen(p);
    while (n > 0) {
        if (!isspace(p[n - 1])) {
            break;
        }
        n--;
    }

    char *b = malloc(n + 1);
    if (b == NULL) {
        err(EXIT_FAILURE, "malloc");
    }
    strlcpy(b, p, n + 1);

    return b;
}

static int iscomment(char *s)
{
    return strncmp(s, "#", 1) == 0;
}

static int parseline(char *s, int *lvl, int *min, int *max)
{
    if (strncmp(s, "level", 5) != 0) {
        return -1;
    }

    s += 5;

    if (!isdigit(*s)) {
        return -1;
    }
    *lvl = *s++ - 0x30;

    while (isspace(*s)) {
        s++;
    }
    if (*s++ != '=') {
        return -1;
    }
    while (isspace(*s)) {
        s++;
    }

    int maxnum = 8;
    char buf[maxnum + 1];

    char *p = strchr(s, '-');
    if (p == NULL) {
        return -1;
    }
    int n = p - s;
    if (n > maxnum) {
        return -1;
    }
    strlcpy(buf, s, n + 1);
    *min = strtonum(buf, 0, 1024, NULL);
    if (errno != 0) {
        return -1;
    }
    s = p + 1;
    *max = strtonum(s, 0, 1024, NULL);
    if (errno != 0) {
        return -1;
    }

    return 0;
}

static void parseconfig(const char *fname, struct level *levels)
{
    FILE *f = fopen(fname, "r");
    if (f == NULL) {
        err(EXIT_FAILURE, "failed to open %s", fname);
    }

    int presents[NLEVELS] = {0};
    int linenum = 0;
    char *line = NULL;
    size_t linelen = 0;
    ssize_t n;

    while ((n = getline(&line, &linelen, f)) != -1) {
        linenum++;
        char *tline = trim(line);
        if (!iscomment(tline)) {
            int lvl;
            int min;
            int max;

            if (parseline(tline, &lvl, &min, &max) == -1) {
                errx(EXIT_FAILURE,
                    "failed to parse configuration file at line %d",
                    linenum);
            }
            if (lvl < 0 || lvl >= NLEVELS) {
                errx(EXIT_FAILURE, "invalid level %d", lvl);
            }
            levels[lvl].min = min;
            levels[lvl].max = max;
            presents[lvl] = 1;
        }

        free(tline);
    }

    free(line);
    if (ferror(f)) {
        err(EXIT_FAILURE, "failed to read configuration file");
    }

    fclose(f);

    for (int i = 0; i < NLEVELS; i++) {
        if (!presents[i]) {
            errx(EXIT_FAILURE, "missing configuration for level %d", i);
        }
    }
}

static void initmib(void)
{
    mib_temp[0] = CTL_HW;
    mib_temp[1] = HW_SENSORS;
    struct sensordev sdev;
    size_t sdevlen = sizeof(sdev);
    int found = 0;

    for (int dev = 0; ; dev++) {
        mib_temp[2] = dev;

        if (sysctl(mib_temp, 3, &sdev, &sdevlen, NULL, 0) == -1) {
            if (errno == ENXIO) {
                continue;
            }
            if (errno == ENOENT) {
                break;
            }
            err(EXIT_FAILURE, "CPU temperature sensor lookup failed");
        }
        if (strncmp(sdev.xname, "cpu", 3) != 0) {
            continue;
        }
        if(sdev.maxnumt[SENSOR_TEMP] <= 0) {
            continue;
        }

        mib_temp[2] = sdev.num;
        mib_temp[3] = SENSOR_TEMP;
        mib_temp[4] = 0;
        found = 1;
        break;
    }

    if (!found) {
        errx(EXIT_FAILURE, "no CPU temperature sensor found");
    }
}

static void setlevel(int lvl)
{
    sysctl(mib_flvl, 2, NULL, 0, &lvl, sizeof(lvl));
}

static void setauto(int sig __unused)
{
    setlevel(FAN_AUTO);
    exit(EXIT_FAILURE);
}

static int gettemp(void)
{
    struct sensor s;
    size_t slen = sizeof(s);

    if (sysctl(mib_temp, 5, &s, &slen, NULL, 0) == -1) {
        err(EXIT_FAILURE, "failed to read temperature");
    }

    return (s.value - 273150000) / 1000 / 1000;
}

int main(int argc, char *argv[])
{
    struct level levels[] = {
        {FAN_LEVEL0, 0, 0},
        {FAN_LEVEL1, 0, 0},
        {FAN_LEVEL2, 0, 0},
        {FAN_LEVEL3, 0, 0},
        {FAN_LEVEL4, 0, 0},
        {FAN_LEVEL5, 0, 0},
        {FAN_LEVEL6, 0, 0},
        {FAN_LEVEL7, 0, 0},
        {FAN_UNTHROTTLE, 0, 0},
    };
    int level = 3;
    int debug = 0;
    const char *config = CONFIG;

    int opt;
    while((opt = getopt(argc, argv, "c:dh")) != -1) {
        switch(opt) {
        case 'c':
            config = optarg;
            break;

        case 'd':
            debug = 1;
            break;

        case 'h':
            usage();
            return 0;

        default:
            usage();
            return EXIT_FAILURE;
        }
    }

    parseconfig(config, levels);

    if (getuid() != 0) {
        errx(EXIT_FAILURE, "must be root");
    }

    if (debug) {
        printf("Using levels configuration: ");
        for (int i = 0; i < NLEVELS; i++) {
            printf("%d=%d-%d", levels[i].num, levels[i].min, levels[i].max);
            if (i < NLEVELS - 1) {
                printf(", ");
            }
        }
        printf("\n");
    }

    initmib();
    setlevel(levels[level].num);

    signal(SIGINT, setauto);
    signal(SIGTERM, setauto);
    signal(SIGHUP, setauto);
    signal(SIGSEGV, setauto);

    if (!debug) {
        if (daemon(0, 0) != 0) {
            err(EXIT_FAILURE, "daemonization failed");
        }
    }

    for (;;) {
        int t = gettemp();

        if (debug) {
            printf("Temperature is %d\n", t);
        }

        if (t < levels[level].min && level > 0) {
            --level;
            if (debug) {
                printf("Setting fan level to %d.\n", levels[level].num);
            }
            setlevel(levels[level].num);
        } else if( t > levels[level].max && level < NLEVELS) {
            ++level;
            if (debug) {
                printf("Setting fan level to %d.\n", levels[level].num);
            }
            setlevel(levels[level].num);
        }

        sleep(2);
    }
}
