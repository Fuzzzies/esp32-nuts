#include "nut_server.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "ups_data.h"

static const char *TAG = "nut";

#define UPS_NAME    "ups"
#define UPS_DESC    "esp32-nuts USB UPS"
#define MAX_CLIENTS 4
#define LINE_MAX    160

typedef struct {
    int fd;
    char line[LINE_MAX];
    size_t len;
} client_t;

typedef struct {
    const char *name;
    char value[64];
} nut_var_t;

static int sendf(int fd, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send(fd, buf, n, 0);
}

static void add_var(nut_var_t *vars, size_t *n, size_t max,
                    const char *name, const char *fmt, ...)
{
    if (*n >= max) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vars[*n].value, sizeof(vars[*n].value), fmt, ap);
    va_end(ap);
    vars[*n].name = name;
    (*n)++;
}

static size_t build_vars(nut_var_t *vars, size_t max)
{
    ups_state_t s;
    ups_data_get(&s);

    char status[48];
    ups_status_string(&s, status, sizeof(status));

    size_t n = 0;
    add_var(vars, &n, max, "device.type", "ups");
    if (s.mfr[0])    add_var(vars, &n, max, "device.mfr", "%s", s.mfr);
    if (s.model[0])  add_var(vars, &n, max, "device.model", "%s", s.model);
    if (s.serial[0]) add_var(vars, &n, max, "device.serial", "%s", s.serial);
    if (s.mfr[0])    add_var(vars, &n, max, "ups.mfr", "%s", s.mfr);
    if (s.model[0])  add_var(vars, &n, max, "ups.model", "%s", s.model);
    add_var(vars, &n, max, "ups.status", "%s", status);
    add_var(vars, &n, max, "driver.name", "esp32-nuts");
    add_var(vars, &n, max, "driver.version", "0.1.0");

    if (!isnan(s.load_percent))      add_var(vars, &n, max, "ups.load", "%.0f", s.load_percent);
    if (!isnan(s.realpower_nominal)) add_var(vars, &n, max, "ups.realpower.nominal", "%.0f", s.realpower_nominal);
    if (!isnan(ups_load_watts(&s)))  add_var(vars, &n, max, "ups.realpower", "%.0f", ups_load_watts(&s));
    if (!isnan(s.battery_charge))    add_var(vars, &n, max, "battery.charge", "%.0f", s.battery_charge);
    if (!isnan(s.battery_runtime))   add_var(vars, &n, max, "battery.runtime", "%.0f", s.battery_runtime);
    if (!isnan(s.battery_voltage))   add_var(vars, &n, max, "battery.voltage", "%.1f", s.battery_voltage);
    if (!isnan(s.input_voltage))     add_var(vars, &n, max, "input.voltage", "%.1f", s.input_voltage);
    if (!isnan(s.output_voltage))    add_var(vars, &n, max, "output.voltage", "%.1f", s.output_voltage);
    return n;
}

// Splits in place; returns token count.
static int tokenize(char *line, char *tok[], int max)
{
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t && n < max;
         t = strtok_r(NULL, " \t", &save)) {
        tok[n++] = t;
    }
    return n;
}

// Returns false if the connection should be closed.
static bool handle_line(int fd, char *line)
{
    char *tok[5];
    int n = tokenize(line, tok, 5);
    if (n == 0) {
        return true;
    }

    if (strcasecmp(tok[0], "LIST") == 0 && n >= 2 && strcasecmp(tok[1], "UPS") == 0) {
        sendf(fd, "BEGIN LIST UPS\n");
        sendf(fd, "UPS %s \"%s\"\n", UPS_NAME, UPS_DESC);
        sendf(fd, "END LIST UPS\n");
    } else if (strcasecmp(tok[0], "LIST") == 0 && n >= 3 && strcasecmp(tok[1], "VAR") == 0) {
        if (strcmp(tok[2], UPS_NAME) != 0) {
            sendf(fd, "ERR UNKNOWN-UPS\n");
            return true;
        }
        nut_var_t vars[24];
        size_t count = build_vars(vars, 24);
        sendf(fd, "BEGIN LIST VAR %s\n", UPS_NAME);
        for (size_t i = 0; i < count; i++) {
            sendf(fd, "VAR %s %s \"%s\"\n", UPS_NAME, vars[i].name, vars[i].value);
        }
        sendf(fd, "END LIST VAR %s\n", UPS_NAME);
    } else if (strcasecmp(tok[0], "GET") == 0 && n >= 4 && strcasecmp(tok[1], "VAR") == 0) {
        if (strcmp(tok[2], UPS_NAME) != 0) {
            sendf(fd, "ERR UNKNOWN-UPS\n");
            return true;
        }
        nut_var_t vars[24];
        size_t count = build_vars(vars, 24);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(vars[i].name, tok[3]) == 0) {
                sendf(fd, "VAR %s %s \"%s\"\n", UPS_NAME, vars[i].name, vars[i].value);
                return true;
            }
        }
        sendf(fd, "ERR VAR-NOT-SUPPORTED\n");
    } else if (strcasecmp(tok[0], "GET") == 0 && n >= 3 && strcasecmp(tok[1], "UPSDESC") == 0) {
        sendf(fd, "UPSDESC %s \"%s\"\n", UPS_NAME, UPS_DESC);
    } else if (strcasecmp(tok[0], "USERNAME") == 0 || strcasecmp(tok[0], "PASSWORD") == 0 ||
               strcasecmp(tok[0], "LOGIN") == 0) {
        sendf(fd, "OK\n");
    } else if (strcasecmp(tok[0], "LOGOUT") == 0) {
        sendf(fd, "OK Goodbye\n");
        return false;
    } else if (strcasecmp(tok[0], "STARTTLS") == 0) {
        sendf(fd, "ERR FEATURE-NOT-SUPPORTED\n");
    } else if (strcasecmp(tok[0], "VER") == 0) {
        sendf(fd, "esp32-nuts 0.1.0 (NUT compatible)\n");
    } else if (strcasecmp(tok[0], "NETVER") == 0 || strcasecmp(tok[0], "PROTVER") == 0) {
        sendf(fd, "1.3\n");
    } else {
        sendf(fd, "ERR UNKNOWN-COMMAND\n");
    }
    return true;
}

static void close_client(client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        c->len = 0;
    }
}

static void nut_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(CONFIG_ESPNUTS_NUT_PORT),
    };
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "bind/listen failed on port %d", CONFIG_ESPNUTS_NUT_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "NUT server listening on %d", CONFIG_ESPNUTS_NUT_PORT);

    client_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].len = 0;
    }

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > maxfd) {
                    maxfd = clients[i].fd;
                }
            }
        }

        struct timeval tv = { .tv_sec = 1 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    close(fd);
                } else {
                    clients[slot].fd = fd;
                    clients[slot].len = 0;
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0 || !FD_ISSET(c->fd, &rfds)) {
                continue;
            }
            char tmp[64];
            int r = recv(c->fd, tmp, sizeof(tmp), 0);
            if (r <= 0) {
                close_client(c);
                continue;
            }
            for (int j = 0; j < r; j++) {
                char ch = tmp[j];
                if (ch == '\n') {
                    // strip trailing \r
                    while (c->len > 0 && c->line[c->len - 1] == '\r') {
                        c->len--;
                    }
                    c->line[c->len] = '\0';
                    c->len = 0;
                    if (!handle_line(c->fd, c->line)) {
                        close_client(c);
                        break;
                    }
                } else if (c->len < LINE_MAX - 1) {
                    c->line[c->len++] = ch;
                }
            }
        }
    }
}

void nut_server_start(void)
{
    xTaskCreate(nut_task, "nut_server", 4096, NULL, 5, NULL);
}
