/*
 * Keyboard debounce filter for Linux.
 *
 * Intercepts a keyboard's evdev device, creates a virtual clone via uinput,
 * and suppresses key-down events that arrive too quickly after the previous
 * key-down for the same key. Eliminates key chatter / double-firing on
 * mechanical keyboards with worn switches.
 *
 * Only filters rapid duplicate presses. Normal typing, held-key repeats,
 * and key-up events are unaffected.
 *
 * Requirements:
 *     - Linux with evdev and uinput
 *     - User must be in the 'input' group (or run as root)
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#define DEFAULT_DEBOUNCE_MS 75

#define NBITS(x) ((((x) - 1) / (int)(8 * sizeof(long))) + 1)

#ifndef test_bit
#define test_bit(bit, array) \
    (((const unsigned long *)(array))[(bit) / (8 * sizeof(long))] & \
     (1UL << ((bit) % (8 * sizeof(long)))))
#endif

#ifndef set_bit
#define set_bit(bit, array) \
    ((array)[(bit) / (8 * sizeof(long))] |= (1UL << ((bit) % (8 * sizeof(long)))))
#endif

#ifndef clear_bit
#define clear_bit(bit, array) \
    ((array)[(bit) / (8 * sizeof(long))] &= ~(1UL << ((bit) % (8 * sizeof(long)))))
#endif

#define KEY_STATE_BYTES ((KEY_MAX + 7) / 8)
#define EVENT_BATCH_SIZE 64

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int open_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    return fd;
}

static int get_device_name(int fd, char *name, size_t name_size)
{
    if (ioctl(fd, EVIOCGNAME((int)name_size - 1), name) < 0) {
        snprintf(name, name_size, "unknown");
        return -1;
    }
    return 0;
}

static int count_ev_key_bits(int fd)
{
    unsigned long bits[NBITS(KEY_MAX)];

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        return 0;
    }

    int count = 0;
    for (int code = 0; code <= KEY_MAX; code++) {
        if (test_bit(code, bits)) {
            count++;
        }
    }
    return count;
}

static int has_keyboard_keys(int fd, int min_keys)
{
    unsigned long ev_bits[NBITS(EV_MAX)];

    memset(ev_bits, 0, sizeof(ev_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return 0;
    }

    if (!test_bit(EV_KEY, ev_bits)) {
        return 0;
    }

    return count_ev_key_bits(fd) > min_keys;
}

static int max_code_for_ev_type(unsigned int ev_type)
{
    switch (ev_type) {
    case EV_KEY:
        return KEY_MAX;
    case EV_REL:
        return REL_MAX;
    case EV_ABS:
        return ABS_MAX;
    case EV_MSC:
        return MSC_MAX;
    case EV_SW:
        return SW_MAX;
    case EV_LED:
        return LED_MAX;
    case EV_SND:
        return SND_MAX;
    case EV_FF:
        return FF_MAX;
    default:
        return 0;
    }
}

static int setup_uinput_from_device(int src_fd, int uinput_fd, const char *device_name)
{
    struct input_id id;
    unsigned long ev_bits[NBITS(EV_MAX)];
    char uinput_name[UINPUT_MAX_NAME_SIZE];

    if (ioctl(src_fd, EVIOCGID, &id) < 0) {
        perror("EVIOCGID");
        return -1;
    }

    memset(ev_bits, 0, sizeof(ev_bits));
    if (ioctl(src_fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        perror("EVIOCGBIT(EV)");
        return -1;
    }

    for (int ev_type = 0; ev_type <= EV_MAX; ev_type++) {
        int max_code;
        unsigned long type_bits[NBITS(KEY_MAX)];

        if (!test_bit(ev_type, ev_bits)) {
            continue;
        }

        if (ioctl(uinput_fd, UI_SET_EVBIT, ev_type) < 0) {
            perror("UI_SET_EVBIT");
            return -1;
        }

        max_code = max_code_for_ev_type((unsigned int)ev_type);
        if (max_code <= 0) {
            continue;
        }

        memset(type_bits, 0, sizeof(type_bits));
        if (ioctl(src_fd, EVIOCGBIT(ev_type, sizeof(type_bits)), type_bits) < 0) {
            perror("EVIOCGBIT(type)");
            return -1;
        }

        for (int code = 0; code <= max_code; code++) {
            if (!test_bit(code, type_bits)) {
                continue;
            }

            switch (ev_type) {
            case EV_KEY:
                if (ioctl(uinput_fd, UI_SET_KEYBIT, code) < 0) {
                    perror("UI_SET_KEYBIT");
                    return -1;
                }
                break;
            case EV_REL:
                if (ioctl(uinput_fd, UI_SET_RELBIT, code) < 0) {
                    perror("UI_SET_RELBIT");
                    return -1;
                }
                break;
            case EV_ABS:
                if (ioctl(uinput_fd, UI_SET_ABSBIT, code) < 0) {
                    perror("UI_SET_ABSBIT");
                    return -1;
                }
                break;
            case EV_MSC:
                if (ioctl(uinput_fd, UI_SET_MSCBIT, code) < 0) {
                    perror("UI_SET_MSCBIT");
                    return -1;
                }
                break;
            case EV_SW:
                if (ioctl(uinput_fd, UI_SET_SWBIT, code) < 0) {
                    perror("UI_SET_SWBIT");
                    return -1;
                }
                break;
            case EV_LED:
                if (ioctl(uinput_fd, UI_SET_LEDBIT, code) < 0) {
                    perror("UI_SET_LEDBIT");
                    return -1;
                }
                break;
            case EV_SND:
                if (ioctl(uinput_fd, UI_SET_SNDBIT, code) < 0) {
                    perror("UI_SET_SNDBIT");
                    return -1;
                }
                break;
            case EV_FF:
                if (ioctl(uinput_fd, UI_SET_FFBIT, code) < 0) {
                    perror("UI_SET_FFBIT");
                    return -1;
                }
                break;
            default:
                break;
            }
        }

        if (ev_type == EV_ABS) {
            for (int code = 0; code <= ABS_MAX; code++) {
                struct input_absinfo absinfo;
                struct uinput_abs_setup abs_setup;

                if (!test_bit(code, type_bits)) {
                    continue;
                }

                if (ioctl(src_fd, EVIOCGABS(code), &absinfo) < 0) {
                    perror("EVIOCGABS");
                    return -1;
                }

                memset(&abs_setup, 0, sizeof(abs_setup));
                abs_setup.code = (uint16_t)code;
                abs_setup.absinfo = absinfo;
                if (ioctl(uinput_fd, UI_ABS_SETUP, &abs_setup) < 0) {
                    perror("UI_ABS_SETUP");
                    return -1;
                }
            }
        }
    }

    snprintf(uinput_name, sizeof(uinput_name), "%.*s (debounced)",
             (int)(sizeof(uinput_name) - sizeof(" (debounced)")), device_name);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", uinput_name);
    usetup.id = id;

    if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP");
        return -1;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        return -1;
    }

    return 0;
}

static int foreach_event_device(int (*callback)(const char *path, void *ctx), void *ctx)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        perror("opendir /dev/input");
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (callback(path, ctx) != 0) {
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return 0;
}

static int list_device_callback(const char *path, void *ctx)
{
    (void)ctx;
    int fd = open_device(path);
    char name[256];
    int key_count;

    if (fd < 0) {
        return 0;
    }

    if (!has_keyboard_keys(fd, 50)) {
        close(fd);
        return 0;
    }

    get_device_name(fd, name, sizeof(name));
    key_count = count_ev_key_bits(fd);
    printf("  %s  %s  (%d keys)\n", path, name, key_count);
    close(fd);
    return 0;
}

static void list_keyboards(void)
{
    printf("Available keyboard devices:\n\n");
    foreach_event_device(list_device_callback, NULL);
    printf("\nUse -d /dev/input/eventX to select a device,\n");
    printf("or -n 'partial name' to match by name.\n");
}

typedef struct {
    const char *name_fragment;
    char path[PATH_MAX];
    char name[256];
    bool found;
} find_ctx;

static int find_device_callback(const char *path, void *ctx)
{
    find_ctx *fctx = ctx;
    int fd = open_device(path);
    char name[256];

    if (fd < 0) {
        return 0;
    }

    if (!has_keyboard_keys(fd, 100)) {
        close(fd);
        return 0;
    }

    get_device_name(fd, name, sizeof(name));
    if (fctx->name_fragment != NULL &&
        strcasestr(name, fctx->name_fragment) == NULL) {
        close(fd);
        return 0;
    }

    snprintf(fctx->path, sizeof(fctx->path), "%s", path);
    snprintf(fctx->name, sizeof(fctx->name), "%s", name);
    fctx->found = true;
    close(fd);
    return 1;
}

static bool find_keyboard(const char *name_fragment, char *path, size_t path_size,
                          char *name, size_t name_size)
{
    find_ctx fctx = {
        .name_fragment = name_fragment,
        .found = false,
    };

    foreach_event_device(find_device_callback, &fctx);
    if (!fctx.found) {
        return false;
    }

    snprintf(path, path_size, "%s", fctx.path);
    snprintf(name, name_size, "%s", fctx.name);
    return true;
}

static void write_event(int uinput_fd, const struct input_event *event)
{
    ssize_t n;

    do {
        n = write(uinput_fd, event, sizeof(*event));
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof(*event)) {
        perror("write uinput event");
        keep_running = 0;
    }
}

static void syn_report(int uinput_fd)
{
    struct input_event syn = {
        .type = EV_SYN,
        .code = SYN_REPORT,
        .value = 0,
    };

    write_event(uinput_fd, &syn);
}

static void sync_key_state(int src_fd, int uinput_fd, unsigned char *key_state)
{
    unsigned char src_state[KEY_STATE_BYTES];

    memset(src_state, 0, sizeof(src_state));
    if (ioctl(src_fd, EVIOCGKEY((int)sizeof(src_state)), src_state) < 0) {
        perror("EVIOCGKEY");
        return;
    }

    for (int code = 0; code <= KEY_MAX; code++) {
        bool src_pressed = test_bit(code, src_state);
        bool out_pressed = test_bit(code, key_state);

        if (src_pressed == out_pressed) {
            continue;
        }

        struct input_event ev = {
            .type = EV_KEY,
            .code = (uint16_t)code,
            .value = src_pressed ? 1 : 0,
        };

        write_event(uinput_fd, &ev);
        if (src_pressed) {
            set_bit(code, key_state);
        } else {
            clear_bit(code, key_state);
        }
    }

    syn_report(uinput_fd);
}

static bool should_forward_key_down(unsigned int code, double now, double debounce_s,
                                    double *last_down)
{
    if (code > KEY_MAX) {
        return true;
    }

    if ((now - last_down[code]) < debounce_s) {
        return false;
    }

    last_down[code] = now;
    return true;
}

static void update_key_state(unsigned char *key_state, const struct input_event *event)
{
    unsigned int code = event->code;

    if (code > KEY_MAX) {
        return;
    }

    if (event->value == 0) {
        clear_bit(code, key_state);
    } else if (event->value == 1) {
        set_bit(code, key_state);
    }
}

static bool process_event(int kbd_fd, int uinput_fd, const struct input_event *event,
                          double debounce_s, double *last_down, unsigned char *key_state,
                          bool *drop_until_syn)
{
    if (*drop_until_syn) {
        if (event->type == EV_SYN && event->code == SYN_REPORT) {
            *drop_until_syn = false;
            sync_key_state(kbd_fd, uinput_fd, key_state);
        }
        return false;
    }

    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        *drop_until_syn = true;
        return false;
    }

    if (event->type == EV_KEY) {
        unsigned int code = event->code;

        if (event->value == 1) {
            if (!should_forward_key_down(code, monotonic_seconds(), debounce_s, last_down)) {
                return false;
            }
        } else if (event->value == 0 && code <= KEY_MAX) {
            last_down[code] = 0.0;
        }
    }

    write_event(uinput_fd, event);
    if (event->type == EV_KEY) {
        update_key_state(key_state, event);
    }
    return true;
}

static void run(int debounce_ms, const char *device_path, const char *name_fragment)
{
    char path[PATH_MAX];
    char name[256];
    int kbd_fd = -1;
    int uinput_fd = -1;
    double debounce_s = debounce_ms / 1000.0;
    double *last_down = NULL;
    unsigned char *key_state = NULL;
    bool drop_until_syn = false;

    if (device_path) {
        snprintf(path, sizeof(path), "%s", device_path);
        kbd_fd = open_device(path);
        if (kbd_fd < 0) {
            exit(1);
        }
        get_device_name(kbd_fd, name, sizeof(name));
    } else {
        if (!find_keyboard(name_fragment, path, sizeof(path), name, sizeof(name))) {
            fprintf(stderr, "No keyboard device found.\n");
            fprintf(stderr, "Run with --list to see available devices.\n");
            exit(1);
        }
        kbd_fd = open_device(path);
        if (kbd_fd < 0) {
            exit(1);
        }
    }

    printf("Debouncing: %s (%s), threshold=%dms\n", name, path, debounce_ms);

    uinput_fd = open("/dev/uinput", O_WRONLY);
    if (uinput_fd < 0) {
        perror("open /dev/uinput");
        close(kbd_fd);
        exit(1);
    }

    if (setup_uinput_from_device(kbd_fd, uinput_fd, name) < 0) {
        close(uinput_fd);
        close(kbd_fd);
        exit(1);
    }

    if (ioctl(kbd_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        close(kbd_fd);
        exit(1);
    }

    last_down = calloc((size_t)KEY_MAX + 1, sizeof(double));
    key_state = calloc(KEY_STATE_BYTES, 1);
    if (!last_down || !key_state) {
        perror("calloc");
        free(last_down);
        free(key_state);
        ioctl(kbd_fd, EVIOCGRAB, 0);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        close(kbd_fd);
        exit(1);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (keep_running) {
        struct input_event events[EVENT_BATCH_SIZE];
        ssize_t nbytes;
        int count;

        nbytes = read(kbd_fd, events, sizeof(events));
        if (nbytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read keyboard");
            break;
        }
        if (nbytes == 0) {
            continue;
        }
        if (nbytes % (ssize_t)sizeof(struct input_event) != 0) {
            fprintf(stderr, "Short read from keyboard\n");
            break;
        }

        count = (int)(nbytes / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count && keep_running; i++) {
            process_event(kbd_fd, uinput_fd, &events[i], debounce_s, last_down,
                          key_state, &drop_until_syn);
        }
    }

    free(key_state);
    free(last_down);
    ioctl(kbd_fd, EVIOCGRAB, 0);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(kbd_fd);
    printf("Debounce stopped, keyboard restored.\n");
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Software debounce filter for mechanical keyboards on Linux.\n"
            "\n"
            "Options:\n"
            "  -t, --threshold MS   Debounce threshold in milliseconds (default: %d)\n"
            "  -d, --device PATH    Evdev device path, e.g. /dev/input/event5\n"
            "  -n, --name FRAGMENT  Match keyboard by name fragment\n"
            "      --list           List available keyboard devices and exit\n"
            "  -h, --help           Show this help and exit\n",
            prog, DEFAULT_DEBOUNCE_MS);
}

int main(int argc, char **argv)
{
    int debounce_ms = DEFAULT_DEBOUNCE_MS;
    const char *device_path = NULL;
    const char *name_fragment = NULL;
    bool list_only = false;

    static const struct option long_options[] = {
        {"threshold", required_argument, NULL, 't'},
        {"device", required_argument, NULL, 'd'},
        {"name", required_argument, NULL, 'n'},
        {"list", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:d:n:", long_options, NULL)) != -1) {
        switch (opt) {
        case 't':
            debounce_ms = atoi(optarg);
            if (debounce_ms < 0) {
                fprintf(stderr, "Invalid threshold: %s\n", optarg);
                return 1;
            }
            break;
        case 'd':
            device_path = optarg;
            break;
        case 'n':
            name_fragment = optarg;
            break;
        case 'l':
            list_only = true;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    for (int i = optind; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (list_only) {
        list_keyboards();
        return 0;
    }

    run(debounce_ms, device_path, name_fragment);
    return 0;
}
