"""Validate packaged SDK fixture structure without importing its generator."""
from collections import Counter
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def chunks(data):
    result, position = {}, 0
    while position < len(data):
        assert len(data) - position >= 8, 'Truncated chunk header'
        identifier, size = struct.unpack_from('<II', data, position)
        assert identifier not in result, 'Duplicate chunk identifier'
        position += 8
        assert size <= len(data) - position, 'Truncated chunk data'
        result[identifier] = data[position:position + size]
        position += size
    return result


def string(data, offset):
    end = data.index(b'\0', offset)
    return data[offset:end].decode('ascii'), end + 1


def validate(root):
    manifest = json.loads((root / 'ai-fixture-manifest.json').read_text())
    for name, metadata in manifest.items():
        data = (root / name).read_bytes()
        assert len(data) == metadata['size']
        assert hashlib.sha256(data).hexdigest() == metadata['sha256']
    body = chunks(chunks((root / 'rawdata/objects/ai_test/cube.object').read_bytes())[0x7777])
    assert body[0x0900] == b'\x10\0'
    assert u32(body[0x0903]) == 0  # Static model.
    surfaces = body[0x0907]
    assert u32(surfaces) == 2
    position, names, textures = 4, [], []
    for _ in range(2):
        fields = []
        for _ in range(6):
            value, position = string(surfaces, position)
            fields.append(value)
        flags, fvf, coordinates = struct.unpack_from('<III', surfaces, position)
        position += 12
        assert flags == 1 and fvf == 0x112 and coordinates == 1
        names.append(fields[0])
        textures.append(fields[4])
    assert position == len(surfaces)
    assert names == ['left_half', 'right_half']
    assert textures == [r'ai_test\checker', r'ai_test\checker']
    mesh = chunks(chunks(body[0x0910])[0])
    assert mesh[0x1000] == b'\x11\0'
    assert u32(mesh[0x1005]) == 8 and u32(mesh[0x1006]) == 12
    vertices = list(struct.iter_unpack('<fff', mesh[0x1005][4:]))
    assert max(v[0] for v in vertices) - min(v[0] for v in vertices) == 1
    assert min(v[1] for v in vertices) == 0 and max(v[1] for v in vertices) == 1
    assert max(v[2] for v in vertices) - min(v[2] for v in vertices) == 1
    pairs = list(struct.iter_unpack('<ii', mesh[0x1006][4:]))
    assert len(pairs) == 36
    assert all(0 <= vertex < 8 and reference == index for index, (vertex, reference) in enumerate(pairs))
    edges, signed_volume = Counter(), 0.
    for index in range(0, 36, 3):
        ids = [pairs[index + corner][0] for corner in range(3)]
        assert len(set(ids)) == 3
        for a, b in zip(ids, ids[1:] + ids[:1]):
            edges[tuple(sorted((a, b)))] += 1
        a, b, c = [vertices[i] for i in ids]
        cross = (b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0])
        signed_volume += sum(a[i] * cross[i] for i in range(3)) / 6.
    assert set(edges.values()) == {2}, 'Cube must be a closed manifold'
    assert math.isclose(abs(signed_volume), 1.), 'Cube must enclose one cubic metre'
    assignments = mesh[0x1009]
    assert struct.unpack_from('<H', assignments)[0] == 2
    position, assigned = 2, []
    for expected_name in names:
        actual_name, position = string(assignments, position)
        assert actual_name == expected_name
        count = u32(assignments, position)
        position += 4
        assert count == 6
        assigned.extend(struct.unpack_from('<6I', assignments, position))
        position += 24
    assert sorted(assigned) == list(range(12)) and position == len(assignments)
    assert u32(mesh[0x1008]) == 36 and len(mesh[0x1008]) == 4 + 36 * 9
    for index in range(36):
        count, vmap, uv_index = struct.unpack_from('<Bii', mesh[0x1008], 4 + 9 * index)
        assert (count, vmap, uv_index) == (1, 0, index)
    level = chunks((root / 'rawdata/levels/ai_test.level').read_bytes())
    assert u32(level[0x9df3]) == 5 and u32(level[0x7712]) == 1
    entry = chunks(chunks(level[0x7708])[0])
    assert u32(entry[0x7703]) == 2
    instance = chunks(entry[0x7777])
    assert string(instance[0x0902], 0)[0] == r'ai_test\cube'
    assert string(instance[0xf907], 0)[0] == 'ai_cube'
    assert struct.unpack('<9f', instance[0xf903]) == (0., 0., 0., 0., 0., 0., 1., 1., 1.)
    for name in ['checker', 'alternate']:
        dds = (root / f'gamedata/textures/ai_test/{name}.dds').read_bytes()
        tga = (root / f'rawdata/textures/ai_test/{name}.tga').read_bytes()
        assert dds[:4] == b'DDS ' and u32(dds, 4) == 124
        assert (u32(dds, 12), u32(dds, 16)) == (16, 16)
        assert len(dds) == 128 + 16 * 16 * 4
        assert struct.unpack_from('<HH', tga, 12) == (16, 16)
        assert tga[2] == 2 and tga[16] == 32 and tga[17] == 0x28
        bgra = tga[18:]
        rgba = b''.join(bytes((bgra[i + 2], bgra[i + 1], bgra[i], bgra[i + 3])) for i in range(0, len(bgra), 4))
        assert dds[128:] == rgba
    return {'ok': True, 'files': len(manifest), 'checks': ['sha256', 'chunk_bounds', 'versions', 'separate_surfaces_same_texture',
              'closed_manifold', 'one_cubic_metre', 'surface_face_partition', 'uv_refs', 'scene_instance_transform', 'dds_tga_pixels']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path, nargs='?', default=Path(__file__).parent / 'content')
    print(json.dumps(validate(parser.parse_args().root)))
