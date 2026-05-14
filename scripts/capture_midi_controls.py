#!/usr/bin/env python3
"""Capture MIDI controls in first-seen order and export them to JSON.

Usage examples:
  python3 scripts/capture_midi_controls.py --list-ports
  python3 scripts/capture_midi_controls.py --port "USB2.0-MIDI"

By default, note-off style events are ignored so each button press usually records
one useful signal (note-on / cc / etc). Add --include-note-off if needed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import signal
import sys
from pathlib import Path
from typing import Any


def _build_parser() -> argparse.ArgumentParser:
    default_output = (
        Path.home()
        / "Library"
        / "Application Support"
        / "Sonik"
        / "MidiMappings"
        / "behringer_ddm4000_capture.json"
    )

    parser = argparse.ArgumentParser(
        description="Capture MIDI controls in first-seen order and write a JSON log."
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List available MIDI input ports and exit.",
    )
    parser.add_argument(
        "--port",
        default="",
        help=(
            "Input port name (exact or substring). If omitted and one input exists, "
            "it is selected automatically."
        ),
    )
    parser.add_argument(
        "--output",
        default=str(default_output),
        help=f"Output JSON file path (default: {default_output}).",
    )
    parser.add_argument(
        "--include-note-off",
        action="store_true",
        help="Include note_off and note_on(velocity=0) events.",
    )
    parser.add_argument(
        "--include-realtime",
        action="store_true",
        help="Include realtime MIDI traffic (clock/start/stop/active_sensing/reset).",
    )
    parser.add_argument(
        "--max-events",
        type=int,
        default=0,
        help="Optional max number of accepted events before auto-stop (0 = no limit).",
    )
    parser.add_argument(
        "--no-prompt-labels",
        action="store_true",
        help="Disable interactive label prompt for each newly discovered control.",
    )
    return parser


def _import_mido() -> Any:
    try:
        import mido

        return mido
    except Exception:
        print(
            "Missing dependency: mido (with python-rtmidi backend).\n"
            "Install with:\n"
            "  python3 -m pip install mido python-rtmidi",
            file=sys.stderr,
        )
        raise SystemExit(2)


def _select_port(port_names: list[str], requested: str) -> str:
    if not port_names:
        raise RuntimeError("No MIDI input ports available.")

    if requested:
        lower_requested = requested.lower()

        exact = [p for p in port_names if p == requested]
        if exact:
            return exact[0]

        partial = [p for p in port_names if lower_requested in p.lower()]
        if len(partial) == 1:
            return partial[0]
        if len(partial) > 1:
            raise RuntimeError(
                "Multiple ports match --port. Use a more specific value:\n"
                + "\n".join(f"  - {p}" for p in partial)
            )

        raise RuntimeError(
            "No input port matches --port value. Available ports:\n"
            + "\n".join(f"  - {p}" for p in port_names)
        )

    if len(port_names) == 1:
        return port_names[0]

    raise RuntimeError(
        "Multiple MIDI input ports found. Pass --port with a name or substring:\n"
        + "\n".join(f"  - {p}" for p in port_names)
    )


def _status_from_message(msg: Any) -> str:
    message_type = getattr(msg, "type", "unknown")

    mapping = {
        "note_on": "note",
        "note_off": "note_off",
        "control_change": "cc",
        "program_change": "program",
        "pitchwheel": "pitchwheel",
        "aftertouch": "aftertouch",
        "polytouch": "polytouch",
        "sysex": "sysex",
        "start": "start",
        "continue": "continue",
        "stop": "stop",
        "clock": "clock",
        "active_sensing": "active_sensing",
        "reset": "reset",
    }
    return mapping.get(message_type, message_type)


def _data_fields(msg: Any) -> tuple[int | None, int | None]:
    message_type = getattr(msg, "type", "")

    if message_type in ("note_on", "note_off"):
        return int(msg.note), int(msg.velocity)
    if message_type == "control_change":
        return int(msg.control), int(msg.value)
    if message_type == "program_change":
        return int(msg.program), None
    if message_type == "pitchwheel":
        return None, int(msg.pitch)
    if message_type == "aftertouch":
        return None, int(msg.value)
    if message_type == "polytouch":
        return int(msg.note), int(msg.value)

    return None, None


def _normalized_event(msg: Any, event_index: int) -> dict[str, Any]:
    status = _status_from_message(msg)
    channel = int(msg.channel) + 1 if hasattr(msg, "channel") else None
    data1, data2 = _data_fields(msg)

    return {
        "eventIndex": event_index,
        "timestampUtc": dt.datetime.now(dt.UTC).isoformat(),
        "status": status,
        "channel": channel,
        "data1": data1,
        "data2": data2,
        "rawBytes": [int(b) for b in msg.bytes()],
    }


def _control_signature(event: dict[str, Any]) -> tuple[Any, ...]:
    status = event["status"]
    channel = event["channel"]
    data1 = event["data1"]

    if status in ("note", "note_off"):
        return ("note", channel, data1)
    if status == "cc":
        return ("cc", channel, data1)
    if status == "program":
        return ("program", channel, data1)
    if status == "pitchwheel":
        return ("pitchwheel", channel)
    if status == "aftertouch":
        return ("aftertouch", channel)
    if status == "polytouch":
        return ("polytouch", channel, data1)

    return (
        status,
        channel,
        data1,
        tuple(event.get("rawBytes") or []),
    )


def _should_include(msg: Any, include_note_off: bool, include_realtime: bool) -> bool:
    message_type = getattr(msg, "type", "")

    realtime = {"clock", "start", "continue", "stop", "active_sensing", "reset"}
    if message_type in realtime and not include_realtime:
        return False

    if not include_note_off:
        if message_type == "note_off":
            return False
        if message_type == "note_on" and int(getattr(msg, "velocity", 0)) == 0:
            return False

    return True


def _write_payload(
    output_path: Path,
    selected_port: str,
    args: argparse.Namespace,
    unique_controls: list[dict[str, Any]],
    events: list[dict[str, Any]],
) -> None:
    payload = {
        "capturedAtUtc": dt.datetime.now(dt.UTC).isoformat(),
        "inputPort": selected_port,
        "captureOptions": {
            "includeNoteOff": bool(args.include_note_off),
            "includeRealtime": bool(args.include_realtime),
            "maxEvents": int(args.max_events),
            "promptLabels": not bool(args.no_prompt_labels),
        },
        "summary": {
            "acceptedEventCount": len(events),
            "uniqueControlCount": len(unique_controls),
        },
        "capturedControlsInOrder": unique_controls,
        "eventsInOrder": events,
    }

    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _prompt_label_for_event(event: dict[str, Any]) -> str:
    print(
        "\nNew control detected:",
        f"status={event['status']}",
        f"ch={event['channel']}",
        f"d1={event['data1']}",
        f"d2={event['data2']}",
    )
    print("Enter label (blank = leave empty, 'skip' = leave empty): ", end="", flush=True)

    try:
        raw = input()
    except EOFError:
        return ""

    text = raw.strip()
    if text.lower() == "skip":
        return ""
    return text


def main() -> int:
    args = _build_parser().parse_args()
    mido = _import_mido()

    port_names = list(mido.get_input_names())

    if args.list_ports:
        if not port_names:
            print("No MIDI input ports found.")
            return 1

        print("Available MIDI input ports:")
        for name in port_names:
            print(f"- {name}")
        return 0

    try:
        selected_port = _select_port(port_names, args.port)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    output_path = Path(args.output).expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    stop_requested = False

    def _handle_stop(_sig: int, _frame: Any) -> None:
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGINT, _handle_stop)
    signal.signal(signal.SIGTERM, _handle_stop)

    print(f"Listening on MIDI input: {selected_port}")
    print("Press controls on your mixer. Press Ctrl+C to finish and save JSON.")
    print(f"Output file: {output_path}")
    if args.no_prompt_labels:
        print("Label prompt disabled (--no-prompt-labels).")
    else:
        print("You will be asked a label for each new control the first time it appears.")

    events: list[dict[str, Any]] = []
    unique_controls: list[dict[str, Any]] = []
    signature_to_index: dict[tuple[Any, ...], int] = {}
    accepted_count = 0

    with mido.open_input(selected_port) as inport:
        for msg in inport:
            if stop_requested:
                break

            if not _should_include(msg, args.include_note_off, args.include_realtime):
                continue

            accepted_count += 1
            event = _normalized_event(msg, accepted_count)
            events.append(event)

            signature = _control_signature(event)
            existing_index = signature_to_index.get(signature)
            if existing_index is None:
                label = ""
                if not args.no_prompt_labels:
                    label = _prompt_label_for_event(event)

                signature_to_index[signature] = len(unique_controls)
                unique_controls.append(
                    {
                        "captureIndex": len(unique_controls) + 1,
                        "label": label,
                        "status": event["status"],
                        "channel": event["channel"],
                        "data1": event["data1"],
                        "firstSeenData2": event["data2"],
                        "firstSeenRawBytes": event["rawBytes"],
                        "firstSeenEventIndex": event["eventIndex"],
                        "occurrences": 1,
                    }
                )
            else:
                unique_controls[existing_index]["occurrences"] += 1

            print(
                f"[{event['eventIndex']:04d}] "
                f"{event['status']} ch={event['channel']} "
                f"d1={event['data1']} d2={event['data2']}"
            )

            # Keep output file updated in real-time in case capture is interrupted.
            _write_payload(output_path, selected_port, args, unique_controls, events)

            if args.max_events > 0 and accepted_count >= args.max_events:
                break

    _write_payload(output_path, selected_port, args, unique_controls, events)

    print("\nCapture completed.")
    print(f"Accepted events: {len(events)}")
    print(f"Unique controls: {len(unique_controls)}")
    print(f"Saved JSON: {output_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
