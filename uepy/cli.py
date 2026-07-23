"""Command-line interface for live Unreal Editor inspection."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from . import queries
from .client import UnrealRemoteClient
from .errors import UepyError


MAX_INSPECTION_RESULTS = 100


def _positive_limit(value: str) -> int:
    parsed = int(value)
    if parsed < 1 or parsed > MAX_INSPECTION_RESULTS:
        raise argparse.ArgumentTypeError(
            f"limit must be between 1 and {MAX_INSPECTION_RESULTS}"
        )
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="uepy",
        description="Inspect a running Unreal Editor through Python Remote Execution.",
    )
    parser.add_argument("--engine-root", help="Unreal installation or Engine directory")
    parser.add_argument("--project", help="Select a discovered editor by project name/path")
    parser.add_argument("--node", help="Select a discovered editor by node ID prefix")
    parser.add_argument("--timeout", type=float, default=2.0, help="discovery time in seconds")
    parser.add_argument("--compact", action="store_true", help="emit compact JSON")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("nodes", help="list discoverable Unreal Editor nodes")
    commands.add_parser("status", help="show the selected editor node")
    commands.add_parser("world", help="inspect the current editor world")

    selected = commands.add_parser("selected", help="inspect selected loaded actors")
    selected.add_argument("--limit", type=_positive_limit, default=10)

    actors = commands.add_parser("actors", help="inspect loaded actors")
    actors.add_argument("--match", help="case-insensitive label/name substring")
    actors.add_argument("--class", dest="class_match", help="class-path substring")
    actors.add_argument("--limit", type=_positive_limit, default=10)

    actor = commands.add_parser("actor", help="inspect a loaded actor by label")
    actor.add_argument("label")
    actor.add_argument("--limit", type=_positive_limit, default=10)

    descriptors = commands.add_parser(
        "descriptors", help="inspect World Partition actor descriptors, including unloaded actors"
    )
    descriptors.add_argument("--match", help="case-insensitive label/name substring")
    descriptors.add_argument("--limit", type=_positive_limit, default=10)

    asset = commands.add_parser("asset", help="inspect a saved Unreal asset")
    asset.add_argument("path", help="/Game object or package path")

    mesh = commands.add_parser("mesh", help="inspect static-mesh bounds, LODs, and materials")
    mesh.add_argument("path", help="/Game path to a StaticMesh")

    evaluate = commands.add_parser("eval", help="run an arbitrary expression (not read-only enforced)")
    evaluate.add_argument("expression")
    evaluate.add_argument(
        "--unsafe",
        action="store_true",
        help="acknowledge that arbitrary Unreal Python can mutate editor state",
    )

    execute = commands.add_parser("exec", help="run arbitrary Python (not read-only enforced)")
    source = execute.add_mutually_exclusive_group(required=True)
    source.add_argument("--code", help="literal multiline Python code")
    source.add_argument("--file", type=Path, help="local Python file to send to Unreal")
    execute.add_argument(
        "--unsafe",
        action="store_true",
        help="acknowledge that arbitrary Unreal Python can mutate editor state",
    )
    return parser


def _emit(value: Any, compact: bool) -> None:
    if compact:
        print(json.dumps(value, ensure_ascii=False, separators=(",", ":")))
    else:
        print(json.dumps(value, ensure_ascii=False, indent=2))


def _client(args: argparse.Namespace) -> UnrealRemoteClient:
    return UnrealRemoteClient(
        engine_root=args.engine_root,
        discovery_timeout=args.timeout,
        project=args.project,
        node=args.node,
    )


def _raw_response(response: dict[str, Any]) -> dict[str, Any]:
    return {
        "success": bool(response.get("success")),
        "result": response.get("result"),
        "output": response.get("output", []),
    }


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        with _client(args) as client:
            if args.command == "nodes":
                _emit(client.discover(), args.compact)
                return 0
            if args.command == "status":
                _emit(client.connect(), args.compact)
                return 0

            query_body: str | None = None
            if args.command == "world":
                query_body = queries.world()
            elif args.command == "selected":
                query_body = queries.selected(args.limit)
            elif args.command == "actors":
                query_body = queries.actors(args.match, args.class_match, args.limit)
            elif args.command == "actor":
                query_body = queries.actor(args.label, args.limit)
            elif args.command == "descriptors":
                query_body = queries.actor_descriptors(args.match, args.limit)
            elif args.command == "asset":
                query_body = queries.asset(args.path)
            elif args.command == "mesh":
                query_body = queries.mesh(args.path)

            if query_body is not None:
                _emit(client.query(query_body), args.compact)
                return 0

            if args.command in {"eval", "exec"} and not args.unsafe:
                parser.error(f"{args.command} requires --unsafe")
            if args.command == "eval":
                _emit(_raw_response(client.evaluate(args.expression)), args.compact)
                return 0
            if args.command == "exec":
                script = args.code
                if args.file:
                    script = args.file.read_text(encoding="utf-8")
                _emit(_raw_response(client.execute(script)), args.compact)
                return 0

        parser.error("unknown command")
    except (OSError, UnicodeError, UepyError) as exc:
        print(f"uepy: error: {exc}", file=sys.stderr)
        return 1
    return 0
