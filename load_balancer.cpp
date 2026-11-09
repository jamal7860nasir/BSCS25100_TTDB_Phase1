// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        Node *temp = front;
        while (temp != nullptr)
        {
            Node *nxt = temp->next;
            delete temp;
            temp = nxt;
        }
        rear = nullptr;
        front = nullptr;
        count = 0;
    }
    void enqueue(const T &val)
    {
        Node *neu = new Node;
        neu->data = val;
        neu->next = nullptr;
        if (rear == nullptr)
        {
            front = neu;
            rear = neu;
        }
        else
        {
            rear->next = neu;
            rear = neu;
        }
        count++;
    }
    T dequeue()
    {
        if (isEmpty())
        {
            return T();
        }
        Node *temp = front;
        T val = temp->data;
        front = front->next;
        if (front == nullptr)
        {
            rear = nullptr;
        }
        delete temp;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return count==0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    size_t tot = 0;
    while (tot < n)
    {
        ssize_t sentt = send(sock, buf + tot, n - tot, 0);
        if (sentt <= 0) 
        {
            return false;
        }
        tot =tot + sentt;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    vector<uint8_t> ply;
    ply.push_back(1);
    for (int qe = 0; qe < message.size(); qe++)
    {
        ply.push_back(message[qe]);
    }
    uint32_t cr_vl = compute_crc(ply.data(), ply.size());
    for (int s = 0; s < 4; s++)
    {
        ply.push_back(cr_vl >> (s * 8));
    }
    bool res = sendAll(sock, (const char *)ply.data(), ply.size());
    if (res == true)
    {
        return true;
    }
    else
    {
        return false;
    }
}
int createListenSocket(int port)
{  
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
    {
        cerr << "Socket creation failed" << endl;
        return -1;
    }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        cerr << "bind fail" << endl;
        close(s);
        return -1;
    }
    if (listen(s, LISTEN_BACKLOG) < 0)
    {
        cerr << "listen failed" << endl;
        close(s);
        return -1;
    }
    return s;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0)
    {
        return -1;
    }
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(server_fd);
        return -1;
    }
    return server_fd;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string com = string(SERVER_PATH)+" "+to_string(port)+" &";
    int ret = system(com.c_str());
    if (ret == 0)
    {
        return true;
    }
    return false;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
  int srvr_on = connectToServer(serverHost.c_str(), serverPort);
    if (srvr_on >= 0)
    {
        return srvr_on;
    }
    if (!startServer(serverPort))
    {
        return -1;
    }
    for (int32_t ae = 0; ae < STARTUP_MAX_ATTEMPTS; ae++)
    {
        usleep(60500);
        srvr_on = connectToServer(serverHost.c_str(), serverPort);
        if (srvr_on >= 0)
        {
            return srvr_on;
        }
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int clnt = c.sock;
    int srvr = connectToServerWithWakeup();
    if (srvr < 0)
    {
        sendError(clnt, "backend server failed to start");
        close(clnt);
        return;
    }
    char li_si[IO_BUFFER_SIZE];
    while (true)
    {
        ssize_t rd_sz = recv(clnt, li_si, sizeof(li_si), 0);
        if (rd_sz <= 0)
        {
            break;
        }
        if (!sendAll(srvr, li_si, rd_sz))
        {
            break;
        }
    }
    shutdown(srvr, SHUT_WR);
    while (true)
    {
        ssize_t rd_sz = recv(srvr, li_si, sizeof(li_si), 0);
        if (rd_sz <= 0)
        {
            break;
        }
        if (!sendAll(clnt, li_si, rd_sz))
        {
            break;
        }
    }
    close(srvr);
    close(clnt);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag)
    {
        sockaddr_in clnt;
        socklen_t clnt_len = sizeof(clnt);
        int cs = accept(listenSock, (struct sockaddr *)&clnt, &clnt_len);
        if (cs < 0)
        {
            if (!flag)
            {
                break;
            }
            cout << "accept error" << endl;
            continue;
        }
        ClientInfo info;
        info.sock = cs;
        info.arrivedAt = time(nullptr);
        char siz[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&(clnt.sin_addr),siz,INET_ADDRSTRLEN);
        info.address = string(siz);
        info.port = ntohs(clnt.sin_port);
        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            queueMutex.unlock();
            sendError(cs, "server busy, try again later");
            close(cs);
        }
        else
        {
            pendingClients.enqueue(info);
            queueMutex.unlock();
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag)
    {
        ClientInfo detl;
        bool clnt_fnd = false;
        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {   
            detl = pendingClients.dequeue();
            clnt_fnd = true;
        }
        queueMutex.unlock();
        if (clnt_fnd)
        {
            relayOneClient(detl);
        }
        else
        {
            usleep(10000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    int li_s = createListenSocket(lbPort);
    thread acpt(acceptLoop, li_s);
    acpt.detach();
    dispatcherLoop();
    return 0;
}