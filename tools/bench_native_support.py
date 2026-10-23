"""Native benchmark bindings and complete rendered-grid verification.

Timing excludes Python orchestration. Used only by native_bench.py.
"""
import ctypes as C
from pathlib import Path
import statistics
import time
import math
import coding_bench as cb

class Buffer(C.Structure):
    _fields_ = [('data', C.c_void_p), ('size', C.c_size_t), ('capacity', C.c_size_t)]

def bindings(lib):
    sig = {
        'kmx_render_create': ([C.POINTER(C.c_void_p)], C.c_int),
        'kmx_render_free': ([C.c_void_p], None),
        'kmx_render_frame': ([C.c_void_p, C.POINTER(cb.Grid), C.POINTER(Buffer)], C.c_int),
        'kmx_render_invalidate': ([C.c_void_p], None),
        'kmx_buffer_free': ([C.POINTER(Buffer)], None),
        'kmx_buffer_reset': ([C.POINTER(Buffer)], None),
        'kmx_term_create': ([C.POINTER(C.c_void_p), C.c_int, C.c_int], C.c_int),
        'kmx_term_feed': ([C.c_void_p, C.c_void_p, C.c_size_t], C.c_int),
        'kmx_term_snapshot': ([C.c_void_p, C.POINTER(cb.Grid)], C.c_int),
        'kmx_term_free': ([C.c_void_p], None),
        'kmx_grid_free': ([C.POINTER(cb.Grid)], None),
        'kmx_grid_equal': ([C.POINTER(cb.Grid), C.POINTER(cb.Grid)], C.c_bool),
        'kmx_sync_create': ([C.POINTER(C.c_void_p), C.c_int, C.c_int], C.c_int),
        'kmx_sync_feed': ([C.c_void_p, C.c_void_p, C.c_size_t], C.c_int),
        'kmx_sync_poll': ([C.c_void_p, C.c_uint64, C.POINTER(Buffer), C.POINTER(C.c_bool), C.c_void_p], C.c_int),
        'kmx_sync_ack_at': ([C.c_void_p, C.c_uint64, C.c_uint64], C.c_int),
        'kmx_sync_set_interval': ([C.c_void_p, C.c_uint], None),
        'kmx_sync_current': ([C.c_void_p], C.POINTER(cb.Grid)),
        'kmx_sync_free': ([C.c_void_p], None),
    }
    for name, (args, res) in sig.items():
        fn = getattr(lib, name)
        fn.argtypes = args
        fn.restype = res
    return lib

def distribution(values):
    if not values:
        return {}
    s = sorted(values)
    return {
        'n': len(s),
        'mean_ms': statistics.mean(s),
        'p50_ms': statistics.median(s),
        'p95_ms': s[math.ceil(len(s) * 0.95) - 1],
        'max_ms': max(s),
        'samples_ms': values,
    }
visible = None

def load_comparator(path):
    global visible
    visible = C.CDLL(str(Path(path).resolve()))
    visible.visible_equal.argtypes = [C.POINTER(cb.Grid), C.POINTER(cb.Grid)]
    visible.visible_equal.restype = C.c_int
stats = {}

def init_stats():
    stats.clear()
    stats.update(decode=[], render=[], terminal_feed=[], verify=[], terminal_bytes=0, frames=0)

class RenderedReceiver(cb.Receiver):

    def __init__(self, lib, rows, cols):
        super().__init__(bindings(lib), rows, cols)
        self.render = C.c_void_p()
        self.term = C.c_void_p()
        self.output = Buffer()
        self.grid = cb.Grid()
        self.check(lib.kmx_render_create(C.byref(self.render)))
        self.check(lib.kmx_term_create(C.byref(self.term), rows, cols))

    def apply(self, payload):
        t = time.perf_counter_ns()
        seq = super().apply(payload)
        stats['decode'].append((time.perf_counter_ns() - t) / 1000000.0)
        grid = self.lib.kmx_receiver_grid(self.handle)
        self.lib.kmx_buffer_reset(C.byref(self.output))
        t = time.perf_counter_ns()
        self.check(self.lib.kmx_render_frame(self.render, grid, C.byref(self.output)))
        stats['render'].append((time.perf_counter_ns() - t) / 1000000.0)
        t = time.perf_counter_ns()
        self.check(self.lib.kmx_term_feed(self.term, self.output.data, self.output.size))
        stats['terminal_feed'].append((time.perf_counter_ns() - t) / 1000000.0)
        t = time.perf_counter_ns()
        self.check(self.lib.kmx_term_snapshot(self.term, C.byref(self.grid)))
        if not visible.visible_equal(grid, C.byref(self.grid)):
            a = grid.contents
            b = self.grid
            cells = []
            for i in range(a.rows * a.cols):
                if bytes(a.cells[i]) != bytes(b.cells[i]):
                    cells.append((i, list(a.cells[i].chars), list(b.cells[i].chars), a.cells[i].attrs, b.cells[i].attrs))
                    if len(cells) >= 3:
                        break
            raise AssertionError(f'render mismatch: cursor={(a.cursor_row, a.cursor_col, a.cursor_visible)}/{(b.cursor_row, b.cursor_col, b.cursor_visible)} cells={cells}')
        stats['verify'].append((time.perf_counter_ns() - t) / 1000000.0)
        stats['frames'] += 1
        stats['terminal_bytes'] += self.output.size
        return seq

    def close(self):
        self.lib.kmx_grid_free(C.byref(self.grid))
        self.lib.kmx_buffer_free(C.byref(self.output))
        self.lib.kmx_render_free(self.render)
        self.lib.kmx_term_free(self.term)
        super().close()

def metrics():
    return {k: distribution(v) if isinstance(v, list) else v for k, v in stats.items()}
