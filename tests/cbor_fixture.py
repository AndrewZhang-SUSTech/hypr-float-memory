# SPDX-FileCopyrightText: 2026 andrew
# SPDX-License-Identifier: BSD-3-Clause
"""Minimal CBOR fixture codec for the integers, strings, and arrays in state tests."""
def encode(value):
    if value is None: return b"\xf6"
    if isinstance(value, int):
        major, body = 0, b''
        number = value if value >= 0 else -1-value
        major = 0 if value >= 0 else 1
    elif isinstance(value, str):
        body = value.encode(); major, number = 3, len(body)
    else:
        body = b''.join(map(encode, value)); major, number = 4, len(value)
    if number < 24:
        header = bytes([(major << 5) | number])
    else:
        width = next(w for w in (1, 2, 4, 8) if number < 1 << (8*w))
        header = bytes([(major << 5) | {1:24,2:25,4:26,8:27}[width]]) + number.to_bytes(width, 'big')
    return header + body

def decode(data):
    position = 0
    def read():
        nonlocal position
        tag = data[position]; position += 1
        major, number = tag >> 5, tag & 31
        if tag == 0xf6: return None
        if number >= 24:
            width = {24:1,25:2,26:4,27:8}[number]
            number = int.from_bytes(data[position:position+width], 'big'); position += width
        if major == 0: return number
        if major == 1: return -1-number
        if major == 3:
            value = data[position:position+number].decode(); position += number; return value
        if major == 4: return [read() for _ in range(number)]
        raise ValueError('unsupported fixture value')
    result = read()
    assert position == len(data)
    return result

def geometry(data):
    doc = decode(data)
    assert doc[0] == 2 and all(len(row) == 6 for row in doc[1])
    return sorted(row[:5] for row in doc[1])
