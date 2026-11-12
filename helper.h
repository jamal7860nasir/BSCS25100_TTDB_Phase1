#pragma once
#include <iostream>
#include <string>
#include <cstdint>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
using namespace std;
// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 
const int IO_BUFFER_SIZE = 64 * 1024;     
const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;
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
struct TTDBHeader 
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
uint32_t compute_crc(const uint8_t* data, int len)
{
    uint32_t crc = 4294967295U;
    for (int ae = 0; ae < len; ae++)
    {
        crc =crc ^ data[ae];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 1)
            {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}


struct TrailerChecker
{
    vector<uint8_t> bytes_rec;
    void push(uint8_t b)
    {
        bytes_rec.push_back(b);
    }
    bool verify()
    {
        if (bytes_rec.size() < 4)
        {
            return false;
        }
        int data_len = bytes_rec.size() - 4;
        uint32_t calc_crc = compute_crc(bytes_rec.data(), data_len);
        uint32_t crc_vl = 0;
        for (int ae = 0; ae < 4; ae++)
        {
            crc_vl = crc_vl + (((uint32_t)bytes_rec[data_len + ae]) << (ae * 8));
        }
        return calc_crc == crc_vl;
    }
};

inline void writeSnapshot(FILE* f, const Snapshot& snp)
{
    fwrite(&snp.stackDepth, sizeof(int32_t), 1, f);
    int32_t ae = 0;
    while (ae < snp.stackDepth)
    {
        const Frame& frm = snp.callStack[ae];
        int32_t fnLen = frm.func_name.length();
        fwrite(&fnLen, sizeof(int32_t), 1, f);
        if (fnLen > 0)
        {
            fwrite(frm.func_name.data(), sizeof(char), fnLen, f);
        }
        fwrite(&frm.argc, sizeof(int32_t), 1, f);
        int32_t re = 0;
        while (re < frm.argc)
        {
            int32_t nLen = frm.argv[re].name.length();
            fwrite(&nLen, sizeof(int32_t), 1, f);
            if (nLen > 0)
            {
                fwrite(frm.argv[re].name.data(), sizeof(char), nLen, f);
            }
            fwrite(&frm.argv[re].value, sizeof(int32_t), 1, f);
            re++;
        }
        fwrite(&frm.returnLine, sizeof(int32_t), 1, f);
        fwrite(&frm.localCount, sizeof(int32_t), 1, f);
        int32_t ze = 0;
        while (ze < frm.localCount)
        {
            int32_t lLen = frm.locals[ze].name.length();
            fwrite(&lLen, sizeof(int32_t), 1, f);
            if (lLen > 0)
            {
                fwrite(frm.locals[ze].name.data(), sizeof(char), lLen, f);
            }
            fwrite(&frm.locals[ze].value, sizeof(int32_t), 1, f);
            ze++;
        }       
        ae++;
    }
}
inline void readSnapshot(FILE* f, Snapshot& snp)
{
    fread(&snp.stackDepth, sizeof(int32_t), 1, f);
    int32_t ae = 0;
    while (ae < snp.stackDepth)
    {
        Frame& frm = snp.callStack[ae];
        int32_t fnLen = 0;
        fread(&fnLen, sizeof(int32_t), 1, f);
        frm.func_name.resize(fnLen);
        if (fnLen > 0)
        {
            fread(&frm.func_name[0], sizeof(char), fnLen, f);
        }
        fread(&frm.argc, sizeof(int32_t), 1, f);
        int32_t re = 0;
        while (re < frm.argc)
        {
            int32_t nLen = 0;
            fread(&nLen, sizeof(int32_t), 1, f);
            frm.argv[re].name.resize(nLen);
            if (nLen > 0)
            {
                fread(&frm.argv[re].name[0], sizeof(char), nLen, f);
            }
            fread(&frm.argv[re].value, sizeof(int32_t), 1, f);
            re++;
        }
        fread(&frm.returnLine, sizeof(int32_t), 1, f);
        fread(&frm.localCount, sizeof(int32_t), 1, f);
        int32_t ze = 0;
        while (ze < frm.localCount)
        {
            int32_t lLen = 0;
            fread(&lLen, sizeof(int32_t), 1, f);
            frm.locals[ze].name.resize(lLen);
            if (lLen > 0)
            {
                fread(&frm.locals[ze].name[0], sizeof(char), lLen, f);
            }
            fread(&frm.locals[ze].value, sizeof(int32_t), 1, f);
            ze++;
        }       
        ae++;
    }
}