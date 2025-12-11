#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QApplication>
#include <QMessageBox>
#include <QHostAddress>
#include <QByteArray>
#include <QDebug>
#include <QTime>

// DrawingWidget implementation
DrawingWidget::DrawingWidget(QWidget *parent)
    : QWidget(parent), painting(false), isPainting(false), brushSize(3)
{
    //adapt to parent container
    canvas = QPixmap(size());
    canvas.fill(Qt::white);
    currentColor = Qt::black; // Default black brush
}

void DrawingWidget::clearCanvas()
{
    canvas.fill(Qt::white);
    update();
}

void DrawingWidget::setPaintingEnabled(bool enabled)
{
    painting = enabled;
}

void DrawingWidget::addPaintData(const PaintDataMessage& data)
{
    QPainter painter(&canvas);
    QColor color(data.color_r, data.color_g, data.color_b);
    painter.setPen(QPen(color, 3));
    painter.setRenderHint(QPainter::Antialiasing);
    
    if (data.action == 1) { // Press
        painter.drawPoint(data.x, data.y);
        lastPoint = QPoint(data.x, data.y);
    } else if (data.action == 2) { // Move
        if (!lastPoint.isNull()) {
            painter.drawLine(lastPoint, QPoint(data.x, data.y));
        }
        lastPoint = QPoint(data.x, data.y);
    }
    
    update();
}

void DrawingWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.drawPixmap(0, 0, canvas);
}

void DrawingWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Recreate canvas when widget size changes
    canvas = QPixmap(size());
    canvas.fill(Qt::white);
}

void DrawingWidget::mousePressEvent(QMouseEvent *event)
{
    if (!painting) return;
    
    isPainting = true;
    lastPoint = event->pos();
    
    // Draw directly on canvas
    QPainter painter(&canvas);
    painter.setPen(QPen(currentColor, 3));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawPoint(event->pos());
    update();
    
    // Send paint data
    PaintDataMessage data;
    data.base.type = MSG_PAINT_DATA;
    data.base.client_id = 0; // Will be set in MainWindow
    data.base.data_len = sizeof(PaintDataMessage) - sizeof(BaseMessage);
    data.x = event->position().x();
    data.y = event->position().y();
    data.action = 1; // Press
    
    emit paintDataGenerated(data);
}

void DrawingWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!painting || !isPainting) return;
    
    // Draw directly on canvas
    QPainter painter(&canvas);
    painter.setPen(QPen(currentColor, 3));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawLine(lastPoint, event->pos());
    update();
    
    // Send paint data
    PaintDataMessage data;
    data.base.type = MSG_PAINT_DATA;
    data.base.client_id = 0;
    data.base.data_len = sizeof(PaintDataMessage) - sizeof(BaseMessage);
    data.x = event->position().x();
    data.y = event->position().y();
    data.action = 2; // Move
    
    emit paintDataGenerated(data);
    
    lastPoint = event->pos();
}

void DrawingWidget::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    if (!painting) return;
    
    isPainting = false;
}

// MainWindow implementation
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , tcpSocket(nullptr)
    , udpSocket(nullptr)
    , serverHost("127.0.0.1")
    , serverPort(1234)
    , clientId(-1)
    , connected(false)
    , gameState(GAME_WAITING)
    , isPainter(false)
    , remainingTime(0)
    , currentRoomId(-1)
    , gameTimer(new QTimer(this))
    , drawingWidget(nullptr)
    , udpFlushTimer(new QTimer(this))
{
    ui->setupUi(this);
    
    // Create drawing widget
    drawingWidget = new DrawingWidget(this);
    // Add custom drawing widget to UI's drawingWidget container
    QVBoxLayout* drawingLayout = new QVBoxLayout(ui->drawingWidget);
    drawingLayout->setContentsMargins(0, 0, 0, 0);
    drawingLayout->addWidget(drawingWidget);
    // Hide brush size controls
    if (ui->brushLabel) ui->brushLabel->hide();
    if (ui->brushSlider) ui->brushSlider->hide();
    
    // Connect signals and slots (connectButton removed, auto-connect on startup)
    connect(ui->readyButton, &QPushButton::clicked, this, &MainWindow::sendReady);
    connect(ui->submitButton, &QPushButton::clicked, this, &MainWindow::submitGuess);
    connect(ui->clearButton, &QPushButton::clicked, this, &MainWindow::clearCanvas);
    connect(drawingWidget, &DrawingWidget::paintDataGenerated, this, &MainWindow::onPaintDataGenerated);
    connect(gameTimer, &QTimer::timeout, this, &MainWindow::updateTimer);
    // 50ms throttle for UDP paint data
    connect(udpFlushTimer, &QTimer::timeout, this, &MainWindow::flushUdpQueue);
    udpFlushTimer->start(50);
    
    // Connect History button
    connect(ui->historyButton, &QPushButton::clicked, this, &MainWindow::requestHistory);
    
    // Connect color selection buttons
    connect(ui->colorButtonBlack, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonRed, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonBlue, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonGreen, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonYellow, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonPurple, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    connect(ui->colorButtonCyan, &QPushButton::clicked, this, &MainWindow::onColorButtonClicked);
    // Set default color (black)
    drawingWidget->setCurrentColor(Qt::black);
    // Highlight default color button
    ui->colorButtonBlack->setStyleSheet("QPushButton { background-color: #000000; border: 4px solid #000; border-radius: 20px; }");

    // Initialize UI state
    updateUI();

    // Automatic connection
    connectToServer();

    // Connect new buttons
    connect(ui->roomListButton, &QPushButton::clicked, this, &MainWindow::showRoomList);
    connect(ui->leaveRoomButton, &QPushButton::clicked, this, &MainWindow::leaveRoom);
}

void MainWindow::requestHistory()
{
    if (!connected) return;
    
    HistoryRequestMessage msg;
    msg.base.type = MSG_HISTORY_REQ;
    msg.base.client_id = clientId;
    msg.base.data_len = 0;
    
    sendTcpMessage(msg.base);
    historyRecords.clear();
    addChatMessage("Requesting history...");
}

void MainWindow::showHistoryDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Game History");
    dialog.resize(600, 400);
    
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QTableWidget *table = new QTableWidget(&dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({"Game ID", "Word", "Your Guess", "Time"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); // Make read-only
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    
    table->setRowCount(historyRecords.size());
    for (int i = 0; i < historyRecords.size(); ++i) {
        const auto& record = historyRecords[i];
        table->setItem(i, 0, new QTableWidgetItem(QString::number(record.game_id)));
        table->setItem(i, 1, new QTableWidgetItem(record.word));
        table->setItem(i, 2, new QTableWidgetItem(record.user_guess));
        table->setItem(i, 3, new QTableWidgetItem(record.game_time));
    }
    
    layout->addWidget(table);
    dialog.exec();
}

MainWindow::~MainWindow()
{
    if (tcpSocket) {
        tcpSocket->disconnectFromHost();
        delete tcpSocket;
    }
    if (udpSocket) {
        delete udpSocket;
    }
    delete ui;
}

void MainWindow::connectToServer()
{
    // Auto-connect without nickname (nickname will be set when joining/creating room)
    
    // Create TCP socket
    tcpSocket = new QTcpSocket(this);
    connect(tcpSocket, &QTcpSocket::connected, this, [this]() {
        connected = true;
        ui->statusLabel->setText("Connected");
        ui->roomListButton->setEnabled(true);
        ui->historyButton->setEnabled(true);
        addChatMessage("Connected to server");
        updateIdentityDisplay();
        
        // Send join message
        ClientJoinMessage joinMsg;
        joinMsg.base.type = MSG_CLIENT_JOIN;
        joinMsg.base.client_id = 0;
        joinMsg.base.data_len = sizeof(ClientJoinMessage) - sizeof(BaseMessage);
        strcpy(joinMsg.nickname, nickname.toUtf8().constData());
        
        sendTcpMessage(joinMsg.base);
    });
    
    connect(tcpSocket, &QTcpSocket::disconnected, this, [this]() {
        connected = false;
        ui->statusLabel->setText("Disconnected");
        ui->roomListButton->setEnabled(false);
        ui->readyButton->setEnabled(false);
        ui->historyButton->setEnabled(false);
        ui->leaveRoomButton->setEnabled(false);
        addChatMessage("Disconnected from server");
    });
    
    connect(tcpSocket, &QTcpSocket::readyRead, this, &MainWindow::onTcpDataReceived);
    
    // Create UDP socket
    udpSocket = new QUdpSocket(this);
    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::onUdpDataReceived);
    
    // Connect to server
    tcpSocket->connectToHost(serverHost, serverPort);
}

void MainWindow::sendReady()
{
    if (!connected) return;
    
    BaseMessage msg;
    msg.type = MSG_CLIENT_READY;
    msg.client_id = clientId;
    msg.data_len = 0;
    
    sendTcpMessage(msg);
    ui->readyButton->setEnabled(false);
    addChatMessage("Ready sent");
}

void MainWindow::submitGuess()
{
    if (!connected) return;
    
    // If painter in painting phase, send finish painting message
    if (isPainter && gameState == GAME_PAINTING) {
        BaseMessage finishMsg;
        finishMsg.type = MSG_PAINTER_FINISH;
        finishMsg.client_id = clientId;
        finishMsg.data_len = 0;
        
        sendTcpMessage(finishMsg);
        ui->submitButton->setEnabled(false);
        addChatMessage("Painting finished, entering guessing phase");
        return;
    }
    
    // If in guessing phase, submit guess
    if (gameState != GAME_GUESSING) return;
    
    QString guess = ui->guessEdit->text().trimmed();
    if (guess.isEmpty()) return;
    
    GuessSubmitMessage guessMsg;
    guessMsg.base.type = MSG_GUESS_SUBMIT;
    guessMsg.base.client_id = clientId;
    guessMsg.base.data_len = sizeof(GuessSubmitMessage) - sizeof(BaseMessage);
    strcpy(guessMsg.guess, guess.toUtf8().constData());
    
    sendTcpMessage(guessMsg.base);
    ui->submitButton->setEnabled(false);
    ui->guessEdit->setEnabled(false);
    addChatMessage(QString("Submit guess: %1").arg(guess));
}

void MainWindow::onTcpDataReceived()
{
    while (tcpSocket->bytesAvailable() >= static_cast<qint64>(sizeof(BaseMessage))) {
        QByteArray data = tcpSocket->read(sizeof(BaseMessage));
        BaseMessage* msg = (BaseMessage*)data.data();
        
        if (tcpSocket->bytesAvailable() >= msg->data_len) {
            QByteArray fullData = tcpSocket->read(msg->data_len);
            data.append(fullData);
            handleTcpMessage(*(BaseMessage*)data.data());
        }
    }
}

void MainWindow::onUdpDataReceived()
{
    while (udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(udpSocket->pendingDatagramSize());
        udpSocket->readDatagram(datagram.data(), datagram.size());
        
        BaseMessage* msg = (BaseMessage*)datagram.data();
        handleUdpMessage(*msg);
    }
}

void MainWindow::updateTimer()
{
    if (remainingTime > 0) {
        remainingTime--;
        int minutes = remainingTime / 60;
        int seconds = remainingTime % 60;
        ui->timerLabel->setText(QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0')));
    } else {
        gameTimer->stop();
        // Local phase advance: painting ends -> enter guessing phase
        if (gameState == GAME_PAINTING) {
            updateGameState(GAME_GUESSING);
        }
    }
}

void MainWindow::clearCanvas()
{
    drawingWidget->clearCanvas();
    // When painter clears, notify other clients
    if (isPainter && (gameState == GAME_PAINTING || gameState == GAME_GUESSING)) {
        PaintDataMessage msg;
        msg.base.type = MSG_PAINT_DATA;
        msg.base.client_id = (uint8_t)clientId;
        msg.base.data_len = sizeof(PaintDataMessage) - sizeof(BaseMessage);
        msg.x = 0; msg.y = 0;
        msg.action = 3; // Clear canvas
        // Send directly, don't wait for throttle
        sendUdpMessage(msg.base);
    }
}

void MainWindow::onPaintDataGenerated(const PaintDataMessage& data)
{
    if (isPainter && gameState == GAME_PAINTING) {
        PaintDataMessage sendData = data;
        sendData.base.client_id = clientId;
        sendData.color_r = drawingWidget->getCurrentColor().red();
        sendData.color_g = drawingWidget->getCurrentColor().green();
        sendData.color_b = drawingWidget->getCurrentColor().blue();
        pendingPaintQueue.append(sendData);
    }
}

void MainWindow::sendTcpMessage(const BaseMessage& msg)
{
    if (tcpSocket && tcpSocket->state() == QAbstractSocket::ConnectedState) {
        QByteArray data((char*)&msg, sizeof(BaseMessage) + msg.data_len);
        tcpSocket->write(data);
    }
}

void MainWindow::sendUdpMessage(const BaseMessage& msg)
{
    if (udpSocket) {
        QByteArray data((char*)&msg, sizeof(BaseMessage) + msg.data_len);
        udpSocket->writeDatagram(data, QHostAddress(serverHost), serverPort);
    }
}

void MainWindow::flushUdpQueue()
{
    if (!udpSocket || pendingPaintQueue.isEmpty()) return;
    // Send all paint data in queue (server already forwards directly)
    while (!pendingPaintQueue.isEmpty()) {
        PaintDataMessage msg = pendingPaintQueue.takeFirst();
        sendUdpMessage(msg.base);
    }
}

void MainWindow::handleTcpMessage(const BaseMessage& msg)
{
    switch (msg.type) {
        case MSG_GAME_START: {
            GameStartMessage* startMsg = (GameStartMessage*)&msg;
            clientId = msg.client_id;
            isPainter = (startMsg->painter_id == clientId);
            currentWord = QString::fromUtf8(startMsg->word);
            remainingTime = startMsg->paint_time;
            
            updateGameState(GAME_PAINTING);
            gameTimer->start(1000);
            
            if (isPainter) {
                addChatMessage(QString("You are the painter! Word: %1").arg(currentWord));
                drawingWidget->setPaintingEnabled(true);
                ui->clearButton->setEnabled(true);
                ui->submitButton->setEnabled(true);
                ui->submitButton->setText("Finish Drawing");
            } else {
                addChatMessage("Game started! Watch the canvas and guess");
                drawingWidget->setPaintingEnabled(false);
            }
            updateIdentityDisplay();

            // Send UDP registration packet once to ensure server records this client's UDP address
            PaintDataMessage reg;
            reg.base.type = MSG_PAINT_DATA;
            reg.base.client_id = (uint8_t)clientId;
            reg.base.data_len = sizeof(PaintDataMessage) - sizeof(BaseMessage);
            reg.x = 0; reg.y = 0; reg.action = 0; // Registration packet
            sendUdpMessage(reg.base);
            break;
        }
        
        case MSG_PAINTER_FINISH: {
            // Received painter finish painting message, switch to guessing phase
            if (gameState == GAME_PAINTING) {
                updateGameState(GAME_GUESSING);
                gameTimer->stop();
                if (isPainter) {
                    addChatMessage("Painting phase ended");
                    drawingWidget->setPaintingEnabled(false);
                    ui->submitButton->setEnabled(false);
                } else {
                    addChatMessage("Painting finished! Enter your guess");
                    ui->guessEdit->setEnabled(true);
                    ui->submitButton->setEnabled(true);
                    ui->submitButton->setText("Submit");
                }
                remainingTime = 30;
                gameTimer->start(1000);
                updateIdentityDisplay();
            }
            break;
        }
        
        case MSG_GAME_END: {
            GameEndMessage* endMsg = (GameEndMessage*)&msg;
            updateGameState(GAME_FINISHED);
            gameTimer->stop();
            
            // Display game result
            QString result = QString("Game over! Answer: %1").arg(QString::fromUtf8(endMsg->correct_word));
            if (endMsg->winner_id == clientId) {
                result += " - You guessed it! You win!";
            } else if (endMsg->winner_id != 255) {
                result += QString(" - Player %1 guessed it!").arg(endMsg->winner_id);
            } else {
                result += " - No one guessed it!";
            }
            addChatMessage(result);
            
    // Reset UI state to allow ready again
    ui->readyButton->setEnabled(true);
    ui->readyButton->setText("Ready");
    ui->historyButton->setEnabled(true);
    ui->guessEdit->setEnabled(false);
            ui->guessEdit->clear();
            ui->submitButton->setEnabled(false);
            ui->clearButton->setEnabled(false);
            drawingWidget->setPaintingEnabled(false);
            drawingWidget->clearCanvas();
            isPainter = false; // Reset painter state
            ui->aiLabel->setText("AI Prediction: Waiting..."); // Reset AI label
            ui->aiLabel->setStyleSheet("QLabel { background-color: #f3e5f5; color: #7b1fa2; border: 2px solid #9c27b0; border-radius: 10px; font-size: 14px; font-weight: bold; padding: 8px; }");
            updateIdentityDisplay();
            break;
        }
        
        case MSG_ERROR: {
            addChatMessage("Server error");
            break;
        }
        
        case MSG_HISTORY_DATA: {
            HistoryDataMessage* histMsg = (HistoryDataMessage*)&msg;
            HistoryRecord record;
            record.game_id = histMsg->game_id;
            record.word = QString::fromUtf8(histMsg->word);
            record.user_guess = QString::fromUtf8(histMsg->user_guess);
            record.game_time = QString::fromUtf8(histMsg->game_time);
            historyRecords.append(record);
            break;
        }
        
        case MSG_HISTORY_END: {
            showHistoryDialog();
            break;
        }

        case MSG_ROOM_LIST: {
            RoomListMessage* roomListMsg = (RoomListMessage*)&msg;
            QDialog dialog(this);
            dialog.setWindowTitle("Available Rooms");
            dialog.resize(400, 350);

            QVBoxLayout* layout = new QVBoxLayout(&dialog);
            QListWidget* roomList = new QListWidget(&dialog);
            roomList->setSelectionMode(QAbstractItemView::SingleSelection);
            
            // Display all rooms from message
            for (uint8_t i = 0; i < roomListMsg->num_rooms; ++i) {
                RoomInfo* room = &roomListMsg->rooms[i];
                QString roomText = QString("Room %1: %2 (Players: %3)")
                    .arg(room->room_id)
                    .arg(QString::fromUtf8(room->name))
                    .arg(room->num_players);
                roomList->addItem(roomText);
            }
            
            if (roomListMsg->num_rooms == 0) {
                roomList->addItem("No rooms available");
                roomList->setEnabled(false);
            }
            
            QPushButton* createRoomButton = new QPushButton("Create New Room", &dialog);
            QPushButton* joinRoomButton = new QPushButton("Join Selected Room", &dialog);
            QPushButton* cancelButton = new QPushButton("Cancel", &dialog);
            
            joinRoomButton->setEnabled(roomListMsg->num_rooms > 0 && roomList->currentRow() >= 0);
            QObject::connect(roomList, &QListWidget::itemSelectionChanged, this, [joinRoomButton, roomList]() {
                joinRoomButton->setEnabled(roomList->currentRow() >= 0);
            });

            layout->addWidget(roomList);
            layout->addWidget(createRoomButton);
            layout->addWidget(joinRoomButton);
            layout->addWidget(cancelButton);

            QObject::connect(createRoomButton, &QPushButton::clicked, this, [this, &dialog]() {
                QDialog createDialog(this);
                createDialog.setWindowTitle("Create New Room");
                createDialog.resize(300, 150);

                QVBoxLayout* createLayout = new QVBoxLayout(&createDialog);
                QLineEdit* roomNameEdit = new QLineEdit(&createDialog);
                QLineEdit* nicknameEdit = new QLineEdit(&createDialog);
                QPushButton* createConfirmButton = new QPushButton("Create", &createDialog);
                QPushButton* createCancelButton = new QPushButton("Cancel", &createDialog);

                createLayout->addWidget(new QLabel("Room Name:"));
                createLayout->addWidget(roomNameEdit);
                createLayout->addWidget(new QLabel("Your Nickname:"));
                createLayout->addWidget(nicknameEdit);
                createLayout->addWidget(createConfirmButton);
                createLayout->addWidget(createCancelButton);

                QObject::connect(createConfirmButton, &QPushButton::clicked, this, [this, &createDialog, &dialog, roomNameEdit, nicknameEdit]() {
                    QString roomName = roomNameEdit->text().trimmed();
                    QString nickname = nicknameEdit->text().trimmed();
                    if (roomName.isEmpty() || nickname.isEmpty()) {
                        QMessageBox::warning(this, "Error", "Room name and nickname cannot be empty.");
                        return;
                    }
                    CreateRoomMessage createMsg;
                    createMsg.base.type = MSG_CREATE_ROOM;
                    createMsg.base.client_id = clientId;
                    createMsg.base.data_len = sizeof(CreateRoomMessage) - sizeof(BaseMessage);
                    strcpy(createMsg.room_name, roomName.toUtf8().constData());
                    strcpy(createMsg.nickname, nickname.toUtf8().constData());
                    sendTcpMessage(createMsg.base);
                    this->nickname = nickname;
                    createDialog.accept();
                    dialog.accept();
                });
                QObject::connect(createCancelButton, &QPushButton::clicked, &createDialog, &QDialog::reject);
                createDialog.exec();
            });
            QObject::connect(joinRoomButton, &QPushButton::clicked, this, [this, roomList, roomListMsg, &dialog]() {
                int selectedRow = roomList->currentRow();
                if (selectedRow < 0 || selectedRow >= roomListMsg->num_rooms) {
                    QMessageBox::warning(this, "Error", "Please select a room.");
                    return;
                }
                
                QDialog joinDialog(this);
                joinDialog.setWindowTitle("Join Room");
                joinDialog.resize(300, 150);

                QVBoxLayout* joinLayout = new QVBoxLayout(&joinDialog);
                QLabel* roomInfoLabel = new QLabel(QString("Room: %1 - %2")
                    .arg(roomListMsg->rooms[selectedRow].room_id)
                    .arg(QString::fromUtf8(roomListMsg->rooms[selectedRow].name)), &joinDialog);
                QLineEdit* nicknameEdit = new QLineEdit(&joinDialog);
                nicknameEdit->setPlaceholderText("Enter your nickname");
                QPushButton* joinConfirmButton = new QPushButton("Join", &joinDialog);
                QPushButton* joinCancelButton = new QPushButton("Cancel", &joinDialog);

                joinLayout->addWidget(roomInfoLabel);
                joinLayout->addWidget(new QLabel("Your Nickname:"));
                joinLayout->addWidget(nicknameEdit);
                joinLayout->addWidget(joinConfirmButton);
                joinLayout->addWidget(joinCancelButton);

                QObject::connect(joinConfirmButton, &QPushButton::clicked, this, [this, &joinDialog, &dialog, roomListMsg, selectedRow, nicknameEdit]() {
                    QString nickname = nicknameEdit->text().trimmed();
                    if (nickname.isEmpty()) {
                        QMessageBox::warning(this, "Error", "Nickname cannot be empty.");
                        return;
                    }
                    JoinRoomMessage joinMsg;
                    joinMsg.base.type = MSG_JOIN_ROOM;
                    joinMsg.base.client_id = clientId;
                    joinMsg.base.data_len = sizeof(JoinRoomMessage) - sizeof(BaseMessage);
                    joinMsg.room_id = roomListMsg->rooms[selectedRow].room_id;
                    strcpy(joinMsg.nickname, nickname.toUtf8().constData());
                    sendTcpMessage(joinMsg.base);
                    this->nickname = nickname;
                    joinDialog.accept();
                    dialog.accept();
                });
                QObject::connect(joinCancelButton, &QPushButton::clicked, &joinDialog, &QDialog::reject);
                joinDialog.exec();
            });
            QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
            dialog.exec();
            break;
        }

        case MSG_ROOM_CREATED: {
            RoomCreatedMessage* createdMsg = (RoomCreatedMessage*)&msg;
            currentRoomId = createdMsg->room_id;
            nickname = QString::fromUtf8(createdMsg->nickname);
            ui->infoLabel->setText(QString("Room: %1 - %2 (Players: %3)").arg(currentRoomId).arg(QString::fromUtf8(createdMsg->room_name)).arg(createdMsg->num_players));
            ui->readyButton->setEnabled(true);
            ui->readyButton->setText("Ready");
            ui->leaveRoomButton->setEnabled(true);
            addChatMessage(QString("Room %1 created successfully. You are now in room %1.").arg(currentRoomId));
            updateUI();
            break;
        }

        case MSG_ROOM_JOINED: {
            RoomJoinedMessage* joinedMsg = (RoomJoinedMessage*)&msg;
            currentRoomId = joinedMsg->room_id;
            nickname = QString::fromUtf8(joinedMsg->nickname);
            ui->infoLabel->setText(QString("Room: %1 - %2 (Players: %3)").arg(currentRoomId).arg(QString::fromUtf8(joinedMsg->room_name)).arg(joinedMsg->num_players));
            ui->readyButton->setEnabled(true);
            ui->readyButton->setText("Ready");
            ui->leaveRoomButton->setEnabled(true);
            addChatMessage(QString("You joined room %1: %2").arg(currentRoomId).arg(QString::fromUtf8(joinedMsg->room_name)));
            updateUI();
            break;
        }

        case MSG_ROOM_LEFT: {
            RoomLeftMessage* leftMsg = (RoomLeftMessage*)&msg;
            if (leftMsg->room_id == currentRoomId || currentRoomId == leftMsg->room_id) {
                currentRoomId = -1;
                ui->infoLabel->setText("Room Info: Not in room");
                ui->readyButton->setEnabled(false);
                ui->readyButton->setText("Ready");
                ui->leaveRoomButton->setEnabled(false);
                ui->aiLabel->setText("AI Prediction: Waiting..."); // Reset AI label
                addChatMessage("You left the room.");
            }
            break;
        }
        
        case MSG_AI_GUESS_RESULT: {
            AiGuessResultMessage* aiMsg = (AiGuessResultMessage*)&msg;
            QString prediction = QString::fromUtf8(aiMsg->predicted_word);
            int score = aiMsg->score;
            bool isCorrect = aiMsg->is_correct;
            
            QString text;
            if (isCorrect) {
                text = QString("AI: Correct! (%1% match)").arg(score);
                ui->aiLabel->setStyleSheet("QLabel { background-color: #e8f5e8; color: #2e7d32; border: 2px solid #4caf50; border-radius: 10px; font-size: 14px; font-weight: bold; padding: 8px; }");
            } else {
                text = QString("AI: %1 (Wrong, %2% match with answer)").arg(prediction).arg(score);
                ui->aiLabel->setStyleSheet("QLabel { background-color: #ffebee; color: #c62828; border: 2px solid #ef5350; border-radius: 10px; font-size: 14px; font-weight: bold; padding: 8px; }");
            }
            ui->aiLabel->setText(text);
            addChatMessage(text);
            break;
        }
    }
}

void MainWindow::handleUdpMessage(const BaseMessage& msg)
{
    if (msg.type == MSG_PAINT_DATA) {
        PaintDataMessage* paintMsg = (PaintDataMessage*)&msg;
        if (!isPainter) {
            if (paintMsg->action == 3) {
                // Synchronize clear
                drawingWidget->clearCanvas();
            } else {
                drawingWidget->addPaintData(*paintMsg);
            }
        }
    }
}

void MainWindow::updateGameState(GameState state)
{
    gameState = state;
    updateUI();
}

void MainWindow::updateUI()
{
    switch (gameState) {
        case GAME_WAITING:
            ui->gameInfoLabel->setText("Waiting to connect...");
            break;
        case GAME_READY:
            ui->gameInfoLabel->setText("Waiting for others to get ready...");
            break;
        case GAME_PAINTING:
            if (isPainter) {
                ui->gameInfoLabel->setText(QString("Painting - Word: %1").arg(currentWord));
                ui->guessEdit->setEnabled(false);
                ui->submitButton->setEnabled(true);
                ui->submitButton->setText("Finish Drawing");
                ui->colorButtonBlack->setEnabled(true);
                ui->colorButtonRed->setEnabled(true);
                ui->colorButtonBlue->setEnabled(true);
                ui->colorButtonGreen->setEnabled(true);
                ui->colorButtonYellow->setEnabled(true);
                ui->colorButtonPurple->setEnabled(true);
                ui->colorButtonCyan->setEnabled(true);
            } else {
                ui->gameInfoLabel->setText("Painting - Please watch the canvas");
                ui->guessEdit->setEnabled(false);
                ui->submitButton->setEnabled(false);
                ui->colorButtonBlack->setEnabled(false);
                ui->colorButtonRed->setEnabled(false);
                ui->colorButtonBlue->setEnabled(false);
                ui->colorButtonGreen->setEnabled(false);
                ui->colorButtonYellow->setEnabled(false);
                ui->colorButtonPurple->setEnabled(false);
                ui->colorButtonCyan->setEnabled(false);
            }
            break;
        case GAME_GUESSING:
            ui->gameInfoLabel->setText("Guessing - Enter your answer");
            // Painter cannot guess
            ui->guessEdit->setEnabled(!isPainter);
            ui->submitButton->setEnabled(!isPainter);
            if (!isPainter) {
                ui->submitButton->setText("Submit");
            }
            remainingTime = 30; // 30 seconds guessing time
            gameTimer->start(1000);
            break;
        case GAME_FINISHED:
            ui->gameInfoLabel->setText("Finished");
            break;
    }
}

void MainWindow::updateIdentityDisplay()
{
    if (!connected) {
        ui->identityLabel->setText("Role: Disconnected");
        ui->identityLabel->setStyleSheet("QLabel { background-color: #ffebee; color: #c62828; border: 2px solid #ef5350; border-radius: 10px; font-size: 16px; font-weight: bold; padding: 10px; }");
    } else if (gameState == GAME_WAITING || gameState == GAME_READY) {
        ui->identityLabel->setText("Role: Waiting");
        ui->identityLabel->setStyleSheet("QLabel { background-color: #fff3e0; color: #e65100; border: 2px solid #ff9800; border-radius: 10px; font-size: 16px; font-weight: bold; padding: 10px; }");
    } else if (isPainter) {
        ui->identityLabel->setText(QString("Role: 🎨 Painter (Word: %1)").arg(currentWord));
        ui->identityLabel->setStyleSheet("QLabel { background-color: #e8f5e8; color: #2e7d32; border: 2px solid #4caf50; border-radius: 10px; font-size: 16px; font-weight: bold; padding: 10px; }");
    } else {
        ui->identityLabel->setText("Role: 🔍 Guesser");
        ui->identityLabel->setStyleSheet("QLabel { background-color: #e3f2fd; color: #1565c0; border: 2px solid #2196f3; border-radius: 10px; font-size: 16px; font-weight: bold; padding: 10px; }");
    }
}

void MainWindow::addChatMessage(const QString& message)
{
    QString timeStr = QTime::currentTime().toString("hh:mm:ss");
    ui->chatTextEdit->append(QString("[%1] %2").arg(timeStr).arg(message));
}

void MainWindow::onColorButtonClicked() {
    QPushButton* button = qobject_cast<QPushButton*>(sender());
    if (!button) return;
    
    QColor color;
    QString buttonName = button->objectName();
    
    // Reset all button borders
    ui->colorButtonBlack->setStyleSheet("QPushButton { background-color: #000000; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonRed->setStyleSheet("QPushButton { background-color: #ff0000; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonBlue->setStyleSheet("QPushButton { background-color: #0000ff; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonGreen->setStyleSheet("QPushButton { background-color: #008000; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonYellow->setStyleSheet("QPushButton { background-color: #ffff00; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonPurple->setStyleSheet("QPushButton { background-color: #800080; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    ui->colorButtonCyan->setStyleSheet("QPushButton { background-color: #00ffff; border: 3px solid #333; border-radius: 20px; } QPushButton:hover { border: 3px solid #000; }");
    
    // Set color and highlight selected button
    if (buttonName == "colorButtonBlack") {
        color = Qt::black;
        ui->colorButtonBlack->setStyleSheet("QPushButton { background-color: #000000; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonRed") {
        color = Qt::red;
        ui->colorButtonRed->setStyleSheet("QPushButton { background-color: #ff0000; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonBlue") {
        color = Qt::blue;
        ui->colorButtonBlue->setStyleSheet("QPushButton { background-color: #0000ff; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonGreen") {
        color = Qt::darkGreen;
        ui->colorButtonGreen->setStyleSheet("QPushButton { background-color: #008000; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonYellow") {
        color = Qt::yellow;
        ui->colorButtonYellow->setStyleSheet("QPushButton { background-color: #ffff00; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonPurple") {
        color = QColor(128, 0, 128); // Purple
        ui->colorButtonPurple->setStyleSheet("QPushButton { background-color: #800080; border: 4px solid #000; border-radius: 20px; }");
    } else if (buttonName == "colorButtonCyan") {
        color = Qt::cyan;
        ui->colorButtonCyan->setStyleSheet("QPushButton { background-color: #00ffff; border: 4px solid #000; border-radius: 20px; }");
    }
    
    drawingWidget->setCurrentColor(color);
}

void MainWindow::showRoomList()
{
    if (!connected) {
        QMessageBox::warning(this, "Error", "Not connected to server.");
        return;
    }
    RoomListRequestMessage msg;
    msg.base.type = MSG_ROOM_LIST_REQ;
    msg.base.client_id = clientId;
    msg.base.data_len = 0;
    sendTcpMessage(msg.base);
    addChatMessage("Requesting room list...");
}

void MainWindow::leaveRoom()
{
    if (!connected || currentRoomId == -1) {
        QMessageBox::warning(this, "Error", "You are not in a room.");
        return;
    }
    
    int roomIdToLeave = currentRoomId;
    LeaveRoomMessage msg;
    msg.base.type = MSG_LEAVE_ROOM;
    msg.base.client_id = clientId;
    msg.base.data_len = sizeof(LeaveRoomMessage) - sizeof(BaseMessage);
    msg.room_id = currentRoomId;
    sendTcpMessage(msg.base);
    currentRoomId = -1;
    ui->infoLabel->setText("Room Info: Not in room");
    ui->readyButton->setEnabled(false);
    ui->readyButton->setText("Ready");
    ui->leaveRoomButton->setEnabled(false);
    addChatMessage(QString("You left room %1").arg(roomIdToLeave));
}
