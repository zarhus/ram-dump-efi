import os
import sys
import re

BLOCK_SIZE = 65536

def print_usage():
    print("""Usage:
python validate.py DIRECTORY
Arguments:
    DIRECTORY - directory with files to analyze""")

def get_base_address(filename):
    match = re.search(r'0x([0-9a-fA-F]+)\.csv$', filename)
    if not match:
        raise ValueError(f"Could not parse start address from filename: {filename}")
    return int(match.group(1), 16)

# For some reason zeroed memory sometimes contains several patterns like this:
# 0000 0000 0000 0000 0011 0001 0000 0000 (hex)
# This needs to be treated as 0 by the tool
# Effectively, this function analyzes a block and returns True only if all its
# non-zero patterns are a multiple of \x11\x01.
def is_effectively_zero(block):
    allowed_pattern = b'\x11\x01'
    if all(b == 0 for b in block):
        return True
    non_zero_bytes = bytes(b for b in block if b != 0)
    if not non_zero_bytes:
        return True
    if len(non_zero_bytes) % len(allowed_pattern) != 0:
        return False

    repeated = allowed_pattern * (len(non_zero_bytes) // len(allowed_pattern))
    return non_zero_bytes == repeated

def analyze_ram_dump(filepath):
    print(f"Analyzing {os.path.basename(filepath)}\n")
    base_address = get_base_address(os.path.basename(filepath))

    zero_regions = []
    non_zero_regions = []

    with open(filepath, 'rb') as f:
        is_zero = None
        offset = 0
        start_addr = 0
        end_addr = 0
        # Regions of continuous zero or non-zero blocks
        region_start = None
        while True:
            block = f.read(BLOCK_SIZE)
            if not block:
                break
            start_addr = base_address + offset
            end_addr = start_addr + len(block)
            if is_zero is None:
                is_zero = is_effectively_zero(block)
                region_start = start_addr
            elif is_zero != is_effectively_zero(block):
                # Flush previous region
                if is_zero:
                    zero_regions.append((region_start, start_addr))
                else:
                    non_zero_regions.append((region_start, start_addr))
                is_zero = not is_zero
                region_start = start_addr

            offset += len(block)
        if region_start is not None:
            if is_zero:
                zero_regions.append((region_start, end_addr))
            else:
                non_zero_regions.append((region_start, end_addr))


    print("=== Zeroed Memory Regions ===")
    for start, end in zero_regions:
        print(f"0x{start:08x} - 0x{end:08x}")

    print("\n=== Non-Zero Memory Regions ===")
    for start, end in non_zero_regions:
        print(f"0x{start:08x} - 0x{end:08x}")
    
    print("\n===========================\n")

if len(sys.argv) < 2:
    print("Missing argument - DIRECTORY")
    print_usage()
    exit(1)

directory = sys.argv[1]

if not os.path.isdir(directory):
    print(f"Error: {directory} is not a valid directory.")
    sys.exit(1)

csv_files = sorted(f for f in os.listdir(directory) if f.endswith(".csv"))

if not csv_files:
    print("No CSV files found in the directory.")
    sys.exit(0)

for csv_file in csv_files:
    full_path = os.path.join(directory, csv_file)
    try:
        analyze_ram_dump(full_path)
    except Exception as e:
        print(f"Skipping {csv_file}: {e}")

