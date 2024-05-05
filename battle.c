/*
 * socket demonstrations:
 * This is the server side of an "internet domain" socket connection, for
 * communicating over the network.
 *
 * In this case we are willing to wait for chatter from the client
 * _or_ for a new connection.
*/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

#ifndef PORT
    #define PORT 56218
#endif

#define MAX_PARTIAL 5
#define MAX_NAME 20
#define MAX_HP 21
#define MAX_POW 3
#define MAX_BLC 3
#define A_DMG 2
#define P_DMG 6
#define B_DMG 1
#define MAX_LINE 200
#define MOV_MSG "\r\n(a)ttack\r\n(p)ower move\r\n(b)lock\r\n(s)peak something\r\n\r\n"
#define MOV_MSG_LEN 57
#define WAIT_MSG "Awaiting next opponent...\r\n"
#define WAIT_MSG_LEN 28
#define SPEAK "\r\nSpeak: "
#define TIE_MSG "\r\nYou achieve a tie!\r\n\r\n"
#define TIE_MSG_LEN 23
#define NO_SPAM "\r\nDO NOT SPAM\r\n\r\n"
#define NO_SPAM_LEN 18

typedef struct Linkedclient Client; // Alias
typedef Client * volatile Clientptr;
struct Linkedclient {
    char name[MAX_NAME + 1];
    int soc;
    Clientptr prev;
    Clientptr next;
    short hp;
    short pow;
    short blc;
};

typedef struct Linkedbattle Battle; // Alias
struct Linkedbattle {
    int pid;
    Clientptr c1;
    Clientptr c2;
    Battle *prev;
    Battle *next;
};

Battle *battlelist;
Clientptr registerlist; // Clients waiting for registration (name)
Clientptr matchedclient; // Mached clients for a battle
Clientptr matchingclient; // Clients waiting for match
char e;

int _init_server();
Clientptr init_client(int soc);
int getname(Clientptr client);
Clientptr add_client(Clientptr list, Clientptr client);
Battle *add_battle(Battle *list, Battle *battle);
void remove_client(Clientptr client, int notify);
Clientptr poll_client(Clientptr list, Clientptr client);
Battle *poll_battle(Battle *list, Battle *battle);
void welcome_client(Clientptr client);
void _start_battle(Clientptr c1, Clientptr c2);
void _end_battle(pid_t battlepid);
int client_connection(Clientptr client);
void notify_all(char *msg, int msglen);
Battle *poll_battle(Battle *list, Battle *battle);
void _match();
void sigchld_handler(int sig);
Battle *init_battle(pid_t pid, Clientptr client1, Clientptr client2);
void init_battler(Clientptr client);
void battle(Clientptr c1, Clientptr c2);
void battle(Clientptr c1, Clientptr c2);
void play_turn(Clientptr c1, Clientptr c2, char buf[], short max, fd_set set);
void settle(Clientptr winner, Clientptr loser, short tie, char buf[]);
void evaluate(Clientptr c1, Clientptr c2, char buf[]);
short dmg(char c);
int speak(char buf[], Clientptr speaker, Clientptr listener);
char move(Clientptr client, char mov);



int main() { // Launch Server
    int listen_soc = _init_server();
    int max = listen_soc;
    fd_set regiset, set;
    FD_ZERO(&regiset);
    FD_SET(listen_soc, &regiset);
    sigset_t lock, unlock;
    sigemptyset(&lock);
    // Lock sigchld_handler from matching when the main process is matching
    sigaddset(&lock, SIGCHLD);
    // Prevent processes from shutting down by writting on closed socket
    sigaddset(&lock, SIGPIPE);
    sigaddset(&unlock, SIGPIPE);
    while (1) {
        set = regiset;
        int n = select(max + 1, &set, NULL, NULL, NULL);
        if (n == -1) {
            fprintf(stderr, "%s/select: %s\n", __func__, strerror(errno));
            continue;
        }
        if (FD_ISSET(listen_soc, &set)) { // New Client comming
            int new_soc = accept(listen_soc, NULL, NULL);
            if (new_soc == -1) fprintf(stderr, "%s/accept: %s\n", __func__, strerror(errno));
            FD_SET(new_soc, &regiset);
            registerlist = add_client(registerlist, init_client(new_soc));
            write(new_soc, "What is your name?", 19);
            if (new_soc > max) max = new_soc;
            if (--n == 0) continue;
        }
        Clientptr next;
        for (Clientptr cur = registerlist; cur && n; cur = next) { // Registering clients inputting names
            next = cur->next;
            if (!FD_ISSET(cur->soc, &set)) continue; // Socket not ready
            short got = getname(cur);
            if (got < 0) continue; // Haven't finished the name yet
            if (got > 0) { // Name Complete
                registerlist = poll_client(registerlist, cur);
                welcome_client(cur);
                sigprocmask(SIG_BLOCK, &lock, &unlock); // Block sigchld (which also calls _match())
                _match(); // Registered client inidcating potential match
                sigprocmask(SIG_SETMASK, &unlock, &lock);
            }
            else { // Got nothing, indicating disconnected client (or error)
                registerlist = poll_client(registerlist, cur);
                remove_client(cur, 0);
            }
            FD_CLR(cur->soc, &regiset);
            if (--n == 0) break;
        }
    }
}

/*
 * Initialize the server
*/
int _init_server() {
    // For SIGCHLD handler
    struct sigaction action;
    action.sa_handler = sigchld_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &action, NULL) < 0) fprintf(stderr, "%s/sigaction/CHLD: %s\n", __func__, strerror(errno));
    // Lists
    registerlist = NULL; // Lists
    matchingclient = NULL;
    matchedclient = NULL;
    battlelist = NULL;
    // Socket
    int listen_soc = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_soc == -1) fprintf(stderr, "%s/socket: %s\n", __func__, strerror(errno));
    int yes = 1;
    if ((setsockopt(listen_soc, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) fprintf(stderr, "%s/setsockopt: %s\n", __func__, strerror(errno));
    struct sockaddr_in r = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr = {.s_addr = INADDR_ANY}};
    if (bind(listen_soc, (struct sockaddr *) &r, sizeof(r))) fprintf(stderr, "%s/bind: %s\n", __func__, strerror(errno));
    if (listen(listen_soc, MAX_PARTIAL)) fprintf(stderr, "%s/listen: %s\n", __func__, strerror(errno));
    return listen_soc;
}

/*
 * Parse User input to name
*/
int getname(Clientptr client) {
    int got = read(client->soc, client->name + client->hp, MAX_NAME - client->hp); // Should not eceed MAX_NAME
    if (got > 0) { // Got something
        client->hp += got;
        if (client->name[client->hp - 1] == '\n') { // newline
            client->name[client->hp -= 1] = '\0';
            return 1;
        }
        if (client->hp == MAX_NAME) { // reach MAX_NAME
            client->name[MAX_NAME] = '\0';
            return 1;
        }
        return -1; // Not finished yet
    }
    return 0;
}

/*
 * Welcome a registered client
*/
void welcome_client(Clientptr client) {
    char msg[client->hp + 24];
    if (snprintf(msg, sizeof(msg), "**%s enters the arena**\r\n", client->name) < 0) fprintf(stderr, "%s/snprintf: %s\n", __func__, strerror(errno));
    notify_all(msg, sizeof(msg));
    matchingclient = add_client(matchingclient, client);
    if (write(client->soc, WAIT_MSG, WAIT_MSG_LEN) == -1) fprintf(stderr, "%s/write: %s\n", __func__, strerror(errno));
}

/*
 * Create a new client
*/
Clientptr init_client(int soc) {
    Clientptr client = malloc(sizeof(Client));
    client->soc = soc;
    client->hp = 0;
    return client;
}

/*
 * add a client to the top of a list
*/
Clientptr add_client(Clientptr list, Clientptr node) {
    node->prev = NULL;
    node->next = list;
    if (list) list->prev = node;
    return node;
}

/*
 * add a battle to the top of a list
*/
Battle *add_battle(Battle *list, Battle *node) {
    node->prev = NULL;
    node->next = list;
    if (list) list->prev = node;
    return node;
}

/*
 * Try matching the two clients
*/
void _match() {
    while (matchingclient && matchingclient->next) {
        Clientptr c1 = matchingclient;
        if (!client_connection(c1)) { // Clear zombie client
            matchingclient = poll_client(matchingclient, c1);
            remove_client(c1, 1);
            continue;
        }
        Clientptr c2 = (c1->next->next) ? c1->next->next:c1->next; // Prioritize matcching clients exit from different battles
        if (!client_connection(c2)) { // Clear zombie client
            matchingclient = poll_client(matchingclient, c2);
            remove_client(c2, 1);
            continue;
        }
        // Put matched clients in a battle
        matchingclient = poll_client(matchingclient, c1);
        matchedclient = add_client(matchedclient, c1);
        matchingclient = poll_client(matchingclient, c2);
        matchedclient = add_client(matchedclient, c2);
        _start_battle(c1, c2);
    }
}

/*
 * Prepare for and start a new battle given two matched clients
*/
void _start_battle(Clientptr c1, Clientptr c2) {
    pid_t pid = fork();
    if (pid != 0) {
        if (pid < 0) { // Fatal error
            fprintf(stderr, "%s/fork: %s\n", __func__, strerror(errno));
            exit(1);
        }
        else battlelist = add_battle(battlelist, init_battle(pid, c1, c2));
        return;
    }
    init_battler(c1);
    init_battler(c2);
    battle(c1, c2); // Battle start
}

/*
 * End a battle, should only be called by SIGCHLD_handler
*/
void _end_battle(pid_t battlepid) {
    for (Battle *b = battlelist; b; b = b->next) { // Find ended battle
        if (b->pid != battlepid) continue; // By pid
        // Resume clients waiting for next battle
        matchedclient = poll_client(matchedclient, b->c1);
        if (client_connection(b->c1)) matchingclient = add_client(matchingclient, b->c1);
        else remove_client(b->c1, 1);
        matchedclient = poll_client(matchedclient, b->c2);
        if (client_connection(b->c2)) matchingclient = add_client(matchingclient, b->c2);
        else remove_client(b->c2, 1);
        // Remove the battle
        battlelist = poll_battle(battlelist, b);
        break;
    }
}

/*
 * Initialize a battleS
*/
Battle *init_battle(pid_t pid, Clientptr client1, Clientptr client2) {
    Battle *battle = malloc(sizeof(Battle));
    battle->pid = pid;
    battle->c1 = client1;
    battle->c2 = client2;
    return battle;
}

/*
 * Initialize battle stats for a client
 * Notice these stats only exist in a battle (child process)
*/
void init_battler(Clientptr client) {
    client->hp = MAX_HP;
    client->pow = MAX_POW;
    client->blc = MAX_BLC;
}

/*
 * Hold a battle
*/
void battle(Clientptr c1, Clientptr c2) {
    char buf[MAX_LINE + 1]; // Buffer used throughout
    // Opening
    short n, max = (c1->soc > c2->soc) ? c1->soc:c2->soc;;
    if ((n = sprintf(buf, "You engage %s!", c2->name)) < 0) fprintf(stderr, "%s/snprintf/c1: %s\n", __func__, strerror(errno));
    write(c1->soc, buf, n + 1);
    if ((n = sprintf(buf, "You engage %s!", c1->name)) < 0) fprintf(stderr, "%s/snprintf/c2: %s\n", __func__, strerror(errno));
    write(c2->soc, buf, n + 1);
    // Set of both clients (for select)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(c1->soc, &set);
    FD_SET(c2->soc, &set);
    // Play turns until one client die
    while (c1->hp > 0 && c2->hp >0) play_turn(c1, c2, buf, max, set);
    // Evaluate battle result and settle
    evaluate(c1, c2, buf);
    // Bypass dynamically allocated space (malloc) handling
    execlp("/bin/true", "true", (char *) NULL);
}

/*
 * playe a turn until 1. both clients have picked the move or 2. either clients drops
*/
void play_turn(Clientptr c1, Clientptr c2, char buf[], short max, fd_set set) { // max: max fd
    // Clear prior garbage
    if (recv(c1->soc, buf, MAX_LINE + 1, MSG_DONTWAIT) > MAX_LINE) {
        c1->hp = 0;
        write(c1->soc, NO_SPAM, NO_SPAM_LEN);
        return;
    }
    if (recv(c2->soc, buf, MAX_LINE + 1, MSG_DONTWAIT) > MAX_LINE) {
        c2->hp = 0;
        write(c2->soc, NO_SPAM, NO_SPAM_LEN);
        return;
    }
    // Turn info
    int n;
    if ((n = sprintf(buf, "\r\n\r\nYour hitpoints:%d\r\nYour powermoves:%d\r\nYour block:%d\r\n\r\n%s's hitpoints: %d\r\n", c1->hp, c1->pow, c2->blc, c2->name, c2->hp))) fprintf(stderr, "%s/snprintf/c1: %s\n", __func__, strerror(errno));;
    write(c1->soc, buf, n + 1);
    if ((n = sprintf(buf, "\r\n\r\nYour hitpoints:%d\r\nYour powermoves:%d\r\nYour block:%d\r\n\r\n%s's hitpoints: %d\r\n", c2->hp, c2->pow, c2->blc, c1->name, c1->hp))) fprintf(stderr, "%s/snprintf/c2: %s\n", __func__, strerror(errno));;
    write(c2->soc, buf, n + 1);
    // Move list
    write(c1->soc, MOV_MSG, MOV_MSG_LEN);
    write(c2->soc, MOV_MSG, MOV_MSG_LEN);
    // damages & moves c1/c2 perform in this turn
    short dmg1 = -1, dmg2 = -1;
    char mov1 = '\0', mov2 = '\0';
    // Loop until one of two conditions meet
    while (select(max + 1, &set, NULL, NULL, NULL) > 0) {
        if (!mov1) { // c1 has not picked a move
            if (FD_ISSET(c1->soc, &set)) { // c1 sent something
                n = read(c1->soc, buf, 1); // Read one char
                if (n == 0) { // c1 disconnected
                    c1->hp = 0;
                    return;
                }
                if (n == -1) { // Error
                    fprintf(stderr, "%s/read: %s\n", __func__, strerror(errno));
                    buf[0] = 'a';
                }
                mov1 = move(c1, buf[0]); // Parse c1 move
                if (mov1) { // A legal move
                    if (mov1 != 's') { // A battle move, no speaking from c1 anymore
                        FD_CLR(c1->soc, &set);
                        // notify c2 that c1 moved
                        if ((n = sprintf(buf, "\r\n%s has made a choice\r\n", c1->name)) < 0) fprintf(stderr, "%s/snprintf/c1: %s\n", __func__, strerror(errno));
                        write(c2->soc, buf, n + 1);
                        // Calculate Damage
                        if (dmg1 < 0) dmg1 = dmg(mov1); // damage is not blocked
                        if (mov1 == 'b') dmg2 = 0; // block c2 damage
                        if (mov2) break; // Both clients moved
                        else max = c2->soc; // Wait for c2
                    }
                    else if (!speak(buf, c1, c2)) { // c1 disconnects during speaking
                        c1->hp = 0;
                        return;
                    }
                    else mov1 = '\0'; // Unintelligible move
                }
            }
            else FD_SET(c1->soc, &set); // Resume listening c1
        }
        if (!mov2) { // c2 has not picked a move
            if (FD_ISSET(c2->soc, &set)) { // c2 sent something
                n = read(c2->soc, buf, 1); // Read one char
                if (n == 0) { // c2 disconnected
                    c2->hp = 0;
                    return;
                }
                if (n == -1) { // Error
                    fprintf(stderr, "%s/read: %s\n", __func__, strerror(errno));
                    buf[0] = 'a';
                }
                mov2 = move(c2, buf[0]); // Parse c2 move
                if (mov2) { // A legal move
                    if (mov2 != 's') { // A battle move, no speaking from c2 anymore
                        FD_CLR(c2->soc, &set);
                        // notify c1 that c2 moved
                        if ((n = sprintf(buf, "\r\n%s has made a choice\r\n", c2->name)) < 0) fprintf(stderr, "%s/snprintf/c2: %s\n", __func__, strerror(errno));;
                        write(c1->soc, buf, n + 1);
                        // Calculate Damage
                        if (dmg2 < 0) dmg2 = dmg(mov2); // damage is not blocked
                        if (mov2 == 'b') dmg1 = 0; // block c1 damage
                        if (mov1) break; // Both clients moved
                        else max = c1->soc; // Wait for c1
                    }
                    else if (!speak(buf, c2, c1)) { // c2 disconnects during speaking
                        c2->hp = 0;
                        return;
                    }
                    else mov2 = '\0'; // Unintelligible move
                }
            }
            else FD_SET(c2->soc, &set); // Resume listening c2
        }
    }
    // Evaluate damgages
    c1->hp -= dmg2;
    c2->hp -= dmg1;
}

/*
 * Return a client move in char, or NULL if the client inputted move is unintelligible 
*/
char move(Clientptr client, char mov) {
    if (mov == 'a' || mov == 's') return mov; // Attack/Speak
    if (mov == 'p') { // Power move
        if (client->pow < 1) return '\0'; // Not pp left
        client->pow -= 1; // Deduct pp
        return mov;
    }
    if (mov == 'b') {
        if (client->blc < 1) return '\0'; // No bp left
        client->blc -= 1; // Decuct bp
        return mov;
    }
    return '\0'; // Unintelligible move
}

/*
 * Return the damage of a move
*/
short dmg(char c) {
    if (c == 'a') return A_DMG;
    if (c == 'p') return P_DMG;
    if (c == 'b') return B_DMG;
    return 0;
}

/*
 * One client speak to the other, neither clients perform any action during the speach
*/
int speak(char buf[], Clientptr speaker, Clientptr listener) {
    short i;
    if ((i = sprintf(buf, SPEAK)) < 0) fprintf(stderr, "%s/snprintf/speaker: %s\n", __func__, strerror(errno));
    write(speaker->soc, buf, i + 1);
    if ((i = sprintf(buf, "\r\n%s takes a break to tell you:\r\n", speaker->name)) < 0) fprintf(stderr, "%s/snprintf/listener: %s\n", __func__, strerror(errno));;
    write(listener->soc, buf, i + 1);
    for (i = 0; i <= MAX_LINE; i++) {
        if (read(speaker->soc, buf + i, 1) == 0) return 0;
        if (buf[i] == '\n') break;
    }
    write(listener->soc, buf, i + 1);
    return 1;
}

/*
 * Evaluate the battle result and settle
*/
void evaluate(Clientptr c1, Clientptr c2, char buf[]) {
    if (c1->hp < 1 && c2->hp < 1) settle(c1, c2, 1, buf); // Tie
    else if (c1->hp < 1) settle(c2, c1, 0, buf); // c2 win
    else settle(c1, c2, 0, buf); // c1 win
}

/*
 * settle a battle
*/
void settle(Clientptr winner, Clientptr loser, short tie, char buf[]) {
    if (tie) { // Tie
        write(winner->soc, TIE_MSG, TIE_MSG_LEN);
        write(loser->soc, TIE_MSG, TIE_MSG_LEN);
        write(winner->soc, WAIT_MSG, WAIT_MSG_LEN);
        write(loser->soc, WAIT_MSG, WAIT_MSG_LEN);
        return;
    }
    // Notify battle victory/defeat
    short n;
    if (!client_connection(loser)) {
        if ((n = sprintf(buf, "\r\n--%s dropped. You win!\r\n\r\n", loser->name)) < 0) fprintf(stderr, "%s/snprintf/drop: %s\n", __func__, strerror(errno));
    }
    else {
        if ((n = sprintf(buf, "\r\nYou are no match for %s...You scurry away...\r\n\r\n", winner->name)) < 0) fprintf(stderr, "%s/snprintf/loser: %s\n", __func__, strerror(errno));;
        write(loser->soc, buf, n + 1);
        write(loser->soc, WAIT_MSG, WAIT_MSG_LEN);
        if ((n = sprintf(buf, "\r\n%s gives up. You win!\r\n\r\n", loser->name)) < 0) fprintf(stderr, "%s/snprintf/winner: %s\n", __func__, strerror(errno));;
    }
    if (!client_connection(winner)) return;
    write(winner->soc, buf, n + 1);
    write(winner->soc, WAIT_MSG, WAIT_MSG_LEN);
}

/*
 * Notify everyone somethign 
*/
void notify_all(char *msg, int msglen) {
    for (Clientptr c = registerlist; c; c = c->next) write(c->soc, msg, msglen);
    for (Clientptr c = matchedclient; c; c = c->next) write(c->soc, msg, msglen);
    for (Clientptr c = matchingclient; c; c = c->next) write(c->soc, msg, msglen);
}

/*
 * Check if a client is still connected
*/
int client_connection(Clientptr client) {
    if (recv(client->soc, &e, 1, MSG_NOSIGNAL | MSG_PEEK | MSG_DONTWAIT)) return 1;
    if (close(client->soc) == -1) fprintf(stderr, "%s/close: %s\n", __func__, strerror(errno));
    return 0;
}

/*
 * Permenantly clear a (disconnected) client
*/
void remove_client(Clientptr client, int notify) {
    close(client->soc);
    if (notify) { // Notify everyone
        char msg[MAX_NAME + 14];
        if (sprintf(msg, "**%s leaves**\r\n", client->name) < 0) fprintf(stderr, "%s/snprintf: %s\n", __func__, strerror(errno));
        notify_all(msg, sizeof(msg));
    }
    free(client);
}

/*
 * Retrieve and remove a client form a list
*/
Clientptr poll_client(Clientptr list, Clientptr client) {
    Clientptr next = client->next, prev = client->prev;
    if (next) {
        next->prev = prev;
        client->next = NULL;
    }
    if (prev) {
        prev->next = next;
        client->prev = NULL;
    }
    else return next;
    return list;
}

/*
 * Permenantly clear an (ended) battle
*/
Battle *poll_battle(Battle *list, Battle *battle) {
    Battle *next = battle->next, *prev = battle->prev;
    free(battle);
    if (next) next->prev = prev;
    if (prev) prev->next = next;
    else return next;
    return list;
}

/*
 * Handle ended battles
*/
void sigchld_handler(int sig) {
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) { // A battle just finished
        _end_battle(pid); // End the battle
        _match(); // An ended battle implies a new match
    }
}
