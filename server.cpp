
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

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

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
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
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}