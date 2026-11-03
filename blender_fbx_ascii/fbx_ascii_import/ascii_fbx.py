# SPDX-License-Identifier: GPL-2.0-or-later
"""
Parse an ASCII FBX (7.x) file and re-encode it as a binary FBX, so that
Blender's built-in FBX importer (binary only) can load it.

The ASCII format carries no type information, so values are typed the same
way the FBX SDK writes them in binary files (checked against real files).
"""

import base64
import re

import numpy as np

_BINARY_MAGIC = b'Kaydara FBX Binary'

# --------------------------------------------------------------------------
# Type rules

# Property types (2nd field of a "P:" record) whose values are 32-bit ints.
_P_INT32 = {b'int', b'Integer', b'enum', b'bool', b'Bool', b'Visibility Inheritance'}
# ... and 64-bit ints.
_P_INT64 = {b'KTime', b'ULongLong', b'LongLong', b'Long'}
# Everything else numeric in a P record is a double.

# Arrays that must be float64 even when every value happens to be an integer.
_ARRAY_FLOAT64 = {
    'Vertices', 'Normals', 'NormalsW', 'Binormals', 'BinormalsW', 'Tangents', 'TangentsW',
    'UV', 'Colors', 'Weights', 'Transform', 'TransformLink', 'TransformAssociateModel',
    'Matrix', 'FullWeights', 'Points', 'KnotVector', 'KnotVectorU', 'KnotVectorV',
    'EdgeCrease', 'VertexCrease',
}
_ARRAY_FLOAT32 = {'KeyValueFloat', 'KeyAttrDataFloat'}
_ARRAY_INT64 = {'KeyTime'}
_ARRAY_BOOL = {'Visibility', 'Hole'}

# Scalar elements whose numbers are doubles even if written without a decimal point.
_SCALAR_FLOAT64 = {'Default', 'Position', 'Up', 'LookAt', 'CameraOrthoZoom'}
# Scalar elements whose numbers are int64 (object ids, times).
# Elements whose first string is a "Class::Name" object name.
_NAME_FIRST = {'SceneInfo', 'TextureName', 'Media'}
_SCALAR_INT64 = {'RootNode', 'Node', 'LocalTime', 'ReferenceTime'}

_INT32_MIN, _INT32_MAX = -(1 << 31), (1 << 31) - 1

# --------------------------------------------------------------------------
# Tokenizer helpers (index based, on a str)

_RE_SKIP = re.compile(r'(?:\s+|;[^\n]*)*')
_RE_KEY = re.compile(r'([A-Za-z_][\w|\-]*)[ \t]*:')
_RE_STRING = re.compile(r'"([^"]*)"')
_RE_NUMBER = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?(?![\w.#])')
_RE_ARRAY = re.compile(r'\*(\d+)')
_RE_BARE = re.compile(r'[^\s,{};"]+')
_RE_INT = re.compile(r'[-+]?\d+$')


class _Str(str):
    """Marks a value that was a quoted string in the file."""
    __slots__ = ()


class _Array:
    __slots__ = ('text',)

    def __init__(self, text):
        self.text = text


class _Bare(str):
    """An unquoted word, e.g. the T in 'Shading: T'."""
    __slots__ = ()


class Node:
    __slots__ = ('name', 'values', 'children')

    def __init__(self, name):
        self.name = name
        self.values = []
        self.children = []


class ParseError(Exception):
    pass


def _unescape(s):
    if '&' in s:
        s = s.replace('&quot;', '"').replace('&lf;', '\n').replace('&cr;', '\r')
    return s


def _float(tok):
    try:
        return float(tok)
    except ValueError:
        t = tok.lower()
        if 'nan' in t or 'ind' in t:
            return float('nan')
        if 'inf' in t:
            return float('-inf') if t.startswith('-') else float('inf')
        raise


class _Parser:
    def __init__(self, text):
        self.text = text
        self.pos = 0
        self.n = len(text)

    def error(self, msg):
        line = self.text.count('\n', 0, self.pos) + 1
        raise ParseError("%s (line %d)" % (msg, line))

    def skip(self):
        self.pos = _RE_SKIP.match(self.text, self.pos).end()

    def parse_children(self, nested):
        out = []
        text = self.text
        while True:
            self.skip()
            if self.pos >= self.n:
                if nested:
                    self.error("Unexpected end of file, missing '}'")
                return out
            if text[self.pos] == '}':
                if not nested:
                    self.error("Unexpected '}'")
                self.pos += 1
                return out
            m = _RE_KEY.match(text, self.pos)
            if not m:
                self.error("Expected an element name, got %r" % text[self.pos:self.pos + 30])
            self.pos = m.end()
            out.append(self.parse_element(m.group(1)))

    def parse_value(self):
        text = self.text
        c = text[self.pos]
        if c == '"':
            m = _RE_STRING.match(text, self.pos)
            if not m:
                self.error("Unterminated string")
            self.pos = m.end()
            return _Str(_unescape(m.group(1)))
        if c == '*':
            m = _RE_ARRAY.match(text, self.pos)
            if m:
                self.pos = m.end()
                return self.parse_array_block()
        m = _RE_NUMBER.match(text, self.pos)
        if m:
            self.pos = m.end()
            tok = m.group(0)
            return int(tok) if _RE_INT.match(tok) else float(tok)
        m = _RE_BARE.match(text, self.pos)
        if m:
            self.pos = m.end()
            tok = m.group(0)
            try:
                return _float(tok)  # e.g. "-1.#IND", "nan"
            except ValueError:
                return _Bare(tok)
        self.error("Unexpected character %r" % c)

    def parse_array_block(self):
        # *N { a: v,v,v... }
        self.skip()
        text = self.text
        if self.pos >= self.n or text[self.pos] != '{':
            self.error("Expected '{' after array size")
        self.pos += 1
        self.skip()
        m = _RE_KEY.match(text, self.pos)
        if m and m.group(1) == 'a':
            self.pos = m.end()
        end = text.find('}', self.pos)
        if end == -1:
            self.error("Unterminated array")
        data = text[self.pos:end]
        self.pos = end + 1
        return _Array(data)

    def parse_element(self, name):
        node = Node(name)
        text = self.text
        values = node.values
        expect_value = True
        while True:
            self.skip()
            if self.pos >= self.n:
                return node
            c = text[self.pos]
            if c == ',':
                self.pos += 1
                expect_value = True
                continue
            if c == '{':
                self.pos += 1
                node.children = self.parse_children(True)
                return node
            if c == '}' or not expect_value:
                return node
            if _RE_KEY.match(text, self.pos):
                return node  # next element starts, this one has no (more) values
            values.append(self.parse_value())
            expect_value = False


def parse(data):
    """Parse ASCII FBX bytes, return (list of root Nodes, fbx version)."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        text = data.decode('latin-1')
    if text.startswith('﻿'):
        text = text[1:]
    nodes = _Parser(text).parse_children(False)

    version = 0
    for n in nodes:
        if n.name == 'FBXHeaderExtension':
            for c in n.children:
                if c.name == 'FBXVersion' and c.values:
                    version = int(c.values[0])
    if not version:
        m = re.match(r'\s*;\s*FBX\s+(\d+)\.(\d+)\.(\d+)', text)
        if m:
            version = int(m.group(1)) * 1000 + int(m.group(2)) * 100 + int(m.group(3)) * 10
    return nodes, version


# --------------------------------------------------------------------------
# Conversion to binary (uses Blender's own FBX encoder)

def _array_values(name, text):
    s = text.strip()
    if not s:
        kind = 'd' if name in _ARRAY_FLOAT64 else 'f' if name in _ARRAY_FLOAT32 else \
               'l' if name in _ARRAY_INT64 else 'b' if name in _ARRAY_BOOL else 'i'
        dtype = {'d': np.float64, 'f': np.float32, 'l': np.int64, 'b': np.bool_, 'i': np.int32}[kind]
        return kind, np.zeros(0, dtype=dtype)
    if s[0] == '"':
        return 's', ''.join(_unescape(x) for x in _RE_STRING.findall(s))
    parts = [p.strip() for p in s.split(',')]
    if parts and parts[-1] == '':
        parts.pop()
    is_float = any(('.' in p or 'e' in p or 'E' in p or 'n' in p.lower()) for p in parts)

    if name in _ARRAY_FLOAT64 or (is_float and name not in _ARRAY_FLOAT32):
        return 'd', np.array([_float(p) for p in parts], dtype=np.float64)
    if name in _ARRAY_FLOAT32:
        return 'f', np.array([_float(p) for p in parts], dtype=np.float32)
    ints = np.array([int(p) for p in parts], dtype=np.int64)
    if name in _ARRAY_BOOL:
        return 'b', ints.astype(np.bool_)
    if name in _ARRAY_INT64 or (ints.size and (ints.min() < _INT32_MIN or ints.max() > _INT32_MAX)):
        return 'l', ints
    return 'i', ints.astype(np.int32)


def _swap_class_name(s):
    # ASCII "Model::Cube" -> binary b"Cube\x00\x01Model"
    b = s.encode('utf-8')
    i = b.find(b'::')
    if i == -1:
        return b
    return b[i + 2:] + b'\x00\x01' + b[:i]


def _add_int(elem, v, force64=False):
    if force64 or not (_INT32_MIN <= v <= _INT32_MAX):
        elem.add_int64(v)
    else:
        elem.add_int32(v)


def _convert(encode_bin, node, parent_name, out_parent):
    elem = encode_bin.FBXElem(node.name.encode('utf-8'))
    out_parent.elems.append(elem)
    name = node.name
    values = node.values

    if name == 'P' and len(values) >= 4:
        ptype = values[1].encode('utf-8') if isinstance(values[1], str) else b''
        for i, v in enumerate(values):
            if isinstance(v, str):
                elem.add_string(v.encode('utf-8'))
            elif i < 4:
                elem.add_string(str(v).encode('utf-8'))
            elif ptype in _P_INT32:
                elem.add_int32(int(v))
            elif ptype in _P_INT64:
                elem.add_int64(int(v))
            else:
                elem.add_float64(float(v))
    elif name == 'Content' and any(isinstance(v, (_Str, _Array)) for v in values):
        b64 = ''
        for v in values:
            if isinstance(v, _Str):
                b64 += v
            elif isinstance(v, _Array):
                b64 += _array_values(name, v.text)[1] if v.text.strip().startswith('"') else ''
        try:
            elem.add_bytes(base64.b64decode(b64))
        except Exception:
            elem.add_bytes(b'')
    else:
        is_obj = parent_name == 'Objects' or (parent_name == 'Documents' and name == 'Document')
        name_index = 0 if name in _NAME_FIRST else 1 if is_obj else -1
        is_conn = name == 'C' and parent_name == 'Connections'
        for i, v in enumerate(values):
            if isinstance(v, _Array):
                kind, arr = _array_values(name, v.text)
                if kind == 's':
                    elem.add_string(arr.encode('utf-8'))
                else:
                    {'d': elem.add_float64_array, 'f': elem.add_float32_array,
                     'l': elem.add_int64_array, 'i': elem.add_int32_array,
                     'b': elem.add_bool_array}[kind](arr)
            elif isinstance(v, _Bare):
                b = v.encode('utf-8')
                if len(b) == 1:
                    elem.add_char(b)
                else:
                    elem.add_string(b)
            elif isinstance(v, str):
                if i == name_index:
                    elem.add_string(_swap_class_name(v))
                else:
                    elem.add_string(v.encode('utf-8'))
            elif isinstance(v, bool):
                elem.add_int32(int(v))
            elif isinstance(v, int):
                if name in _SCALAR_FLOAT64:
                    elem.add_float64(float(v))
                else:
                    _add_int(elem, v, force64=(name in _SCALAR_INT64 or (is_obj and i == 0)
                                               or (is_conn and i in (1, 2))))
            else:
                elem.add_float64(float(v))

    for c in node.children:
        _convert(encode_bin, c, name, elem)


def is_binary_fbx(filepath):
    with open(filepath, 'rb') as f:
        return f.read(len(_BINARY_MAGIC)) == _BINARY_MAGIC


def convert_file(src, dst, encode_bin):
    """Convert ASCII FBX file `src` into binary FBX file `dst`. Returns the FBX version."""
    with open(src, 'rb') as f:
        data = f.read()
    nodes, version = parse(data)
    if version < 7100:
        raise ParseError("ASCII FBX version %d is not supported (needs 7.1 or newer; "
                         "FBX 6.x has a different layout)" % version)
    root = encode_bin.FBXElem(b'')
    # Arrays get compressed on worker threads; they are only complete once
    # this context manager exits, so write the file afterwards.
    with encode_bin.FBXElem.enable_multithreading_cm():
        for n in nodes:
            _convert(encode_bin, n, '', root)
    # The binary writer overwrites FileId/CreationTime and requires them to be
    # bytes/string, so always provide fresh ones.
    root.elems = [e for e in root.elems if e.id not in (b'FileId', b'CreationTime')]
    file_id = encode_bin.FBXElem(b'FileId')
    file_id.add_bytes(b'\x00' * 16)
    creation = encode_bin.FBXElem(b'CreationTime')
    creation.add_string(b'1970-01-01 10:00:00:000')
    root.elems[1:1] = [file_id, creation]
    encode_bin.write(dst, root, version)
    return version
