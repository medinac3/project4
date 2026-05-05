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
    exit(1);
}

// Clients 

typedef struct _Client {
    int sockfd;
    char username[64];
    char ip[INET_ADDRSTRLEN];
    struct _Client *next;
} Client;

static Client *client_list = NULL;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_client(int sockfd, const char *username, const char *ip) {
    Client *c = malloc(sizeof(Client));
    if (!c) error("ERROR malloc Client");
    c->sockfd = sockfd;
    strncpy(c->username, username, 63); c->username[63] = '\0';
    strncpy(c->ip, ip, INET_ADDRSTRLEN - 1); c->ip[INET_ADDRSTRLEN - 1] = '\0';
    pthread_mutex_lock(&list_mutex);
    c->next = client_list;
    client_list = c;
    pthread_mutex_unlock(&list_mutex);
}

// copy username/ip out. Returns 1 if found
static int remove_client(int sockfd, char *out_user, char *out_ip) {
    pthread_mutex_lock(&list_mutex);
    Client *prev = NULL, *cur = client_list;
    while (cur) {
        if (cur->sockfd == sockfd) {
            if (prev) prev->next = cur->next;
            else      client_list = cur->next;
            strncpy(out_user, cur->username, 63); out_user[63] = '\0';
            strncpy(out_ip,   cur->ip, INET_ADDRSTRLEN - 1);
            out_ip[INET_ADDRSTRLEN - 1] = '\0';
            free(cur);
            pthread_mutex_unlock(&list_mutex);
            return 1;
        }
        prev = cur; cur = cur->next;
    }
    pthread_mutex_unlock(&list_mutex);
    return 0;
}
// broadcasts to every connected client
static void broadcast_all(const char *msg) {
    int len = strlen(msg);
    pthread_mutex_lock(&list_mutex);
    for (Client *c = client_list; c; c = c->next)
        send(c->sockfd, msg, len, 0);
    pthread_mutex_unlock(&list_mutex);
}

// Send to everyone except but one
static void broadcast_others(int fromfd, const char *msg) {
    int len = strlen(msg);
    pthread_mutex_lock(&list_mutex);
    for (Client *c = client_list; c; c = c->next)
        if (c->sockfd != fromfd)
            send(c->sockfd, msg, len, 0);
    pthread_mutex_unlock(&list_mutex);
}

// prints connected clients to server
static void print_client_list(void) {
    pthread_mutex_lock(&list_mutex);
    printf("── Connected clients ──\n");
    int count = 0;
    for (Client *c = client_list; c; c = c->next) {
        printf("  %s (%s)\n", c->username, c->ip);
        count++;
    }
    if (count == 0) printf("  (none)\n");
    printf("──────────────────────\n");
    pthread_mutex_unlock(&list_mutex);
}

typedef struct {
    int clisockfd;
    char ip[INET_ADDRSTRLEN];
} ThreadArgs;

static void *thread_main(void *arg) {
    pthread_detach(pthread_self());

    int fd = ((ThreadArgs *)arg)->clisockfd;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, ((ThreadArgs *)arg)->ip, INET_ADDRSTRLEN - 1);
    ip[INET_ADDRSTRLEN - 1] = '\0';
    free(arg);

    char buf[BUF_SIZE];
    int n;

// sets first message as username and adds to list, broadcasts join message too
    memset(buf, 0, BUF_SIZE);
    n = recv(fd, buf, BUF_SIZE - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    buf[strcspn(buf, "\r\n")] = '\0';

    char username[64];
    strncpy(username, buf, 63); username[63] = '\0';

    add_client(fd, username, ip);
    print_client_list();

    char notice[BUF_SIZE + 128];
    snprintf(notice, sizeof(notice), "%s (%s) joined the chat room!\n", username, ip);
    broadcast_all(notice);

// chat loop here
    while (1) {
        memset(buf, 0, BUF_SIZE);
        n = recv(fd, buf, BUF_SIZE - 1, 0);
        if (n <= 0) break;
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) break;

        char msg[BUF_SIZE + 128];
        snprintf(msg, sizeof(msg), "[%s (%s)] %s\n", username, ip, buf);
        broadcast_others(fd, msg);
    }

// disconnet message 
    char rm_user[64], rm_ip[INET_ADDRSTRLEN];
    remove_client(fd, rm_user, rm_ip);

    snprintf(notice, sizeof(notice), "%s (%s) left the room!\n", rm_user, rm_ip);
    broadcast_all(notice);
    print_client_list();

    close(fd);
    return NULL;
}

int main(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port        = htons(PORT_NUM);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, 5);
    printf("Server listening on port %d...\n", PORT_NUM);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t clen = sizeof(cli_addr);
        int newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clen);
        if (newsockfd < 0) error("ERROR on accept");

        ThreadArgs *args = malloc(sizeof(ThreadArgs));
        if (!args) error("ERROR malloc ThreadArgs");
        args->clisockfd = newsockfd;
        strncpy(args->ip, inet_ntoa(cli_addr.sin_addr), INET_ADDRSTRLEN - 1);
        args->ip[INET_ADDRSTRLEN - 1] = '\0';

        pthread_t tid;
        if (pthread_create(&tid, NULL, thread_main, args) != 0)
            error("ERROR creating thread");
    }

    return 0;
}
