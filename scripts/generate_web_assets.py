#!/usr/bin/env python3

"""Generate C web assets for firmware embedding."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".ico": "image/x-icon",
    ".txt": "text/plain; charset=utf-8",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate embedded web assets C source.")
    parser.add_argument("--input-dir", required=True, help="Source directory with web files.")
    parser.add_argument("--output-c", required=True, help="Output .c file path.")
    return parser.parse_args()


def sanitize_identifier(value: str) -> str:
    ident = re.sub(r"[^0-9a-zA-Z_]", "_", value)
    if ident and ident[0].isdigit():
        ident = f"f_{ident}"
    return ident or "asset"


def bytes_to_c_array(data: bytes) -> str:
    if not data:
        return ""
    items = [f"0x{byte:02x}" for byte in data]
    lines: list[str] = []
    chunk_size = 12
    for index in range(0, len(items), chunk_size):
        chunk = ", ".join(items[index : index + chunk_size])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def content_type_for_path(path: Path) -> str:
    return CONTENT_TYPES.get(path.suffix.lower(), "application/octet-stream")


def main() -> int:
    args = parse_args()
    input_dir = Path(args.input_dir).resolve()
    output_c = Path(args.output_c).resolve()

    if not input_dir.exists() or not input_dir.is_dir():
        print(f"Input directory not found: {input_dir}", file=sys.stderr)
        return 1

    files = sorted(
        path
        for path in input_dir.iterdir()
        if path.is_file() and not path.name.startswith(".")
    )
    if not files:
        print(f"No files found in {input_dir}", file=sys.stderr)
        return 1

    if not any(path.name == "index.html" for path in files):
        print(f"index.html is required in {input_dir}", file=sys.stderr)
        return 1

    output_c.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    lines.append('#include "web/web_assets.h"')
    lines.append("")
    lines.append("#include <stddef.h>")
    lines.append("#include <stdint.h>")
    lines.append("#include <string.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("  const char *path;")
    lines.append("  const char *content_type;")
    lines.append("  const uint8_t *body;")
    lines.append("  size_t body_length;")
    lines.append("} web_asset_t;")
    lines.append("")

    symbols: dict[Path, str] = {}
    sizes: dict[Path, int] = {}
    for path in files:
        symbol = sanitize_identifier(path.name)
        data = path.read_bytes()
        symbols[path] = symbol
        sizes[path] = len(data)
        lines.append(f"static const uint8_t k_asset_{symbol}[] = {{")
        lines.append(bytes_to_c_array(data))
        lines.append("};")
        lines.append("")

    lines.append("static const web_asset_t k_assets[] = {")
    for path in files:
        symbol = symbols[path]
        content_type = content_type_for_path(path)
        route = f"/{path.name}"
        lines.append(
            f'    {{.path = "{route}", .content_type = "{content_type}", '
            f".body = k_asset_{symbol}, .body_length = {sizes[path]}u}},"
        )
        if path.name == "index.html":
            lines.append(
                f'    {{.path = "/", .content_type = "{content_type}", '
                f".body = k_asset_{symbol}, .body_length = {sizes[path]}u}},"
            )
    lines.append("};")
    lines.append("")
    lines.append("bool web_assets_get(const char *request_path, const char **content_type,")
    lines.append("                    const uint8_t **body, size_t *body_length) {")
    lines.append("  size_t index = 0u;")
    lines.append("")
    lines.append("  if (request_path == NULL || content_type == NULL || body == NULL ||")
    lines.append("      body_length == NULL) {")
    lines.append("    return false;")
    lines.append("  }")
    lines.append("")
    lines.append("  for (index = 0u; index < sizeof(k_assets) / sizeof(k_assets[0]); ++index) {")
    lines.append("    if (strcmp(request_path, k_assets[index].path) == 0) {")
    lines.append("      *content_type = k_assets[index].content_type;")
    lines.append("      *body = k_assets[index].body;")
    lines.append("      *body_length = k_assets[index].body_length;")
    lines.append("      return true;")
    lines.append("    }")
    lines.append("  }")
    lines.append("")
    lines.append("  return false;")
    lines.append("}")
    lines.append("")

    output_c.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
