// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    int tol=0;
    while (tol < n)
    {
        int red_si = recv(sock,buf+tol,n-tol,0);
        if (red_si<=0)
        {
            return false;
        }
        tol = tol + red_si;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    int snt_si=0;
    while (snt_si< n)
    {
        int st_sz = send(sock, buf + snt_si, n - snt_si, MSG_NOSIGNAL);
        if (st_sz <= 0)
        {
            return false;
        }
        snt_si = snt_si +st_sz;
    }
    return true;
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int rs = socket(AF_INET, SOCK_STREAM, 0);
    if (rs < 0)
    {
        return -1;
    }
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(rs, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(rs);
        return -1;
    }
    return rs;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE *readd = fopen(sourcePath, "rb");
    if (!readd)
    {
        return false;
    }
    fseek(readd, 0, SEEK_END);
    int file_si = ftell(readd);
    fseek(readd, 0, SEEK_SET);
    vector<uint8_t> bfr_src(file_si);
    if (fread(bfr_src.data(), 1, file_si, readd) != (size_t)file_si)
    {
        fclose(readd);
        return false;
    }
    fclose(readd);
    uint32_t crc_val = compute_crc(bfr_src.data(), bfr_src.size());
    for (int s = 0; s < 4; s++)
    {
        bfr_src.push_back(crc_val >> (s * 8));
    }
    if (!sendAll(sock, (const char *)bfr_src.data(), bfr_src.size()))
    {
        return false;
    }
    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    TrailerChecker chk;
    uint8_t bfr[1024];
    bool st_got = false;
    uint8_t st_byte = 0;
    FILE *fl_wr = nullptr;
    while (true)
    {
        int r = recv(sock, (char *)bfr, sizeof(bfr), 0);
        if (r <= 0)
        {
            break;
        }
        for (int ae = 0; ae < r; ae++)
        {
            uint8_t b = bfr[ae];
            chk.push(b);
            if (!st_got)
            {
                st_byte = b;
                st_got = true;

                if (st_byte == 0)
                {
                    fl_wr = fopen(outTdbgPath, "wb");
                    if (!fl_wr)
                    {
                        return false;
                    }
                }
            }
            else
            {
                if (st_byte == 0 && fl_wr)
                {
                    fputc(b, fl_wr);
                }
                else if (st_byte == 1)
                {
                    outError.push_back((char)b);
                }
            }
        }
    }
    if (fl_wr)
    {
        fclose(fl_wr);
        fl_wr = nullptr;
    }
    if (!chk.verify())
    {
        if (st_byte == 0)
        {
            remove(outTdbgPath);
        }
        outError = "checksum mismatch,response corrupted";
        return false;
    }
    if (st_byte == 1)
    {
        if (outError.size() >= 4)
        {
            outError.resize(outError.size() - 4);
        }
        return false;
    }
    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
    FILE *readd = fopen(path, "rb");
    if (!readd)
    {
        return false;
    }
    TTDBHeader hder;
    if (fread(&hder, sizeof(TTDBHeader), 1, readd) != 1)
    {
        fclose(readd);
        return false;
    }
    if (hder.magic[0] != 'T' || hder.magic[1] != 'T' || hder.magic[2] != 'D' || hder.magic[3] != 'B')
    {
        fclose(readd);
        return false;
    }
    r.header = hder;
    r.file_index = fileno(readd);
    if (fseek(readd, hder.indexOffset, SEEK_SET) != 0)
    {
        fclose(readd);
        return false;
    }
    for (int ae = 0; ae < hder.stepCount; ae++)
    {
        int64_t ofs = 0;
        if (fread(&ofs, sizeof(int64_t), 1, readd) != 1)
        {
            fclose(readd);
            return false;
        }
        r.index.push_back(ofs);
    }
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    if (r.file_index >= 0)
    {
        close(r.file_index);
        r.file_index = -1;
    }
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
if (step < 0 || step >= (int32_t)r.index.size())
    {
        return nullptr;
    }
    int64_t target_ofs = r.index[step];
    if (lseek(r.file_index, target_ofs, SEEK_SET) == -1)
    {
        return nullptr;
    }
    Snapshot *snp = new Snapshot();
    ssize_t red_byt = read(r.file_index, snp, sizeof(Snapshot));
    if (red_byt != (ssize_t)sizeof(Snapshot))
    {
        delete snp;
        return nullptr;
    }
    return snp;
}
bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outTdbgPath, string &outError)
{
    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "failed to connect to load balancer";
        return false;
    }
    if (!uploadSource(sock, sourcePath))
    {
        close(sock);
        outError = "failed to upload source file";
        return false;
    }
    bool res = receiveResponse(sock, outTdbgPath, outError);
    close(sock);
    return res;
}
// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   s.active_flag = 0;
    s.currentStep = 0;
    s.reader.file_index = -1;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snp = readSnapshotAt(s.reader, s.currentStep);
    if (!snp)
    {
        cout << "damaged tdbg file"<<endl;
        return false;
    }
    printSnapshot(s.currentStep + 1, s.reader.header.stepCount, *snp);
    delete snp;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        return false;
    }
    int64_t nxt_st = s.currentStep + delta;
    if (nxt_st < 0 || nxt_st >= s.reader.header.stepCount)
    {
        return false;
    }
    s.currentStep = nxt_st;
    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string err_msg;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_FILE, err_msg))
    {
        cout << err_msg << endl;
        return false;
    }
    TdbgReader temp_r;
    if (!openTdbg(TDBG_FILE, temp_r))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }
    if (s.active_flag)
    {
        closeTdbg(s.reader);
    }
    s.reader = temp_r;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "step " << step << " of " << totalSteps << endl;
    for (int ae = 0; ae < s.stackDepth; ae++)
    {
        const Frame &frm = s.callStack[ae];
        cout << "frame " << ae << " " << frm.func_name << endl;
        for (int j = 0; j < frm.argc; j++)
        {
            cout << "arg " << frm.argv[j].name << " " << frm.argv[j].value << endl;
        }
        cout << "return line " << frm.returnLine << endl;
        if (frm.localCount > 0)
        {
            for (int j = 0; j < frm.localCount; j++)
            {
                cout << "local " << frm.locals[j].name << " " << frm.locals[j].value << endl;
            }
        }
    }
}
void printHelp()
{
    // Print the commands
    cout << "start filepath" << endl;
    cout << "next" << endl;
    cout << "prv" << endl;
    cout << "jmp next n" << endl;
    cout << "jmp prv n" << endl;
    cout << "help" << endl;
    cout << "quit" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> wrds;
    string w = "";
    for (int ae = 0; ae < line.length(); ae++)
    {
        if (line[ae] == ' ' || line[ae] == '\t' || line[ae] == '\n' || line[ae] == '\r')
        {
            if (!w.empty())
            {
                wrds.push_back(w);
                w = "";
            }
        }
        else
        {
            w =w + line[ae];
        }
    }
    if (!w.empty())
    {
        wrds.push_back(w);
    }
    if (wrds.empty())
    {
        return true;
    }
    if (wrds[0] == "ttdb")
    {
        vector<string> temp;
        for (int ae = 1; ae < wrds.size(); ae++)
        {
            temp.push_back(wrds[ae]);
        }
        wrds = temp;
        
        if (wrds.empty())
        {
            return true;
        }
    }
    string cmd = wrds[0];
    if (cmd == "start")
    {
        if (wrds.size() != 2)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        return cmdStart(s, wrds[1]);
    }
    else if (cmd == "next")
    {
        if (wrds.size() != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        return cmdNext(s);
    }
    else if (cmd == "prv")
    {
        if (wrds.size() != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        return cmdPrv(s);
    }
    else if (cmd == "jmp")
    {
        if (wrds.size() != 3)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        int n = atoi(wrds[2].c_str());
        if (n <= 0)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        if (wrds[1] == "next")
        {
            return cmdJmpNext(s, n);
        }
        else if (wrds[1] == "prv")
        {
            return cmdJmpPrv(s, n);
        }
        else
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
    }
    else if (cmd == "help")
    {
        if (wrds.size() != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        printHelp();
        return true;
    }
    else if (cmd == "quit" || cmd == "exit")
    {
        if (wrds.size() != 1)
        {
            cout << "unknown command" << endl;
            printHelp();
            return false;
        }
        quitRequested = true;
        return true;
    }
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    if (argc >= 2)
    {
        LB_HOST = argv[1];
    }
    if (argc >= 3)
    {
        LB_PORT = atoi(argv[2]);
    }
    DebugSession session;
    initSession(session);
    cout << "welcome to time travel debugger" << endl;
    printHelp();
    bool quitRequested = false;
    string line;
    while (true)
    {
        if (quitRequested)
        {
            break;
        }
        cout << PROMPT;
        if (!getline(cin, line))
        {
            break;
        }
        runCommand(session, line, quitRequested);
    }
    closeSession(session);
    return 0;
}
