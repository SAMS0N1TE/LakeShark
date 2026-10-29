"""components/ls_textpack/gen/lszstd.py, the build's zstd frame writer.

XXH64 against known answers always; with Python 3.14+ (compression.zstd,
libzstd) every frame is also decoded and compared byte for byte, over the
cases that exercise each path: raw and compressed blocks, raw / direct /
FSE-coded Huffman weights, one and four literal streams, repeat offsets,
predefined and custom sequence tables, frame sizes up to the 64 KiB CZ7
window. The C side (CartoCore's decoder) is test_ls_textpack."""

import os
import random
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'components', 'ls_textpack', 'gen'))
import lszstd  # noqa: E402

try:
    from compression import zstd as libzstd
except ImportError:
    libzstd = None


def cases():
    rnd = random.Random(7)
    text = (b"usage: lora lrprobe <kHz 30000..3800000> [bw 31250|62500|125000|250000] "
            b"[gain 0..13, 14 auto]\nP25 control-channel state: IDEN, neighbours\n")
    uk = 'Українська, ШП, ' \
         'НАЛАШТ. '.encode('utf-8')
    yield b'a' * 40
    yield bytes(range(32, 127)) * 3
    yield text[:255]
    yield text * 2
    yield text * 40
    yield (text + uk) * 30
    yield uk * 400
    yield bytes(rnd.randrange(256) for _ in range(3000))          # incompressible
    yield bytes(rnd.choice(b'ab\x80\xff') for _ in range(5000))
    yield bytes(range(256)) * 20                                   # all 256 symbols
    yield b'x' * 20000                                             # one symbol, long runs
    big = bytearray()
    while len(big) < 65536:
        big += text[rnd.randrange(len(text) // 2):] + uk[:rnd.randrange(len(uk))]
    yield bytes(big[:65536])


class Lszstd(unittest.TestCase):
    def test_xxh64_known_answers(self):
        self.assertEqual(lszstd.xxh64(b''), 0xEF46DB3751D8E999)
        self.assertEqual(lszstd.xxh64(b'a'), 0xD24EC4F1A98C6E5B)

    def test_cz7_header(self):
        z = lszstd.cz7(b'0123456789' * 10)
        self.assertEqual(z[:4], b'CZ7\0')
        self.assertEqual(int.from_bytes(z[4:8], 'little'), 100)

    @unittest.skipIf(libzstd is None, 'needs Python 3.14 compression.zstd')
    def test_libzstd_decodes_every_frame(self):
        for data in cases():
            frame = lszstd.compress_frame(data)
            self.assertEqual(libzstd.decompress(frame), data, len(data))


if __name__ == '__main__':
    unittest.main()
