#!/usr/bin/env python3
"""Check explicit ReXGlue entry points against generated registration and ELF symbols."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tomllib


class VerificationError(Exception):
    pass


def read_toml(path):
    with path.open('rb') as source:
        return tomllib.load(source)


def load_expected(manifest):
    """Follow SDK v0.10 includes and last-wins function overrides."""
    table = read_toml(manifest)
    if not isinstance(table.get('entrypoint'), dict):
        raise VerificationError(f'{manifest}: missing [entrypoint]')
    functions = {}
    visited = set()

    def merge(config, directory, depth=0):
        if depth > 32:
            raise VerificationError('Config include depth exceeds 32')
        for name in config.get('includes', []):
            path = (directory / name).resolve()
            if path in visited:
                raise VerificationError(f'Circular or repeated config include: {path}')
            visited.add(path)
            merge(read_toml(path), path.parent, depth + 1)
        for key, value in config.get('functions', {}).items():
            address = int(key, 16)
            if not 0 < address <= 0xFFFFFFFF or address % 4:
                raise VerificationError(f'Invalid guest function address: {key}')
            if not isinstance(value, dict):
                raise VerificationError(f'Function {key} must be a table')
            functions[address] = value

    merge(table['entrypoint'], manifest.parent)
    # A shared chunk is emitted inside its parent, not necessarily registered
    # separately. Verify the parent's entry instead of inventing a chunk entry.
    expected = set()
    for address, config in functions.items():
        parent = config.get('parent', 0)
        if not isinstance(parent, int) or parent < 0 or parent > 0xFFFFFFFF or parent % 4:
            raise VerificationError(f'Invalid parent for 0x{address:08X}')
        expected.add(parent or address)
    return expected


def read_registrations(path):
    source = re.sub(r'//[^\n]*|/\*.*?\*/', '', path.read_text(), flags=re.S)
    bindings = {}
    for address, symbol in re.findall(
        r'\bregistrar\s*->\s*SetFunction\s*\(\s*(0[xX][0-9a-fA-F]+)\s*,\s*([A-Za-z_]\w*)\s*\)\s*;',
        source,
    ):
        address = int(address, 16)
        if address in bindings:
            raise VerificationError(f'Duplicate dispatch registration: 0x{address:08X}')
        bindings[address] = symbol
    return bindings


def read_function_symbols(binary, nm):
    result = subprocess.run(
        [nm, '--defined-only', '--format=posix', str(binary)],
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        raise VerificationError(f'nm failed for {binary}: {result.stderr.strip()}')
    # GNU nm and llvm-nm both support POSIX output: name type value [size].
    return {
        fields[0] for line in result.stdout.splitlines()
        if len(fields := line.split()) >= 2 and fields[1] in {'T', 't', 'W', 'w', 'I', 'i'}
    }


def verify(manifest, registration, binary=None, nm='nm'):
    expected = load_expected(manifest)
    bindings = read_registrations(registration)
    missing = sorted(expected - bindings.keys())
    if missing:
        addresses = ', '.join(f'0x{address:08X}' for address in missing)
        raise VerificationError(
            f'Missing generated dispatch entries: {addresses}. '
            'Regenerate with the matching manifest; review any rejected boundaries.'
        )
    if binary is not None:
        symbols = read_function_symbols(binary, nm)
        missing = [(address, bindings[address]) for address in sorted(expected)
                   if bindings[address] not in symbols]
        if missing:
            details = ', '.join(f'0x{address:08X} ({symbol})' for address, symbol in missing)
            raise VerificationError(
                f'Missing compiled functions: {details}. '
                'The executable may be stale or stripped; rebuild and check before stripping.'
            )
    return len(expected)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--registration', required=True, type=Path)
    parser.add_argument('--binary', type=Path, help='Also check the linked Linux executable')
    parser.add_argument('--nm', default='nm', help='GNU nm or llvm-nm path')
    args = parser.parse_args(argv)
    try:
        count = verify(args.manifest, args.registration, args.binary, args.nm)
    except (VerificationError, OSError, ValueError, TypeError) as error:
        print(f'Function dispatch verification failed: {error}', file=sys.stderr)
        return 1
    stage = 'generated dispatch and executable' if args.binary else 'generated dispatch'
    print(f'Function dispatch verification passed: {count} configured function/parent entries in {stage}.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
