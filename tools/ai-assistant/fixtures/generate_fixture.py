"""Deterministic, original one-metre cube + DDS/TGA + binary SDK scene.

Writes only to a chosen staging directory. These files use the public SDK
chunk format from EditObjectIO.cpp, EditMeshIO.cpp and SceneObjectIO.cpp.
No third party game asset is copied into this fixture.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct


def p(fmt, *values):
    return struct.pack('<' + fmt, *values)


def z(text):
    return text.encode('ascii') + b'\0'


def chunk(identifier, data):
    return p('II', identifier, len(data)) + data


def chunks(*entries):
    return b''.join(chunk(identifier, data) for identifier, data in entries)


def cube_object():
    vertices = [(-.5, 0, -.5), (.5, 0, -.5), (.5, 1, -.5), (-.5, 1, -.5),
                (-.5, 0, .5), (.5, 0, .5), (.5, 1, .5), (-.5, 1, .5)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 4, 7, 3),
             (1, 2, 6, 5), (3, 7, 6, 2), (0, 1, 5, 4)]
    face_vertices, uvs = [], []
    corner_uvs = [(0., 1.), (0., 0.), (1., 0.), (1., 1.)]
    for quad in quads:
        for triangle in [(0, 1, 2), (0, 2, 3)]:
            for corner in triangle:
                face_vertices.append(quad[corner])
                uvs.append(corner_uvs[corner])
    faces = b''.join(p('ii', vertex, index) for index, vertex in enumerate(face_vertices))
    vmrefs = p('I', 36) + b''.join(p('Bii', 1, 0, index) for index in range(36))
    vmap = p('I', 1) + z('Texture') + p('BBBI', 2, 1, 0, 36)
    vmap += b''.join(p('ff', *uv) for uv in uvs)
    vmap += b''.join(p('i', index) for index in face_vertices)
    vmap += b''.join(p('i', index // 3) for index in range(36))
    surf_faces = p('H', 2)
    for surface, indices in [('left_half', range(6)), ('right_half', range(6, 12))]:
        surf_faces += z(surface) + p('I', 6) + b''.join(p('I', index) for index in indices)
    mesh = chunks((0x1000, p('H', 0x11)), (0x1001, z('ai_cube_mesh')),
                  (0x1002, p('B', 9)), (0x1004, p('6f', -.5, 0, -.5, .5, 1, .5)),
                  (0x1005, p('I', 8) + b''.join(p('fff', *vertex) for vertex in vertices)),
                  (0x1006, p('I', 12) + faces), (0x1008, vmrefs),
                  (0x1009, surf_faces), (0x1012, vmap))
    surfaces = p('I', 2)
    for name in ['left_half', 'right_half']:
        surfaces += b''.join(z(value) for value in [name, r'def_shaders\def_trans_v',
                            r'def_shaders\def_vertex', 'default', r'ai_test\checker', 'Texture'])
        surfaces += p('III', 1, 0x112, 1)
    return chunk(0x7777, chunks((0x0900, p('H', 0x10)), (0x0903, p('I', 0)),
                                (0x0907, surfaces), (0x0910, chunk(0, mesh))))


def scene():
    level_options = chunks((0x7801, p('I', 12)), (0x7802, z('ai_test')),
                           (0x7804, z('')), (0x7803, z('')), (0x7853, z('1.0')),
                           (0x7849, p('I', 9)), (0x7851, p('BB', 1, 1)), (0x7852, p('H', 1)))
    instance = chunks((0xF906, p('I', 0)), (0xF907, z('ai_cube')),
                      (0xF903, p('9f', 0, 0, 0, 0, 0, 0, 1, 1, 1)),
                      (0x0900, p('H', 0x12)), (0x0902, z(r'ai_test\cube')),
                      (0x0905, p('I', 0)))
    object_entry = chunks((0x7703, p('I', 2)), (0x7777, instance))
    return chunks((0x9DF3, p('I', 5)), (0x7711, level_options),
                  (0x7709, p('6f', .5, -.6, 0, 2.5, 2.5, -3.5)),
                  (0x7000, bytes.fromhex('4958524159414954455354435542453031')),
                  (0x7712, p('I', 1)), (0x7708, chunk(0, object_entry)))


def image_pixels(alternate=False):
    colors = [(35, 180, 240, 255), (240, 235, 220, 255)]
    if alternate:
        colors = [(240, 90, 50, 255), (100, 35, 190, 255)]
    return b''.join(bytes(colors[((x // 4) + (y // 4)) % 2]) for y in range(16) for x in range(16))


def dds(pixels):
    # DDS RGB(A), four uncompressed masks; DX11/DirectXTex supports this format.
    header = p('7I', 124, 0x100F, 16, 16, 16 * 4, 0, 1) + bytes(44)
    header += p('8I', 32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    header += p('5I', 0x1000, 0, 0, 0, 0)
    assert len(header) == 124
    return b'DDS ' + header + pixels


def tga(pixels):
    bgra = b''.join(bytes((pixels[index + 2], pixels[index + 1], pixels[index], pixels[index + 3]))
                    for index in range(0, len(pixels), 4))
    return p('BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, 16, 16, 32, 0x28) + bgra


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = {'rawdata/objects/ai_test/cube.object': cube_object(),
             'rawdata/levels/ai_test.level': scene()}
    for name, alternate in [('checker', False), ('alternate', True)]:
        pixels = image_pixels(alternate)
        files[f'gamedata/textures/ai_test/{name}.dds'] = dds(pixels)
        files[f'rawdata/textures/ai_test/{name}.tga'] = tga(pixels)
    for relative, data in files.items():
        target = args.output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    manifest = {name: {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in files.items()}
    target = args.output / 'ai-fixture-manifest.json'
    target.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'files': len(files), 'manifest': str(target)}))


if __name__ == '__main__':
    main()
