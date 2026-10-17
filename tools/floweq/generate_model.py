#!/usr/bin/env python3
"""Convert flowEQ's decoder weights to the C++ table nc.eq.flow is built with.

flowEQ (https://github.com/csteinmetz1/flowEQ, BSD-3-Clause) is a MATLAB plugin: a disentangled
variational autoencoder over the thirteen parameters of a five band parametric EQ. Its plugin/Decoder.m
loads four arrays from a .mat file and runs one forward pass:

    h  = max(0, z * W1 + b1)          # 3 (or 1, or 2) latent values -> 1024 hidden units, ReLU
    y  = 1 / (1 + exp(-(h * W2 + b2))) # 1024 -> 13 normalised parameters, sigmoid
    p  = y .* (xmax - xmin) + xmin     # normalised 0..1 -> gain/frequency/Q

This script reads the .mat (`plugin/assets/vae3d_beta_0.0010.mat`, the model flowEQ-lite hardcodes),
checks its shape, and writes the same four arrays as a C++ header. The weights are float32 in Keras,
and every one of them round-trips through float32 exactly, so the table is emitted as hex float
literals: exact, and a C++17 literal the reader can check.

The header is checked in (include/namchain/floweq/model.h). Regenerate it only when the model changes:

    tools/floweq/generate_model.py third-party/flowEQ/plugin/assets/vae3d_beta_0.0010.mat

Run it with any Python 3: it parses MAT v5 with struct only -- no numpy, no scipy -- so it works in a
bare checkout. `--golden` prints the decoder's output for a few latent points, which is where the
values in tests/test_floweq.cpp come from.
"""

import argparse
import struct
import sys

# MAT v5 element types (matfile_format.pdf, "Data Element Types").
MI_INT8 = 1
MI_UINT8 = 2
MI_INT16 = 3
MI_UINT16 = 4
MI_INT32 = 5
MI_UINT32 = 6
MI_SINGLE = 7
MI_DOUBLE = 9
MI_INT64 = 12
MI_UINT64 = 13
MI_MATRIX = 14
MI_COMPRESSED = 15

# The decoder's shape and the normalisation Decoder.m carries. Kept beside the reader rather than
# read from the .mat: they are part of the plugin's contract, not of the file.
HIDDEN = 1024
OUTPUTS = 13
LATENT = 3
XMAX = [12.0, 1000.0, 12.0, 3900.0, 10.0, 12.0, 4700.0, 10.0, 12.0, 10000.0, 10.0, 12.0, 20000.0]
XMIN = [-12.0, 22.0, -12.0, 82.0, 0.1, -12.0, 180.0, 0.1, -12.0, 220.0, 0.1, -12.0, 580.0]
PARAM_NAMES = [
    "low shelf gain dB", "low shelf Hz", "band 1 gain dB", "band 1 Hz", "band 1 Q",
    "band 2 gain dB", "band 2 Hz", "band 2 Q", "band 3 gain dB", "band 3 Hz", "band 3 Q",
    "high shelf gain dB", "high shelf Hz",
]


class MatError(Exception):
    pass


def read_element(buf, offset):
    """One data element: (type, size, payload, next offset). Handles the small-element form."""
    if offset + 8 > len(buf):
        raise MatError("truncated element header at %d" % offset)
    first, second = struct.unpack_from("<II", buf, offset)
    if first >> 16:
        # A small data element packs its type in the low 16 bits and its size in the high 16, with
        # the payload in the four bytes that would otherwise be the size.
        kind = first & 0xFFFF
        size = first >> 16
        return kind, size, buf[offset + 4:offset + 4 + size], offset + 8
    payload = buf[offset + 8:offset + 8 + second]
    if len(payload) != second:
        raise MatError("truncated element at %d" % offset)
    return first, second, payload, offset + 8 + ((second + 7) // 8) * 8


NUMERIC_TYPES = {
    MI_INT8: ("b", 1), MI_UINT8: ("B", 1), MI_INT16: ("h", 2), MI_UINT16: ("H", 2),
    MI_INT32: ("i", 4), MI_UINT32: ("I", 4), MI_SINGLE: ("f", 4), MI_DOUBLE: ("d", 8),
    MI_INT64: ("q", 8), MI_UINT64: ("Q", 8),
}


def parse_matrix(payload):
    """A miMATRIX payload -> (name, dims, row-major list of numbers)."""
    kind, size, data, offset = read_element(payload, 0)
    if kind != MI_UINT32 or size < 8:
        raise MatError("a matrix does not start with its array flags")
    kind, size, data, offset = read_element(payload, offset)
    dims = list(struct.unpack("<%di" % (size // 4), data[:size]))
    kind, size, data, offset = read_element(payload, offset)
    name = data[:size].decode("ascii", "replace")
    kind, size, data, offset = read_element(payload, offset)
    if kind not in NUMERIC_TYPES:
        raise MatError("'%s' is not a numeric array (type %d)" % (name, kind))
    code, width = NUMERIC_TYPES[kind]
    count = size // width
    values = list(struct.unpack("<%d%s" % (count, code), data[:count * width]))
    if len(dims) < 2:
        return name, dims, values
    # MATLAB stores a matrix column by column. The plugin's C++ (and this table) use row-major, so
    # element (r, c) moves from r + rows*c to r*cols + c.
    rows, cols = dims[0], dims[1]
    if rows * cols != len(values):
        raise MatError("'%s' has dimensions %s but %d values" % (name, dims, len(values)))
    row_major = [0.0] * len(values)
    for r in range(rows):
        for c in range(cols):
            row_major[r * cols + c] = values[r + rows * c]
    return name, dims, row_major


def read_mat(path):
    """The variables of a MAT v5 file, as {name: (dims, row-major values)}."""
    with open(path, "rb") as handle:
        buf = handle.read()
    if buf[:6] != b"MATLAB":
        raise MatError("%s is not a MAT file" % path)
    if buf[124:126] != b"\x00\x01":
        raise MatError("%s is not a little-endian MAT v5 file" % path)
    variables = {}
    offset = 128
    while offset < len(buf):
        kind, size, payload, offset = read_element(buf, offset)
        if kind == MI_COMPRESSED:
            import zlib
            return read_compressed(buf, offset, zlib.decompress(payload))
        if kind != MI_MATRIX:
            continue
        name, dims, values = parse_matrix(payload)
        variables[name] = (dims, values)
    return variables


def read_compressed(buf, offset, payload):
    """MAT v5 can compress each variable; flowEQ's files are uncompressed, but be tolerant."""
    variables = {}
    inner = 0
    while inner < len(payload):
        kind, size, data, inner = read_element(payload, inner)
        if kind != MI_MATRIX:
            continue
        name, dims, values = parse_matrix(data)
        variables[name] = (dims, values)
    # Anything after the compressed elements in the original stream.
    while offset < len(buf):
        kind, size, payload2, offset = read_element(buf, offset)
        if kind != MI_MATRIX:
            continue
        name, dims, values = parse_matrix(payload2)
        variables[name] = (dims, values)
    return variables


def as_float32(values, what):
    """Round to float32 and refuse a value that does not round-trip: the C++ table is float."""
    packed = struct.pack("<%df" % len(values), *values)
    folded = list(struct.unpack("<%df" % len(values), packed))
    for index, (original, folded_value) in enumerate(zip(values, folded)):
        if original != folded_value:
            raise MatError("%s[%d] is %r, which is not a float32 (%r)" % (what, index, original, folded_value))
    return folded


def hex_literal(value):
    """A C++ float literal with every bit of a float32, and nothing else: 0x1.8p+0f."""
    text = float(value).hex()  # '0x1.8000000000000p+0', '-0x0.0p+0'
    sign = ""
    if text.startswith("-"):
        sign, text = "-", text[1:]
    mantissa, _, exponent = text.partition("p")
    whole, _, fraction = mantissa.partition(".")
    fraction = fraction.rstrip("0")
    return "%s%sp%s%sf" % (sign, whole + ("." + fraction if fraction else ""), exponent, "")


def format_floats(values, indent, per_line=6):
    lines = []
    for start in range(0, len(values), per_line):
        chunk = ", ".join(hex_literal(value) for value in values[start:start + per_line])
        lines.append(indent + chunk + ",")
    return "\n".join(lines)


def fnv1a(values):
    """A hash of the float bits, so a reader can tell one trained model from another."""
    digest = 0xCBF29CE484222325
    for value in values:
        for byte in struct.pack("<f", value):
            digest ^= byte
            digest = (digest * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return digest


def decode(latent, weights):
    """The forward pass, in double precision: the reference the C++ decoder is checked against."""
    w1, b1, w2, b2 = weights["W1"], weights["b1"], weights["W2"], weights["b2"]
    hidden = [0.0] * HIDDEN
    for j in range(HIDDEN):
        total = b1[j]
        for i in range(LATENT):
            total += latent[i] * w1[i * HIDDEN + j]
        hidden[j] = total if total > 0.0 else 0.0
    params = [0.0] * OUTPUTS
    for k in range(OUTPUTS):
        total = b2[k]
        for j in range(HIDDEN):
            total += hidden[j] * w2[j * OUTPUTS + k]
        y = 1.0 / (1.0 + pow(2.718281828459045, -total))
        params[k] = y * (XMAX[k] - XMIN[k]) + XMIN[k]
    return params


def write_header(path, model_name, source_name, weights):
    lines = []
    lines.append("// Generated by tools/floweq/generate_model.py -- do not edit.")
    lines.append("//")
    lines.append("// The decoder of flowEQ's disentangled variational autoencoder, converted from")
    lines.append("// flowEQ/plugin/assets/%s.mat. flowEQ is BSD-3-Clause, Copyright (c) 2019" % model_name)
    lines.append("// Christian Steinmetz; see docs/floweq.md for the notice and what the model is.")
    lines.append("//")
    lines.append("// Shapes, as flowEQ's Decoder.m uses them (weights are float32, exactly as trained):")
    lines.append("//   z (3) -> W1 (3x1024) + b1 -> ReLU -> W2 (1024x13) + b2 -> sigmoid -> 13 values in 0..1")
    lines.append("//   p[k] = y[k] * (kMax[k] - kMin[k]) + kMin[k], the thirteen EQ parameters.")
    lines.append("// Every array is row-major: kW1[i * kHidden + j] is latent i's weight on hidden unit j,")
    lines.append("// and kW2[j * kParams + k] is hidden unit j's weight on parameter k.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace namchain")
    lines.append("{")
    lines.append("namespace floweq")
    lines.append("{")
    lines.append("")
    lines.append("inline constexpr int kLatentDims = %d;" % LATENT)
    lines.append("inline constexpr int kHidden = %d;" % HIDDEN)
    lines.append("inline constexpr int kParams = %d;" % OUTPUTS)
    lines.append('inline constexpr const char* kModelName = "%s";' % model_name)
    lines.append('inline constexpr const char* kModelSource = "%s";' % source_name)
    lines.append("inline constexpr unsigned long long kWeightsHash = 0x%016xULL;" % fnv1a(
        weights["W1"] + weights["b1"] + weights["W2"] + weights["b2"]))
    lines.append("")
    lines.append("/// The parameter names, in the order the decoder emits them. For a report, not for the")
    lines.append("/// render path.")
    lines.append("inline constexpr const char* kParamNames[kParams] = {")
    for name in PARAM_NAMES:
        lines.append('  "%s",' % name)
    lines.append("};")
    lines.append("")
    lines.append("/// Decoder.m's xmax/xmin: the nominal range of each parameter, which the sigmoid's 0..1 is")
    lines.append("/// stretched across.")
    lines.append("inline constexpr float kMax[kParams] = {")
    lines.append(format_floats(XMAX, "  "))
    lines.append("};")
    lines.append("inline constexpr float kMin[kParams] = {")
    lines.append(format_floats(XMIN, "  "))
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr float kW1[kLatentDims * kHidden] = {")
    lines.append(format_floats(weights["W1"], "  "))
    lines.append("};")
    lines.append("inline constexpr float kB1[kHidden] = {")
    lines.append(format_floats(weights["b1"], "  "))
    lines.append("};")
    lines.append("inline constexpr float kW2[kHidden * kParams] = {")
    lines.append(format_floats(weights["W2"], "  "))
    lines.append("};")
    lines.append("inline constexpr float kB2[kParams] = {")
    lines.append(format_floats(weights["b2"], "  "))
    lines.append("};")
    lines.append("")
    lines.append("} // namespace floweq")
    lines.append("} // namespace namchain")
    lines.append("")
    with open(path, "w") as handle:
        handle.write("\n".join(lines))


def main(argv):
    parser = argparse.ArgumentParser(description="Convert a flowEQ decoder .mat to a C++ table.")
    parser.add_argument("mat", help="plugin/assets/vae3d_beta_0.0010.mat from a flowEQ checkout")
    parser.add_argument("--output", default="include/namchain/floweq/model.h",
                        help="the header to write (default: include/namchain/floweq/model.h)")
    parser.add_argument("--golden", action="store_true",
                        help="print the decoder's output for a few latent points, for tests")
    args = parser.parse_args(argv)

    try:
        variables = read_mat(args.mat)
        for name in ("W1", "b1", "W2", "b2"):
            if name not in variables:
                raise MatError("%s has no '%s'" % (args.mat, name))
            if len(variables[name][0]) != 2:
                raise MatError("'%s' is not a matrix" % name)
        shapes = {name: tuple(variables[name][0]) for name in ("W1", "b1", "W2", "b2")}
        if shapes["W1"] != (LATENT, HIDDEN):
            raise MatError("W1 is %s, expected %s" % (shapes["W1"], (LATENT, HIDDEN)))
        if shapes["b1"] != (1, HIDDEN):
            raise MatError("b1 is %s, expected %s" % (shapes["b1"], (1, HIDDEN)))
        if shapes["W2"] != (HIDDEN, OUTPUTS):
            raise MatError("W2 is %s, expected %s" % (shapes["W2"], (HIDDEN, OUTPUTS)))
        if shapes["b2"] != (1, OUTPUTS):
            raise MatError("b2 is %s, expected %s" % (shapes["b2"], (1, OUTPUTS)))
        weights = {name: as_float32(variables[name][1], name) for name in ("W1", "b1", "W2", "b2")}
    except MatError as error:
        print("generate_model: %s" % error, file=sys.stderr)
        return 1

    model_name = args.mat.rsplit("/", 1)[-1].removesuffix(".mat")
    write_header(args.output, model_name, args.mat.rsplit("/", 1)[-1], weights)
    print("wrote %s from %s (%d+%d+%d+%d weights)" % (
        args.output, args.mat, len(weights["W1"]), len(weights["b1"]), len(weights["W2"]), len(weights["b2"])))
    print("weights hash 0x%016x" % fnv1a(weights["W1"] + weights["b1"] + weights["W2"] + weights["b2"]))

    if args.golden:
        print()
        print("The decoder's output (a reference in double precision, for tests/test_floweq.cpp):")
        for latent in ([0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0],
                       [0.5, 0.0, 0.0], [2.0, 2.0, 2.0], [-2.0, -2.0, -2.0]):
            params = decode(latent, weights)
            print("  z = (%g, %g, %g):" % tuple(latent))
            print("    " + ", ".join("%.6f" % value for value in params))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
