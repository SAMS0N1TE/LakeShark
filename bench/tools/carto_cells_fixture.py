"""Run the upstream fixture builder; Python 3.14 also supplies Zstd in stdlib."""
import runpy
import sys
import types
import struct
try:
    import zstandard
except ModuleNotFoundError:
    from compression import zstd
    class Parameters:
        @staticmethod
        def from_level(level, window_log, write_checksum):
            p = zstd.CompressionParameter
            return {p.compression_level: level, p.window_log: window_log,
                    p.checksum_flag: write_checksum}
    class Compressor:
        def __init__(self, compression_params):
            self.options = compression_params
        def compress(self, data):
            return zstd.compress(data, options=self.options)
    sys.modules['zstandard'] = types.SimpleNamespace(
        ZstdCompressionParameters=Parameters, ZstdCompressor=Compressor)
# The fixture needs only a deterministic CTILE container writer. Keep the
# deployment builder and its optional host dependencies out of the vendor.
def write_map(path, template, tiles, zmin, zmax):
    tiles = sorted(tiles, key=lambda t: t[:3])
    header = bytearray(template)
    header[28:30] = bytes((zmin, zmax))
    struct.pack_into('<I', header, 32, len(tiles))
    offset = 64 + 24 * len(tiles)
    directory = bytearray()
    blobs = bytearray()
    for z, x, y, tile in tiles:
        directory += struct.pack('<B3xIIQI', z, x, y, offset, len(tile))
        blobs += tile
        offset += len(tile)
    struct.pack_into('<Q', header, 48, offset)
    path.write_bytes(header + directory + blobs)
    return header, directory

sys.modules['build_cells'] = types.SimpleNamespace(write_map=write_map)
script = sys.argv.pop(1)
sys.argv[0] = script
runpy.run_path(script, run_name='__main__')
