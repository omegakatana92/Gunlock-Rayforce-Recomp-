import ctypes, sys, os
_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libdis.so'))
_buf = ctypes.create_string_buffer(256)
_rom = None
def load(rom, cpu=0):
    global _rom
    _rom = ctypes.create_string_buffer(bytes(rom), len(rom))
    _lib.dis_set_rom(_rom, len(rom))
    _lib.dis_set_cpu(cpu)
def dis(pc):
    n = _lib.dis(pc, _buf)
    return n, _buf.value.decode()
def valid(op): return _lib.dis_valid(op) != 0
if __name__ == '__main__':
    rom = open(sys.argv[1],'rb').read(); load(rom, int(sys.argv[4]) if len(sys.argv) > 4 else 2)
    pc = int(sys.argv[2],16); cnt=int(sys.argv[3])
    for i in range(cnt):
        n,t = dis(pc); print('%06x  %s' % (pc, t)); pc += n
