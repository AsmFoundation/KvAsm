"""Content-free statistics over the Claude Code transcripts that built KvNode.

The paper reports the *shape* of the agentic development effort: how many sessions,
human prompts and tool calls there were, which model ran, how much assembly was
edited, when the human intervened and how much wall-clock time the agent was active.
This script reads every ``<session-id>.jsonl`` transcript of the project directory and
writes those numbers as JSON so that each figure in the paper can be re-derived.

Privacy contract: nothing a person typed and nothing a tool returned is written out.
Prompts are counted, timed and de-duplicated through a SHA-256 digest that is never
emitted; only counts, timestamps, tool names and model ids leave the script.

Run: ``python session_stats.py <transcript-dir> [--exclude <session-id> ...]``
(the paper excludes the session that wrote it).
"""

from __future__ import annotations

import collections
import hashlib
import json
import pathlib
import sys
from datetime import datetime, timedelta

#: Two consecutive records further apart than this are an idle break (the human away,
#: a background benchmark running) rather than working time. 15 minutes keeps ordinary
#: long tool runs (a Docker build that runs every suite, a few-minute A/B block) as work,
#: so the active-time figures are a conservative lower bound, not an upper one.
ACTIVE_GAP = timedelta(minutes=15)

#: Text that Claude Code injects as a user record when a session resumes after its
#: context window was summarized; counted as a context compaction, never as a prompt.
COMPACTION_MARKER = "This session is being continued from a previous conversation"


def parse_ts(value: str | None) -> datetime | None:
    """Parse a transcript ISO-8601 timestamp (``...Z``) into an aware datetime.

    Args:
        value: The raw ``timestamp`` string of a record, or ``None`` when absent.

    Returns:
        The parsed datetime, or ``None`` when the record carries no timestamp, so
        callers skip it instead of inventing a time.

    Raises:
        ValueError: If a timestamp is present but not ISO-8601; a malformed transcript
            should fail loudly rather than silently skew the time span.
    """
    if not value:
        return None
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def prompt_text(rec: dict) -> str | None:
    """Return the text of a record if, and only if, it is a prompt the human typed.

    Tool results are also ``type == "user"`` records, and subagent traffic is marked
    ``isSidechain``; counting either would inflate the human's share of the work.
    Claude Code labels typed prompts with ``origin.kind == "human"``.

    Args:
        rec: One decoded JSONL record.

    Returns:
        The prompt text for a main-thread, human-typed prompt; ``None`` for every
        other record (tool results, compaction summaries, slash-command echoes).
    """
    if rec.get("type") != "user" or rec.get("isSidechain"):
        return None
    if (rec.get("origin") or {}).get("kind") != "human":
        return None
    content = (rec.get("message") or {}).get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        blocks = [b for b in content if isinstance(b, dict)]
        # A list content is a prompt only if it holds text and no tool_result block.
        if any(b.get("type") == "tool_result" for b in blocks):
            return None
        texts = [b.get("text", "") for b in blocks if b.get("type") == "text"]
        return " ".join(texts) if texts else None
    return None


def is_compaction(rec: dict) -> bool:
    """Tell whether a record is the summary injected after a context compaction.

    Args:
        rec: One decoded JSONL record.

    Returns:
        True for the main-thread user record that starts a continued session.
    """
    if rec.get("type") != "user" or rec.get("isSidechain"):
        return False
    content = (rec.get("message") or {}).get("content")
    return isinstance(content, str) and content.startswith(COMPACTION_MARKER)


def active_intervals(stamps: list[datetime]) -> list[tuple[datetime, datetime]]:
    """Merge a session's record timestamps into intervals of continuous activity.

    Args:
        stamps: Every timestamp seen in one transcript, in any order.

    Returns:
        Sorted, non-overlapping (start, end) intervals in which no gap between
        consecutive records exceeds ``ACTIVE_GAP``. A lone record yields a
        zero-length interval, which contributes nothing to the active time.
    """
    out: list[tuple[datetime, datetime]] = []
    for ts in sorted(stamps):
        if out and ts - out[-1][1] <= ACTIVE_GAP:
            out[-1] = (out[-1][0], ts)
        else:
            out.append((ts, ts))
    return out


def union_hours(intervals: list[tuple[datetime, datetime]]) -> float:
    """Total hours covered by at least one interval (parallel sessions counted once).

    Args:
        intervals: (start, end) pairs from any number of sessions; they may overlap.

    Returns:
        The length of their union in hours: wall-clock time during which at least
        one session was active.
    """
    total = timedelta(0)
    cur_start = cur_end = None
    for start, end in sorted(intervals):
        if cur_end is None or start > cur_end:
            if cur_end is not None:
                total += cur_end - cur_start
            cur_start, cur_end = start, end
        elif end > cur_end:
            cur_end = end
    if cur_end is not None:
        total += cur_end - cur_start
    return total.total_seconds() / 3600


def max_concurrency(intervals: list[tuple[datetime, datetime]]) -> int:
    """Largest number of sessions active at the same instant.

    Args:
        intervals: Active intervals of every session (zero-length ones are ignored,
            since a single record is not a working period).

    Returns:
        The peak count of overlapping intervals.
    """
    events: list[tuple[datetime, int]] = []
    for start, end in intervals:
        if end > start:
            events += [(start, 1), (end, -1)]
    # Ends sort before starts at the same instant, so touching intervals do not overlap.
    events.sort(key=lambda e: (e[0], e[1]))
    current = best = 0
    for _, delta in events:
        current += delta
        best = max(best, current)
    return best


def session_stats(path: pathlib.Path) -> tuple[dict, dict[str, tuple[int, int]]]:
    """Summarize one transcript into counts, prompt times and active intervals.

    Args:
        path: The ``<session-id>.jsonl`` transcript to read.

    Returns:
        ``(row, digests)``: the row holds the session id, first/last timestamps, span
        and active hours, prompt timestamps, compactions, assistant message count,
        tool calls by name, edits that touched ``.asm``/``.inc`` files, models seen
        and token sums; ``digests`` maps the SHA-256 hex digest of each prompt text to
        its (characters, words) size, for cross-session de-duplication only (digests
        are never written out).

    Raises:
        OSError: If the transcript cannot be read.
        json.JSONDecodeError: If a line is not valid JSON (a truncated transcript
            should be noticed, not silently skipped).
    """
    stamps: list[datetime] = []
    prompt_times: list[str] = []
    digests: dict[str, tuple[int, int]] = {}
    prompt_count = 0
    compactions = assistant_msgs = asm_edits = 0
    tools: collections.Counter = collections.Counter()
    models: collections.Counter = collections.Counter()
    out_tokens = in_tokens = 0
    seen_msg_ids: set[str] = set()
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            ts = parse_ts(rec.get("timestamp"))
            if ts is not None:
                stamps.append(ts)
            text = prompt_text(rec)
            if text is not None:
                text = text.strip()
                prompt_count += 1
                digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
                digests[digest] = (len(text), len(text.split()))
                if ts is not None:
                    prompt_times.append(ts.isoformat())
            if is_compaction(rec):
                compactions += 1
            if rec.get("type") != "assistant" or rec.get("isSidechain"):
                continue
            msg = rec.get("message") or {}
            # One API message is split into several records (one per content block)
            # and usage repeats on each, so usage is counted once per message id.
            mid = msg.get("id")
            if mid and mid not in seen_msg_ids:
                seen_msg_ids.add(mid)
                assistant_msgs += 1
                if msg.get("model") and msg["model"] != "<synthetic>":
                    models[msg["model"]] += 1
                usage = msg.get("usage") or {}
                out_tokens += usage.get("output_tokens") or 0
                in_tokens += ((usage.get("input_tokens") or 0)
                              + (usage.get("cache_read_input_tokens") or 0)
                              + (usage.get("cache_creation_input_tokens") or 0))
            for block in msg.get("content") or []:
                if not isinstance(block, dict) or block.get("type") != "tool_use":
                    continue
                name = block.get("name", "?")
                tools[name] += 1
                target = str((block.get("input") or {}).get("file_path", "")).lower()
                if name in ("Edit", "Write", "MultiEdit") and target.endswith((".asm", ".inc")):
                    asm_edits += 1
    intervals = active_intervals(stamps)
    first, last = (min(stamps), max(stamps)) if stamps else (None, None)
    row = {
        "session": path.stem,
        "first": first.isoformat() if first else None,
        "last": last.isoformat() if last else None,
        "span_hours": round((last - first).total_seconds() / 3600, 1) if stamps else 0.0,
        "active_hours": round(union_hours(intervals), 1),
        "intervals": [(a.isoformat(), b.isoformat()) for a, b in intervals if b > a],
        "human_prompts": prompt_count,
        "prompt_times": sorted(prompt_times),
        "context_compactions": compactions,
        "assistant_messages": assistant_msgs,
        "tool_calls": sum(tools.values()),
        "tools": dict(tools.most_common()),
        "asm_edits": asm_edits,
        "models": dict(models),
        "output_tokens": out_tokens,
        "input_tokens_incl_cache": in_tokens,
    }
    return row, digests


def main(argv: list[str]) -> int:
    """Print per-session rows and project totals for every transcript in a directory.

    Args:
        argv: ``[transcript_dir, "--exclude", id, ...]``; excluded session ids (the
            paper-writing session itself) are left out of every number.

    Returns:
        Process exit code: 0 on success, 2 on a usage error.
    """
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    directory = pathlib.Path(argv[0])
    excluded = set(argv[argv.index("--exclude") + 1:]) if "--exclude" in argv else set()
    rows: list[dict] = []
    distinct: dict[str, tuple[int, int]] = {}
    for path in sorted(directory.glob("*.jsonl")):
        if path.stem in excluded:
            continue
        row, digests = session_stats(path)
        rows.append(row)
        distinct.update(digests)
    rows.sort(key=lambda r: r["first"] or "")
    tools: collections.Counter = collections.Counter()
    for r in rows:
        tools.update(r["tools"])
    every_interval = [(datetime.fromisoformat(a), datetime.fromisoformat(b))
                      for r in rows for a, b in r["intervals"]]
    total = {
        "sessions": len(rows),
        "human_prompts": sum(r["human_prompts"] for r in rows),
        # The same prompt was sometimes sent to several parallel sessions at once,
        # so the human's actual input is measured over distinct prompt texts.
        "distinct_human_prompts": len(distinct),
        "distinct_prompt_chars": sum(chars for chars, _ in distinct.values()),
        "distinct_prompt_words": sum(words for _, words in distinct.values()),
        "context_compactions": sum(r["context_compactions"] for r in rows),
        "assistant_messages": sum(r["assistant_messages"] for r in rows),
        "tool_calls": sum(r["tool_calls"] for r in rows),
        "tools": dict(tools.most_common()),
        "asm_edits": sum(r["asm_edits"] for r in rows),
        "output_tokens": sum(r["output_tokens"] for r in rows),
        "input_tokens_incl_cache": sum(r["input_tokens_incl_cache"] for r in rows),
        "models": sorted({m for r in rows for m in r["models"]}),
        "span_hours_sum": round(sum(r["span_hours"] for r in rows), 1),
        "active_hours_sum": round(sum(r["active_hours"] for r in rows), 1),
        "active_hours_union": round(union_hours(every_interval), 1),
        "max_concurrent_sessions": max_concurrency(every_interval),
        "first": min((r["first"] for r in rows if r["first"]), default=None),
        "last": max((r["last"] for r in rows if r["last"]), default=None),
        "active_gap_minutes": ACTIVE_GAP.total_seconds() / 60,
    }
    print(json.dumps({"rows": rows, "total": total}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
