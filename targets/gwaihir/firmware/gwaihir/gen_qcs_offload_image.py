#!/usr/bin/env python3
# Generate a headless QCS job + OFFLOAD_IMAGE for ANY compiled model, by DERIVING the
# dispatch/binding structure from its Flow IR (`iree-compile --compile-to=flow`). No
# per-model hardcoding: dispatches, bindings, intermediates and the output are all read
# from the IR, then the L2-SPM aperture is laid out and emitted as ASCII-hex (one 32-bit
# LE word per line) that the tb backdoors at IMG_PA = L2_BASE + QCS_JOB_DESCRIPTOR_OFFSET.
# QCS ABI: transport/cluster_command_stream.h. Device ptrs are OFFSETS into the aperture.
#
# The wire layout below is a hand-encoding of that header's structs; it is pinned to the
# header's sizes by _Static_assert-equivalent asserts (DESC_BYTES, DISPATCH_FIXED,
# BINDING_BYTES) so a same-version struct change trips here instead of silently corrupting
# device memory. A field REORDER within a struct is NOT caught by a size check -- keep the
# field offsets below in lockstep with cluster_command_stream.h (there is no C twin to
# round-trip against in this standalone generator).
#
#   gen_qcs_offload_image.py --flow model.flow.mlir --input-dir <dir with input{N}.npy>
#                            --out job.hex [--golden-out y.npy]
import argparse
import re
import struct
import numpy as np

QCS_MAGIC = 0x31534351
QCS_VERSION = 2
JOB_ID = 0xC0FFEE
DESC_OFF = 0x10000            # QCS_JOB_DESCRIPTOR_OFFSET
STREAM_OFF = 0x11000
DATA_OFF = 0x20000           # first buffer page; each buffer gets its own 4 KiB page
PAGE = 0x1000
# struct sizes, pinned to cluster_command_stream.h (sizeof qcs_job_descriptor_t / the
# fixed head of qcs_dispatch_t before bindings[] / sizeof qcs_binding_t).
DESC_BYTES = 0x38
DISPATCH_FIXED = 0x40
BINDING_BYTES = 0x10

# f64 device kernels today; the parsed element type drives both the buffer size and the
# seeded numpy dtype so an f32/f16 model can't overflow its page or halve its binding length.
DTYPE = {'f64': ('<f8', 8), 'f32': ('<f4', 4), 'f16': ('<f2', 2)}


def dtype_of(ty):
    if ty not in DTYPE:
        raise SystemExit(f'unsupported element type {ty!r} (add it to DTYPE with its numpy dtype)')
    return DTYPE[ty]


def elems(shape):  # "16x16" -> 256, "16" -> 16
    n = 1
    for d in shape.split('x'):
        n *= int(d)
    return n


def parse_flow(text):
    # inputs: %ssa = hal.tensor.import %argK "inputN" ... -> tensor<SHAPExTYPE>
    inputs = {}  # ssa -> (name, elem_dtype, bytes)
    for m in re.finditer(r'(%\w+)\s*=\s*hal\.tensor\.import\s+%\w+\s+"(input\d+)"[^>]*->\s*tensor<([0-9x]+)x(\w+)>', text):
        ssa, name, shape, ty = m.groups()
        npty, width = dtype_of(ty)
        inputs[ssa] = (name, npty, elems(shape) * width)
    # dispatches: %res = flow.dispatch @exec::@NAME_dispatch_D_...(%ops) : (tys) -> tensor<SHAPExTY>
    disp = []  # (ordinal, [operand ssas], result ssa, result_bytes)
    for m in re.finditer(r'(%\w+)\s*=\s*flow\.dispatch\s+@\w+::@\w*?dispatch_(\d+)\w*\(([^)]*)\)\s*:\s*\([^)]*\)\s*->\s*tensor<([0-9x]+)x(\w+)>', text):
        res, ordn, ops, shape, ty = m.groups()
        _, width = dtype_of(ty)
        operands = [o.strip() for o in ops.split(',') if o.strip()]
        disp.append((int(ordn), operands, res, elems(shape) * width))
    disp.sort(key=lambda d: d[0])
    # Fail loud on anything the regexes could silently drop (dynamic dims, multi-result,
    # 0-D tensors): the parsed counts must match the raw op counts, else a binding is missing.
    if len(inputs) != text.count('hal.tensor.import'):
        raise SystemExit('parsed fewer hal.tensor.import than present (dynamic/unsupported shape?)')
    if len(disp) != text.count('flow.dispatch'):
        raise SystemExit('parsed fewer flow.dispatch than present (multi-result/dynamic shape?)')
    if not disp:
        raise SystemExit('no flow.dispatch found -- is this --compile-to=flow IR?')
    consumed = {o for d in disp for o in d[1]}
    outputs = [d[2] for d in disp if d[2] not in consumed]  # dispatch result not fed onward
    return inputs, disp, outputs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--flow', required=True)
    ap.add_argument('--input-dir', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    assert DESC_BYTES == 0x38 and DISPATCH_FIXED == 0x40 and BINDING_BYTES == 0x10, \
        'wire-layout sizes drifted from cluster_command_stream.h'

    with open(a.flow) as fp:
        text = fp.read()
    inputs, disp, outputs = parse_flow(text)

    # Assign each distinct buffer (input / intermediate / output) its own L2 page.
    order = list(inputs)
    for _, _, res, _ in disp:
        if res not in order:
            order.append(res)
    off = {ssa: DATA_OFF + i * PAGE for i, ssa in enumerate(order)}
    size = {ssa: b for ssa, (_, _, b) in inputs.items()}
    for _, _, res, b in disp:
        size[res] = b
    end = max(off[s] + size[s] for s in order)

    img = bytearray(((end - DESC_OFF + PAGE - 1) // PAGE) * PAGE)

    def put(o, data):
        img[o - DESC_OFF:o - DESC_OFF + len(data)] = data

    # command stream: one DISPATCH record per dispatch, bindings = operands + result
    stream = bytearray()
    for ordn, ops, res, _ in disp:
        binds = ops + [res]
        rec = bytearray(DISPATCH_FIXED + len(binds) * BINDING_BYTES)
        struct.pack_into('<II', rec, 0x00, 1, len(rec))       # type=DISPATCH, size
        struct.pack_into('<II', rec, 0x08, 0, ordn)           # executable_id=0, export_ordinal
        struct.pack_into('<III', rec, 0x14, 1, 1, 1)          # workgroup_count
        struct.pack_into('<I', rec, 0x3c, len(binds))         # binding_count
        for i, s in enumerate(binds):
            struct.pack_into('<QQ', rec, DISPATCH_FIXED + i * BINDING_BYTES, off[s], size[s])
        stream += rec
    if STREAM_OFF + len(stream) > DATA_OFF:
        raise SystemExit(f'command stream ({len(stream)} B) overruns the data window at {DATA_OFF:#x}')
    put(STREAM_OFF, stream)

    desc = bytearray(DESC_BYTES)
    struct.pack_into('<IIII', desc, 0x00, QCS_MAGIC, QCS_VERSION, 0, JOB_ID)
    struct.pack_into('<I', desc, 0x18, len(disp))             # record_count
    struct.pack_into('<Q', desc, 0x28, STREAM_OFF)            # cmd_stream_ptr (offset)
    struct.pack_into('<Q', desc, 0x30, len(stream))
    put(DESC_OFF, desc)

    # seed the inputs from input{N}.npy; intermediates/outputs left zero
    for ssa, (name, npty, _) in inputs.items():
        arr = np.load(f'{a.input_dir}/{name}.npy').astype(npty)
        put(off[ssa], arr.tobytes())

    with open(a.out, 'w') as fp:
        for i in range(0, len(img), 4):
            fp.write('%08x\n' % struct.unpack_from('<I', img, i)[0])

    print(f'{a.out}: {len(img)} bytes, {len(disp)} dispatch(es), {len(order)} buffers')
    for ordn, ops, res, _ in disp:
        b = [(inputs.get(s, (s,))[0], hex(off[s])) for s in ops + [res]]
        print(f'  dispatch ord={ordn} bindings={b}')
    print(f'  output(s) at: {[hex(off[o]) for o in outputs]}  (read back for verify)')


if __name__ == '__main__':
    main()
