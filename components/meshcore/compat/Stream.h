/* The four methods of Arduino's Stream that MeshCore actually uses.

   LS-969  MeshCore's core is portable C++ - the only Arduino dependency in
   it is this class, and only in Identity::readFrom/writeTo/printTo and
   Utils::printHex. Rather than drag in an Arduino compatibility layer for
   four methods, this is the four methods.

   Verified against the vendored source on 2026-09-09: `readBytes`, `write`,
   `print` and `println` are the entire surface. If a future MeshCore version
   reaches for more, the build breaks here rather than silently linking
   against something that behaves differently, which is the point of keeping
   it this small. */
#ifndef LS_COMPAT_STREAM_H
#define LS_COMPAT_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Stream {
public:
    virtual ~Stream() {}

    virtual size_t readBytes(uint8_t *buf, size_t len) = 0;
    virtual size_t write(const uint8_t *buf, size_t len) = 0;

    /* Arduino's print is overloaded across every scalar. MeshCore uses it
       for a C string and for a single char, so those are what exist. */
    virtual size_t print(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    virtual size_t print(char c)        { return write((const uint8_t *)&c, 1); }
    virtual size_t println()            { return print("\n"); }
    virtual size_t println(const char *s) { return print(s) + println(); }
};

/* A Stream over a fixed buffer. This is how an identity is read from and
   written to NVS without a filesystem in the middle. Bounded by
   construction: a short read returns short rather than running off the end,
   and MeshCore checks every length it gets back. */
class BufferStream : public Stream {
public:
    BufferStream(uint8_t *buf, size_t cap, size_t filled = 0)
        : _buf(buf), _cap(cap), _len(filled), _pos(0) {}

    size_t readBytes(uint8_t *dst, size_t len) override
    {
        size_t avail = _len > _pos ? _len - _pos : 0;
        if (len > avail) len = avail;
        if (len && dst) memcpy(dst, _buf + _pos, len);
        _pos += len;
        return len;
    }

    size_t write(const uint8_t *src, size_t len) override
    {
        size_t room = _cap > _pos ? _cap - _pos : 0;
        if (len > room) len = room;
        if (len && src) memcpy(_buf + _pos, src, len);
        _pos += len;
        if (_pos > _len) _len = _pos;
        return len;
    }

    size_t length() const { return _len; }
    void   rewind()       { _pos = 0; }

private:
    uint8_t *_buf;
    size_t   _cap, _len, _pos;
};

/* A Stream onto the console, for printTo(). */
class PrintStream : public Stream {
public:
    size_t readBytes(uint8_t *, size_t) override { return 0; }
    size_t write(const uint8_t *buf, size_t len) override
    {
        for (size_t i = 0; i < len; i++) putchar(buf[i]);
        return len;
    }
};

#endif /* LS_COMPAT_STREAM_H */
