#!/usr/bin/env python3
"""usage: add_forwarders.py <in.dll> <out.dll> <name=module.function> ...

Rebuilds a PE's export directory in a new section with extra forwarded exports.
Existing forwarders are moved along, since a forwarder is recognised by its RVA
lying inside the export directory."""
import struct
import sys

import pefile


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def main():
    src, dst, *specs = sys.argv[1:]
    additions = [s.split('=', 1) for s in specs]
    pe = pefile.PE(src)
    exp_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']]
    old_start, old_end = exp_dir.VirtualAddress, exp_dir.VirtualAddress + exp_dir.Size
    d = pe.DIRECTORY_ENTRY_EXPORT.struct

    functions = list(struct.unpack_from(f'<{d.NumberOfFunctions}I', pe.get_data(d.AddressOfFunctions, 4 * d.NumberOfFunctions)))
    name_rvas = struct.unpack_from(f'<{d.NumberOfNames}I', pe.get_data(d.AddressOfNames, 4 * d.NumberOfNames))
    name_ords = struct.unpack_from(f'<{d.NumberOfNames}H', pe.get_data(d.AddressOfNameOrdinals, 2 * d.NumberOfNames))
    names = [(pe.get_string_at_rva(rva), ordinal) for rva, ordinal in zip(name_rvas, name_ords)]
    existing = {n for n, _ in names}
    forwarders = {i: pe.get_string_at_rva(rva) for i, rva in enumerate(functions) if old_start <= rva < old_end}

    for name, target in additions:
        if name.encode() in existing:
            sys.exit(f'{name} is already exported')
        index = len(functions)
        functions.append(0)
        forwarders[index] = target.encode()
        names.append((name.encode(), index))
    names.sort(key=lambda entry: entry[0])

    section_alignment = pe.OPTIONAL_HEADER.SectionAlignment
    file_alignment = pe.OPTIONAL_HEADER.FileAlignment
    last = max(pe.sections, key=lambda s: s.VirtualAddress)
    base_rva = align(last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData), section_alignment)

    # Layout: directory, function table, name pointers, name ordinals, then strings
    n_funcs, n_names = len(functions), len(names)
    funcs_off = 40
    names_off = funcs_off + 4 * n_funcs
    ords_off = names_off + 4 * n_names
    strings_off = ords_off + 2 * n_names
    blob = bytearray(strings_off)

    def add_string(value):
        offset = len(blob)
        blob.extend(value + b'\0')
        return base_rva + offset

    dll_name_rva = add_string(pe.get_string_at_rva(d.Name))
    name_ptrs = [add_string(name) for name, _ in names]
    for index, target in sorted(forwarders.items()):
        functions[index] = add_string(target)

    struct.pack_into('<IIHHIIIIIII', blob, 0, 0, d.TimeDateStamp, d.MajorVersion, d.MinorVersion, dll_name_rva, d.Base,
                     n_funcs, n_names, base_rva + funcs_off, base_rva + names_off, base_rva + ords_off)
    struct.pack_into(f'<{n_funcs}I', blob, funcs_off, *functions)
    struct.pack_into(f'<{n_names}I', blob, names_off, *name_ptrs)
    struct.pack_into(f'<{n_names}H', blob, ords_off, *[ordinal for _, ordinal in names])
    virtual_size = len(blob)
    blob.extend(b'\0' * (align(len(blob), file_alignment) - len(blob)))

    # Section header slot right after the existing ones
    header_off = pe.sections[-1].get_file_offset() + 40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.PointerToRawData)
    if header_off + 40 > min(pe.OPTIONAL_HEADER.SizeOfHeaders, first_raw):
        sys.exit('no room for another section header')
    raw = bytearray(pe.__data__)
    raw_ptr = align(max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections), file_alignment)
    if len(raw) > raw_ptr:
        sys.exit('file has trailing data after its sections')
    raw.extend(b'\0' * (raw_ptr - len(raw)))
    struct.pack_into('<8sIIIIIIHHI', raw, header_off, b'.hbexp', virtual_size, base_rva, len(blob), raw_ptr, 0, 0, 0, 0,
                     0x40000040)
    raw.extend(blob)

    pe = pefile.PE(data=bytes(raw))
    pe.FILE_HEADER.NumberOfSections += 1
    pe.OPTIONAL_HEADER.SizeOfImage = align(base_rva + virtual_size, section_alignment)
    pe.OPTIONAL_HEADER.SizeOfInitializedData += len(blob)
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[0].VirtualAddress = base_rva
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[0].Size = virtual_size
    pe.OPTIONAL_HEADER.CheckSum = 0
    data = bytearray(pe.write())
    pe = pefile.PE(data=bytes(data))
    pe.OPTIONAL_HEADER.CheckSum = pe.generate_checksum()
    pe.write(dst)
    print(f'{dst}: {n_funcs} functions, {n_names} names, {len(forwarders)} forwarders, export directory at 0x{base_rva:x}')


main()
