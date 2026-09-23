#include "trace_file.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool is_xz_file(const std::string& filename) {
    static const unsigned char magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
    unsigned char head[6] = {0};
    FILE* f = std::fopen(filename.c_str(), "rb");
    if (!f) return false;
    size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    return n == sizeof(head) && std::memcmp(head, magic, sizeof(magic)) == 0;
}

TraceFile::TraceFile(const std::string& filename) : buf(1 << 20) {
    use_xz = is_xz_file(filename);
    if (use_xz) {
        fp = std::fopen(filename.c_str(), "rb");
        if (!fp || lzma_stream_decoder(&xz, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return;
        xz_in.resize(1 << 20);
    } else {
        // gzopen reads non-gzip files transparently, so plain traces share this path.
        gz = gzopen(filename.c_str(), "rb");
        if (!gz) return;
        gzbuffer(gz, 1 << 20);
    }
    ok = true;
    for (const char* ext : {".tar.gz", ".tgz", ".tar.xz", ".txz"}) {
        if (ends_with(filename, ext)) {
            ok = open_tar_member();
            break;
        }
    }
}

TraceFile::~TraceFile() {
    if (gz) gzclose(gz);
    if (fp) std::fclose(fp);
    lzma_end(&xz);
}

[[noreturn]] static void decode_error(const std::string& msg) {
    std::cerr << "Error: trace decode failed: " << msg << std::endl;
    std::exit(1);
}

// Decompressed bytes into dst: >0 bytes read, 0 at end of stream.
// Corrupt or truncated input is fatal so partial runs never report stats.
long TraceFile::read_raw(char* dst, size_t n) {
    if (!use_xz) {
        int got = gzread(gz, dst, (unsigned)n);
        int errnum = Z_OK;
        const char* msg = gzerror(gz, &errnum);
        if (got < 0 || (errnum != Z_OK && errnum != Z_STREAM_END)) decode_error(msg);
        return got;
    }
    if (xz_done) return 0;
    xz.next_out = reinterpret_cast<uint8_t*>(dst);
    xz.avail_out = n;
    while (xz.avail_out > 0) {
        if (xz.avail_in == 0 && !xz_in_eof) {
            xz.next_in = xz_in.data();
            xz.avail_in = std::fread(xz_in.data(), 1, xz_in.size(), fp);
            if (xz.avail_in == 0) xz_in_eof = true;
        }
        lzma_ret ret = lzma_code(&xz, xz_in_eof ? LZMA_FINISH : LZMA_RUN);
        if (ret == LZMA_STREAM_END) {
            xz_done = true;
            break;
        }
        if (ret != LZMA_OK) decode_error("xz (lzma_ret " + std::to_string(ret) + ")");
    }
    return (long)(n - xz.avail_out);
}

bool TraceFile::fill() {
    long n = read_raw(buf.data(), buf.size());
    pos = 0;
    len = n > 0 ? (size_t)n : 0;
    return len > 0;
}

bool TraceFile::read_exact(char* dst, size_t n) {
    while (n > 0) {
        if (pos == len && !fill()) return false;
        size_t take = std::min(n, len - pos);
        std::memcpy(dst, buf.data() + pos, take);
        pos += take;
        dst += take;
        n -= take;
    }
    return true;
}

bool TraceFile::skip(uint64_t n) {
    while (n > 0) {
        if (pos == len && !fill()) return false;
        size_t take = (size_t)std::min<uint64_t>(n, len - pos);
        pos += take;
        n -= take;
    }
    return true;
}

// Skip tar headers (and non-regular entries such as pax/GNU long-name records)
// until the first regular file, then limit reads to its size.
bool TraceFile::open_tar_member() {
    char header[512];
    while (read_exact(header, sizeof(header))) {
        if (header[0] == '\0') return false;  // end-of-archive block
        uint64_t size = std::strtoull(std::string(header + 124, 12).c_str(), nullptr, 8);
        char type = header[156];
        if (type == '0' || type == '\0') {
            remaining = size;
            return true;
        }
        if (!skip((size + 511) / 512 * 512)) return false;
    }
    return false;
}

bool TraceFile::getline(std::string& line) {
    line.clear();
    while (remaining > 0) {
        if (pos == len && !fill()) break;
        size_t avail = (size_t)std::min<uint64_t>(len - pos, remaining);
        const char* start = buf.data() + pos;
        const char* nl = static_cast<const char*>(std::memchr(start, '\n', avail));
        size_t take = nl ? (size_t)(nl - start) + 1 : avail;
        line.append(start, nl ? take - 1 : take);
        pos += take;
        if (remaining != UINT64_MAX) remaining -= take;
        if (nl) return true;
    }
    return !line.empty();
}
