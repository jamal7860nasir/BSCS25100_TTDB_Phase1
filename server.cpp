#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include "helper.h"
using namespace std;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; 
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;
public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            return;
        }
        Node* tmp = new Node;
        tmp->data = val;
        tmp->next = top;
        top = tmp;
        count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        // pop the top value on the stack
        if (top == nullptr)
        {
            return T();
        }
        Node* neu = top;
        T val = neu->data;  
        top = top->next;    
        delete neu;        
        count--;             
        return val;
    }
    T& peek()
    {
        return top->data;
    }
    bool isEmpty()
    {
        if (top == nullptr)
        {
            return true;
        }
        return false;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* crn = top;
        int32_t ae = 0;
        while (crn != nullptr && ae < maxLen)
        {
            out[ae] = crn->data;
            crn = crn->next;
            ae++;
        }
        return ae;
    }
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* tp = new TimelineNode();
        tp->data = s;
        tp->next = nullptr;
        if (stepCount == 0)
        {
            tp->prev = nullptr;
            head = tp;
            tail = tp;
        }
        else
        {
            tp->prev = tail;
            tail->next = tp;
            tail = tp;
        }
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string nbl;
    while (getline(in, nbl))
    {
        int32_t le = nbl.length();
        int32_t sr = 0;
        while (sr < le && (nbl[sr] == ' ' || nbl[sr] == '\t' || nbl[sr] == '\r' || nbl[sr] == '\n'))
        {
            sr++;
        }
        if (sr == le)
        {
            continue; 
        }
        int32_t end = le - 1;
        while (end >= sr && (nbl[end] == ' ' || nbl[end] == '\t' || nbl[end] == '\r' || nbl[end] == '\n'))
        {
            end--;
        }
        out = "";
        for (int32_t s = sr; s <= end; s++)
        {
            out += nbl[s];
        }
        return true;
    }
    return false;  // read the next nonblank line
}
string firstWord(const string& line)
{
    // returns first word from the input 
    int32_t len = line.length();
    int32_t wr = 0;
    while (wr < len && line[wr] != ' ' && line[wr] != '\t')
    {
        wr++;
    }
    string rst = "";
    for (int32_t a = 0; a < wr; a++)
    {
        rst = rst + line[a];
    }
    return rst;
}
string secondWord(const string& line)
{
    int32_t le = line.length();
    int32_t w = 0;
    while (w < le && line[w] != ' ' && line[w] != '\t')
    {
        w++;
    }
    while (w < le && (line[w] == ' ' || line[w] == '\t'))
    {
        w++;
    }
    if (w == le)
    {
        return "";
    }
    string res = "";
    while (w < le && line[w] != ' ' && line[w] != '\t')
    {
        res = res + line[w];
        w++;
    }
    return res;
    // returns the second word
}
bool validateProgram(const char* sourcePath)
{    
    ifstream red(sourcePath);
    if (!red.is_open())
    {
        return 0;
    }
    string li;
    char ct = '0';
    while (readSourceLine(red, li))
    {
        string fw = firstWord(li);
        if (fw == "func")
        {
            if (ct == '1')
            {
                return 0; 
            }
            ct = '1';
        }
        else if (fw == "func_end")
        {
            if (ct == '0')
            {
                return 0; 
            }
            ct = '0'; 
        }
    }
    if (ct == '1')
    {
        return 0; 
    }
    return 1;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t sp = ftell(f);
    int32_t si=text.length();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&si, sizeof(int32_t), 1, f);
    fwrite(&text[0], sizeof(char), si, f);
    return sp;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t oset = 0;
    int32_t sz = 0;
    if (fread(&oset, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    if (fread(&sz, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }
    outText.resize(sz);
    if (fread(&outText[0], sizeof(char), sz, f) != (size_t)sz)
    {
        return -1;
    }
    return oset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    ifstream sorc(sourcePath);
    if (!sorc.is_open())
    {
        cout << "file eror" << endl;
        return -1;
    }
    FILE* wri_bin = fopen(resolveBinPath, "wb+");
    if (wri_bin == NULL)
    {
        sorc.close();
        cout << "error" << endl;  
        return -1;
    }
    string li="";
    while (readSourceLine(sorc, li))
    {
        string wrd = firstWord(li);
        string w2 = secondWord(li);
        int64_t dumb = writeResolveRecord(wri_bin, 0, li);
        if(wrd=="call")
        {
            if (patchCount < MAX_PATCHES)
            {
                patches[patchCount].byteOffsetOfOffsetField = dumb;
                patches[patchCount].targetFuncName = w2;
                patchCount++;
            }
        }
        else if(wrd=="func")
        {
            if (funcCount < MAX_FUNCS)
            {
                funcArray[funcCount].funcName = w2;
                funcArray[funcCount].byteOffsetInResolveBin = dumb;
                funcCount++;
            }
        }
    }
    for(int ae=0;ae<patchCount;ae++)
    {
        int64_t tar_ofs=-2;
        for(int32_t re=0;re<funcCount;re++)
        {
            if(patches[ae].targetFuncName==funcArray[re].funcName)
            {
                tar_ofs=funcArray[re].byteOffsetInResolveBin;
                break;
            }
        }
        if(tar_ofs!=-2)
        {
            fseek(wri_bin,patches[ae].byteOffsetOfOffsetField,SEEK_SET);
            fwrite(&tar_ofs, sizeof(int64_t), 1, wri_bin);
        }
    }
    int64_t of_main = -1;
    for (int32_t id = 0; id < funcCount;id++)
    {
        if (funcArray[id].funcName == "main")
        {
            of_main = funcArray[id].byteOffsetInResolveBin;
            break;
        }
    }
    fclose(wri_bin);
    return of_main;
}


// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t le=line.length();
    int32_t ct=0;
    int32_t tok_cn=0;
    while (ct < le&& tok_cn < maxTokens)
    {
        while (ct < le && line[ct] == ' ')
        {
            ct++;
        }
        if (ct >= le)
        {
            break;
        }
        string wrd = "";
        while (ct < le && line[ct] != ' ')
        {
            wrd = wrd + line[ct];
            ct++;
        }
        if (wrd != "")
        {
            tokens[tok_cn].text = wrd;
            if (tok_cn == 0)
            {
                tokens[tok_cn].type = KEYWORD;
            }
            else if (tok_cn == 1)
            {
                tokens[tok_cn].type = IDENTIFIER;
            }
            else
            {
                tokens[tok_cn].type = PARAM;
            }
            tok_cn++;
        }
    }
    return tok_cn;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* snp = new Snapshot();
    snp->stackDepth = callStack.snapshot_into(snp->callStack, MAX_STACK_DEPTH);
    return snp;
}

Variable* find_var(Frame& frm, const string& nam)
{
    int32_t idx = 0;
    while (idx < frm.argc)
    {
        if (frm.argv[idx].name == nam)
        {
            return &frm.argv[idx];
        }
        idx++;
    }
    idx = 0;
    while (idx < frm.localCount)
    {
        if (frm.locals[idx].name == nam)
        {
            return &frm.locals[idx];
        }
        idx++;
    }
    return nullptr;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
FILE* fp = fopen(resolveBinPath, "rb");
    if (fp == NULL)
    {
        cout << "file not open" << endl;
        return;
    }
    Stack<Frame> cstk;
    fseek(fp, mainOffset, SEEK_SET);
    Frame mfrm;
    mfrm.localCount = 0;
    mfrm.argc = 0;
    mfrm.func_name = "main";
    mfrm.returnLine = -1;
    cstk.push(mfrm);
    string line;
    Token tok[MAX_TOKENS];
    Token htok[MAX_TOKENS];
    char run = 't';
    while (run == 't')
    {
        int64_t bp = ftell(fp);
        int64_t of = readResolveRecord(fp, line);
        if (of == -1)
        {
            break;
        }
        int32_t n = tokenizeLine(line, tok, MAX_TOKENS);
        if (n == 0 || tok[0].text == "func")
        {
            continue;
        }
        if (tok[0].text == "set" || tok[0].text == "add" || tok[0].text == "sub" || tok[0].text == "mul" || tok[0].text == "div")
        {
            Frame& cur = cstk.peek();
            int32_t val = 0;
            if (n > 2)
            {
                string rhs = tok[2].text;
                char is_num = 'f';
                if (rhs[0] >= '0' && rhs[0] <= '9')
                {
                    is_num = 't';
                }
                if (rhs.length() > 1 && rhs[0] == '-' && rhs[1] >= '0' && rhs[1] <= '9')
                {
                    is_num = 't';
                }
                if (is_num == 't')
                {
                    int32_t sgn = 1;
                    int32_t pos = 0;
                    if (rhs[0] == '-')
                    {
                        sgn = -1;
                        pos = 1;
                    }
                    while (pos < (int32_t)rhs.length())
                    {
                        val = val * 10 + (rhs[pos] - '0');
                        pos++;
                    }
                    val = val * sgn;
                }
                else
                {
                    Variable* vp = find_var(cur, rhs);
                    if (vp != nullptr)
                    {
                        val = vp->value;
                    }
                }
            }
            Variable* vp = find_var(cur, tok[1].text);
            if (vp == nullptr && cur.localCount < MAX_VARS_PER_FRAME)
            {
                cur.locals[cur.localCount].name = tok[1].text;
                cur.locals[cur.localCount].value = 0;
                vp = &cur.locals[cur.localCount];
                cur.localCount++;
            }
            if (vp != nullptr)
            {
                if (tok[0].text == "set")
                {
                    vp->value = val;
                }
                else if (tok[0].text == "add")
                {
                    vp->value = vp->value + val;
                }
                else if (tok[0].text == "sub")
                {
                    vp->value = vp->value - val;
                }
                else if (tok[0].text == "mul")
                {
                    vp->value = vp->value * val;
                }
                else if (tok[0].text == "div")
                {
                    if (val == 0)
                    {
                        cout << "divide by zero" << endl;
                    }
                    else
                    {
                        vp->value = vp->value / val;
                    }
                }
            }
        }
        else if (tok[0].text == "call")
        {
            if (cstk.depth() >= MAX_STACK_DEPTH)
            {
                cout << "stack overflow" << endl;
                run = 'f';
            }
            else
            {
                Frame& cur = cstk.peek();
                Frame nfrm;
                nfrm.func_name = tok[1].text;
                nfrm.argc = 0;
                nfrm.localCount = 0;
                nfrm.returnLine = (int32_t)bp;
                fseek(fp, of, SEEK_SET);
                string hline;
                int32_t mc = 0;
                if (readResolveRecord(fp, hline) != -1)
                {
                    mc = tokenizeLine(hline, htok, MAX_TOKENS);
                }

                if (mc < 2 || htok[0].text != "func" || htok[1].text != tok[1].text)
                {
                    cout << "function not found" << endl;
                    run = 'f';
                }
                else
                {
                    int32_t arg_idx = 0;
                    while (arg_idx < mc - 2 && arg_idx < n - 2)
                    {
                        string param_txt = tok[2 + arg_idx].text;
                        int32_t pval = 0;
                        char is_num = 'f';
                        if (param_txt[0] >= '0' && param_txt[0] <= '9')
                        {
                            is_num = 't';
                        }
                        if (param_txt.length() > 1 && param_txt[0] == '-' && param_txt[1] >= '0' && param_txt[1] <= '9')
                        {
                            is_num = 't';
                        }
                        if (is_num == 't')
                        {
                            int32_t sgn = 1;
                            int32_t pos = 0;
                            if (param_txt[0] == '-')
                            {
                                sgn = -1;
                                pos = 1;
                            }
                            while (pos < (int32_t)param_txt.length())
                            {
                                pval = pval * 10 + (param_txt[pos] - '0');
                                pos++;
                            }
                            pval = pval * sgn;
                        }
                        else
                        {
                            Variable* vp = find_var(cur, param_txt);
                            if (vp != nullptr)
                            {
                                pval = vp->value;
                            }
                        }

                        nfrm.argv[arg_idx].name = htok[2 + arg_idx].text;
                        nfrm.argv[arg_idx].value = pval;
                        arg_idx++;
                    }
                    nfrm.argc = arg_idx;
                    cstk.push(nfrm);
                }
            }
        }
        else if (tok[0].text =="func_end")
        {
            if (cstk.depth() <= 1)
            {
                run = 'f';
            }
            else
            {
                Frame pf = cstk.pop();
                Frame& cur = cstk.peek();
                fseek(fp, pf.returnLine, SEEK_SET);
                string lstr;
                int32_t hlen = 0;
                if (readResolveRecord(fp, lstr) != -1)
                {
                    hlen = tokenizeLine(lstr, htok, MAX_TOKENS);
                }
                int32_t k = 0;
                while (k < pf.argc && k < hlen - 2)
                {
                    string var_nam = htok[2 + k].text;
                    Variable* vp = find_var(cur, var_nam);
                    if (vp != nullptr)
                    {
                        vp->value = pf.argv[k].value;
                    }
                    k++;
                }
            }
        }
        timeline.record(buildSnapshot(cstk));
    }
    fclose(fp);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
FILE* tb = fopen(tdbgPath, "wb+");
    if (tb == NULL) 
    {
        return;
    }
    int32_t stp = timeline.getStepCount();
    TTDBHeader hdr;
    hdr.magic[0] = 'T'; hdr.magic[1] = 'T'; hdr.magic[2] = 'D'; hdr.magic[3] = 'B';
    hdr.version = 1;
    hdr.stepCount = stp;
    hdr.indexOffset = 0;
    fwrite(&hdr, sizeof(TTDBHeader), 1, tb);
    int64_t* id_arr = new int64_t[stp];
    int32_t ctn = 0;
    TimelineNode* cr = timeline.begin();
    while (cr != nullptr && ctn < stp)
    {
        id_arr[ctn] = ftell(tb);
        if (cr->data != nullptr)
        {
            writeSnapshot(tb, *cr->data);
        }
        cr = cr->next;
        ctn++;
    }
    int64_t idx_pos = ftell(tb);
    fwrite(id_arr, sizeof(int64_t), stp, tb);
    hdr.indexOffset = idx_pos;
    fseek(tb, 0, SEEK_SET);
    fwrite(&hdr, sizeof(TTDBHeader), 1, tb);
    delete[] id_arr;
    fclose(tb);
}
// main section
int32_t main(int argc, char* argv[])
{
    int port = 5001;
    if (argc >= 2)
    {
        port = atoi(argv[1]);
    }
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return 1;
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(sfd);
        return 1;
    }
    if (listen(sfd, 5) < 0)
    {
        close(sfd);
        return 1;
    }
    int cfd = accept(sfd, NULL, NULL);
    if (cfd < 0)
    {
        close(sfd);
        return 1;
    }
    ofstream outSource("source.bin", ios::binary);
    char buf[1024];
    vector<uint8_t> rawIncoming;
    while (true)
    {
        ssize_t r = recv(cfd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        rawIncoming.insert(rawIncoming.end(), buf, buf + r);
    }
    size_t payloadLen = rawIncoming.size();
    if (payloadLen >= 4) payloadLen -= 4; 
    if (payloadLen > 0)
    {
        outSource.write(reinterpret_cast<const char*>(rawIncoming.data()), payloadLen);
    }
    outSource.close();
    bool success = false;
    if (validateProgram("source.bin"))
    {
        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
        if (mainOffset >= 0)
        {
            Timeline timeline;
            executeProgram("resolve.bin", mainOffset, timeline);
            writeTdbg(timeline, "session.tdbg");
            success = true;
        }
    }
    vector<uint8_t> reply;
    if (success)
    {
        reply.push_back(0);
        ifstream inTdbg("session.tdbg", ios::binary | ios::ate);
        if (inTdbg.is_open())
        {
            streamsize sz = inTdbg.tellg();
            inTdbg.seekg(0, ios::beg);
            vector<uint8_t> tdbgData(sz);
            inTdbg.read(reinterpret_cast<char*>(tdbgData.data()), sz);
            inTdbg.close();
            reply.insert(reply.end(), tdbgData.begin(), tdbgData.end());
        }
    }
    else
    {
        reply.push_back(1);
        string errMsg = "validation or compilation failed";
        reply.insert(reply.end(), errMsg.begin(), errMsg.end());
    }
    uint32_t crcVal = compute_crc(reply.data(), reply.size());
    for (int s = 0; s < 4; s++)
    {
        reply.push_back(crcVal >> (s * 8));
    }
    send(cfd, reply.data(), reply.size(), 0);
    close(cfd);
    close(sfd);
    return 0;
}