"""Generate original, minimal weather meshes for a standalone IX-Ray SDK test installation.

Only used when a stock DM does not exist. Binary layout follows CDetail::Load
and IRender_DetailModel::fvfVertexIn in the pinned IX-Ray develop revision.
The render shader and gradient texture are from the SDK assets.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import re
import struct

SHADER = r'effects\lightning'
TEXTURE = r'fx\fx_thunderbolts_gradient'
MODELS = [f'dm_lightning-{i:02}.dm' for i in range(1, 5)] + [f'dm_lightning_stancia_{i:02}.dm' for i in range(1, 4)]
PATTERN = (0, 80, -55, 145, 10, -125, 70, -55, 0)


def make_mesh(variant):
    vertices, indices = [], []
    for plane in range(2):
        start = len(vertices)
        for i in range(9):
            x = PATTERN[(i + variant) % 9] / 4096 if i not in (0, 8) else 0
            z = PATTERN[(i * 2 + variant + 1) % 9] / 8192 if i not in (0, 8) else 0
            y = -i / 8
            width = (4, 9, 10, 9, 8, 7, 6, 4, 2)[i] / 4096
            v = .125 + i * .03125
            for side, u in [(-1, .25), (1, .75)]:
                vertices.append((x + side * width if plane == 0 else x,
                                 y, z + side * width if plane == 1 else z, u, v))
            if i < 8:
                a = start + i * 2
                indices.extend((a, a + 2, a + 1, a + 1, a + 2, a + 3))
    return (SHADER.encode('ascii') + b'\0' + TEXTURE.encode('ascii') + b'\0' +
            struct.pack('<IffII', 0, 1., 1., len(vertices), len(indices)) +
            b''.join(struct.pack('<5f', *vertex) for vertex in vertices) +
            struct.pack(f'<{len(indices)}H', *indices))


def validate_mesh(data):
    shader, texture, payload = data.split(b'\0', 2)
    assert shader.decode() == SHADER and texture.decode() == TEXTURE
    flags, minimum, maximum, n_vertices, n_indices = struct.unpack_from('<IffII', payload)
    assert (flags, minimum, maximum) == (0, 1., 1.)
    vertex_end = 20 + n_vertices * 20
    assert n_vertices and n_indices and n_indices % 3 == 0
    assert len(payload) == vertex_end + n_indices * 2
    vertices = list(struct.iter_unpack('<5f', payload[20:vertex_end]))
    indices = struct.unpack(f'<{n_indices}H', payload[vertex_end:])
    assert max(indices) < n_vertices and set(indices) == set(range(n_vertices))
    assert all(math.isfinite(value) for vertex in vertices for value in vertex)
    assert min(v[1] for v in vertices) == -1. and max(v[1] for v in vertices) == 0.
    assert all(abs(x) < .1 and abs(z) < .1 and 0 <= u <= 1 and 0 <= v <= .5 for x, _, z, u, v in vertices)
    for a, b, c in zip(indices[0::3], indices[1::3], indices[2::3]):
        edge1 = [vertices[b][i] - vertices[a][i] for i in range(3)]
        edge2 = [vertices[c][i] - vertices[a][i] for i in range(3)]
        cross = [edge1[(i + 1) % 3] * edge2[(i + 2) % 3] - edge1[(i + 2) % 3] * edge2[(i + 1) % 3] for i in range(3)]
        assert sum(axis * axis for axis in cross) > 0., 'Degenerate triangle'
    return {'vertices': n_vertices, 'indices': n_indices, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--verify', action='store_true', help='Validate existing generated meshes, without writing files')
    parser.add_argument('--configs', type=Path, help='Optional SDK gamedata/configs root for reference coverage check')
    args = parser.parse_args()
    manifest = {'shader': SHADER, 'texture': TEXTURE, 'models': {}}
    if args.configs:
        names = set()
        for source in ['environment/thunderbolts.ltx', 'weathers/thunderbolt.ltx']:
            text = (args.configs / source).read_text(encoding='latin-1')
            names.update(re.findall(r'^\s*lightning_model\s*=\s*dm\\([^\s;]+)', text, re.M | re.I))
        assert names == set(MODELS), f'Unexpected model references: {sorted(names ^ set(MODELS))}'
    for variant, name in enumerate(MODELS):
        relative = f'gamedata/meshes/dm/{name}'
        path = args.output / relative
        if not args.verify:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(make_mesh(variant))
        manifest['models'][relative] = validate_mesh(path.read_bytes())
    path = args.output / 'ai-lightning-manifest.json'
    if args.verify:
        assert json.loads(path.read_text()) == manifest
    else:
        path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'ok': True, 'meshes': len(manifest['models']), 'manifest': str(path)}))


if __name__ == '__main__':
    main()
