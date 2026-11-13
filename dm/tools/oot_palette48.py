#!/usr/bin/env python3
"""Closed, SHA-gated palette48 rewrite for five locally owned translated sources.

No shader bodies are bundled: the scalar fallback is extracted from INPUT.
This tool performs no compilation, installation, networking, or collection.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import sys

class ContractError(ValueError):
    pass

def require(condition, message):
    if not condition:
        raise ContractError(message)
CONTRACTS = (
    (
        'e3d81ddfed3e0ec8_b',
        '269e49800449ce5af699b657dc7b36907603a0617425c0bfb39b5b2c74fb3500',
        '23efc75b2e0d8489b90614cf0a980d2f1d72b38c4a75e5ee55b4ef4bbd25833c',
    ),
    (
        'fbc55ea8f49e4772_b',
        '269d4343d65072cff50bb57716a0ee2895c862392f5b4327cd9d033853baa676',
        '2f89684fcb3465959131a8ed6f0adc60b3ff597e30fea5d9f4b6f7561ab953b3',
    ),
    (
        '566c7e0c6e8c5dc5_b',
        '7011579cfe9562b0d9d09b8f0a63cfa9abdaf235cb0f300e75f879e3a3a1784a',
        '7e8676e4e19a75af7f16550f81a166bcfe1e4ad41936183f831692075ce86e72',
    ),
    (
        '090e3ef632e3222a_b',
        '1449f006f9f76c74850f71bcdff8ea0b7381c8e6813ef411c4db873c23a98e1f',
        'fdc15260fe272ee9456cf3e64cc61523dc434f1721e5057bcbb8031c08a60299',
    ),
    (
        '371fbb2aa4022b42_b',
        'af538920f55c699574767dd190d318d4675dc9f067d4346be28c1a6e9f1a05dd',
        'b1be7d54a7775fb5c3d93aca15f81451e6b512be580b8404dbfcb6dc19609006',
    ),
)

def sha256(data):
    return hashlib.sha256(data).hexdigest()
DECL = re.compile('^    (int|uint|bool|float) (_\\d+) = (.*);$', re.M)
LOAD = re.compile('^    float (_\\d+) = select\\(0\\.0, \\(\\*spvDescriptorSet0\\.(\\w+)\\)\\._m0\\[(_\\d+) \\? (_\\d+) : 0\\]\\[(_\\d+)\\], \\3\\);$', re.M)
ROOT = re.compile('^    int (_\\d+) = as_type<int>\\((_\\d+)\\) \\* 48;$', re.M)
refs = lambda x: set(re.findall('\\b_\\d+\\b', x))
u32 = lambda x: int(x) & 4294967295
i32 = lambda x: (u32(x) ^ 2147483648) - 2147483648

def analyse(t):
    ds = {m[2]: (m[1], m[3], m[0]) for m in DECL.finditer(t)}
    roots = tuple((m[1] for m in ROOT.finditer(t)))

    def rootdeps(v):
        if v in roots:
            return {v}
        return set().union(*(rootdeps(x) for x in refs(ds[v][1]) if x in ds)) if v in ds else set()

    def val(v, root, base):
        if v == root:
            return i32(base)
        ty, expr, _ = ds[v]
        env = {x: val(x, root, base) for x in refs(expr)}
        # Expression analysis only runs after the exact original SHA gate.
        z = eval(expr, {'__builtins__': {}, 'uint': u32, 'int': i32}, env)
        return u32(z) if ty == 'uint' else i32(z)
    groups = {r: [] for r in roots}
    for m in LOAD.finditer(t):
        dep = rootdeps(m[4]) | rootdeps(m[5])
        require(len(dep) == 1, 'ambiguous palette root')
        root = next(iter(dep))
        off = val(m[4], root, 0) * 4 + val(m[5], root, 0)
        require(0 <= off < 12, 'component outside record')
        for base in [0, 16, 32, 48, 2147483632, 2147483648, 4294967264, 4294967280, 4294967232, 305419888]:
            got = val(m[4], root, base) * 4 + val(m[5], root, base)
            require(got == u32(base + off * 4) // 4, 'address contract mismatch')
        groups[root].append((off, m))
    for r, ls in groups.items():
        require(sorted((x[0] for x in ls)) == list(range(12)), 'record does not contain exactly twelve components')
    return (ds, groups)

def transform(source_bytes):
    """Return (exact transformed UTF-8 bytes, source-free verification report).

    Unknown bytes are rejected before decoding or analysing expressions.
    """
    source_sha = sha256(source_bytes)
    matches = [row for row in CONTRACTS if row[1] == source_sha]
    require(len(matches) == 1, 'input SHA256 is not one of the five allowed originals')
    key, _, expected_output_sha = matches[0]
    original = source_bytes.decode('utf-8')
    starts = list(re.finditer('(?m)^vertex ', original))
    require(len(starts) == 3, 'expected three original entries')
    chunks = []
    helper = None
    counts = []
    for ei, m in enumerate(starts):
        start = m.start()
        end = starts[ei + 1].start() if ei + 1 < len(starts) else len(original)
        t = original[start:end]
        ds, groups = analyse(t)
        if helper is None:
            root, ls = next(iter(groups.items()))
            ls = sorted(ls, key=lambda x: x[0])
            palette = ls[0][1][2]
            cb = ds[ls[0][1][3]][1]
            sizes = re.findall('\\(\\(\\*spvDescriptorSet0\\.(\\w+)\\)\\._m0\\[(\\d+)\\]\\.([xyzw]) >> 2u\\)', cb)
            require(len(sizes) == 1, 'ambiguous cbsize expression')
            sz = sizes[0]
            sizeexpr = f'((*spvDescriptorSet0.{sz[0]})._m0[{sz[1]}].{sz[2]} >> 2u)'
            needed = set()

            def need(v):
                if v == root or v in needed:
                    return
                require(v in ds, 'unknown dependency')
                for x in refs(ds[v][1]):
                    need(x)
                needed.add(v)
            for _, lm in ls:
                need(lm[1])
            fallback = '\n'.join((d[2] for v, d in ds.items() if v in needed))
            components = [lm[1] for _, lm in ls]
            helper = '''// DM_PALETTE48_BEGIN: exact scalar fallback retained from this SHA-gated source.
struct dm_palette48 { float4 row[3]; };
static __attribute__((always_inline)) dm_palette48 dm_palette48_load(constant spvDescriptorSetBuffer0& spvDescriptorSet0, int ROOT)
{
    dm_palette48 result;
    uint dm_base = uint(ROOT) >> 2u;
    uint dm_words = SIZE;
    if (dm_base <= dm_words && dm_words - dm_base >= 12u)
    {
        uint dm_row = dm_base >> 2u;
        result.row[0] = (*spvDescriptorSet0.PALETTE)._m0[dm_row];
        result.row[1] = (*spvDescriptorSet0.PALETTE)._m0[dm_row + 1u];
        result.row[2] = (*spvDescriptorSet0.PALETTE)._m0[dm_row + 2u];
        return result;
    }
FALLBACK
    result.row[0] = float4(C0);
    result.row[1] = float4(C1);
    result.row[2] = float4(C2);
    return result;
}
// DM_PALETTE48_END

'''.replace('ROOT', root).replace('SIZE', sizeexpr).replace('spvDescriptorSet0.PALETTE', 'spvDescriptorSet0.' + palette).replace('FALLBACK', fallback)
            for k in range(3):
                helper = helper.replace('C' + str(k), ', '.join(components[k * 4:k * 4 + 4]))
        for _, gls in groups.items():
            for _, gm in gls:
                require(gm[2] == palette, 'palette binding changed')
                require(sizeexpr in ds[gm[3]][1], 'cbsize binding changed')
        original_fmas = [l for l in t.splitlines() if 'fma(' in l]
        old = t
        dead = set()
        for gi, (root, ls) in enumerate(groups.items()):
            rn = 'dm_record_' + str(gi)
            rootline = ds[root][2]
            t = t.replace(rootline, rootline + f'\n    dm_palette48 {rn} = dm_palette48_load(spvDescriptorSet0, {root});', 1)

            def collect(v):
                if v == root or v in dead:
                    return
                if v not in ds:
                    return
                dead.add(v)
                for x in refs(ds[v][1]):
                    collect(x)
            for off, lm in ls:
                for x in refs(ds[lm[1]][1]):
                    collect(x)
                t = t.replace(lm[0], f'    float {lm[1]} = {rn}.row[{off // 4}][{off % 4}];', 1)
        while True:
            removed = False
            for v in sorted(dead):
                if v in ds and len(re.findall('\\b' + v + '\\b', t)) == 1:
                    require(ds[v][2] in t, 'missing removable declaration')
                    t = t.replace(ds[v][2] + '\n', '', 1)
                    removed = True
            if not removed:
                break
        require([l for l in t.splitlines() if 'fma(' in l] == original_fmas, 'FMA text changed')
        oldfp = [l for l in old.splitlines() if re.match('    float(?:[234])? _\\d+ = ', l) and (not LOAD.fullmatch(l))]
        newfp = [l for l in t.splitlines() if re.match('    float(?:[234])? _\\d+ = ', l) and (not re.match('    float _\\d+ = dm_record_', l))]
        require(oldfp == newfp, 'non-palette floating-point text changed')
        counts.append({'entry': ei, 'records': len(groups), 'scalar_loads': sum((len(ls) for ls in groups.values())), 'fma_lines_identical': True, 'other_float_lines_identical': True})
        chunks.append((start, end, t, old))
    candidate = original
    for start, end, t, old in reversed(chunks):
        candidate = candidate[:start] + t + candidate[end:]
    candidate = candidate[:starts[0].start()] + helper + candidate[starts[0].start():]
    inverse = candidate.replace(helper, '', 1)
    for start, end, t, old in chunks:
        require(inverse.count(t) == 1, 'inverse chunk is ambiguous')
        inverse = inverse.replace(t, old, 1)
    require(inverse == original, 'inverse differs from input')
    output = candidate.encode('utf-8')
    require(sha256(output) == expected_output_sha, 'output differs from the closed compiled-source contract')
    report = {'schema': 1, 'shader_key': key, 'original_sha256': source_sha, 'output_sha256': sha256(output), 'expected_output_sha256': expected_output_sha, 'inverse_byte_exact': True, 'entries': counts, 'helper_scalar_fallback_reads': 12, 'helper_fast_float4_loads': 3, 'always_inline': True, 'compiled_by_this_tool': False}
    return (output, report)

def _new_path(path, label):
    require(not os.path.lexists(path), label + ' already exists; refusing to overwrite')
    require(path.parent.is_dir(), label + ' parent directory does not exist')

def write_result(input_path, output_path, report_path=None):
    """Write new files only. On failure, remove only files created by this call."""
    input_path, output_path = (Path(input_path), Path(output_path))
    report_path = Path(report_path) if report_path is not None else None
    require(input_path.resolve() != output_path.resolve(), 'input and output must be different files')
    _new_path(output_path, 'output')
    if report_path is not None:
        require(report_path.resolve() not in (input_path.resolve(), output_path.resolve()), 'report must be a separate new file')
        _new_path(report_path, 'report')
    output, report = transform(input_path.read_bytes())
    created = []
    try:
        with output_path.open('xb') as stream:
            created.append(output_path)
            stream.write(output)
        if report_path is not None:
            with report_path.open('x', encoding='utf-8', newline='\n') as stream:
                created.append(report_path)
                json.dump(report, stream, indent=2)
                stream.write('\n')
    except BaseException:
        for path in reversed(created):
            path.unlink(missing_ok=True)
        raise
    return report

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, metavar='INPUT.metal')
    parser.add_argument('output', type=Path, metavar='OUTPUT.metal')
    parser.add_argument('--report', type=Path, metavar='REPORT.json', help='write a new source-free proof report')
    args = parser.parse_args(argv)
    try:
        report = write_result(args.input, args.output, args.report)
    except (OSError, UnicodeError, ContractError) as error:
        parser.exit(2, 'error: ' + str(error) + '\n')
    print(json.dumps(report, sort_keys=True))
    return 0
if __name__ == '__main__':
    sys.exit(main())
