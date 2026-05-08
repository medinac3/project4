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
#define MAX_ROOMS 4

void error(const char *msg) {
    perror(msg);
    exit(0);
}

// colors: red, green, yellow, blue, magenta, cyan
#define MAX_COLORS 6
static int ansi_colors[MAX_COLORS] = {31, 32, 33, 34, 35, 36};

#define MAX_USERS 32
typedef struct { char name[64]; int color; } UserColor;
static UserColor color_table[MAX_USERS];
static int color_count = 0;
static pthread_mutex_t color_mutex = PTHREAD_MUTEX_INITIALIZER;

// gives assigned color code for username; assigns one at intial encounter 
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

typedef struct { int sockfd; int room_no; } ThreadArgs;

// receives: display incoming messages, color chat messages
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
// system messages (join/leave) printed
        printf("%s", buf);
        if (buf[strlen(buf) - 1] != '\n') printf("\n");
        fflush(stdout);
    }

    return NULL;
}

// send thread: read stdin and forward to server
static void *thread_main_send(void *arg) {
    int sockfd = ((ThreadArgs *)arg)->sockfd;
    free(arg);

    char buf[BUF_SIZE];
    while (1) {
        memset(buf, 0, BUF_SIZE);
        if (fgets(buf, BUF_SIZE - 1, stdin) == NULL) break;
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strlen(buf) == 0) break; /* empty input means disconnect */

        int n = send(sockfd, buf, strlen(buf), 0);
        if (n < 0) break;
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    char room_str[16];
    
    if (argc > 3) error("Too many arguments.\nUsage: ./main_client IP-address room number or 'new'");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");


    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port        = htons(PORT_NUM);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) // connect to server
        error("ERROR connecting");

    int room_to_send = 0;

    if(argc < 3){ // if a room number or 'new' is not given
        char menu_buffer[BUF_SIZE];
        memset(menu_buffer, 0, BUF_SIZE);
        int n = recv(sockfd, menu_buffer, BUF_SIZE - 1, 0);                    // recieve menu
    
        if (n <= 0) error("Server closed connection.");
    
        printf("Server says following options are available:\n%s\n", menu_buffer);
        printf("\033[1mChoose the room number or type 'new': \033[0m");
        fflush(stdout);

        char choice[16];
        fgets(choice, sizeof(choice), stdin);
        choice[strcspn(choice, "\n")] = '\0'; // Remove newline

        strncpy(room_str, choice, 15);
    }else{
        char junk[BUF_SIZE];
        recv(sockfd, junk, BUF_SIZE - 1, 0);                                   // recieve menu
        strncpy(room_str, argv[2], 15);
    }


    if (strcmp(room_str, "new") == 0) {            // find room number                             
        room_to_send = 0; 
    } else {
        if (room_str[0] >= '0' && room_str[0] <= '9') { // checks if value entered is an integer
            room_to_send = atoi(room_str);
            if (room_to_send < 0 || room_to_send > MAX_ROOMS) { // Set your own max
                fprintf(stderr, "Error: Room number must be between 0 and MAX_ROOMS\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "Error: Argument must be a number or 'new'\n");
            exit(1);
        }
    }


    if (send(sockfd, room_str, strlen(room_str), 0) < 0)       // send room number
        error("Error sending room choice");

    usleep(10000);
    // get and send username
    char username[64];
    printf("Type your user name: ");
    fflush(stdout); 
    if (fgets(username, sizeof(username), stdin) == NULL) error("ERROR reading username");
    username[strcspn(username, "\r\n")] = '\0';

    if (send(sockfd, username, strlen(username), 0) < 0)       // send username
        error("ERROR sending username");

    ThreadArgs *rargs = malloc(sizeof(ThreadArgs));
    if (!rargs) error("ERROR malloc");
    rargs->sockfd = sockfd;
    pthread_t rtid;
    if (pthread_create(&rtid, NULL, thread_main_recv, rargs) != 0)
        error("ERROR creating recv thread");


    ThreadArgs *sargs = malloc(sizeof(ThreadArgs));
    if (!sargs) error("ERROR malloc");
    sargs->sockfd = sockfd;
    sargs->room_no = room_to_send;;
    pthread_t stid;
    if (pthread_create(&stid, NULL, thread_main_send, sargs) != 0)
        error("ERROR creating send thread");

    pthread_join(stid, NULL);
    close(sockfd);

    return 0;
}
