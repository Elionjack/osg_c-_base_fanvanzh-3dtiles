#!/usr/bin/env python3
"""
3D Tiles Hierarchy Splitter
============================
Splits a large monolithic tileset.json into a hierarchical structure with
external tileset references.

Usage:
    python split_tileset.py <input_tileset.json> <output_dir> [--max-tiles N]

Example:
    python split_tileset.py tileset.json ./output --max-tiles 500
"""

import argparse
import json
import os
import shutil
import sys
import time
from collections import defaultdict
from pathlib import Path


# ---------------------------------------------------------------------------
# Buffered character reader – reads a large file efficiently with peek/read
# and absolute byte-position tracking.
# ---------------------------------------------------------------------------

class BufferedCharReader:
    """Efficient buffered character reader for large files.

    Reads binary data in chunks and yields one UTF-8 character at a time
    while tracking the absolute byte offset.
    """

    def __init__(self, filepath, chunk_size=8 * 1024 * 1024):
        self.f = open(filepath, 'rb')
        self.chunk_size = chunk_size
        self.buf = b''
        self.pos = 0          # cursor inside self.buf (byte offset)
        self.abs_pos = 0      # absolute byte position in file
        self.eof = False

    def _refill(self):
        """Slide remaining bytes to front and read next chunk."""
        remaining = self.buf[self.pos:]
        self.buf = remaining
        self.pos = 0
        chunk = self.f.read(self.chunk_size)
        if chunk:
            self.buf += chunk
        else:
            self.eof = True

    def _ensure(self, n_bytes=4):
        """Make sure at least *n_bytes* are available in buffer."""
        while self.pos + n_bytes > len(self.buf) and not self.eof:
            self._refill()

    def read_char(self):
        """Read and return the next Unicode character, or None at EOF."""
        self._ensure(4)
        if self.pos >= len(self.buf):
            return None

        b = self.buf[self.pos]
        if b < 0x80:
            # ASCII (1 byte)
            ch = chr(b)
            self.pos += 1
            self.abs_pos += 1
            return ch

        # Multi-byte UTF-8
        if b < 0xE0:
            width = 2
        elif b < 0xF0:
            width = 3
        else:
            width = 4

        self._ensure(width)
        if self.pos + width > len(self.buf):
            return None

        try:
            ch = self.buf[self.pos:self.pos + width].decode('utf-8')
        except UnicodeDecodeError:
            ch = '�'
        self.pos += width
        self.abs_pos += width
        return ch

    @property
    def byte_pos(self):
        return self.abs_pos

    def close(self):
        self.f.close()


# ---------------------------------------------------------------------------
# Pass 1: scan the tileset and count tiles per depth level.
# ---------------------------------------------------------------------------

class TilesetScanner:
    """Streaming scanner that counts tiles at each tree depth.

    Uses a finite-state machine that tracks:
      - JSON string boundaries (to ignore braces inside strings)
      - JSON object/array nesting (brace_depth)
      - Tile-tree depth (tile_depth) via a context stack
    """

    def __init__(self, filepath):
        self.r = BufferedCharReader(filepath)
        self.depth_counts = defaultdict(int)  # tile_depth → count
        self.max_depth_seen = 0

    def scan(self):
        r = self.r

        # --- parser state ---
        in_string = False
        escape = False
        brace_depth = 0           # JSON { / [ nesting (strings excluded)
        tile_depth = 0            # current tile-tree depth (0 = root)
        tile_stack = []           # depths of enclosing tiles
        reading_key = False       # are we reading a JSON key right now?
        current_key = ''          # the key being (or just) read
        just_got_key = ''         # key value that was just completed
        key_depth = None          # brace_depth at which the key was read

        # Stack: for each brace_depth we care about, what "kind" of value.
        # Values: 'children' (inside a children array), 'tile' (inside a tile obj),
        #         'root' (root tile), None (other)
        depth_kind = {}           # brace_depth → kind string

        # The depth at which the current children array began (its '[' depth).
        children_array_depths = []  # stack of brace_depths for children arrays

        ch = r.read_char()
        while ch is not None:
            if escape:
                escape = False
                if reading_key:
                    current_key += ch
                ch = r.read_char()
                continue

            if in_string:
                if ch == '\\':
                    escape = True
                elif ch == '"':
                    in_string = False
                    # String just ended – was it a key or a value?
                    if reading_key:
                        just_got_key = current_key
                        current_key = ''
                        reading_key = False
                elif reading_key:
                    current_key += ch
                ch = r.read_char()
                continue

            # --- not in a string ---
            if ch == '"':
                in_string = True
                current_key = ''
                ch = r.read_char()
                continue

            if ch == '{':
                brace_depth += 1
                # Determine what kind of object this is
                key = just_got_key
                just_got_key = ''

                is_tile = False
                if key == 'root' and brace_depth == 2:
                    # Root tile
                    is_tile = True
                    td = 0
                elif children_array_depths and brace_depth == children_array_depths[-1] + 1:
                    # Object inside a children array → it's a tile
                    is_tile = True
                    td = len(children_array_depths)  # = len(tile_stack) + 1?...
                    # Actually tile_depth = number of children arrays we're inside
                    td = len(children_array_depths)

                if is_tile:
                    tile_stack.append(td)
                    tile_depth = td
                    depth_kind[brace_depth] = 'tile'
                    self.depth_counts[td] += 1
                    if td > self.max_depth_seen:
                        self.max_depth_seen = td
                else:
                    depth_kind[brace_depth] = None

                ch = r.read_char()
                continue

            if ch == '}':
                # Leaving an object
                if brace_depth in depth_kind and depth_kind[brace_depth] == 'tile':
                    if tile_stack:
                        tile_stack.pop()
                    tile_depth = tile_stack[-1] if tile_stack else -1
                del depth_kind[brace_depth]
                brace_depth -= 1
                ch = r.read_char()
                continue

            if ch == '[':
                brace_depth += 1
                key = just_got_key
                just_got_key = ''

                if key == 'children':
                    # This is a children array
                    children_array_depths.append(brace_depth)
                    depth_kind[brace_depth] = 'children'
                else:
                    depth_kind[brace_depth] = None

                ch = r.read_char()
                continue

            if ch == ']':
                if brace_depth in depth_kind and depth_kind[brace_depth] == 'children':
                    if children_array_depths and children_array_depths[-1] == brace_depth:
                        children_array_depths.pop()
                del depth_kind[brace_depth]
                brace_depth -= 1
                ch = r.read_char()
                continue

            if ch == ':':
                # A key just finished; the next value determines context
                just_got_key = ''  # will be set when we finish reading the key string
                ch = r.read_char()
                continue

            if ch == ',':
                just_got_key = ''
                ch = r.read_char()
                continue

            # Whitespace – just skip
            if ch in (' ', '\t', '\n', '\r'):
                ch = r.read_char()
                continue

            # Any other character – skip (numbers, true, false, null chars)
            # For keys, we've already captured them via the string handler.
            # For values, we don't care about the actual value right now.
            # Consume until we hit a structural character.
            just_got_key = ''
            ch = r.read_char()

        self.r.close()
        return dict(self.depth_counts)


# ---------------------------------------------------------------------------
# Pass 2: extract subtrees and write output.
# ---------------------------------------------------------------------------

class TilesetSplitter:
    """Streaming splitter that extracts subtrees at a chosen depth.

    For each tile at *split_depth*:
      - Its full subtree (the tile object itself) is written to a separate
        tileset JSON file under ``subtilesets/``.
      - In the main tileset, that tile is replaced by a lightweight reference
        (boundingVolume + content.uri pointing to the sub-tileset).
    """

    def __init__(self, input_path, output_dir, split_depth):
        self.input_path = input_path
        self.output_dir = Path(output_dir)
        self.split_depth = split_depth

        # Will be populated during processing
        self.subtileset_count = 0

    def split(self):
        split_depth = self.split_depth
        out_dir = self.output_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        sub_dir = out_dir / 'subtilesets'
        sub_dir.mkdir(parents=True, exist_ok=True)

        # Copy Data directory (symlink or copy)
        src_data = Path(self.input_path).parent / 'Data'
        dst_data = out_dir / 'Data'
        if src_data.exists() and not dst_data.exists():
            print(f"Copying Data directory (this may take a while)...")
            shutil.copytree(str(src_data), str(dst_data))
            print(f"Data directory copied.")

        r = BufferedCharReader(self.input_path)

        # Open main output file for writing
        main_out_path = out_dir / 'tileset.json'
        main_out = open(str(main_out_path), 'w', encoding='utf-8', buffering=8*1024*1024)

        # --- parser state (same as scanner) ---
        in_string = False
        escape = False
        brace_depth = 0
        tile_stack = []           # list of tile depths
        reading_key = False
        current_key = ''
        just_got_key = ''
        depth_kind = {}
        children_array_depths = []

        # --- splitter-specific state ---
        # When we enter a tile at split_depth, we start capturing its raw JSON
        # into a buffer.  Events within that tile's braces go to the buffer
        # instead of main_out.
        capture_stack = []        # stack of brace_depths being captured
        capture_start_pos = None
        capture_filename = None

        # We also need to know the tile's content URI (for naming external files)
        # and its boundingVolume / geometricError / refine (for the reference tile).
        current_capture_info = {}

        # Phase: are we currently capturing a subtree?
        capturing = False
        capture_brace_target = None  # brace_depth of the tile we're capturing

        ch = r.read_char()
        while ch is not None:
            if escape:
                escape = False
                if reading_key:
                    current_key += ch
                if capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            if in_string:
                if capturing:
                    main_out.write(ch)
                if ch == '\\':
                    escape = True
                elif ch == '"':
                    in_string = False
                    if reading_key:
                        just_got_key = current_key
                        current_key = ''
                        reading_key = False
                elif reading_key:
                    current_key += ch
                ch = r.read_char()
                continue

            # --- not in a string ---
            if ch == '"':
                in_string = True
                current_key = ''
                if capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            if ch == '{':
                brace_depth += 1
                key = just_got_key
                just_got_key = ''

                # Determine if this is a tile
                is_tile = False
                td = -1
                if key == 'root' and brace_depth == 2:
                    is_tile = True
                    td = 0
                elif children_array_depths and brace_depth == children_array_depths[-1] + 1:
                    is_tile = True
                    td = len(children_array_depths)

                if is_tile:
                    tile_stack.append(td)
                    depth_kind[brace_depth] = 'tile'

                    if td == split_depth:
                        # Start capturing this subtree
                        capturing = True
                        capture_brace_target = brace_depth
                        capture_start_pos = r.byte_pos - 1  # position of '{'
                        current_capture_info = {
                            'td': td,
                            'content_uri': None,
                            'geometric_error': None,
                            'refine': None,
                        }
                        # Don't write '{' to main_out – will write reference instead
                    else:
                        if not capturing:
                            main_out.write(ch)
                else:
                    depth_kind[brace_depth] = None
                    if not capturing:
                        main_out.write(ch)

                ch = r.read_char()
                continue

            if ch == '}':
                if brace_depth in depth_kind and depth_kind[brace_depth] == 'tile':
                    td = tile_stack.pop() if tile_stack else -1

                    if td == split_depth and capturing and brace_depth == capture_brace_target:
                        # End of a captured subtree
                        capturing = False

                        # Write external tileset reference to main output
                        ref_json = self._build_reference_tile(current_capture_info)
                        main_out.write(ref_json)

                        # Write the captured subtree to a sub-tileset file
                        # We need to re-read the raw text from capture_start_pos
                        sub_filename = self._derive_subtileset_name(current_capture_info)
                        self._write_subtileset(
                            capture_start_pos, r.byte_pos,
                            sub_dir / sub_filename,
                            current_capture_info
                        )
                        self.subtileset_count += 1

                        current_capture_info = {}
                        capture_brace_target = None
                    elif not capturing:
                        main_out.write(ch)
                else:
                    del depth_kind[brace_depth]
                    if not capturing:
                        main_out.write(ch)

                brace_depth -= 1
                ch = r.read_char()
                continue

            if ch == '[':
                brace_depth += 1
                key = just_got_key
                just_got_key = ''

                if key == 'children':
                    children_array_depths.append(brace_depth)
                    depth_kind[brace_depth] = 'children'
                else:
                    depth_kind[brace_depth] = None

                if not capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            if ch == ']':
                if brace_depth in depth_kind and depth_kind[brace_depth] == 'children':
                    if children_array_depths and children_array_depths[-1] == brace_depth:
                        children_array_depths.pop()
                del depth_kind[brace_depth]
                brace_depth -= 1
                if not capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            if ch == ':':
                just_got_key = ''
                if not capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            if ch == ',':
                just_got_key = ''
                if not capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            # Whitespace
            if ch in (' ', '\t', '\n', '\r'):
                if not capturing:
                    main_out.write(ch)
                ch = r.read_char()
                continue

            # Other characters (number digits, true/false/null chars)
            # We track keys that might indicate content URI info
            if not capturing:
                main_out.write(ch)

            just_got_key = ''
            ch = r.read_char()

        main_out.close()
        r.close()

        print(f"Split complete: {self.subtileset_count} sub-tilesets written.")
        print(f"Main tileset: {main_out_path}")
        print(f"Sub-tilesets: {out_dir / 'subtilesets'}")

    def _build_reference_tile(self, info):
        """Build the JSON text for a reference tile that points to an external tileset.

        The reference keeps boundingVolume, geometricError, refine, and
        replaces content with a URI to the external tileset file.
        """
        # We need boundingVolume and other properties from the original tile.
        # These were captured during parsing – stored in info dict.
        # For now, build a minimal reference.  The bounding volume is embedded
        # inline by re-reading from the file.
        bv = info.get('bounding_volume')
        ge = info.get('geometric_error')
        ref = info.get('refine', 'REPLACE')
        sub_uri = info.get('subtileset_uri', './subtilesets/unknown.json')

        # Since we don't parse the boundingVolume during streaming,
        # we re-read the raw JSON for the tile and extract properties.
        # This is handled by _write_subtileset which writes the full tile
        # as the root of the external tileset.
        #
        # For the reference tile in the main file, we write a tile with
        # content.uri pointing to the external file.

        ref_tile = {
            "boundingVolume": info.get("bounding_volume", {"box": [0,0,0,1,0,0,0,1,0,0,0,1]}),
            "content": {"uri": sub_uri},
            "geometricError": info.get("geometric_error", 0),
            "refine": info.get("refine", "REPLACE")
        }

        return json.dumps(ref_tile, separators=(',', ':'))

    def _derive_subtileset_name(self, info):
        """Derive a sub-tileset filename from tile info."""
        uri = info.get('content_uri', '')
        if uri:
            # e.g. "./Data/Tile_-001_+050/Tile_-001_+050_L15_0.glb"
            # Extract "Tile_-001_+050"
            parts = uri.replace('\\', '/').split('/')
            # parts: ['.', 'Data', 'Tile_-001_+050', 'Tile_-001_+050_L15_0.glb']
            if len(parts) >= 3:
                tile_dir = parts[2]  # 'Tile_-001_+050'
                return f"{tile_dir}.json"
        return f"subtile_{self.subtileset_count:05d}.json"

    def _write_subtileset(self, start_byte, end_byte, filepath, info):
        """Extract raw tile JSON from the input file and write as a complete tileset.json.

        Re-reads the byte range [start_byte, end_byte) from the input file,
        wraps it in a valid tileset envelope.
        """
        # Read the raw tile JSON
        with open(self.input_path, 'rb') as f:
            f.seek(start_byte)
            raw = f.read(end_byte - start_byte)

        tile_text = raw.decode('utf-8')

        # Parse the tile to extract key properties for the reference
        try:
            tile_obj = json.loads(tile_text)
        except json.JSONDecodeError:
            # Fallback: use raw text as-is
            tile_obj = None

        if tile_obj:
            info['bounding_volume'] = tile_obj.get('boundingVolume')
            info['geometric_error'] = tile_obj.get('geometricError')
            info['refine'] = tile_obj.get('refine', 'REPLACE')
            content = tile_obj.get('content', {})
            if isinstance(content, dict):
                info['content_uri'] = content.get('uri', '')

        # Derive sub-URI for the reference
        sub_name = filepath.name
        info['subtileset_uri'] = f"./subtilesets/{sub_name}"

        # Build the external tileset.json
        if tile_obj:
            root_ge = tile_obj.get('geometricError', 5000)
        else:
            root_ge = 5000

        tileset = {
            "asset": {
                "version": "1.1"
            },
            "geometricError": root_ge,
            "root": tile_obj if tile_obj else {}
        }

        # Remove the root tile's own children (they're already nested within)
        # Actually KEEP the children – the external tileset should contain the full subtree.

        filepath.parent.mkdir(parents=True, exist_ok=True)
        with open(str(filepath), 'w', encoding='utf-8') as f:
            json.dump(tileset, f, separators=(',', ':'))

        print(f"  Wrote sub-tileset: {filepath.name} ({end_byte - start_byte:,} bytes)")


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def auto_select_depth(depth_counts):
    """Choose the best split depth automatically.

    Strategy: find the shallowest depth where the tile count is between
    min_tiles (100) and max_tiles (2000).  If no such depth exists, pick
    the depth closest to the target range.
    """
    min_tiles = 100
    max_tiles = 2000

    if not depth_counts:
        return 1

    sorted_depths = sorted(depth_counts.items())
    print("\nTile count per depth:")
    for depth, count in sorted_depths:
        marker = ""
        if min_tiles <= count <= max_tiles:
            marker = "  <-- candidate"
        print(f"  depth {depth:3d}: {count:8d} tiles{marker}")

    # Prefer depth within range
    best_depth = None
    best_score = float('inf')

    for depth, count in sorted_depths:
        if depth == 0:
            continue  # never split at root
        if min_tiles <= count <= max_tiles:
            # Within range – prefer shallower (fewer output files)
            score = depth
            if score < best_score:
                best_score = score
                best_depth = depth

    if best_depth is not None:
        return best_depth

    # No depth in range – pick the closest to max_tiles from below
    for depth, count in reversed(sorted_depths):
        if depth == 0:
            continue
        if count <= max_tiles:
            return depth

    # All depths have more than max_tiles – pick the shallowest non-root
    for depth, count in sorted_depths:
        if depth == 0:
            continue
        return depth

    return 1


def main():
    parser = argparse.ArgumentParser(
        description="Split a monolithic 3D Tiles tileset.json into hierarchical external tilesets."
    )
    parser.add_argument(
        'input', metavar='INPUT',
        help='Path to the input tileset.json'
    )
    parser.add_argument(
        'output_dir', metavar='OUTPUT_DIR',
        help='Directory for output files'
    )
    parser.add_argument(
        '--depth', '-d', type=int, default=None,
        help='Split depth (0=root).  If not specified, auto-determined.'
    )
    parser.add_argument(
        '--max-tiles', type=int, default=2000,
        help='Maximum tiles per sub-tileset for auto depth (default: 2000)'
    )
    parser.add_argument(
        '--min-tiles', type=int, default=100,
        help='Minimum tiles per sub-tileset for auto depth (default: 100)'
    )
    parser.add_argument(
        '--no-copy-data', action='store_true',
        help='Skip copying the Data directory (use symlink or manual copy)'
    )
    args = parser.parse_args()

    input_path = args.input
    output_dir = args.output_dir

    if not os.path.isfile(input_path):
        print(f"ERROR: Input file not found: {input_path}")
        sys.exit(1)

    print(f"Input:  {input_path}")
    print(f"Output: {output_dir}")
    file_size = os.path.getsize(input_path)
    print(f"Size:   {file_size / (1024**2):.1f} MB")

    # --- Pass 1: Scan ---
    print("\n=== Pass 1: Scanning tile hierarchy ===")
    t0 = time.time()
    scanner = TilesetScanner(input_path)
    depth_counts = scanner.scan()
    t1 = time.time()
    print(f"Scan complete in {t1 - t0:.1f}s")
    print(f"Total tiles found: {sum(depth_counts.values()):,}")
    print(f"Max depth: {scanner.max_depth_seen}")

    # --- Determine split depth ---
    if args.depth is not None:
        split_depth = args.depth
        print(f"\nUsing user-specified split depth: {split_depth}")
    else:
        # Override the class defaults for auto_select
        global _AUTO_MIN, _AUTO_MAX
        _AUTO_MIN = args.min_tiles
        _AUTO_MAX = args.max_tiles

        split_depth = auto_select_depth(depth_counts)
        print(f"\nAuto-selected split depth: {split_depth}")
        count_at_depth = depth_counts.get(split_depth, 0)
        print(f"Tiles at depth {split_depth}: {count_at_depth:,}")
        print(f"(Each will become a separate external tileset)")

    if split_depth not in depth_counts or depth_counts[split_depth] == 0:
        print(f"ERROR: No tiles at depth {split_depth}. Cannot split.")
        sys.exit(1)

    # --- Pass 2: Split ---
    print(f"\n=== Pass 2: Extracting subtrees at depth {split_depth} ===")
    t0 = time.time()
    splitter = TilesetSplitter(input_path, output_dir, split_depth)
    splitter.split()
    t1 = time.time()
    print(f"Split complete in {t1 - t0:.1f}s")

    # --- Summary ---
    print(f"\n=== Summary ===")
    print(f"Main tileset:   {output_dir}/tileset.json")
    print(f"Sub-tilesets:   {output_dir}/subtilesets/ ({splitter.subtileset_count} files)")
    if not args.no_copy_data:
        print(f"Data directory: {output_dir}/Data/")
    print(f"\nDone!")


if __name__ == '__main__':
    main()
