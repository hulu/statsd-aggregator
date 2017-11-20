/**
 * statsd-aggregator: a local daemon for aggregating statsd metrics
 * (https://github.com/etsy/statsd/).
**/

#pragma GCC diagnostic ignored "-Wstrict-aliasing"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <ev.h>
#include <netdb.h>
#include <sys/fcntl.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>

// Size of buffer for outgoing packets. Should be below MTU.
// TODO Probably should be configured via configuration file?
#define DOWNSTREAM_BUF_SIZE 1450
#define DOWNSTREAM_BUF_NUM 16
// Size of other temporary buffers
#define DATA_BUF_SIZE 4096
#define LOG_BUF_SIZE 2048

// worst scenario: a lot of metrics with unique short names.
// Metric would look like: aa:1|c\n
// Metric length is 7 chars
// Example: 1450 / 7 = 207 so we need 207 slots if DOWNSTREAM_BUF_SIZE is 1450
#define NUM_OF_SLOTS (DOWNSTREAM_BUF_SIZE / 7)

#define MAX_COUNTER_LENGTH 18 // because of "%.15g|c\n"

// default interval to check if downstream ips changed
#define DEFAULT_DNS_REFRESH_INTERVAL 60

// default interval to check downstream health
#define DEFAULT_DS_HEALTHCHECK_INTERVAL 1.0

#define DEFAULT_LOG_LEVEL 0
#define MAX_DOWNSTREAM_NUM 32

// structure to accumulate metrics data for specific name
typedef struct {
    char buffer[DOWNSTREAM_BUF_SIZE];
    int name_length;
    int length;
    double counter;
    int type;
} slot_s;

struct downstream_host_s {
    struct sockaddr_in sa_in_data;
};

// structure that holds downstream data
struct downstream_s {
    // buffer where data is added
    int active_buffer_idx;
    char *active_buffer;
    int active_buffer_length;
    // buffer ready for flush
    int flush_buffer_idx;
    // memory for active and flush buffers
    char buffer[DOWNSTREAM_BUF_SIZE * DOWNSTREAM_BUF_NUM];
    // lengths of buffers from the above array
    int buffer_length[DOWNSTREAM_BUF_NUM];
    char *data_host;
    int data_port;
    // sockaddr for data (used for flush)
    struct sockaddr_in sa_in_data;
    // new sockaddr filled in by the downstream_refresh()
    struct sockaddr_in sa_in_data_new[MAX_DOWNSTREAM_NUM];
    // flag that new sockaddr data is available
    int sa_in_data_new_ready;
    // id extended ev_io structure used for sending data to downstream
    struct ev_io flush_watcher;
    // last time data was flushed to downstream
    ev_tstamp last_flush_time;
    // slots for accumulating metrics
    slot_s slots[NUM_OF_SLOTS];
    // how many slots are used
    int slots_used;
    // how many downstream hosts we have
    int downstream_host_num;
};

// globally accessed structure with commonly used data
struct global_s {
    // port we are listening on
    int data_port;
    struct downstream_s downstream;
    // how often we flush data
    ev_tstamp downstream_flush_interval;
    // how noisy is our log
    int log_level;
    // how often we want to check if downstream ips were changed
    int dns_refresh_interval;
    // how often we check health of the downstreams
    ev_tstamp downstream_health_check_interval;
};

struct global_s global;

// numeric values for log levels
enum log_level_e {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR
};

enum metric_type_e {
    TYPE_UNKNOWN,
    TYPE_COUNTER,
    TYPE_OTHER
};

// and function to convert numeric values into strings
char *log_level_name(enum log_level_e level) {
    static char *name[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    return name[level];
}

// function to log message
void log_msg(int level, char *format, ...) {
    va_list args;
    time_t t;
    struct tm *tinfo;
    char buffer[LOG_BUF_SIZE];
    int l = 0;

    if (level < global.log_level) {
        return;
    }
    va_start(args, format);
    time(&t);
    tinfo = localtime(&t);
    l = strftime(buffer, LOG_BUF_SIZE, "%Y-%m-%d %H:%M:%S", tinfo);
    l += sprintf(buffer + l, " %s ", log_level_name(level));
    vsnprintf(buffer + l, LOG_BUF_SIZE - l, format, args);
    va_end(args);
    fprintf(stdout, "%s\n", buffer);
    fflush(stdout);
}

// this function flushes data to downstream
void downstream_flush_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    int bytes_send;
    int flush_buffer_idx = global.downstream.flush_buffer_idx;

    if (EV_ERROR & revents) {
        log_msg(ERROR, "%s: invalid event %s", __func__, strerror(errno));
        return;
    }

    bytes_send = sendto(watcher->fd,
        global.downstream.buffer + flush_buffer_idx * DOWNSTREAM_BUF_SIZE,
        global.downstream.buffer_length[flush_buffer_idx],
        0,
        (struct sockaddr *) (&global.downstream.sa_in_data),
        sizeof(global.downstream.sa_in_data));
    // update flush time
    global.downstream.last_flush_time = ev_now(loop);
    global.downstream.buffer_length[flush_buffer_idx] = 0;
    global.downstream.flush_buffer_idx = (flush_buffer_idx + 1) % DOWNSTREAM_BUF_NUM;
    log_msg(TRACE, "%s: flushed buffer %d", __func__, flush_buffer_idx);
    if (global.downstream.flush_buffer_idx == global.downstream.active_buffer_idx) {
        ev_io_stop(loop, watcher);
    }
    if (bytes_send < 0) {
        log_msg(ERROR, "%s: sendto() failed %s", __func__, strerror(errno));
    }
}

/* this function switches active and flush buffers, registers handler to send data when
 * socket would be ready
 */
void downstream_schedule_flush() {
    int i = 0;
    int slot_data_length = 0;
    int active_buffer_length = 0;
    struct ev_io *watcher = NULL;
    int new_active_buffer_idx = (global.downstream.active_buffer_idx + 1) % DOWNSTREAM_BUF_NUM;
    // if active_buffer_idx == flush_buffer_idx this means that all previous
    // flushes are done (no filled buffers in the queue) and we need to schedule new one
    int need_to_schedule_flush = (global.downstream.active_buffer_idx == global.downstream.flush_buffer_idx);

    if (global.downstream.buffer_length[new_active_buffer_idx] > 0) {
        log_msg(ERROR, "%s: previous flush is not completed, loosing data.", __func__);
        global.downstream.active_buffer_length = 0;
        global.downstream.slots_used = 0;
        return;
    }
    for (i = 0; i < global.downstream.slots_used; i++) {
        slot_data_length = global.downstream.slots[i].length;
        if (slot_data_length == global.downstream.slots[i].name_length) {
            continue;
        }
        *(global.downstream.slots[i].buffer + slot_data_length - 1) = '\n';
        memcpy(global.downstream.active_buffer + active_buffer_length, global.downstream.slots[i].buffer, slot_data_length);
        active_buffer_length += slot_data_length;
    }
    log_msg(TRACE, "%s: flushing buffer: \"%.*s\"", __func__, active_buffer_length, global.downstream.active_buffer);
    global.downstream.buffer_length[global.downstream.active_buffer_idx] = active_buffer_length;
    global.downstream.active_buffer = global.downstream.buffer + new_active_buffer_idx * DOWNSTREAM_BUF_SIZE;
    global.downstream.active_buffer_length = 0;
    global.downstream.slots_used = 0;
    global.downstream.active_buffer_idx = new_active_buffer_idx;
    log_msg(TRACE, "%s: new active buffer idx = %d", __func__, new_active_buffer_idx);
    if (need_to_schedule_flush) {
        watcher = &(global.downstream.flush_watcher);
        ev_io_init(watcher, downstream_flush_cb, watcher->fd, EV_WRITE);
        ev_io_start(ev_default_loop(0), watcher);
    }
}

int add_slot(char *line, int name_length) {
    global.downstream.slots[global.downstream.slots_used].name_length = name_length;
    global.downstream.slots[global.downstream.slots_used].length = name_length;
    global.downstream.slots[global.downstream.slots_used].type = TYPE_UNKNOWN;
    global.downstream.slots[global.downstream.slots_used].counter = 0.0;
    global.downstream.active_buffer_length += name_length;
    memcpy(global.downstream.slots[global.downstream.slots_used].buffer, line, name_length);
    log_msg(TRACE, "%s: created %.*s at slot %d", __func__, name_length, line, global.downstream.slots_used);
    return global.downstream.slots_used++;
}

int find_slot(char *line, int name_length) {
    int i = 0;
    for (i = 0; i < global.downstream.slots_used; i++) {
        if (global.downstream.slots[i].name_length == name_length) {
            if (memcmp(line, global.downstream.slots[i].buffer, name_length) == 0) {
                log_msg(TRACE, "%s: found %.*s at slot %d", __func__, name_length, line, i);
                return i;
            }
        }
    }
    if (global.downstream.active_buffer_length + name_length > DOWNSTREAM_BUF_SIZE) {
        log_msg(TRACE, "%s: active_buffer_length = %d, name_length = %d, scheduling flush", __func__, global.downstream.active_buffer_length, name_length);
        downstream_schedule_flush();
    }
    return add_slot(line, name_length);
}

void insert_values_into_slot(int initial_slot_idx, char *line, char *colon_ptr, int length) {
    int slot_idx = initial_slot_idx;
    ssize_t bytes_in_buffer;
    char *buffer_ptr = colon_ptr + 1;
    char *delimiter_ptr = colon_ptr;
    char *target_ptr = NULL;
    int data_length = 0;
    int name_length = global.downstream.slots[slot_idx].name_length;
    char *type_ptr = NULL;
    int metric_type = 0;
    double counter = 0;
    char *counter_ptr = NULL;
    int counter_len = 0;
    char *endptr = NULL;
    char *rate_ptr = NULL;
    double rate = 1;

    bytes_in_buffer = length - (colon_ptr - line) - 1;
    log_msg(TRACE, "%s: metrics data \"%.*s\"", __func__, (int)bytes_in_buffer, colon_ptr);
    while (delimiter_ptr != NULL) {
        delimiter_ptr = memchr(buffer_ptr, ':', bytes_in_buffer);
        if (delimiter_ptr == NULL) {
            data_length = bytes_in_buffer;
        } else {
            data_length = delimiter_ptr - buffer_ptr + 1;
        }
        type_ptr = memchr(buffer_ptr, '|', data_length);
        if (type_ptr == NULL) {
            log_msg(ERROR, "%s: invalid metric data \"%.*s\"", __func__, data_length, buffer_ptr);
            bytes_in_buffer -= data_length;
            buffer_ptr += data_length;
            continue;
        }
        metric_type = TYPE_OTHER;
        if (*(type_ptr + 1) == 'c') {
            metric_type = TYPE_COUNTER;
        }
        if (global.downstream.slots[slot_idx].type == TYPE_UNKNOWN) {
            global.downstream.slots[slot_idx].type = metric_type;
        } else {
            if (global.downstream.slots[slot_idx].type != metric_type) {
                log_msg(ERROR, "%s: got improper metric type for \"%.*s\"", __func__, global.downstream.slots[slot_idx].name_length, global.downstream.slots[slot_idx].buffer);
                bytes_in_buffer -= data_length;
                buffer_ptr += data_length;
                continue;
            }
        }
        // if metric is counter let's use maximum possible length of resulting string (because of "%.15g|c\n" below)
        if (global.downstream.active_buffer_length + (metric_type == TYPE_COUNTER ? MAX_COUNTER_LENGTH : data_length) > DOWNSTREAM_BUF_SIZE) {
            downstream_schedule_flush();
            slot_idx = add_slot(line, name_length);
            global.downstream.slots[slot_idx].type = metric_type;
        }
        target_ptr = global.downstream.slots[slot_idx].buffer + global.downstream.slots[slot_idx].length;
        log_msg(TRACE, "%s: adding \"%.*s\"", __func__, data_length, buffer_ptr);
        if (metric_type == TYPE_COUNTER) {
            rate = 1;
            rate_ptr = memchr(type_ptr + 1, '|', data_length - (type_ptr - buffer_ptr));
            if (rate_ptr != NULL && *(rate_ptr + 1) == '@') {
                errno = 0;
                rate = strtod(rate_ptr + 2, &endptr);
                if (errno != 0 || (endptr + 1) != (buffer_ptr + data_length)) {
                    log_msg(TRACE, "%s: invalid rate in counter data \"%.*s\"", __func__, data_length - 1, buffer_ptr);
                    rate = 1;
                }
            }
            errno = 0;
            counter = strtod(buffer_ptr, &endptr) / rate;
            if (errno != 0 || endptr != type_ptr) {
                log_msg(ERROR, "%s: invalid value in counter data \"%.*s\"", __func__, data_length - 1, buffer_ptr);
            } else {
                counter_ptr = global.downstream.slots[slot_idx].buffer + name_length;
                global.downstream.slots[slot_idx].counter += counter;
                counter_len = sprintf(counter_ptr, "%.15g|c\n", global.downstream.slots[slot_idx].counter);
                global.downstream.active_buffer_length -= global.downstream.slots[slot_idx].length;
                global.downstream.slots[slot_idx].length = global.downstream.slots[slot_idx].name_length + counter_len;
                global.downstream.active_buffer_length += global.downstream.slots[slot_idx].length;
                log_msg(TRACE, "%s: counter delta = %.15g, counter value = %.15g", __func__, counter, global.downstream.slots[slot_idx].counter);
            }
        } else {
            memcpy(target_ptr, buffer_ptr, data_length);
            target_ptr += data_length;
            *(target_ptr - 1) = ':';
            global.downstream.slots[slot_idx].length += data_length;
            global.downstream.active_buffer_length += data_length;
        }
        bytes_in_buffer -= data_length;
        buffer_ptr += data_length;
    }
    log_msg(TRACE, "%s: buffer after insert: \"%.*s\"", __func__, global.downstream.slots[slot_idx].length, global.downstream.slots[slot_idx].buffer);
}

// function to process single metrics line
int process_data_line(char *line, int length) {
    int slot_idx = -1;
    char *colon_ptr = memchr(line, ':', length);
    // if ':' wasn't found this is not valid statsd metric
    if (colon_ptr == NULL) {
        *(line + length - 1) = 0;
        log_msg(ERROR, "%s: invalid metric %s", __func__, line);
        return 1;
    }
    slot_idx = find_slot(line, colon_ptr - line + 1);
    insert_values_into_slot(slot_idx, line, colon_ptr, length);
    return 0;
}

void udp_read_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    char buffer[DATA_BUF_SIZE];
    ssize_t bytes_in_buffer;
    char *buffer_ptr = buffer;
    char *delimiter_ptr = buffer;
    int line_length = 0;

    if (EV_ERROR & revents) {
        log_msg(ERROR, "%s: invalid event %s", __func__, strerror(errno));
        return;
    }

    bytes_in_buffer = recv(watcher->fd, buffer, DATA_BUF_SIZE - 1, 0);

    if (bytes_in_buffer < 0) {
        log_msg(ERROR, "%s: read() failed %s", __func__, strerror(errno));
        return;
    }

    if (bytes_in_buffer > 0) {
        if (buffer[bytes_in_buffer - 1] != '\n') {
            buffer[bytes_in_buffer++] = '\n';
        }
        log_msg(TRACE, "%s: got packet %.*s", __func__, bytes_in_buffer, buffer);
        while ((delimiter_ptr = memchr(buffer_ptr, '\n', bytes_in_buffer)) != NULL) {
            delimiter_ptr++;
            line_length = delimiter_ptr - buffer_ptr;
            // minimum metrics line should look like X:1|c\n
            // so lines with length less than 6 can be ignored
            // if we've got counter like 1|c|@0.3 it would expand to 3.33333333333|c
            // so to be on safe side let's limit maximum line length so that we would be able to fit counter in any case
            if (line_length > 6 && line_length < (DOWNSTREAM_BUF_SIZE - MAX_COUNTER_LENGTH)) {
                // if line has valid length let's process it
                process_data_line(buffer_ptr, line_length);
            } else {
                log_msg(ERROR, "%s: invalid length %d of metric %.*s", __func__, line_length - 1, line_length - 1, buffer_ptr);
            }
            // this is not last metric, let's advance line start pointer
            buffer_ptr = delimiter_ptr;
            bytes_in_buffer -= line_length;
        }
    }
}

// this function cycles through downstreams and flushes them on scheduled basis
void downstream_flush_timer_cb(struct ev_loop *loop, struct ev_periodic *p, int revents) {
    ev_tstamp now = ev_now(loop);

    if (now - global.downstream.last_flush_time > global.downstream_flush_interval &&
            global.downstream.active_buffer_length > 0) {
        downstream_schedule_flush();
    }
}

void init_sockaddr_in() {
    int i = 0;
    struct sockaddr_in *sa_in = NULL;
    struct hostent *he = gethostbyname(global.downstream.data_host);

    if (he == NULL || he->h_addr_list == NULL || (he->h_addr_list)[0] == NULL ) {
        log_msg(ERROR, "%s: gethostbyname() failed %s", __func__, strerror(errno));
        return;
    }
    for (i = 0; i < MAX_DOWNSTREAM_NUM && he->h_addr_list[i] != NULL; i++) {
        sa_in = global.downstream.sa_in_data_new + i;
        bzero(sa_in, sizeof(*sa_in));
        sa_in->sin_family = AF_INET;
        sa_in->sin_port = htons(global.downstream.data_port);
        memcpy(&(sa_in->sin_addr), he->h_addr_list[i], he->h_length);
        log_msg(DEBUG, "%s %x", inet_ntoa(*(struct in_addr *)(he->h_addr_list[i])), (int)(sa_in->sin_addr.s_addr));
    }
    global.downstream.sa_in_data_new_ready = 1;
}

// function to init downstream from config file line
int init_downstream(char *hosts) {
    int i = 0;
    char *host = hosts;
    char *data_port_s = NULL;
    int host_len = 0;

    // argument line has the following format: host:data_port
    // now let's initialize downstreams
    global.downstream.slots_used = 0;
    global.downstream.last_flush_time = ev_time();
    global.downstream.active_buffer_idx = 0;
    global.downstream.active_buffer = global.downstream.buffer;
    global.downstream.active_buffer_length = 0;
    global.downstream.flush_buffer_idx = 0;
    global.downstream.flush_watcher.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);;
    if (global.downstream.flush_watcher.fd < 0) {
        log_msg(ERROR, "%s: socket() failed %s", __func__, strerror(errno));
        return 1;
    }
    for (i = 0; i < DOWNSTREAM_BUF_NUM; i++) {
        global.downstream.buffer_length[i] = 0;
    }
    data_port_s = strchr(host, ':');
    if (data_port_s == NULL) {
        log_msg(ERROR, "%s: no data port for %s", __func__, host);
        return 1;
    }
    *data_port_s++ = 0;
    host_len = data_port_s - host;
    global.downstream.data_host = (char *)malloc(host_len);
    memcpy(global.downstream.data_host, host, host_len);
    global.downstream.data_port = atoi(data_port_s);
    global.downstream.sa_in_data_new_ready = 0;
    init_sockaddr_in();
    if (global.downstream.sa_in_data_new_ready != 1) {
        log_msg(ERROR, "%s: failed to initialize sockaddr_in structures", __func__);
        return 1;
    }
    return 0;
}

// function to parse single line from config file
int process_config_line(char *line) {
    // valid line should contain '=' symbol
    char *value_ptr = strchr(line, '=');
    if (value_ptr == NULL) {
        log_msg(ERROR, "%s: bad line in config \"%s\"", __func__, line);
        return 1;
    }
    *value_ptr++ = 0;
    if (strcmp("data_port", line) == 0) {
        global.data_port = atoi(value_ptr);
    } else if (strcmp("downstream_flush_interval", line) == 0) {
        global.downstream_flush_interval = atof(value_ptr);
    } else if (strcmp("log_level", line) == 0) {
        global.log_level = atoi(value_ptr);
    } else if (strcmp("dns_refresh_interval", line) == 0) {
        global.dns_refresh_interval = atoi(value_ptr);
    } else if (strcmp("downstream_health_check_interval", line) == 0) {
        global.downstream_health_check_interval = atof(value_ptr);
    } else if (strcmp("downstream", line) == 0) {
        return init_downstream(value_ptr);
    } else {
        log_msg(ERROR, "%s: unknown parameter \"%s\"", __func__, line);
        return 1;
    }
    return 0;
}

// this function is called if SIGHUP is received
void on_sighup(int sig) {
    log_msg(INFO, "%s: sighup received", __func__);
}

void on_sigint(int sig) {
    log_msg(INFO, "%s: sigint received", __func__);
    exit(0);
}

// this function loads config file and initializes config fields
int init_config(char *filename) {
    size_t n = 0;
    int l = 0;
    int failures = 0;
    char *buffer = NULL;

    global.log_level = DEFAULT_LOG_LEVEL;
    global.dns_refresh_interval = DEFAULT_DNS_REFRESH_INTERVAL;
    global.downstream_health_check_interval = DEFAULT_DS_HEALTHCHECK_INTERVAL;
    FILE *config_file = fopen(filename, "rt");
    if (config_file == NULL) {
        log_msg(ERROR, "%s: fopen() failed %s", __func__, strerror(errno));
        return 1;
    }
    // config file can contain very long lines e.g. to specify downstreams
    // using getline() here since it automatically adjusts buffer
    while ((l = getline(&buffer, &n, config_file)) > 0) {
        if (buffer[l - 1] == '\n') {
            buffer[l - 1] = 0;
        }
        if (buffer[0] != '\n' && buffer[0] != '#') {
            failures += process_config_line(buffer);
        }
    }
    // buffer is reused by getline() so we need to free it only once
    free(buffer);
    fclose(config_file);
    if (failures > 0) {
        log_msg(ERROR, "%s: failed to load config file", __func__);
        return 1;
    }
    if (signal(SIGHUP, on_sighup) == SIG_ERR) {
        log_msg(ERROR, "%s: signal() failed", __func__);
        return 1;
    }
    if (signal(SIGINT, on_sigint) == SIG_ERR) {
        log_msg(ERROR, "%s: signal() failed", __func__);
        return 1;
    }
    return 0;
}

void *downstream_refresh(void *args) {
    while(1) {
        sleep(global.dns_refresh_interval);
        // if sockaddr data was copied let's refresh data
        if (global.downstream.sa_in_data_new_ready == 0) {
            init_sockaddr_in();
        }
    }
    return NULL;
}

void update_downstreams() {
    // if there is new sockaddr data let's copy it and reset the flag
    if (global.downstream.sa_in_data_new_ready == 1) {
        global.downstream.sa_in_data = global.downstream.sa_in_data_new[0];
        global.downstream.sa_in_data_new_ready = 0;
    }
}

void downstream_healthcheck_timer_cb(struct ev_loop *loop, struct ev_periodic *p, int revents) {
    update_downstreams();
}

// http://stackoverflow.com/questions/791982/determine-if-a-string-is-a-valid-ip-address-in-c
int is_valid_ip_address(char *ip_addr) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ip_addr, &(sa.sin_addr));
    return result != 0;
}

int main(int argc, char *argv[]) {
    struct ev_loop *loop = ev_default_loop(0);
    int data_socket;
    struct sockaddr_in addr;
    struct ev_io socket_watcher;
    struct ev_periodic downstream_flush_timer_watcher;
    struct ev_periodic downstream_healthcheck_timer_watcher;
    ev_tstamp downstream_flush_timer_at = 0.0;
    ev_tstamp downstream_healthcheck_timer_at = 0.0;
    pthread_t downstream_socket_refresh_thread;

   if (argc != 2) {
        fprintf(stdout, "Usage: %s config.file\n", argv[0]);
        exit(1);
    }
    if (init_config(argv[1]) != 0) {
        log_msg(ERROR, "%s: init_config() failed", __func__);
        exit(1);
    }

    if ((data_socket = socket(PF_INET, SOCK_DGRAM, 0)) < 0 ) {
        log_msg(ERROR, "%s: socket() error %s", __func__, strerror(errno));
        return(1);
    }
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(global.data_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(data_socket, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        log_msg(ERROR, "%s: bind() failed %s", __func__, strerror(errno));
        return(1);
    }

    // if downstream is specified via ip address no need to run downstream_refresh()
    if (! is_valid_ip_address(global.downstream.data_host)) {
        pthread_create(&downstream_socket_refresh_thread, NULL, downstream_refresh, NULL);
    }

    ev_io_init(&socket_watcher, udp_read_cb, data_socket, EV_READ);
    ev_io_start(loop, &socket_watcher);

    ev_periodic_init (&downstream_flush_timer_watcher, downstream_flush_timer_cb, downstream_flush_timer_at, global.downstream_flush_interval, 0);
    ev_periodic_start (loop, &downstream_flush_timer_watcher);

    ev_periodic_init (&downstream_healthcheck_timer_watcher, downstream_healthcheck_timer_cb, downstream_healthcheck_timer_at, global.downstream_health_check_interval, 0);
    ev_periodic_start (loop, &downstream_healthcheck_timer_watcher);

    ev_loop(loop, 0);
    log_msg(ERROR, "%s: ev_loop() exited", __func__);
    return(0);
}
