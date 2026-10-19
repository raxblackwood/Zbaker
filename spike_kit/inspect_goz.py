#!/usr/bin/env python3
import sys
import os
import struct

GOZ_TAG_END_OF_FILE = 0
GOZ_TAG_MESH = 1
GOZ_TAG_FLAGS = 5001
GOZ_TAG_SUBDIV = 5002
GOZ_TAG_POINT_LIST = 10001
GOZ_TAG_FACE4_LIST_FORMAT_1 = 20001
GOZ_TAG_FACE4_LIST_FORMAT_2 = 20002
GOZ_TAG_FACE3_LIST = 20003
GOZ_TAG_UV4_LIST = 25001
GOZ_TAG_UV3_LIST = 25002
GOZ_TAG_MASK8_LIST = 30001
GOZ_TAG_MASK16_LIST = 30002
GOZ_TAG_MRGB_LIST = 35001
GOZ_TAG_GROUPS_LIST = 40001
GOZ_TAG_CREASE_LIST = 40002

TAG_NAMES = {
    0: "END_OF_FILE",
    1: "MESH_NAME",
    5001: "FLAGS",
    5002: "SUBDIV",
    10001: "POINT_LIST (Vertices)",
    20001: "FACE4_FORMAT_1 (Quads/Tris)",
    20002: "FACE4_FORMAT_2",
    20003: "FACE3_LIST (Triangles)",
    25001: "UV4_LIST",
    25002: "UV3_LIST",
    30001: "MASK8_LIST",
    30002: "MASK16_LIST",
    35001: "MRGB_LIST (Polypaint)",
    40001: "GROUPS_LIST (Polygroups)",
    40002: "CREASE_LIST"
}

def inspect_goz(filepath):
    if not os.path.exists(filepath):
        print(f"[Error] File not found: {filepath}")
        return False

    filesize = os.path.getsize(filepath)
    print(f"==================================================")
    print(f" GoZ File: {os.path.basename(filepath)}")
    print(f" Full Path: {filepath}")
    print(f" File Size: {filesize:,} bytes ({filesize / (1024*1024):.2f} MB)")
    print(f"==================================================")

    with open(filepath, "rb") as f:
        magic = f.read(4)
        if magic != b"GoZb":
            print(f"[Error] Invalid magic header: {magic} (Expected b'GoZb')")
            return False

        header_desc = f.read(28).decode("ascii", errors="replace").strip("\x00")
        print(f"Header: GoZb | Description: '{header_desc}'")
        print("--------------------------------------------------")

        point_count = 0
        face_count = 0
        polygroup_count = 0
        polypaint_count = 0
        uv_count = 0
        subdiv_info = None

        while True:
            header_bytes = f.read(16)
            if len(header_bytes) < 16:
                break

            tag, size, count, modifier = struct.unpack("<III f", header_bytes)
            tag_name = TAG_NAMES.get(tag, f"UNKNOWN_TAG_{tag}")

            data_size = size - 16
            print(f"Block: Tag={tag:<5} ({tag_name:<25}) | Size={size:<10} | Count={count:<8} | Mod={modifier:.3f}")

            if tag == GOZ_TAG_END_OF_FILE:
                break
            elif tag == GOZ_TAG_POINT_LIST:
                point_count = count
            elif tag in (GOZ_TAG_FACE4_LIST_FORMAT_1, GOZ_TAG_FACE4_LIST_FORMAT_2, GOZ_TAG_FACE3_LIST):
                face_count = count
            elif tag == GOZ_TAG_GROUPS_LIST:
                polygroup_count = count
            elif tag == GOZ_TAG_MRGB_LIST:
                polypaint_count = count
            elif tag in (GOZ_TAG_UV4_LIST, GOZ_TAG_UV3_LIST):
                uv_count = count
            elif tag == GOZ_TAG_SUBDIV and data_size >= 16:
                cur_sd, max_sd, dyn_sharp, dyn_smooth = struct.unpack("<IIII", f.read(16))
                subdiv_info = (cur_sd, max_sd, dyn_sharp, dyn_smooth)
                data_size -= 16

            if data_size > 0:
                f.seek(data_size, os.SEEK_CUR)

    print("--------------------------------------------------")
    print(f"SUMMARY FOR SPIKE TEST EVALUATION:")
    print(f"  * Vertex Count:    {point_count:,}")
    print(f"  * Face Count:      {face_count:,}")
    print(f"  * PolyGroup Count: {polygroup_count:,} ({'Present' if polygroup_count > 0 else 'None'})")
    print(f"  * PolyPaint (MRGB):{polypaint_count:,} ({'Present' if polypaint_count > 0 else 'None'})")
    print(f"  * UV Count:        {uv_count:,} ({'Present' if uv_count > 0 else 'None'})")
    if subdiv_info:
        print(f"  * SubDiv Levels:   Current SDiv={subdiv_info[0]}, Max SDiv={subdiv_info[1]}")
    else:
        print(f"  * SubDiv Levels:   No SubDiv tag present")
    print(f"==================================================")
    return True

if __name__ == "__main__":
    if len(sys.argv) > 1:
        target = sys.argv[1]
    else:
        default_dir = r"C:\Users\Public\Pixologic\GoZProjects\Default"
        goz_files = [os.path.join(default_dir, f) for f in os.listdir(default_dir) if f.lower().endswith(".goz")] if os.path.exists(default_dir) else []
        if not goz_files:
            print("Usage: python inspect_goz.py <path_to_file.GoZ>")
            print(f"No .GoZ files currently in {default_dir}")
            sys.exit(1)
        # Pick the most recently modified GoZ file
        goz_files.sort(key=os.path.getmtime, reverse=True)
        target = goz_files[0]
        print(f"Inspecting most recent GoZ file: {target}")

    inspect_goz(target)
