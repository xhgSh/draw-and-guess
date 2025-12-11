#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include "protocol.h"
#include "sqlite3.h"

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024

// Client information
typedef struct {
    int socket_fd;
    int id;
    char nickname[32];
    int ready;
    int is_painter;
    char guess[64];
    int has_guessed;
    struct sockaddr_in udp_addr;
    int has_udp_addr;
    int room_id; // Add room_id to ClientInfo
} ClientInfo;

// Game information
typedef struct {
    GameState state;
    int painter_id;
    char current_word[32];
    int ready_count;
    int total_clients;
    time_t paint_start_time;
    time_t guess_start_time;
    int current_game_id;
} GameInfo;

// Add room struct
#define MAX_ROOMS 10
#define MAX_DRAWING_POINTS 4096

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t action;
} DrawingPoint;

typedef struct {
    uint8_t id;
    char name[32];
    ClientInfo clients[MAX_CLIENTS];
    GameInfo game;
    int client_count;
    DrawingPoint drawing_history[MAX_DRAWING_POINTS];
    int history_count;
    // AI prediction result (stored but not broadcast until all clients submit)
    char ai_predicted_word[32];
    uint8_t ai_score;
    uint8_t ai_is_correct;
    int ai_result_ready; // 1 if AI result is available, 0 otherwise
} Room;

Room rooms[MAX_ROOMS];
pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

ClientInfo clients[MAX_CLIENTS];
    GameInfo game; // Kept for struct definition, not used as global
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int udp_socket;
int tcp_socket;
int running = 1;
sqlite3 *db;

// Initialize Database
void init_db() {
    int rc = sqlite3_open("game_data.db", &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    } else {
        printf("Opened database successfully\n");
    }

    char *err_msg = 0;
    
    // Create words table
    const char *sql_words = "CREATE TABLE IF NOT EXISTS words ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "word TEXT UNIQUE NOT NULL);";
                            
    rc = sqlite3_exec(db, sql_words, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create words): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    // Create history table
    const char *sql_history = "CREATE TABLE IF NOT EXISTS history ("
                              "record_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "game_id INTEGER,"
                              "word TEXT,"
                              "username TEXT,"
                              "user_guess TEXT,"
                              "game_time TEXT);";
                              
    rc = sqlite3_exec(db, sql_history, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create history): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    // Create drawing_data table
    const char *sql_drawing = "CREATE TABLE IF NOT EXISTS drawing_data ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "game_id INTEGER,"
                              "x INTEGER,"
                              "y INTEGER,"
                              "action INTEGER,"
                              "color_r INTEGER,"
                              "color_g INTEGER,"
                              "color_b INTEGER,"
                              "timestamp INTEGER);";
    
    rc = sqlite3_exec(db, sql_drawing, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create drawing_data): %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    
    // Check if words exist, if not add some
    const char *count_words = "SELECT count(*) FROM words;";
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, count_words, -1, &stmt, 0);
    
    int count = 0;
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    
    if (count == 0) {
        printf("Populating words table...\n");
        const char *initial_words[] = {
            "apple", "banana", "watermelon", "car", "mouse", 
            "computer", "ocean", "mountain", "sun", "moon",
            "house", "tree", "dog", "cat", "bird"
        };
        int num_words = sizeof(initial_words) / sizeof(initial_words[0]);
        
        for (int i = 0; i < num_words; i++) {
            char sql_insert[128];
            sprintf(sql_insert, "INSERT INTO words (word) VALUES ('%s');", initial_words[i]);
            sqlite3_exec(db, sql_insert, 0, 0, 0);
        }
    }
}

void get_random_word(char *buffer) {
    const char *sql = "SELECT word FROM words ORDER BY RANDOM() LIMIT 1;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *word = sqlite3_column_text(stmt, 0);
            strcpy(buffer, (const char*)word);
        } else {
             // Fallback
            strcpy(buffer, "apple");
        }
    } else {
        strcpy(buffer, "apple");
    }
    sqlite3_finalize(stmt);
}

// Forward declarations
void broadcast_message(BaseMessage* msg, int exclude_id, int room_id);

void* ai_guess_thread(void* arg) {
    int room_id = *(int*)arg;
    free(arg);
    
    printf("AI Thread: Starting inference for room %d\n", room_id);
    
    pthread_mutex_lock(&rooms_mutex);
    Room* room = &rooms[room_id];
    
    // Serialize data while locked
    char* json_buf = malloc(256 * 1024);
    if (!json_buf) {
        pthread_mutex_unlock(&rooms_mutex);
        return NULL;
    }
    
    // Get all words from database
    char candidates_json[64 * 1024] = "["; // Large buffer for all words
    int first = 1;
    const char *sql = "SELECT word FROM words;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *word = (const char*)sqlite3_column_text(stmt, 0);
            if (!first) {
                strcat(candidates_json, ", ");
            }
            first = 0;
            strcat(candidates_json, "\"");
            strcat(candidates_json, word);
            strcat(candidates_json, "\"");
        }
        sqlite3_finalize(stmt);
    }
    strcat(candidates_json, "]");
    
    int offset = sprintf(json_buf, "{\"target\": \"%s\", \"candidates\": %s, \"drawing\": [", 
                         room->game.current_word, candidates_json);
    
    for (int i = 0; i < room->history_count; i++) {
        if (i > 0) offset += sprintf(json_buf + offset, ",");
        offset += sprintf(json_buf + offset, "{\"x\":%d,\"y\":%d,\"action\":%d}", 
                          room->drawing_history[i].x, 
                          room->drawing_history[i].y, 
                          room->drawing_history[i].action);
    }
    sprintf(json_buf + offset, "]}");
    
    pthread_mutex_unlock(&rooms_mutex);
    
    // Connect to AI service
    int ai_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ai_sock < 0) {
        free(json_buf);
        return NULL;
    }
    
    struct sockaddr_in ai_addr;
    ai_addr.sin_family = AF_INET;
    ai_addr.sin_port = htons(5000);
    ai_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(ai_sock, (struct sockaddr*)&ai_addr, sizeof(ai_addr)) < 0) {
        printf("AI Thread Room %d: Failed to connect to AI service on port 5000. Is ai_service.py running?\n", room_id);
        free(json_buf);
        close(ai_sock);
        pthread_mutex_unlock(&rooms_mutex);
        return NULL;
    }
    
    printf("AI Thread Room %d: Connected to AI service, sending data...\n", room_id);
    
    // Send data
    uint32_t len = htonl(strlen(json_buf));
    send(ai_sock, &len, 4, 0);
    send(ai_sock, json_buf, strlen(json_buf), 0);
    free(json_buf);
    
    // Receive response
    uint32_t resp_len;
    if (recv(ai_sock, &resp_len, 4, 0) == 4) {
        resp_len = ntohl(resp_len);
        printf("AI Thread Room %d: Receiving response, length: %u\n", room_id, resp_len);
        char* resp_buf = malloc(resp_len + 1);
        if (!resp_buf) {
            printf("AI Thread Room %d: Failed to allocate response buffer\n", room_id);
            close(ai_sock);
            return NULL;
        }
        int received = 0;
        while (received < resp_len) {
            int r = recv(ai_sock, resp_buf + received, resp_len - received, 0);
            if (r <= 0) {
                printf("AI Thread Room %d: Error receiving response data\n", room_id);
                free(resp_buf);
                close(ai_sock);
                return NULL;
            }
            received += r;
        }
        resp_buf[received] = '\0';
        
        printf("AI Thread Room %d: Received response: %s\n", room_id, resp_buf);
        
        char predicted[32] = "Unknown";
        int is_correct = 0;
        int score = 0;
        
        char* p = strstr(resp_buf, "\"predicted_word\": \"");
        if (p) {
            p += 19;
            char* end = strchr(p, '\"');
            if (end) {
                *end = '\0';
                strncpy(predicted, p, 31);
                *end = '\"'; // Restore
            }
        }
        
        p = strstr(resp_buf, "\"is_correct\": ");
        if (p) is_correct = atoi(p + 14);
        
        p = strstr(resp_buf, "\"score\": ");
        if (p) score = atoi(p + 9);
        
        // Store result instead of broadcasting immediately
        pthread_mutex_lock(&rooms_mutex);
        Room* room = &rooms[room_id];
        strcpy(room->ai_predicted_word, predicted);
        room->ai_score = score;
        room->ai_is_correct = is_correct;
        room->ai_result_ready = 1;
        pthread_mutex_unlock(&rooms_mutex);
        
        printf("AI Result Room %d: Predicted=%s, Correct=%d, Score=%d (stored, will broadcast after all guesses)\n", room_id, predicted, is_correct, score);
        
        free(resp_buf);
    } else {
        printf("AI Thread Room %d: Failed to receive response length from AI service\n", room_id);
    }
    
    close(ai_sock);
    return NULL;
}

void* handle_tcp_client(void* arg);
void* handle_udp_server(void* arg);
void broadcast_message(BaseMessage* msg, int exclude_id, int room_id);
void start_game(int room_id);
void end_game(int room_id);
void cleanup();
void init_game(GameInfo* game_info);

// Signal handler function (handle Ctrl+C and other interrupt signals)
void signal_handler(int sig) {
    printf("\nClosing...\n");
    running = 0;
    cleanup();
    exit(0);
}

// Initialize game state
void init_game(GameInfo* game_info) {
    game_info->state = GAME_WAITING;      // Set to waiting for players state
    game_info->painter_id = -1;           // Reset painter ID
    game_info->ready_count = 0;           // Reset ready count
    game_info->total_clients = 0;         // Reset total clients
    memset(game_info->current_word, 0, sizeof(game_info->current_word)); // Clear current word
}

// Add client to client list
int add_client(int socket_fd) {
    pthread_mutex_lock(&clients_mutex);
    
    // Find an empty client slot
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket_fd == -1) {
            //init client info
            clients[i].socket_fd = socket_fd;
            clients[i].id = i;
            clients[i].ready = 0;
            clients[i].is_painter = 0;
            clients[i].has_guessed = 0;
            //Clear old info
            memset(clients[i].nickname, 0, sizeof(clients[i].nickname));
            memset(clients[i].guess, 0, sizeof(clients[i].guess));
            clients[i].has_udp_addr = 0;
            clients[i].room_id = -1; // Initialize room_id
            memset(&clients[i].udp_addr, 0, sizeof(clients[i].udp_addr));
            
            // game.total_clients++; // Removed global game update
            printf("Client %d connected\n", i);
            
            pthread_mutex_unlock(&clients_mutex);
            return i;//client id
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

//call when a client disconnects
void remove_client(int client_id) {
    pthread_mutex_lock(&clients_mutex);
    
    if (client_id >= 0 && client_id < MAX_CLIENTS && clients[client_id].socket_fd != -1) {
        close(clients[client_id].socket_fd);
        clients[client_id].socket_fd = -1;
        // game.total_clients--; // Removed global game update
        
        int room_id = clients[client_id].room_id;
        if (room_id != -1) {
            pthread_mutex_lock(&rooms_mutex);
            rooms[room_id].game.total_clients--;
        if (clients[client_id].ready) {
                rooms[room_id].game.ready_count--;
        }
            // Remove from room clients list
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (rooms[room_id].clients[i].id == client_id) {
                    rooms[room_id].clients[i].socket_fd = -1;
                    rooms[room_id].client_count--;
                    if (rooms[room_id].client_count == 0) {
                         memset(rooms[room_id].name, 0, sizeof(rooms[room_id].name));
                         init_game(&rooms[room_id].game);
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&rooms_mutex);
        }
        clients[client_id].room_id = -1;
        
        printf("Client %d disconnected\n", client_id);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

//Broadcast to guesser in a specific room
void broadcast_message(BaseMessage* msg, int exclude_id, int room_id) {
    if (room_id < 0 || room_id >= MAX_ROOMS) return;

    pthread_mutex_lock(&rooms_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int client_idx = rooms[room_id].clients[i].id;
        if (rooms[room_id].clients[i].socket_fd != -1 && client_idx != exclude_id) {
            // Use the global clients array to get the socket_fd because room clients might be copies or just IDs
            // But here we stored copies in room->clients. Better to use global clients array using ID if possible 
            // or just use the socket_fd stored in room.
            // Let's use the socket_fd stored in room->clients
             if (rooms[room_id].clients[i].socket_fd != -1)
                send(rooms[room_id].clients[i].socket_fd, msg, sizeof(BaseMessage) + msg->data_len, 0);
        }
    }
    pthread_mutex_unlock(&rooms_mutex);
}

void start_game(int room_id) {
    pthread_mutex_lock(&rooms_mutex);
    Room* room = &rooms[room_id];
    GameInfo* game = &room->game;

    // Check if all clients are ready and at least 2 clients
    if (game->state != GAME_READY || game->ready_count != game->total_clients || game->total_clients < 2) {
        printf("Room %d cannot start: state=%d, ready=%d, total=%d\n", room_id, game->state, game->ready_count, game->total_clients);
        pthread_mutex_unlock(&rooms_mutex);
        return;
    }

    //Random seed for selecting painter
    srand((unsigned int)time(NULL));
    int start_index = rand() % room->client_count;
    int painter_idx = -1;
    
    // Find valid painter in this room
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (room->clients[i].socket_fd != -1) {
            if (count == start_index) {
                painter_idx = i; // Index in room->clients array
                game->painter_id = room->clients[i].id; // Global client ID
                room->clients[i].is_painter = 1;
                // Update global client info too
                clients[room->clients[i].id].is_painter = 1;
            break;
        }
            count++;
        }
    }

    if (game->painter_id == -1) {
        pthread_mutex_unlock(&rooms_mutex);
        return;
    }

    // Random seed for select word
    get_random_word(game->current_word);

    game->state = GAME_PAINTING;
    game->paint_start_time = time(NULL);
    game->current_game_id = (int)time(NULL) + rand(); // Generate unique game ID for this session
    
    // Initialize drawing history for AI
    room->history_count = 0;
    
    // Reset AI result for new game
    room->ai_result_ready = 0;
    memset(room->ai_predicted_word, 0, sizeof(room->ai_predicted_word));

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (room->clients[i].socket_fd != -1) {
            GameStartMessage start_msg;
            start_msg.base.type = MSG_GAME_START;
            start_msg.base.client_id = (uint8_t)room->clients[i].id;
            start_msg.base.data_len = sizeof(GameStartMessage) - sizeof(BaseMessage);
            start_msg.painter_id = (uint8_t)game->painter_id;
            strcpy(start_msg.word, game->current_word);
            start_msg.paint_time = 60;
            send(room->clients[i].socket_fd, &start_msg, sizeof(start_msg), 0);
        }
    }

    printf("Room %d Game started! Painter: Client %d, Word: %s\n", room_id, game->painter_id, game->current_word);

    pthread_mutex_unlock(&rooms_mutex);
}

void end_game(int room_id) {
    pthread_mutex_lock(&rooms_mutex);
    Room* room = &rooms[room_id];
    GameInfo* game = &room->game;
    
    if (game->state != GAME_GUESSING) {
        pthread_mutex_unlock(&rooms_mutex);
        return;
    }
    
    game->state = GAME_FINISHED;
    
    GameEndMessage end_msg;
    end_msg.base.type = MSG_GAME_END;
    end_msg.base.client_id = 0;
    end_msg.base.data_len = sizeof(GameEndMessage) - sizeof(BaseMessage);
    strcpy(end_msg.correct_word, game->current_word);
    end_msg.winner_id = 255;//default timeout
    end_msg.guess_count = 0;
    
    //find winner in room
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (room->clients[i].socket_fd != -1 && room->clients[i].has_guessed) {
            end_msg.guess_count++;

            if (strcmp(room->clients[i].guess, game->current_word) == 0) {
                end_msg.winner_id = (uint8_t)room->clients[i].id;//check correctoin
        }
    }
    }
    
    pthread_mutex_unlock(&rooms_mutex);
    broadcast_message((BaseMessage*)&end_msg, -1, room_id);
    pthread_mutex_lock(&rooms_mutex);
    
    if (end_msg.winner_id != 255) {
        printf("Room %d Game over! Answer: %s, Winner: Client %d\n", room_id, game->current_word, end_msg.winner_id);
    } else {
        printf("Room %d Game over! Answer: %s, No one guessed it\n", room_id, game->current_word);
    }
    
    // Broadcast AI result after all clients have submitted
    if (room->ai_result_ready) {
        AiGuessResultMessage ai_msg;
        ai_msg.base.type = MSG_AI_GUESS_RESULT;
        ai_msg.base.client_id = 0;
        ai_msg.base.data_len = sizeof(AiGuessResultMessage) - sizeof(BaseMessage);
        strcpy(ai_msg.predicted_word, room->ai_predicted_word);
        ai_msg.is_correct = room->ai_is_correct;
        ai_msg.score = room->ai_score;
        
        pthread_mutex_unlock(&rooms_mutex);
        broadcast_message((BaseMessage*)&ai_msg, -1, room_id);
        pthread_mutex_lock(&rooms_mutex);
        
        printf("Room %d AI Result broadcasted: %s, Score: %d\n", room_id, room->ai_predicted_word, room->ai_score);
        
        // Reset AI result for next game
        room->ai_result_ready = 0;
    }
    
    // Save history to DB
    int game_id = game->current_game_id; // Use the stored game_id
    char time_str[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(time_str, sizeof(time_str)-1, "%Y-%m-%d %H:%M:%S", t);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (room->clients[i].socket_fd != -1) {
            // Save for everyone
            const char *sql = "INSERT INTO history (game_id, word, username, user_guess, game_time) VALUES (?, ?, ?, ?, ?);";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, game_id);
                sqlite3_bind_text(stmt, 2, game->current_word, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 3, room->clients[i].nickname, -1, SQLITE_STATIC);
                
                if (room->clients[i].id == game->painter_id) {
                     sqlite3_bind_text(stmt, 4, "(Painter)", -1, SQLITE_STATIC);
                } else if (room->clients[i].has_guessed) {
                     sqlite3_bind_text(stmt, 4, room->clients[i].guess, -1, SQLITE_STATIC);
                } else {
                     sqlite3_bind_text(stmt, 4, "(No Guess)", -1, SQLITE_STATIC);
                }
                
                sqlite3_bind_text(stmt, 5, time_str, -1, SQLITE_STATIC);
                
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    game->state = GAME_WAITING;
    game->painter_id = -1;
    game->ready_count = 0;
    memset(game->current_word, 0, sizeof(game->current_word));
    
    //reset room client states
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (room->clients[i].socket_fd != -1) {
            room->clients[i].ready = 0;        
            room->clients[i].has_guessed = 0;  
            room->clients[i].is_painter = 0;   
            memset(room->clients[i].guess, 0, sizeof(room->clients[i].guess)); 
            
            // Also update global client info
            clients[room->clients[i].id].ready = 0;
            clients[room->clients[i].id].has_guessed = 0;
            clients[room->clients[i].id].is_painter = 0;
            memset(clients[room->clients[i].id].guess, 0, sizeof(clients[room->clients[i].id].guess));
        }
    }
    
    pthread_mutex_unlock(&rooms_mutex);
}

void* handle_tcp_client(void* arg) {
    int client_id = *(int*)arg;
    char buffer[BUFFER_SIZE];
    
    while (running) {
        int bytes_received = recv(clients[client_id].socket_fd, buffer, BUFFER_SIZE, 0);
        
        if (bytes_received <= 0) {
            remove_client(client_id);
            break;
        }
        
        BaseMessage* msg = (BaseMessage*)buffer;
        
        switch (msg->type) {
            case MSG_CLIENT_JOIN: {
                ClientJoinMessage* join_msg = (ClientJoinMessage*)msg;
                strcpy(clients[client_id].nickname, join_msg->nickname);
                printf("Client %d nickname: %s\n", client_id, join_msg->nickname);
                break;
            }
            
            case MSG_CLIENT_READY: {
                int room_id = clients[client_id].room_id;
                if (room_id != -1) {
                    pthread_mutex_lock(&rooms_mutex);
                    // Find client in room
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (rooms[room_id].clients[i].id == client_id) {
                            if (!rooms[room_id].clients[i].ready) {
                                rooms[room_id].clients[i].ready = 1;
                                clients[client_id].ready = 1; // Update global
                                
                                rooms[room_id].game.ready_count++;
                                
                                if (rooms[room_id].game.state == GAME_WAITING) {
                                    rooms[room_id].game.state = GAME_READY;
                    }
                                
                                // Use game.total_clients instead of client_count
                                int can_start = (rooms[room_id].game.ready_count == rooms[room_id].game.total_clients && rooms[room_id].game.total_clients >= 2);
                                printf("Room %d Client %d ready (%d/%d)\n", room_id, client_id, rooms[room_id].game.ready_count, rooms[room_id].game.total_clients);
                                
                    if (can_start) {
                                    pthread_mutex_unlock(&rooms_mutex);
                                    start_game(room_id);
                                    pthread_mutex_lock(&rooms_mutex);
                    }
                            }
                            break;
                        }
                    }
                    pthread_mutex_unlock(&rooms_mutex);
                } else {
                    printf("Client %d tried to ready but not in a room\n", client_id);
                }
                break;
            }
            
            case MSG_PAINTER_FINISH: {
                int room_id = clients[client_id].room_id;
                if (room_id != -1) {
                    pthread_mutex_lock(&rooms_mutex);
                    if (client_id == rooms[room_id].game.painter_id && rooms[room_id].game.state == GAME_PAINTING) {
                        rooms[room_id].game.state = GAME_GUESSING;
                        rooms[room_id].game.guess_start_time = time(NULL);

                        BaseMessage finish_msg;
                        finish_msg.type = MSG_PAINTER_FINISH;
                        finish_msg.client_id = 0;
                        finish_msg.data_len = 0;
                        
                        pthread_mutex_unlock(&rooms_mutex);
                        broadcast_message(&finish_msg, -1, room_id);
                        
                        printf("Room %d Painter %d finished painting, entering guessing phase\n", room_id, client_id);
                        
                        // Trigger AI guess
                        int* arg = malloc(sizeof(int));
                        *arg = room_id;
                        pthread_t ai_thread;
                        pthread_create(&ai_thread, NULL, ai_guess_thread, arg);
                        pthread_detach(ai_thread);
                    } else {
                        pthread_mutex_unlock(&rooms_mutex);
                    }
                }
                break;
            }
            
            case MSG_GUESS_SUBMIT: {
                // Submit guess message: save client guess, end game if all submitted
                GuessSubmitMessage* guess_msg = (GuessSubmitMessage*)msg;
                int room_id = clients[client_id].room_id;
                
                if (room_id != -1) {
                    pthread_mutex_lock(&rooms_mutex);
                    // Update room client
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (rooms[room_id].clients[i].id == client_id) {
                            strcpy(rooms[room_id].clients[i].guess, guess_msg->guess);
                            rooms[room_id].clients[i].has_guessed = 1;
                            break;
                        }
                    }
                    // Update global client (backup)
                strcpy(clients[client_id].guess, guess_msg->guess);
                clients[client_id].has_guessed = 1;
                
                    printf("Room %d Client %d guess: %s\n", room_id, client_id, guess_msg->guess);
                
                    if (strcmp(guess_msg->guess, rooms[room_id].game.current_word) == 0) {
                        printf("Room %d Client %d guessed correctly!\n", room_id, client_id);
                }
                
                int all_guessed = 1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (rooms[room_id].clients[i].socket_fd != -1 && !rooms[room_id].clients[i].is_painter && !rooms[room_id].clients[i].has_guessed) {
                        all_guessed = 0;
                        break;
                    }
                }
                    pthread_mutex_unlock(&rooms_mutex);
                
                if (all_guessed) {
                        end_game(room_id);
                    }
                }
                break;
            }
            
            case MSG_CLIENT_LEAVE: {
                remove_client(client_id);
                return NULL;
            }
            
            case MSG_HISTORY_REQ: {
                printf("Client %d requested history\n", client_id);
                const char *sql = "SELECT game_id, word, user_guess, game_time FROM history WHERE username = ? ORDER BY record_id DESC LIMIT 50;";
                sqlite3_stmt *stmt;
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, clients[client_id].nickname, -1, SQLITE_STATIC);
                    
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        HistoryDataMessage h_msg;
                        h_msg.base.type = MSG_HISTORY_DATA;
                        h_msg.base.client_id = 0;
                        h_msg.base.data_len = sizeof(HistoryDataMessage) - sizeof(BaseMessage);
                        
                        h_msg.game_id = sqlite3_column_int(stmt, 0);
                        strcpy(h_msg.word, (const char*)sqlite3_column_text(stmt, 1));
                        strcpy(h_msg.user_guess, (const char*)sqlite3_column_text(stmt, 2));
                        strcpy(h_msg.game_time, (const char*)sqlite3_column_text(stmt, 3));
                        
                        send(clients[client_id].socket_fd, &h_msg, sizeof(BaseMessage) + h_msg.base.data_len, 0);
                    }
                    sqlite3_finalize(stmt);
                }
                
                BaseMessage end_msg;
                end_msg.type = MSG_HISTORY_END;
                end_msg.client_id = 0;
                end_msg.data_len = 0;
                send(clients[client_id].socket_fd, &end_msg, sizeof(BaseMessage), 0);
                break;
            }

            case MSG_ROOM_LIST_REQ: {
                pthread_mutex_lock(&rooms_mutex);
                RoomListMessage roomListMsg;
                roomListMsg.base.type = MSG_ROOM_LIST;
                roomListMsg.base.client_id = 0;
                roomListMsg.num_rooms = 0;
                for (int i = 0; i < MAX_ROOMS; i++) {
                    if (rooms[i].name[0] != '\0') {
                        roomListMsg.rooms[roomListMsg.num_rooms].room_id = rooms[i].id;
                        strcpy(roomListMsg.rooms[roomListMsg.num_rooms].name, rooms[i].name);
                        roomListMsg.rooms[roomListMsg.num_rooms].num_players = rooms[i].client_count;
                        roomListMsg.num_rooms++;
                    }
                }
                roomListMsg.base.data_len = sizeof(RoomListMessage) - sizeof(BaseMessage);
                pthread_mutex_unlock(&rooms_mutex);
                send(clients[client_id].socket_fd, &roomListMsg, sizeof(RoomListMessage), 0);
                break;
            }

            case MSG_CREATE_ROOM: {
                CreateRoomMessage* req = (CreateRoomMessage*)msg;
                pthread_mutex_lock(&rooms_mutex);
                int room_id = -1;
                // Find empty room slot
                for (int i = 0; i < MAX_ROOMS; i++) {
                    if (rooms[i].name[0] == '\0') {
                        room_id = i;
                        rooms[i].id = i;
                        strcpy(rooms[i].name, req->room_name);
                        rooms[i].client_count = 0;
                        // Initialize room clients
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                            rooms[i].clients[j].socket_fd = -1;
                        }
                        // Add client to room
                        pthread_mutex_lock(&clients_mutex);
                        strcpy(clients[client_id].nickname, req->nickname);
                        clients[client_id].room_id = room_id; // Set room_id
                        // Copy client info to room
                        rooms[i].clients[0] = clients[client_id];
                        rooms[i].clients[0].id = client_id; // Ensure ID is set
                        rooms[i].client_count = 1;
                        rooms[i].game.total_clients = 1; // Update total_clients
                        pthread_mutex_unlock(&clients_mutex);
                        break;
                    }
                }
                pthread_mutex_unlock(&rooms_mutex);
                
                // Send response
                if (room_id != -1) {
                    RoomCreatedMessage createdMsg;
                    createdMsg.base.type = MSG_ROOM_CREATED;
                    createdMsg.base.client_id = 0;
                    createdMsg.base.data_len = sizeof(RoomCreatedMessage) - sizeof(BaseMessage);
                    createdMsg.room_id = room_id;
                    strcpy(createdMsg.room_name, rooms[room_id].name);
                    strcpy(createdMsg.nickname, req->nickname);
                    createdMsg.num_players = rooms[room_id].client_count;
                    send(clients[client_id].socket_fd, &createdMsg, sizeof(RoomCreatedMessage), 0);
                    printf("Client %d created room %d: %s\n", client_id, room_id, rooms[room_id].name);
                } else {
                    // Send error message
                    BaseMessage errorMsg;
                    errorMsg.type = MSG_ERROR;
                    errorMsg.client_id = client_id;
                    errorMsg.data_len = 0;
                    send(clients[client_id].socket_fd, &errorMsg, sizeof(BaseMessage), 0);
                }
                break;
            }

            case MSG_JOIN_ROOM: {
                JoinRoomMessage* req = (JoinRoomMessage*)msg;
                pthread_mutex_lock(&rooms_mutex);
                int room_id = req->room_id;
                int success = 0;
                
                if (room_id >= 0 && room_id < MAX_ROOMS && rooms[room_id].name[0] != '\0') {
                    if (rooms[room_id].client_count < MAX_CLIENTS) {
                        // Find empty slot in room
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (rooms[room_id].clients[i].socket_fd == -1) {
                                pthread_mutex_lock(&clients_mutex);
                                strcpy(clients[client_id].nickname, req->nickname);
                                clients[client_id].room_id = room_id; // Set room_id
                                // Copy client info to room
                                rooms[room_id].clients[i] = clients[client_id];
                                rooms[room_id].clients[i].id = client_id; // Ensure ID is set
                                rooms[room_id].client_count++;
                                rooms[room_id].game.total_clients++; // Update total_clients
                                pthread_mutex_unlock(&clients_mutex);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&rooms_mutex);
                
                // Send response
                if (success) {
                    RoomJoinedMessage joinedMsg;
                    joinedMsg.base.type = MSG_ROOM_JOINED;
                    joinedMsg.base.client_id = 0;
                    joinedMsg.base.data_len = sizeof(RoomJoinedMessage) - sizeof(BaseMessage);
                    joinedMsg.room_id = room_id;
                    strcpy(joinedMsg.room_name, rooms[room_id].name);
                    strcpy(joinedMsg.nickname, req->nickname);
                    joinedMsg.num_players = rooms[room_id].client_count;
                    send(clients[client_id].socket_fd, &joinedMsg, sizeof(RoomJoinedMessage), 0);
                    printf("Client %d joined room %d: %s\n", client_id, room_id, rooms[room_id].name);
                } else {
                    // Send error message
                    BaseMessage errorMsg;
                    errorMsg.type = MSG_ERROR;
                    errorMsg.client_id = client_id;
                    errorMsg.data_len = 0;
                    send(clients[client_id].socket_fd, &errorMsg, sizeof(BaseMessage), 0);
                }
                break;
            }

            case MSG_LEAVE_ROOM: {
                LeaveRoomMessage* req = (LeaveRoomMessage*)msg;
                pthread_mutex_lock(&rooms_mutex);
                int room_id = req->room_id;
                
                if (room_id >= 0 && room_id < MAX_ROOMS) {
                    // Find and remove client from room
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (rooms[room_id].clients[i].socket_fd == clients[client_id].socket_fd) {
                            // If client was ready, decrease ready_count
                            if (rooms[room_id].clients[i].ready) {
                                rooms[room_id].game.ready_count--;
                            }
                            rooms[room_id].clients[i].socket_fd = -1;
                            rooms[room_id].clients[i].ready = 0;
                            rooms[room_id].client_count--;
                            rooms[room_id].game.total_clients--; // Update total_clients
                            // If room is empty, clear room name
                            if (rooms[room_id].client_count == 0) {
                                memset(rooms[room_id].name, 0, sizeof(rooms[room_id].name));
                            }
                            break;
                        }
                    }
                }
                pthread_mutex_unlock(&rooms_mutex);
                
                // Update global client
                pthread_mutex_lock(&clients_mutex);
                clients[client_id].room_id = -1;
                clients[client_id].ready = 0;
                pthread_mutex_unlock(&clients_mutex);
                
                // Send response
                RoomLeftMessage leftMsg;
                leftMsg.base.type = MSG_ROOM_LEFT;
                leftMsg.base.client_id = 0;
                leftMsg.base.data_len = sizeof(RoomLeftMessage) - sizeof(BaseMessage);
                leftMsg.room_id = room_id;
                send(clients[client_id].socket_fd, &leftMsg, sizeof(RoomLeftMessage), 0);
                printf("Client %d left room %d (total_clients now: %d)\n", client_id, room_id, rooms[room_id].game.total_clients);
                break;
            }
        }
    }
    
    return NULL;
}

void* handle_udp_server(void* arg) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (running) {
        int bytes_received = recvfrom(udp_socket, buffer, BUFFER_SIZE, 0, 
                                    (struct sockaddr*)&client_addr, &client_len);
        
        if (bytes_received > 0) {
            PaintDataMessage* paint_msg = (PaintDataMessage*)buffer;
            uint8_t cid = paint_msg->base.client_id;
            
            int room_id = -1;
            if (cid < MAX_CLIENTS) {
                // Update UDP info
                pthread_mutex_lock(&clients_mutex);
                clients[cid].udp_addr = client_addr;
                clients[cid].has_udp_addr = 1;
                room_id = clients[cid].room_id; // Get room_id
                
                // Also update UDP in room clients if in a room
                if (room_id != -1) {
                    pthread_mutex_lock(&rooms_mutex);
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (rooms[room_id].clients[i].id == cid) {
                            rooms[room_id].clients[i].udp_addr = client_addr;
                            rooms[room_id].clients[i].has_udp_addr = 1;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&rooms_mutex);
                }
                pthread_mutex_unlock(&clients_mutex);
            }

            if (room_id != -1) {
                pthread_mutex_lock(&rooms_mutex);
                // Verify painter
            if (paint_msg->base.client_id == rooms[room_id].game.painter_id && 
               (rooms[room_id].game.state == GAME_PAINTING || paint_msg->action == 3)) {
                
                // Save drawing data to DB and history for AI
                if (rooms[room_id].game.state == GAME_PAINTING) {
                    // Store in drawing_history for AI inference
                    if (rooms[room_id].history_count < MAX_DRAWING_POINTS) {
                        rooms[room_id].drawing_history[rooms[room_id].history_count].x = paint_msg->x;
                        rooms[room_id].drawing_history[rooms[room_id].history_count].y = paint_msg->y;
                        rooms[room_id].drawing_history[rooms[room_id].history_count].action = paint_msg->action;
                        rooms[room_id].history_count++;
                    }
                    
                    char sql_insert[256];
                    sprintf(sql_insert, "INSERT INTO drawing_data (game_id, x, y, action, color_r, color_g, color_b, timestamp) VALUES (%d, %d, %d, %d, %d, %d, %d, %ld);",
                            rooms[room_id].game.current_game_id, 
                            paint_msg->x, paint_msg->y, paint_msg->action,
                            paint_msg->color_r, paint_msg->color_g, paint_msg->color_b,
                            time(NULL));
                    char *err_msg = 0;
                    // Execute async or just fire and forget for speed? 
                    // Exec is blocking. For high frequency UDP, this might be slow.
                    // But let's try.
                    sqlite3_exec(db, sql_insert, 0, 0, &err_msg);
                    if (err_msg) sqlite3_free(err_msg);
                }

                // Broadcast to room clients
                for (int i = 0; i < MAX_CLIENTS; i++) {
                        // Forward to everyone except sender (painter)
                        if (rooms[room_id].clients[i].socket_fd != -1 && 
                            rooms[room_id].clients[i].id != cid && 
                            rooms[room_id].clients[i].has_udp_addr) {
                        sendto(udp_socket, buffer, bytes_received, 0,
                                   (struct sockaddr*)&rooms[room_id].clients[i].udp_addr, 
                                   sizeof(rooms[room_id].clients[i].udp_addr));
                        }
                    }
                }
                pthread_mutex_unlock(&rooms_mutex);
            }
        }
    }
    
    return NULL;
}

//game timer thread
void* game_timer(void* arg) {
    while (running) {
        sleep(1);// check 1 time per 1 second
        
        pthread_mutex_lock(&rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; i++) {
            GameInfo* game = &rooms[i].game;
        
            if (game->state == GAME_PAINTING) {//60s
                time_t elapsed = time(NULL) - game->paint_start_time;
                if (elapsed >= 60) {
                    game->state = GAME_GUESSING;
                    game->guess_start_time = time(NULL);
                    printf("Room %d Painting time over, entering guessing phase\n", i);
                    
                    // Trigger AI
                    int* arg = malloc(sizeof(int));
                    *arg = i;
                    pthread_t ai_thread;
                    pthread_create(&ai_thread, NULL, ai_guess_thread, arg);
                    pthread_detach(ai_thread);
                    
                    // Notify clients? (Optional, clients have own timer)
                    // But state transition needs to be broadcasted if client relies on server for phase change
                    // Currently client relies on timer mostly, but MSG_PAINTER_FINISH triggers phase change too.
                    // We should probably broadcast a message here to ensure sync.
                    // Reusing MSG_PAINTER_FINISH for timeout
                    BaseMessage finish_msg;
                    finish_msg.type = MSG_PAINTER_FINISH;
                    finish_msg.client_id = 0;
                    finish_msg.data_len = 0;
                    
                    // Temporarily unlock to broadcast
                    pthread_mutex_unlock(&rooms_mutex);
                    broadcast_message(&finish_msg, -1, i);
                    pthread_mutex_lock(&rooms_mutex);
            }
            } else if (game->state == GAME_GUESSING) {//30s
                time_t elapsed = time(NULL) - game->guess_start_time;
                if (elapsed >= 30) {
                    // Temporarily unlock to end game
                    pthread_mutex_unlock(&rooms_mutex);
                    end_game(i);
                    pthread_mutex_lock(&rooms_mutex);
            }
        }
        }
        pthread_mutex_unlock(&rooms_mutex);
    }
    
    return NULL;
}

void cleanup() {
    pthread_mutex_lock(&clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket_fd != -1) {
            close(clients[i].socket_fd);
            clients[i].socket_fd = -1;
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
    
    if (tcp_socket != -1) {
        close(tcp_socket);
    }
    
    if (udp_socket != -1) {
        close(udp_socket);
    }
    
    if (db) {
        sqlite3_close(db);
    }
}

// Fully define init_rooms before main
void init_rooms() {
    for (int i = 0; i < MAX_ROOMS; i++) {
        rooms[i].id = i;
        rooms[i].client_count = 0;
        memset(rooms[i].name, 0, sizeof(rooms[i].name));
        for (int j = 0; j < MAX_CLIENTS; j++) {
            rooms[i].clients[j].socket_fd = -1;
        }
        init_game(&rooms[i].game);  // Assuming init_game takes GameInfo*
        rooms[i].history_count = 0;
        rooms[i].ai_result_ready = 0;
        memset(rooms[i].ai_predicted_word, 0, sizeof(rooms[i].ai_predicted_word));
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket_fd = -1;
    }
    
    init_rooms(); // Initialize rooms
    init_db();
    // init_game(); // Removed global game init
    
    //TCP
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket == -1) {
        perror("Failed to create TCP socket");
        return 1;
    }
    
    //UDP
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket == -1) {
        perror("Failed to create UDP socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Failed to bind TCP socket");
        return 1;
    }
    
    if (bind(udp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Failed to bind UDP socket");
        return 1;
    }
    
    
    if (listen(tcp_socket, 10) == -1) {
        perror("Failed to listen on TCP socket");
        return 1;
    }
    
    printf("Listening on port %d\n", SERVER_PORT);
    
    // Start AI service
    printf("Starting AI service...\n");
    #ifdef _WIN32
    system("start /B python ai_service.py");
    #else
    system("python ai_service.py &");
    #endif
    sleep(2); // Give AI service time to start
    
    pthread_t udp_thread;
    pthread_create(&udp_thread, NULL, handle_udp_server, NULL);
    
    

    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, game_timer, NULL);
    

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(tcp_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket == -1) {
            if (running) {
                perror("Accept connection error");
            }
            continue;
        }
        
        printf("Client connected: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        int client_id = add_client(client_socket);
        
        if (client_id != -1) {
            pthread_t client_thread;
            pthread_create(&client_thread, NULL, handle_tcp_client, &client_id);
            pthread_detach(client_thread);
        } else {

            printf("Client limit reached\n");
            close(client_socket);
        }
    }
    
    cleanup();
    return 0;
}
