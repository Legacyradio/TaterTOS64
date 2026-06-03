/*
 * claude.c — native in-OS AI agent for TaterTOS64v3 (DeepSeek backend).
 *
 * Talks to DeepSeek's OpenAI-compatible chat API over the OS socket+TLS stack
 * (ts_http / BearSSL). No Anthropic, no Linux, no porting a foreign binary —
 * a native TaterTOS agent against a model the user owns access to.
 *
 * Reads the API key from /apps/DSKEY.TXT (provisioned into TotFS at build).
 * REPL: read a line -> POST /chat/completions -> print the assistant reply.
 * Phase 1 = single-turn per request. (History/tools = later phases.)
 */

#include "../libc/libc.h"
#include "../libc/fry.h"
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

/* ts_http.h's X.509 time callback calls time_func(NULL); map to libc time(). */
#define time_func time
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20
#endif

#include "ts_http.h"

#define DEEPSEEK_URL "https://api.deepseek.com/chat/completions"
#define KEY_PATH     "/apps/DSKEY.TXT"
#define MODEL        "deepseek-chat"
#define POLL_SLEEP_MS 20
#define POLL_MAX      9000        /* ~180s ceiling */
#define MAX_INPUT     2048
#define MAX_JSON      4096

/* ---- read + trim the API key from TotFS ------------------------------- */
static int load_key(char *out, int max) {
    int fd = (int)fry_open(KEY_PATH, 0 /*O_RDONLY*/);
    if (fd < 0) return -1;
    long n = fry_read(fd, out, (unsigned long)(max - 1));
    fry_close(fd);
    if (n <= 0) return -1;
    out[n] = 0;
    /* strip trailing whitespace/newline */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' ||
                     out[n-1] == ' '  || out[n-1] == '\t')) {
        out[--n] = 0;
    }
    return (n > 0) ? 0 : -1;
}

/* ---- append a JSON-escaped copy of s into buf[pos..] ------------------ */
static int json_escape(char *buf, int pos, int cap, const char *s) {
    for (; *s && pos < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf[pos++] = '\\'; buf[pos++] = (char)c; }
        else if (c == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
        else if (c == '\r') { buf[pos++] = '\\'; buf[pos++] = 'r'; }
        else if (c == '\t') { buf[pos++] = '\\'; buf[pos++] = 't'; }
        else if (c < 0x20)  { pos += snprintf(buf + pos, cap - pos, "\\u%04x", c); }
        else buf[pos++] = (char)c;
    }
    return pos;
}

/* ---- find choices[0].message.content in the JSON response, unescape,
 *      print it. Returns 0 if found. ------------------------------------ */
static int print_reply(const char *body) {
    const char *p = strstr(body, "\"content\"");
    if (!p) return -1;
    p += 9;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;                                   /* now at first content char */
    putchar('\n');
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': putchar('\n'); break;
                case 'r': break;
                case 't': putchar('\t'); break;
                case '"': putchar('"');  break;
                case '\\': putchar('\\'); break;
                case '/': putchar('/');  break;
                case 'u': {               /* \uXXXX — emit ASCII or '?' */
                    if (p[1] && p[2] && p[3] && p[4]) {
                        int hi = 0;
                        for (int i = 1; i <= 4; i++) {
                            char h = p[i]; hi <<= 4;
                            if (h >= '0' && h <= '9') hi |= h - '0';
                            else if (h >= 'a' && h <= 'f') hi |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') hi |= h - 'A' + 10;
                        }
                        putchar(hi < 0x80 ? (char)hi : '?');
                        p += 4;
                    }
                    break;
                }
                default: putchar(*p); break;
            }
            if (*p) p++;
        } else {
            putchar(*p++);
        }
    }
    putchar('\n');
    return 0;
}

static int ask(const char *prompt, const char *auth_hdr) {
    char json[MAX_JSON];
    int n = snprintf(json, sizeof(json),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"",
        MODEL);
    n = json_escape(json, n, (int)sizeof(json), prompt);
    n += snprintf(json + n, sizeof(json) - n,
        "\"}],\"max_tokens\":1024,\"stream\":false}");

    struct ts_http req;
    ts_http_init(&req);
    req.content_type = "application/json";
    req.extra_headers = auth_hdr;

    if (ts_http_post(&req, DEEPSEEK_URL, json, (size_t)n) < 0) {
        printf("claude: request failed: %s\n", req.error);
        ts_http_free(&req);
        return -1;
    }

    int iters = 0;
    for (;;) {
        int rc = ts_http_poll(&req);
        if (rc == 1) break;
        if (rc < 0) { printf("claude: transport error: %s\n", req.error);
                      ts_http_free(&req); return -1; }
        if (++iters > POLL_MAX) { printf("claude: timeout\n");
                                  ts_http_free(&req); return -1; }
        fry_sleep(POLL_SLEEP_MS);
    }

    if (req.response.status_code != 200) {
        printf("claude: HTTP %d %s\n", req.response.status_code,
               req.response.status_text);
        if (req.response.body) { fry_write(1, req.response.body,
                                  req.response.body_len > 300 ? 300
                                  : req.response.body_len); printf("\n"); }
        ts_http_free(&req);
        return -1;
    }

    if (req.response.body && print_reply(req.response.body) < 0)
        printf("claude: (couldn't parse reply)\n");

    ts_http_free(&req);
    return 0;
}

int main(void) {
    char key[256];
    char auth_hdr[320];
    char line[MAX_INPUT];

    puts("\nTaterTOS64v3 — claude (DeepSeek agent)");
    if (load_key(key, sizeof(key)) < 0) {
        printf("claude: no API key at %s\n", KEY_PATH);
        return 1;
    }
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s\r\n", key);

    /* Batch mode: if /apps/CLAUDE_ASK.TXT exists, answer it once and exit
     * (non-interactive — used for automated testing and one-shot prompts). */
    {
        int afd = (int)fry_open("/apps/CLAUDE_ASK.TXT", 0);
        if (afd >= 0) {
            long an = fry_read(afd, line, MAX_INPUT - 1);
            fry_close(afd);
            if (an > 0) {
                while (an > 0 && (line[an-1] == '\n' || line[an-1] == '\r')) an--;
                line[an] = 0;
                printf("you> %s\n", line);
                ask(line, auth_hdr);
                return 0;
            }
        }
    }

    puts("Type a message (or 'exit' to quit).\n");

    for (;;) {
        printf("you> ");
        if (!gets_bounded(line, MAX_INPUT)) break;
        /* trim leading spaces */
        char *p = line; while (*p == ' ') p++;
        if (!*p) continue;
        if (strcmp(p, "exit") == 0 || strcmp(p, "quit") == 0) break;
        ask(p, auth_hdr);
    }
    return 0;
}
