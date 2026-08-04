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


def _shadow_proxy_percent(value: str) -> float:
    parsed = float(value)
    if parsed < 0.01 or parsed >= 100.0:
        raise argparse.ArgumentTypeError(
            "percent must be at least 0.01 and less than 100"
        )
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="uepy",
        description="Inspect a running Unreal Editor through Python Remote Execution.",
    )
    parser.add_argument("--engine-root", help="Unreal installation or Engine directory")
    parser.add_argument(
        "--project", help="Select a discovered editor by project name/path"
    )
    parser.add_argument("--node", help="Select a discovered editor by node ID prefix")
    parser.add_argument(
        "--timeout", type=float, default=2.0, help="discovery time in seconds"
    )
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
        "descriptors",
        help="inspect World Partition actor descriptors, including unloaded actors",
    )
    descriptors.add_argument("--match", help="case-insensitive label/name substring")
    descriptors.add_argument("--limit", type=_positive_limit, default=10)

    asset = commands.add_parser("asset", help="inspect a saved Unreal asset")
    asset.add_argument("path", help="/Game object or package path")

    duplicate = commands.add_parser(
        "duplicate", help="duplicate and save an Unreal asset"
    )
    duplicate.add_argument("source", help="source /Game object or package path")
    duplicate.add_argument(
        "destination", help="destination /Game object or package path"
    )
    duplicate.add_argument(
        "--force",
        action="store_true",
        help="replace the destination asset if it already exists",
    )

    shadow_proxy = commands.add_parser(
        "shadow-proxy",
        help="bake and save a simplified sibling StaticMesh for shadows",
    )
    shadow_proxy.add_argument("source", help="source /Game StaticMesh path")
    shadow_proxy.add_argument(
        "--destination",
        help="destination path (default: sibling SourceName_Shadow)",
    )
    shadow_proxy.add_argument(
        "--percent",
        type=_shadow_proxy_percent,
        default=1.0,
        help="triangle percentage to retain (default: 1.0)",
    )
    shadow_proxy.add_argument(
        "--force",
        action="store_true",
        help="replace an existing SM_Name_Shadow with a freshly built asset",
    )

    animation = commands.add_parser(
        "animation", help="inspect or precisely edit an Animation Sequence"
    )
    animation.add_argument("path", help="/Game path to an Animation Sequence")
    animation.add_argument(
        "--promote-frame",
        type=int,
        help="remove all sampled frames before this zero-based frame index",
    )
    animation.add_argument(
        "--expected-fingerprint",
        help="fingerprint returned by an immediately preceding inspection",
    )
    animation.add_argument(
        "--apply",
        action="store_true",
        help="apply the reviewed operation transactionally without saving",
    )
    animation.add_argument(
        "--unsafe",
        action="store_true",
        help="acknowledge that applying an animation edit mutates editor state",
    )

    blueprint = commands.add_parser(
        "blueprint", help="inspect nodes, pins, and connections in a Blueprint graph"
    )
    blueprint.add_argument("path", help="/Game path to a Blueprint asset")
    blueprint.add_argument(
        "--graph", required=True, help="graph name, such as AnimGraph"
    )
    blueprint.add_argument(
        "--patch",
        type=Path,
        help="validate a versioned JSON patch against the graph",
    )
    blueprint.add_argument(
        "--apply",
        action="store_true",
        help="apply --patch transactionally without saving the asset",
    )
    blueprint.add_argument(
        "--unsafe",
        action="store_true",
        help="acknowledge that applying a graph patch mutates editor state",
    )

    mesh = commands.add_parser(
        "mesh", help="inspect static-mesh bounds, LODs, and materials"
    )
    mesh.add_argument("path", help="/Game path to a StaticMesh")

    material = commands.add_parser(
        "material", help="inspect a Material or Material Instance"
    )
    material.add_argument("path", help="/Game path to a Material or Material Instance")
    material.add_argument(
        "--parameters",
        choices=("all", "overrides", "none"),
        default="all",
        help="parameter detail to include (default: all)",
    )
    material.add_argument(
        "--reference-limit",
        type=_positive_limit,
        default=100,
        help="maximum asset dependencies and referencers to return",
    )

    evaluate = commands.add_parser(
        "eval", help="run an arbitrary expression (not read-only enforced)"
    )
    evaluate.add_argument("expression")
    evaluate.add_argument(
        "--unsafe",
        action="store_true",
        help="acknowledge that arbitrary Unreal Python can mutate editor state",
    )

    execute = commands.add_parser(
        "exec", help="run arbitrary Python (not read-only enforced)"
    )
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
    if args.command == "blueprint":
        if args.apply and args.patch is None:
            parser.error("blueprint --apply requires --patch")
        if args.apply and not args.unsafe:
            parser.error("blueprint --apply requires --unsafe")
    if args.command == "animation":
        if args.apply and args.promote_frame is None:
            parser.error("animation --apply requires --promote-frame")
        if args.promote_frame is not None and args.expected_fingerprint is None:
            parser.error("animation --promote-frame requires --expected-fingerprint")
        if args.apply and not args.unsafe:
            parser.error("animation --apply requires --unsafe")
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
            elif args.command == "duplicate":
                query_body = queries.duplicate_asset(
                    args.source,
                    args.destination,
                    force=args.force,
                )
            elif args.command == "shadow-proxy":
                query_body = queries.bake_shadow_proxy(
                    args.source,
                    destination_path=args.destination,
                    triangle_fraction=args.percent / 100.0,
                    force=args.force,
                )
            elif args.command == "animation":
                if args.promote_frame is None:
                    query_body = queries.animation(args.path)
                else:
                    query_body = queries.promote_animation_frame(
                        args.path,
                        args.promote_frame,
                        args.expected_fingerprint,
                        apply=args.apply,
                    )
            elif args.command == "blueprint":
                if args.patch is None:
                    query_body = queries.blueprint(args.path, args.graph)
                else:
                    query_body = queries.blueprint_patch(
                        args.path,
                        args.graph,
                        args.patch.read_text(encoding="utf-8"),
                        apply=args.apply,
                    )
            elif args.command == "mesh":
                query_body = queries.mesh(args.path)
            elif args.command == "material":
                query_body = queries.material(
                    args.path,
                    parameter_mode=args.parameters,
                    reference_limit=args.reference_limit,
                )

            if query_body is not None:
                result = client.query(query_body)
                if args.command == "duplicate" and not result.get("duplicated", False):
                    error = result.get("error", "Unreal could not duplicate the asset.")
                    print(f"uepy: error: {error}", file=sys.stderr)
                    return 1
                if args.command == "shadow-proxy" and not result.get("baked", False):
                    error = result.get(
                        "error", "Unreal could not bake the shadow proxy."
                    )
                    print(f"uepy: error: {error}", file=sys.stderr)
                    return 1
                _emit(result, args.compact)
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
