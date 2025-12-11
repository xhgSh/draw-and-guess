# Draw and Guess / 你画我猜

<details>
<summary><b>中文 / Chinese</b> (点击展开 / Click to expand)</summary>

启动服务器和客户端
每个客户端输入昵称并点击连接
所有客户端点击Ready
开始游戏！

## Server / 服务器

server是一个multi thread服务器
使用Cygwin进行Socket编程，protocol.h 储存了消息类型和结构体，protocol.c储存了题目，服务器main在draw_guess_server.c中

**集成了SQLite3数据库**：
- `words` 表：存储题目库，服务器启动时自动初始化
- `history` 表：存储游戏战绩（游戏ID、题目、用户名、猜测、时间）

过程如下
1. **初始化数据库**，加载题目库
2. 创建TCP Socket，监听1234端口，用于连接客户端，发布猜题，同步游戏过程，**查询历史战绩**
3. 创建UDP Socket，监听1234端口，用于传画布数据
4. 创建pthread线程：
   - 主线程
   - TCP客户端处理线程
   - UDP服务器监听线程
   - 游戏计时器线程

### 游戏状态：               
- WAITING: 等待玩家
- READY: 准备阶段
- PAINTING: 绘画阶段（60秒）
- GUESSING: 猜测阶段（30秒）
- FINISHED: 游戏结束

### 消息流程
- **TCP**：
  - `MSG_CLIENT_JOIN`: 客户端加入
  - `MSG_CLIENT_READY`: 客户端准备
  - `MSG_GAME_START`: 游戏开始（服务器发送）
  - `MSG_PAINTER_FINISH`: 画手结束绘画
  - `MSG_GUESS_SUBMIT`: 提交猜测
  - `MSG_GAME_END`: 游戏结束（服务器发送）
  - `MSG_ERROR`: 错误消息
  - `MSG_HISTORY_REQ`: 请求历史战绩
  - `MSG_HISTORY_DATA`: 发送历史数据
  - `MSG_HISTORY_END`: 历史数据发送完毕
  - `MSG_ROOM_LIST_REQ`: 请求房间列表
  - `MSG_ROOM_LIST`: 房间列表
  - `MSG_CREATE_ROOM`: 创建房间
  - `MSG_JOIN_ROOM`: 加入房间
  - `MSG_LEAVE_ROOM`: 离开房间
  - `MSG_AI_GUESS_RESULT`: AI预测结果

- **UDP**：
  - `MSG_PAINT_DATA`: 绘画数据（坐标、动作、颜色）
  - 服务器负责转发给其他客户端

使用 `pthread_mutex` 保护客户端列表、游戏状态等共享数据
多线程处理多个客户端连接
UDP转发使用互斥锁确保线程安全

**AI功能**：
- 集成CLIP模型进行图像-文本匹配
- 对绘画进行相似度评分
- AI自动猜测最可能的词
- 在所有客户端提交猜测后显示AI结果

## Client / 客户端

client是由Qt创建的一个桌面application，使用Qt Socket实现网络通信

**功能**：
- History按钮：查询个人历史战绩（连接服务器后可用）
- 战绩显示：Game ID, Word, Your Guess, Time
- 房间列表：创建或加入房间
- 颜色选择：多种画笔颜色（黑、红、蓝、绿、黄、紫、青）
- AI预测显示：显示AI猜测结果和相似度分数

QTcpSocket: 发送/接收控制消息
QUdpSocket: 发送/接收绘画数据
QTimer(50ms): UDP节流定时器
QTimer(1s): 游戏倒计时定时器

main：
UI:MainWindow窗口和DrawingWidget画板
信号槽机制处理事件

**画手端：**
```
用户绘制 → DrawingWidget::mousePressEvent()
         → emit paintDataGenerated()
         → MainWindow::onPaintDataGenerated()
         → 加入pendingPaintQueue队列
         → QTimer(50ms)触发
         → flushUdpQueue()
         → sendUdpMessage()
         → UDP发送到服务器
         → 服务器转发给其他客户端
```

**猜测者端：**
```
服务器转发UDP数据
         → MainWindow::onUdpDataReceived()
         → handleUdpMessage()
         → DrawingWidget::addPaintData()
         → 更新画布
         → update()触发重绘
```

## 游戏流程

1. 客户端连接（TCP）
   ↓
2. 发送MSG_CLIENT_JOIN（昵称）
   ↓
3. 选择或创建房间
   ↓
4. 所有客户端点击Ready
   ↓
5. 服务器发送MSG_GAME_START
   - 随机选择画手
   - 分配题目（如"apple"）
   ↓
6. 绘画阶段（60秒）
   - 画手：可以绘制，显示"Finish Drawing"按钮，可选择画笔颜色
   - 猜测者：观看画板（UDP实时同步）
   - 画手可提前点击"Finish Drawing"结束
   ↓
7. 猜测阶段（30秒）
   - 画手：禁用输入框和按钮
   - 猜测者：输入猜测并提交
   - 所有猜测者提交或30秒后结束
   ↓
8. 服务器发送MSG_GAME_END
   - 显示正确答案
   - 显示获胜者（如果有）
   - 显示AI预测结果和相似度分数
   ↓
9. 回到Ready阶段，可再次开始
   - 此时可点击 "History" 查看刚刚的战绩记录

</details>

<details>
<summary><b>English</b> (Click to expand)</summary>

Start server and clients
Each client enters nickname and clicks connect
All clients click Ready
Start the game!

## Server

Server is a multi-threaded server
Uses Cygwin for Socket programming, protocol.h stores message types and structures, protocol.c stores word bank, server main is in draw_guess_server.c

**Integrated SQLite3 Database**:
- `words` table: Stores word bank, automatically initialized on server startup
- `history` table: Stores game history (game ID, word, username, guess, time)

Process as follows:
1. **Initialize database**, load word bank
2. Create TCP Socket, listen on port 1234, for client connections, word distribution, game state synchronization, **history queries**
3. Create UDP Socket, listen on port 1234, for canvas data transmission
4. Create pthread threads:
   - Main thread
   - TCP client handler thread
   - UDP server listener thread
   - Game timer thread

### Game States:               
- WAITING: Waiting for players
- READY: Ready phase
- PAINTING: Painting phase (60 seconds)
- GUESSING: Guessing phase (30 seconds)
- FINISHED: Game finished

### Message Flow
- **TCP**:
  - `MSG_CLIENT_JOIN`: Client joins
  - `MSG_CLIENT_READY`: Client ready
  - `MSG_GAME_START`: Game start (sent by server)
  - `MSG_PAINTER_FINISH`: Painter finishes drawing
  - `MSG_GUESS_SUBMIT`: Submit guess
  - `MSG_GAME_END`: Game end (sent by server)
  - `MSG_ERROR`: Error message
  - `MSG_HISTORY_REQ`: Request history
  - `MSG_HISTORY_DATA`: Send history data
  - `MSG_HISTORY_END`: History data sent
  - `MSG_ROOM_LIST_REQ`: Request room list
  - `MSG_ROOM_LIST`: Room list
  - `MSG_CREATE_ROOM`: Create room
  - `MSG_JOIN_ROOM`: Join room
  - `MSG_LEAVE_ROOM`: Leave room
  - `MSG_AI_GUESS_RESULT`: AI prediction result

- **UDP**:
  - `MSG_PAINT_DATA`: Painting data (coordinates, action, color)
  - Server forwards to other clients

Uses `pthread_mutex` to protect shared data like client list and game state
Multi-threaded handling of multiple client connections
UDP forwarding uses mutex locks to ensure thread safety

**AI Features**:
- Integrated CLIP model for image-text matching
- Score drawing similarity
- AI automatically guesses most likely word
- Display AI results after all clients submit guesses

## Client

Client is a Qt desktop application using Qt Socket for network communication

**Features**:
- History button: Query personal history (available after connecting to server)
- History display: Game ID, Word, Your Guess, Time
- Room list: Create or join rooms
- Color selection: Multiple brush colors (black, red, blue, green, yellow, purple, cyan)
- AI prediction display: Shows AI guess result and similarity score

QTcpSocket: Send/receive control messages
QUdpSocket: Send/receive painting data
QTimer(50ms): UDP throttling timer
QTimer(1s): Game countdown timer

main:
UI: MainWindow window and DrawingWidget canvas
Signal-slot mechanism for event handling

**Painter Side:**
```
User draws → DrawingWidget::mousePressEvent()
         → emit paintDataGenerated()
         → MainWindow::onPaintDataGenerated()
         → Add to pendingPaintQueue
         → QTimer(50ms) triggers
         → flushUdpQueue()
         → sendUdpMessage()
         → UDP send to server
         → Server forwards to other clients
```

**Guesser Side:**
```
Server forwards UDP data
         → MainWindow::onUdpDataReceived()
         → handleUdpMessage()
         → DrawingWidget::addPaintData()
         → Update canvas
         → update() triggers redraw
```

## Game Flow

1. Client connects (TCP)
   ↓
2. Send MSG_CLIENT_JOIN (nickname)
   ↓
3. Select or create room
   ↓
4. All clients click Ready
   ↓
5. Server sends MSG_GAME_START
   - Randomly select painter
   - Assign word (e.g., "apple")
   ↓
6. Painting phase (60 seconds)
   - Painter: Can draw, shows "Finish Drawing" button, can select brush color
   - Guessers: Watch canvas (UDP real-time sync)
   - Painter can click "Finish Drawing" early to end
   ↓
7. Guessing phase (30 seconds)
   - Painter: Input and buttons disabled
   - Guessers: Enter guess and submit
   - All guessers submit or 30 seconds timeout
   ↓
8. Server sends MSG_GAME_END
   - Display correct answer
   - Display winner (if any)
   - Display AI prediction result and similarity score
   ↓
9. Return to Ready phase, can start again
   - Can click "History" to view recent game records

</details>
