#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QWidget>
#include <QThread>
#include <QMutex>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialog>
#include <QVBoxLayout>
#include <QListWidget>
#include <QInputDialog>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// Protocol definitions
typedef enum {
    MSG_CLIENT_JOIN = 1,
    MSG_CLIENT_READY = 2,
    MSG_GAME_START = 3,
    MSG_PAINT_DATA = 4,
    MSG_GUESS_SUBMIT = 5,
    MSG_GAME_END = 6,
    MSG_CLIENT_LEAVE = 7,
    MSG_ERROR = 8,
    MSG_PAINTER_FINISH = 9,
    MSG_HISTORY_REQ = 10,
    MSG_HISTORY_DATA = 11,
    MSG_HISTORY_END = 12,
    MSG_ROOM_LIST_REQ = 13,
    MSG_ROOM_LIST = 14,
    MSG_CREATE_ROOM = 15,
    MSG_JOIN_ROOM = 16,
    MSG_LEAVE_ROOM = 17,
    MSG_ROOM_CREATED = 18,
    MSG_ROOM_JOINED = 19,
    MSG_ROOM_LEFT = 20,
    MSG_AI_GUESS_REQ = 21,
    MSG_AI_GUESS_RESULT = 22
} MessageType;

typedef enum {
    GAME_WAITING = 0,
    GAME_READY = 1,
    GAME_PAINTING = 2,
    GAME_GUESSING = 3,
    GAME_FINISHED = 4
} GameState;

typedef struct {
    uint8_t type;
    uint8_t client_id;
    uint16_t data_len;
} BaseMessage;

typedef struct {
    BaseMessage base;
    char nickname[32];
} ClientJoinMessage;

typedef struct {
    BaseMessage base;
    uint8_t painter_id;
    char word[32];
    uint32_t paint_time;
} GameStartMessage;

typedef struct {
    BaseMessage base;
    uint16_t x;
    uint16_t y;
    uint8_t action;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
} PaintDataMessage;

typedef struct {
    BaseMessage base;
    char guess[64];
} GuessSubmitMessage;

typedef struct {
    BaseMessage base;
    char correct_word[32];
    uint8_t winner_id;
    uint8_t guess_count;
} GameEndMessage;

typedef struct {
    BaseMessage base;
} HistoryRequestMessage;

typedef struct {
    BaseMessage base;
    int game_id;
    char word[32];
    char user_guess[64];
    char game_time[32];
} HistoryDataMessage;

// Room-related structs
typedef struct {
    BaseMessage base;
} RoomListRequestMessage;

typedef struct {
    uint8_t room_id;
    char name[32];
    uint8_t num_players;
} RoomInfo;

typedef struct {
    BaseMessage base;
    uint8_t num_rooms;
    RoomInfo rooms[10]; // Max 10 rooms
} RoomListMessage;

typedef struct {
    BaseMessage base;
    char room_name[32];
    char nickname[32];
} CreateRoomMessage;

typedef struct {
    BaseMessage base;
    uint8_t room_id;
    char nickname[32];
} JoinRoomMessage;

typedef struct {
    BaseMessage base;
    uint8_t room_id;
} LeaveRoomMessage;

typedef struct {
    BaseMessage base;
    uint8_t room_id;
    char room_name[32];
    char nickname[32];
    uint8_t num_players;
} RoomCreatedMessage;

typedef struct {
    BaseMessage base;
    uint8_t room_id;
    char room_name[32];
    char nickname[32];
    uint8_t num_players;
} RoomJoinedMessage;

typedef struct {
    BaseMessage base;
    uint8_t room_id;
} RoomLeftMessage;

typedef struct {
    BaseMessage base;
} AiGuessRequestMessage;

typedef struct {
    BaseMessage base;
    char predicted_word[32];
    uint8_t score;
    uint8_t is_correct;
} AiGuessResultMessage;

// Custom drawing widget
class DrawingWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DrawingWidget(QWidget *parent = nullptr);
    void clearCanvas();
    void setPaintingEnabled(bool enabled);
    void addPaintData(const PaintDataMessage& data);
    QColor getCurrentColor() const { return currentColor; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

signals:
    void paintDataGenerated(const PaintDataMessage& data);

private:
    QPixmap canvas;
    bool painting;
    bool isPainting;
    QColor currentColor;
    int brushSize;
    QPoint lastPoint;
    
public:
    void setCurrentColor(const QColor& color) { currentColor = color; }
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void connectToServer();
    void sendReady();
    void submitGuess();
    void onTcpDataReceived();
    void onUdpDataReceived();
    void updateTimer();
    void clearCanvas();
    void onPaintDataGenerated(const PaintDataMessage& data);
    void flushUdpQueue();
    void requestHistory();
    void showHistoryDialog();
    void onColorButtonClicked();
    void showRoomList();
    void leaveRoom();

private:
    Ui::MainWindow *ui;
    
    // Network related
    QTcpSocket *tcpSocket;
    QUdpSocket *udpSocket;
    QString serverHost;
    int serverPort;
    int clientId;
    bool connected;
    
    // Game state
    GameState gameState;
    bool isPainter;
    QString currentWord;
    QString nickname;
    int remainingTime;
    int currentRoomId;
    
    // Timers
    QTimer *gameTimer;
    QTimer *udpFlushTimer;
    
    // Drawing widget
    DrawingWidget *drawingWidget;
    QVector<PaintDataMessage> pendingPaintQueue; // UDP send queue
    
    // History
    struct HistoryRecord {
        int game_id;
        QString word;
        QString user_guess;
        QString game_time;
    };
    QVector<HistoryRecord> historyRecords;
    
    // Helper functions
    void sendTcpMessage(const BaseMessage& msg);
    void sendUdpMessage(const BaseMessage& msg);
    void handleTcpMessage(const BaseMessage& msg);
    void handleUdpMessage(const BaseMessage& msg);
    void updateGameState(GameState state);
    void updateUI();
    void addChatMessage(const QString& message);
    void updateIdentityDisplay();
};

#endif
