#ifndef __TRACE_FILE_H__
#define __TRACE_FILE_H__

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <lzma.h>
#include <zlib.h>

// Line reader for text traces: plain, gzip, xz, or a tar archive compressed
// with either (.tar.gz / .tgz / .tar.xz / .txz; the first regular file is read).
class TraceFile {
    bool use_xz = false;
    bool ok = false;
    gzFile gz = nullptr;
    FILE* fp = nullptr;
    lzma_stream xz = LZMA_STREAM_INIT;
    std::vector<uint8_t> xz_in;
    bool xz_in_eof = false;
    bool xz_done = false;

    std::vector<char> buf;
    size_t pos = 0;
    size_t len = 0;
    uint64_t remaining = UINT64_MAX;  // bytes left in the tar member

    long read_raw(char* dst, size_t n);
    bool fill();
    bool read_exact(char* dst, size_t n);
    bool skip(uint64_t n);
    bool open_tar_member();

    public:
    explicit TraceFile(const std::string& filename);
    ~TraceFile();
    TraceFile(const TraceFile&) = delete;
    TraceFile& operator=(const TraceFile&) = delete;

    bool is_open() const { return ok; }
    bool getline(std::string& line);
};

#endif
