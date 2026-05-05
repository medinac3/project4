/* main_client.c — CSC 345 Project 4, Checkpoint 1
 * Chat client with username, per-user ANSI color assignment,
 * and concurrent send/receive threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT_NUM 9004
#define BUF_SIZE 512

void error(const char *msg) {
    perror(msg);
    exit(0);
}

/* ── Per-user color assignment ───────────────────────────────────── */

/* ANSI foreground colors: red, green, yellow, blue, magenta, cyan   */
#define MAX_COLORS 6
static int ansi_colors[MAX_COLORS] = {31, 32, 33, 34, 35, 36};

#define MAX_USERS 32
typedef struct { char name[64]; int color; } UserColor;
static UserColor color_table[MAX_USERS];
static int color_count = 0;
static pthread_mutex_t color_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Returns assigned ANSI color code for username; assigns one on first sight. */
static int get_color(const char *username) {
    pthread_mutex_lock(&color_mutex);
    for (int i = 0; i < color_count; i++) {
        if (strcmp(color_table[i].name, username) == 0) {
            int c = color_table[i].color;
            pthread_mutex_unlock(&color_mutex);
            return c;
        }
    }
    int color = ansi_colors[color_count % MAX_COLORS];
    if (color_count < MAX_USERS) {
        strncpy(color_table[color_count].name, username, 63);
        color_table[color_count].name[63] = '\0';
        color_table[color_count].color = color;
        color_count++;
    }
    pthread_mutex_unlock(&color_mutex);
    return color;
}

/* ── Threads ─────────────────────────────────────────────────────── */

typedef struct { int sockfd; } ThreadArgs;

/* Receive thread: display incoming messages, colorize chat messages. */
static void *thread_main_recv(void *arg) {
    pthread_detach(pthread_self());
    int sockfd = ((ThreadArgs *)arg)->sockfd;
    free(arg);

    char buf[BUF_SIZE];
    int n;

    while (1) {
        memset(buf, 0, BUF_SIZE);
        n = recv(sockfd, buf, BUF_SIZE - 1, 0);
        if (n <= 0) {
            printf("\nDisconnected from server.\n");
            break;
        }

        /* Chat messages start with '[username (IP)]'; colorize them. */
        if (buf[0] == '[') {
            char *paren = strstr(buf, " (");
            if (paren) {
                char uname[64];
                int ulen = paren - (buf + 1);
                if (ulen > 63) ulen = 63;
                strncpy(uname, buf + 1, ulen);
                uname[ulen] = '\0';
                int col = get_color(uname);
                printf("\033[%dm%s\033[0m", col, buf);
                if (buf[strlen(buf) - 1] != '\n') printf("\n");
                fflush(stdout);
                continue;
            }
        }
        /* System messages (join/leave) printed plain. */
        printf("%s", buf);
        if (buf[strlen(buf) - 1] != '\n') printf("\n");
        fflush(stdout);
    }

    return NULL;
}

/* Send thread: read stdin and forward to server. Empty line = disconnect. */
static void *thread_main_send(void *arg) {
    /* NOT detached — main() joins this thread. */
    int sockfd = ((ThreadArgs *)arg)->sockfd;
    free(arg);

    char buf[BUF_SIZE];
    while (1) {
        memset(buf, 0, BUF_SIZE);
        if (fgets(buf, BUF_SIZE - 1, stdin) == NULL) break;
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strlen(buf) == 0) break; /* empty input → disconnect */

        int n = send(sockfd, buf, strlen(buf), 0);
        if (n < 0) break;
    }

    return NULL;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) error("Usage: ./main_client IP-address");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port        = htons(PORT_NUM);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    /* Get and send username */
    char username[64];
    printf("Type your user name: ");
    fflush(stdout);
    if (fgets(username, sizeof(username), stdin) == NULL) error("ERROR reading username");
    username[strcspn(username, "\r\n")] = '\0';

    if (send(sockfd, username, strlen(username), 0) < 0)
        error("ERROR sending username");

    /* Spawn receive thread (detached) */
    ThreadArgs *rargs = malloc(sizeof(ThreadArgs));
    if (!rargs) error("ERROR malloc");
    rargs->sockfd = sockfd;
    pthread_t rtid;
    if (pthread_create(&rtid, NULL, thread_main_recv, rargs) != 0)
        error("ERROR creating recv thread");

    /* Spawn send thread (joined below) */
    ThreadArgs *sargs = malloc(sizeof(ThreadArgs));
    if (!sargs) error("ERROR malloc");
    sargs->sockfd = sockfd;
    pthread_t stid;
    if (pthread_create(&stid, NULL, thread_main_send, sargs) != 0)
        error("ERROR creating send thread");

    /* Wait for user to disconnect, then clean up */
    pthread_join(stid, NULL);
    close(sockfd);

    return 0;
}
