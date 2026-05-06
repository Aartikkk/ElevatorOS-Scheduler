/*
=========================================================================================================================================================================
Title       : main.c
Description : Elevator Operating System Scheduler. Communicates with the Elevator OS simulation via HTTP API to schedule
              passengers to elevators using multithreading with 3 concurrent threads.
Author      : Aarti Krishan Khatri (R11860380)
Date        : 05/05/2026
Version     : 1.0
Usage       : ./scheduler_os <path_to_building_file> <port_number>
Notes       : Requires pthreads and POSIX sockets. Compile on HPCC or WSL.
C Version   : C11
=========================================================================================================================================================================
*/

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_ELEVATORS   64
#define MAX_NAME        128
#define BUFFER_SIZE     512
#define RESPONSE_SIZE   8192
#define LINE_SIZE       256

/* --- structs --- */

typedef struct {
    char name[MAX_NAME];
    int  lowest;
    int  highest;
    int  current;
    int  capacity;
} Elevator;

typedef struct {
    char id[MAX_NAME];
    int  start_floor;
    int  end_floor;
} Person;

typedef struct {
    char person_id[MAX_NAME];
    char elevator_id[MAX_NAME];
} Assignment;

/* circular buffer for Person (input -> scheduler) */
typedef struct {
    Person items[BUFFER_SIZE];
    int    head;
    int    tail;
    int    count;
    int    done;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} PersonQueue;

/* circular buffer for Assignment (scheduler -> output) */
typedef struct {
    Assignment items[BUFFER_SIZE];
    int        head;
    int        tail;
    int        count;
    int        done;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} AssignQueue;

/* --- globals --- */

static Elevator elevators[MAX_ELEVATORS];
static int      num_elevators = 0;
static char     port[16];

static PersonQueue pq;
static AssignQueue aq;

/* tracks how many people have been assigned to each elevator */
static int elev_load[MAX_ELEVATORS];
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- person queue operations --- */

void pq_init(PersonQueue *q) {
    q->head = q->tail = q->count = q->done = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void pq_destroy(PersonQueue *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
}

void pq_push(PersonQueue *q, const Person *p) {
    pthread_mutex_lock(&q->lock);
    while (q->count == BUFFER_SIZE)
        pthread_cond_wait(&q->not_full, &q->lock);

    q->items[q->tail] = *p;
    q->tail = (q->tail + 1) % BUFFER_SIZE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

/* returns 1 if got a person, 0 if queue is done and empty */
int pq_pop(PersonQueue *q, Person *p) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }

    *p = q->items[q->head];
    q->head = (q->head + 1) % BUFFER_SIZE;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

/* assignment queue operations (same pattern) */

void aq_init(AssignQueue *q) {
    q->head = q->tail = q->count = q->done = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void aq_destroy(AssignQueue *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
}

void aq_push(AssignQueue *q, const Assignment *a) {
    pthread_mutex_lock(&q->lock);
    while (q->count == BUFFER_SIZE)
        pthread_cond_wait(&q->not_full, &q->lock);

    q->items[q->tail] = *a;
    q->tail = (q->tail + 1) % BUFFER_SIZE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

int aq_pop(AssignQueue *q, Assignment *a) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }

    *a = q->items[q->head];
    q->head = (q->head + 1) % BUFFER_SIZE;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

/* building file parser */

int parse_building_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    char line[LINE_SIZE];

    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", filename);
        return -1;
    }

    num_elevators = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\n' || line[0] == '\r')
            continue;
        if (num_elevators >= MAX_ELEVATORS) {
            fprintf(stderr, "Error: too many elevators\n");
            fclose(fp);
            return -1;
        }

        int n = sscanf(line, "%127s %d %d %d %d",
                        elevators[num_elevators].name,
                        &elevators[num_elevators].lowest,
                        &elevators[num_elevators].highest,
                        &elevators[num_elevators].current,
                        &elevators[num_elevators].capacity);
        if (n != 5) {
            fprintf(stderr, "Error: bad line in building file\n");
            fclose(fp);
            return -1;
        }
        num_elevators++;
    }

    fclose(fp);
    if (num_elevators == 0) {
        fprintf(stderr, "Error: no elevators in file\n");
        return -1;
    }
    return 0;
}

/* --- HTTP helpers --- */

int connect_to_server(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int send_request(const char *req, char *resp, int resp_size) {
    int sockfd = connect_to_server();
    if (sockfd < 0)
        return -1;

    if (send(sockfd, req, strlen(req), 0) < 0) {
        close(sockfd);
        return -1;
    }

    int total = 0, n;
    while ((n = recv(sockfd, resp + total, resp_size - 1 - total, 0)) > 0) {
        total += n;
        if (total >= resp_size - 1)
            break;
    }
    close(sockfd);

    if (n < 0)
        return -1;

    resp[total] = '\0';
    return total;
}

/* find where the body starts after HTTP headers */
char *get_body(char *resp) {
    char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* --- API calls --- */

int api_start(void) {
    char req[1024], resp[RESPONSE_SIZE];

    snprintf(req, sizeof(req),
        "PUT /Simulation/start HTTP/1.1\r\n"
        "Host: 127.0.0.1:%s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n", port);

    if (send_request(req, resp, sizeof(resp)) < 0)
        return -1;

    char *body = get_body(resp);
    if (!body) return -1;

    printf("Simulation: %s\n", body);
    return 0;
}

/* returns: 0 = running, 1 = complete, 2 = stopped, -1 = error */
int api_check(char *status, int size) {
    char req[1024], resp[RESPONSE_SIZE];

    snprintf(req, sizeof(req),
        "GET /Simulation/check HTTP/1.1\r\n"
        "Host: 127.0.0.1:%s\r\n"
        "Connection: close\r\n\r\n", port);

    if (send_request(req, resp, sizeof(resp)) < 0)
        return -1;

    char *body = get_body(resp);
    if (!body) return -1;

    strncpy(status, body, size - 1);
    status[size - 1] = '\0';
    status[strcspn(status, "\r\n")] = '\0';

    if (strstr(status, "complete")) return 1;
    if (strstr(status, "stopped"))  return 2;
    if (strstr(status, "running"))  return 0;
    return -1;
}

/* returns: 1 = got person, 0 = NONE, -1 = error */
int api_next_input(Person *person) {
    char req[1024], resp[RESPONSE_SIZE];

    snprintf(req, sizeof(req),
        "GET /NextInput HTTP/1.1\r\n"
        "Host: 127.0.0.1:%s\r\n"
        "Connection: close\r\n\r\n", port);

    if (send_request(req, resp, sizeof(resp)) < 0)
        return -1;

    char *body = get_body(resp);
    if (!body) return -1;

    if (strncmp(body, "NONE", 4) == 0)
        return 0;

    /* parse "PersonID|startFloor|endFloor" */
    int n = sscanf(body, "%127[^|]|%d|%d",
                   person->id, &person->start_floor, &person->end_floor);
    if (n != 3) {
        fprintf(stderr, "Error parsing input: %s\n", body);
        return -1;
    }
    return 1;
}

/* assign a person to an elevator, returns 0 on success */
int api_assign(const char *pid, const char *eid) {
    char req[1024], resp[RESPONSE_SIZE];

    snprintf(req, sizeof(req),
        "PUT /AddPersonToElevator/%s/%s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n", pid, eid, port);

    if (send_request(req, resp, sizeof(resp)) < 0)
        return -1;

    char *body = get_body(resp);
    if (!body) return -1;

    if (strstr(body, "added to")) {
        printf("Assigned %s -> %s\n", pid, eid);
        return 0;
    }

    fprintf(stderr, "Assign error: %s\n", body);
    return -1;
}

/* --- scheduling --- */

/* picks least-loaded elevator that covers both floors, round-robins ties */
int pick_elevator(const Person *p) {
    static int rr = 0;
    int valid[MAX_ELEVATORS];
    int nvalid = 0;
    int i;

    for (i = 0; i < num_elevators; i++) {
        if (p->start_floor < elevators[i].lowest || p->start_floor > elevators[i].highest)
            continue;
        if (p->end_floor < elevators[i].lowest || p->end_floor > elevators[i].highest)
            continue;
        valid[nvalid++] = i;
    }

    if (nvalid == 0)
        return -1;

    pthread_mutex_lock(&load_lock);

    int min_load = INT_MAX;
    for (i = 0; i < nvalid; i++) {
        if (elev_load[valid[i]] < min_load)
            min_load = elev_load[valid[i]];
    }

    int tied[MAX_ELEVATORS];
    int ntied = 0;
    for (i = 0; i < nvalid; i++) {
        if (elev_load[valid[i]] == min_load)
            tied[ntied++] = valid[i];
    }

    int best = tied[rr % ntied];
    rr++;

    elev_load[best]++;
    pthread_mutex_unlock(&load_lock);
    return best;
}

/* --- threads --- */

/* polls /NextInput and pushes people to the scheduler queue */
void *input_thread(void *arg) {
    (void)arg;
    Person p;
    int ret;
    int nones = 0;

    while (1) {
        ret = api_next_input(&p);

        if (ret == 1) {
            nones = 0;
            pq_push(&pq, &p);

        } else if (ret == 0) {
            nones++;
            if (nones % 5 == 0) {
                char status[256];
                int s = api_check(status, sizeof(status));
                if (s == 1 || s == 2)
                    break;
            }
            usleep(100000); /* 100ms */

        } else {
            char status[256];
            int s = api_check(status, sizeof(status));
            if (s == 1 || s == 2 || s == -1)
                break;
            usleep(500000);
        }
    }

    /* tell scheduler we're done */
    pthread_mutex_lock(&pq.lock);
    pq.done = 1;
    pthread_cond_broadcast(&pq.not_empty);
    pthread_mutex_unlock(&pq.lock);

    return NULL;
}

/* pops people, picks an elevator, pushes assignments */
void *sched_thread(void *arg) {
    (void)arg;
    Person p;
    Assignment a;

    while (pq_pop(&pq, &p)) {
        int idx = pick_elevator(&p);

        if (idx >= 0) {
            strncpy(a.person_id, p.id, MAX_NAME - 1);
            a.person_id[MAX_NAME - 1] = '\0';
            strncpy(a.elevator_id, elevators[idx].name, MAX_NAME - 1);
            a.elevator_id[MAX_NAME - 1] = '\0';

            aq_push(&aq, &a);
        } else {
            fprintf(stderr, "No elevator for %s (%d -> %d)\n",
                    p.id, p.start_floor, p.end_floor);
        }
    }

    /* tell output thread we're done */
    pthread_mutex_lock(&aq.lock);
    aq.done = 1;
    pthread_cond_broadcast(&aq.not_empty);
    pthread_mutex_unlock(&aq.lock);

    return NULL;
}

/* pops assignments and calls the API */
void *output_thread(void *arg) {
    (void)arg;
    Assignment a;

    while (aq_pop(&aq, &a)) {
        api_assign(a.person_id, a.elevator_id);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    pthread_t t_input, t_sched, t_output;
    int i;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <building_file> <port_number>\n", argv[0]);
        return 1;
    }

    strncpy(port, argv[2], sizeof(port) - 1);
    port[sizeof(port) - 1] = '\0';

    if (atoi(port) <= 0) {
        fprintf(stderr, "Error: invalid port number '%s'\n", argv[2]);
        return 1;
    }

    if (parse_building_file(argv[1]) < 0)
        return 1;

    printf("Elevators loaded: %d\n", num_elevators);
    for (i = 0; i < num_elevators; i++) {
        printf("  %s: floors %d-%d, start=%d, cap=%d\n",
               elevators[i].name, elevators[i].lowest,
               elevators[i].highest, elevators[i].current,
               elevators[i].capacity);
    }

    for (i = 0; i < num_elevators; i++)
        elev_load[i] = 0;

    pq_init(&pq);
    aq_init(&aq);

    if (api_start() < 0) {
        fprintf(stderr, "Could not start simulation\n");
        return 1;
    }

    if (pthread_create(&t_input, NULL, input_thread, NULL) != 0) {
        fprintf(stderr, "Error: failed to create input thread\n");
        return 1;
    }
    if (pthread_create(&t_sched, NULL, sched_thread, NULL) != 0) {
        fprintf(stderr, "Error: failed to create scheduler thread\n");
        return 1;
    }
    if (pthread_create(&t_output, NULL, output_thread, NULL) != 0) {
        fprintf(stderr, "Error: failed to create output thread\n");
        return 1;
    }

    pthread_join(t_input, NULL);
    pthread_join(t_sched, NULL);
    pthread_join(t_output, NULL);

    pq_destroy(&pq);
    aq_destroy(&aq);

    printf("Done.\n");
    return 0;
}