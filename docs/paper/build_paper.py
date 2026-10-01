"""Build the KvNode case-study paper: its figures (PNG) and the Word document (.docx).

The paper's text lives in ``paper.md``, a small Markdown dialect that stays readable as
plain text; this script turns it into an academically styled ``.docx`` with python-docx
and draws every figure with matplotlib from the repository's own records. Numbers that
come from data files (code growth, session statistics, machine-code sizes) are read from
``data/``; numbers published in the benchmark READMEs and ADRs are transcribed here with
their source next to them, because those READMEs are the citable record.

Before anything is written, :func:`check_paper` cross-checks the text: figure, table and
listing numbers must run 1..n in order, every ``[n]`` citation must resolve to a
reference and every reference must be cited, and key figures quoted in the prose must
equal the values derived from the data files. A failed check aborts the build, so a stale
number cannot ship silently.

Run from anywhere: ``python docs/paper/build_paper.py`` (needs matplotlib and
python-docx; fonts Segoe UI, Cambria, Calibri and Consolas, standard on Windows, with
DejaVu fallbacks elsewhere).
"""

from __future__ import annotations

import json
import math
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # file output only; never open a window (also safe headless)
import matplotlib.pyplot as plt  # noqa: E402  (must follow the backend selection)
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, PathPatch  # noqa: E402
from matplotlib.path import Path as MplPath  # noqa: E402
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator  # noqa: E402

from docx import Document  # noqa: E402
from docx.enum.style import WD_STYLE_TYPE  # noqa: E402
from docx.enum.table import WD_TABLE_ALIGNMENT  # noqa: E402
from docx.enum.text import WD_ALIGN_PARAGRAPH  # noqa: E402
from docx.opc.constants import RELATIONSHIP_TYPE  # noqa: E402
from docx.oxml import OxmlElement  # noqa: E402
from docx.oxml.ns import qn  # noqa: E402
from docx.shared import Cm, Inches, Pt, RGBColor  # noqa: E402

# --------------------------------------------------------------------------------------
# Paths and fixed facts
# --------------------------------------------------------------------------------------

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FIG_DIR = HERE / "figures"
DATA_DIR = HERE / "data"
SOURCE = HERE / "paper.md"
OUTPUT = HERE / "KvNode-agentic-assembly-paper.docx"

#: The revision the paper describes. Later commits (this paper among them) must not
#: change the counts it quotes, so every git-derived fact is taken at this revision.
STUDY_REVISION = "6871e31"

#: Short running head printed on every page after the first.
RUNNING_HEAD = "Measured, Not Argued — agentic development of KvNode in x86-64 assembly"

# --------------------------------------------------------------------------------------
# Visual system (validated with the dataviz skill's palette validator, light mode on a
# white surface: lightness band, chroma floor, adjacent CVD separation and the
# normal-vision floor all pass for slots 1-6; slots 3-5 sit below 3:1 contrast, so every
# chart that uses them carries direct labels and its values also appear in a table).
# --------------------------------------------------------------------------------------

SURFACE = "#ffffff"   # print surface (paper)
INK = "#0b0b0b"       # primary text
INK2 = "#52514e"      # secondary text: labels, values, legends
MUTED = "#898781"     # de-emphasis marks (rival systems, context segments)
GRID = "#e1e0d9"      # hairline gridlines
BASELINE = "#c3c2b7"  # axes and baselines
NEUTRAL_FILL = "#f0efec"  # diagram boxes
ACCENT_FILL = "#cde2fb"   # sequential blue step 100: highlighted diagram boxes
#: Categorical slots in their fixed, validated order (blue, orange, aqua, yellow,
#: magenta, green, violet, red). Never cycled: a 9th series would fold into "other".
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
DPI = 220

#: The six compared systems in the order every table and chart uses, so each keeps
#: its color everywhere (color follows the entity, never its rank).
SYSTEMS = ["KvNode", "Redis", "Valkey", "Dragonfly", "Garnet", "Memcached"]
SYSTEM_COLOR = dict(zip(SYSTEMS, SERIES))
#: Marker shapes repeat the color encoding for grayscale print and color-vision deficiency.
SYSTEM_MARKER = dict(zip(SYSTEMS, ["o", "s", "^", "D", "v", "p"]))

# Word typography.
BODY_FONT = "Cambria"
TABLE_FONT = "Calibri"
CODE_FONT = "Consolas"
TEXT_WIDTH_CM = 16.6  # A4 (21 cm) minus 2 x 2.2 cm margins


# ======================================================================================
# Data
# ======================================================================================

def load_json(name: str) -> dict | list:
    """Load one of the paper's committed data files.

    Args:
        name: File name inside ``docs/paper/data`` (e.g. ``"loc_history.json"``).

    Returns:
        The decoded JSON value.

    Raises:
        FileNotFoundError: If the data file was never generated; rerun the matching
            script in ``docs/paper/tools``.
        json.JSONDecodeError: If the file is corrupt.
    """
    with (DATA_DIR / name).open(encoding="utf-8") as fh:
        return json.load(fh)


def git(*args: str) -> str:
    """Run git in the repository and return its standard output.

    Args:
        *args: Arguments after ``git``.

    Returns:
        Decoded stdout.

    Raises:
        subprocess.CalledProcessError: If git fails (for instance, the study revision
            is missing from a shallow clone).
    """
    return subprocess.run(("git", "-C", str(REPO)) + args, check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout


def study_commits() -> list[dict]:
    """Commits on the first-parent line up to the study revision, with code sizes.

    ``loc_history.json`` is generated at whatever HEAD existed then; this trims it to
    the studied revision so later commits never leak into the paper's figures.

    Returns:
        ``loc_history.json`` rows (oldest first) up to and including
        ``STUDY_REVISION``.

    Raises:
        ValueError: If the study revision is not in the data file, which means the data
            predates the revision the paper describes.
    """
    rows = load_json("loc_history.json")
    cut = next((i for i, r in enumerate(rows) if r["commit"] == STUDY_REVISION), None)
    if cut is None:
        raise ValueError(f"{STUDY_REVISION} not in loc_history.json; regenerate it")
    return rows[: cut + 1]


def first_prompt_time() -> datetime:
    """Time of the developer's first prompt, the origin of every "hours since" axis.

    Returns:
        The aware datetime of the earliest record in the session statistics.
    """
    return datetime.fromisoformat(load_json("session_stats.json")["total"]["first"])


def hours_since_start(iso: str) -> float:
    """Convert an ISO-8601 timestamp to hours after the first prompt.

    Args:
        iso: Timestamp with a UTC offset (git ``%cI`` or transcript ISO strings).

    Returns:
        Elapsed hours (may be fractional).
    """
    return (datetime.fromisoformat(iso) - first_prompt_time()).total_seconds() / 3600


def object_code_bytes() -> int:
    """Total executable bytes in KvNode's assembled objects (the binary itself excluded).

    Returns:
        Sum of the ``text`` totals of every ``obj/*.o`` entry in ``elf_sizes.json``.
    """
    files = load_json("elf_sizes.json")["files"]
    return sum(v["text"] for k, v in files.items() if k.endswith(".o"))


def benchmark_run_records() -> int:
    """Count benchmark runs committed as raw JSON lines under ``benchmarks/results``.

    Returns:
        Number of non-empty lines across every ``*.jsonl`` file (one line = one run).
    """
    total = 0
    for path in sorted((REPO / "benchmarks" / "results").rglob("*.jsonl")):
        with path.open(encoding="utf-8") as fh:
            total += sum(1 for line in fh if line.strip())
    return total


# ======================================================================================
# Figure helpers
# ======================================================================================

def setup_matplotlib() -> None:
    """Apply the paper's chart style: system sans, hairline chrome, recessive axes.

    Footgun: ``font.family`` lists a fallback, so a machine without Segoe UI renders
    with DejaVu Sans instead of failing; glyph metrics then differ slightly.
    """
    plt.rcParams.update({
        "font.family": ["Segoe UI", "DejaVu Sans"],
        "font.size": 7.5,
        "axes.titlesize": 8,
        "axes.titleweight": "semibold",
        "axes.titlecolor": INK,
        "axes.titlelocation": "left",
        "axes.titlepad": 6,
        "axes.edgecolor": BASELINE,
        "axes.linewidth": 0.7,
        "axes.labelcolor": INK2,
        "axes.labelsize": 7.5,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "xtick.color": BASELINE,
        "ytick.color": BASELINE,
        "xtick.labelcolor": INK2,
        "ytick.labelcolor": INK2,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "xtick.major.size": 0,
        "ytick.major.size": 0,
        "xtick.minor.size": 0,
        "ytick.minor.size": 0,
        "legend.frameon": False,
        "legend.fontsize": 7,
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "savefig.dpi": DPI,
        # Math text ($2^k$) in the same sans as everything else, never Computer Modern.
        "mathtext.fontset": "custom",
        "mathtext.rm": "Segoe UI",
        "mathtext.it": "Segoe UI:italic",
        "mathtext.default": "regular",
    })


def hairline_grid(ax, axis: str = "y") -> None:
    """Draw solid, one-step-off-surface gridlines behind the data.

    Args:
        ax: The axes.
        axis: ``"x"``, ``"y"`` or ``"both"``.
    """
    ax.grid(axis=axis, color=GRID, linewidth=0.6, linestyle="-")
    ax.set_axisbelow(True)


def save(fig, name: str) -> Path:
    """Write a figure as PNG into ``figures/`` and close it.

    Args:
        fig: The matplotlib figure.
        name: File stem, e.g. ``"fig4_throughput"``.

    Returns:
        The written path.
    """
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    path = FIG_DIR / f"{name}.png"
    fig.savefig(path, dpi=DPI, bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)
    return path


def rounded_bar(ax, base: float, start: float, thickness: float, length: float, color: str,
                horizontal: bool = False, radius_pt: float = 2.2, zorder: float = 3,
                round_end: bool = True, fill: bool = True) -> None:
    """Draw one bar whose data-end is rounded and whose baseline end is square.

    The corner radius is specified in points and converted to data units from the axes'
    current transform, so axis limits and the figure layout must be final before the
    call (later layout changes would distort the corners, not the bar's extent).
    Only valid on linear axes.

    Args:
        ax: The axes to draw into.
        base: Where the bar grows from (y for vertical bars, x for horizontal).
        start: The bar's lower-left coordinate across its thickness (x for vertical
            bars, y for horizontal ones).
        thickness: Bar thickness in data units across the category axis.
        length: Signed length along the value axis; zero draws nothing.
        color: Fill color.
        horizontal: True for a bar growing along x.
        radius_pt: Corner radius in points, clamped to half the thickness and to the
            bar's length.
        zorder: Drawing order.
        round_end: False draws a plain rectangle (used for interior stack segments).
        fill: False draws only the outline (used for the hollow "not significant" bars).
    """
    if length == 0:
        return
    # Data units per point along each axis, from the current data->pixel transform.
    (x0p, y0p), (x1p, y1p) = ax.transData.transform([(0, 0), (1, 1)])
    px_per_pt = ax.figure.dpi / 72.0
    ux = px_per_pt / abs(x1p - x0p)
    uy = px_per_pt / abs(y1p - y0p)
    r_across = min(radius_pt * (uy if horizontal else ux), thickness / 2)
    r_along = min(radius_pt * (ux if horizontal else uy), abs(length))
    if not round_end:
        r_across = r_along = 0.0
    sign = 1 if length > 0 else -1
    end = base + length
    lo, hi = start, start + thickness
    a = end - sign * r_along  # where the rounding starts along the value axis
    # Path in (along, across) coordinates; swapped into (x, y) for vertical bars.
    pts = [(base, lo), (a, lo), (end, lo), (end, lo + r_across), (end, hi - r_across),
           (end, hi), (a, hi), (base, hi), (base, lo)]
    codes = [MplPath.MOVETO, MplPath.LINETO, MplPath.CURVE3, MplPath.CURVE3, MplPath.LINETO,
             MplPath.CURVE3, MplPath.CURVE3, MplPath.LINETO, MplPath.CLOSEPOLY]
    if not horizontal:
        pts = [(across, along) for along, across in pts]
    patch = PathPatch(MplPath(pts, codes), facecolor=color if fill else SURFACE,
                      edgecolor=color if not fill else "none", linewidth=0.9, zorder=zorder)
    ax.add_patch(patch)


def ink_on(fill: str) -> str:
    """Pick white or primary ink for text set inside a colored fill, by WCAG contrast.

    Args:
        fill: The fill color as ``#rrggbb``.

    Returns:
        ``"white"`` or :data:`INK`, whichever contrasts more with the fill.
    """
    def luminance(hex_color: str) -> float:
        """WCAG relative luminance of an sRGB color.

        Args:
            hex_color: ``#rrggbb``.

        Returns:
            Relative luminance in [0, 1].
        """
        channels = [int(hex_color.lstrip("#")[i:i + 2], 16) / 255 for i in (0, 2, 4)]
        lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
        return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]

    fill_l = luminance(fill)
    white = 1.05 / (fill_l + 0.05)
    ink = (fill_l + 0.05) / (luminance(INK) + 0.05)
    return "white" if white >= ink else INK


def diagram_box(ax, x: float, y: float, w: float, h: float, text: str, *,
                fill: str = NEUTRAL_FILL, edge: str = BASELINE, size: float = 6.6,
                weight: str = "normal", color: str = INK, ha: str = "center",
                lw: float = 0.7, round_size: float = 1.2) -> FancyBboxPatch:
    """Draw a rounded diagram box with centered (or left-aligned) text.

    Args:
        ax: Axes in diagram coordinates.
        x: Left edge.
        y: Bottom edge.
        w: Width.
        h: Height.
        text: Box text; ``\\n`` separates lines.
        fill: Fill color.
        edge: Outline color.
        size: Font size in points.
        weight: Font weight of the text.
        color: Text color.
        ha: ``"center"`` or ``"left"``.
        lw: Outline width in points.
        round_size: Corner radius in data units.

    Returns:
        The box patch, so arrows can attach to its outline.
    """
    box = FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0,rounding_size={round_size}",
                         facecolor=fill, edgecolor=edge, linewidth=lw, zorder=2)
    ax.add_patch(box)
    tx = x + w / 2 if ha == "center" else x + 1.2
    ax.text(tx, y + h / 2, text, ha=ha, va="center", fontsize=size, color=color,
            fontweight=weight, zorder=3, linespacing=1.25)
    return box


def arrow(ax, start: tuple[float, float], end: tuple[float, float], *, color: str = INK2,
          rad: float = 0.0, lw: float = 0.8, both: bool = False, patch_a=None,
          patch_b=None) -> None:
    """Draw a thin connector arrow between two points (or two patches' outlines).

    Args:
        ax: Diagram axes.
        start: Tail point in data coordinates.
        end: Head point in data coordinates.
        color: Line color.
        rad: Arc curvature (0 = straight, negative = clockwise bend).
        lw: Line width in points.
        both: True for a double-headed arrow.
        patch_a: Optional patch the tail is clipped to.
        patch_b: Optional patch the head is clipped to.
    """
    style = "<|-|>" if both else "-|>"
    ax.add_patch(FancyArrowPatch(start, end, arrowstyle=style, mutation_scale=7,
                                 color=color, linewidth=lw, zorder=4,
                                 connectionstyle=f"arc3,rad={rad}",
                                 patchA=patch_a, patchB=patch_b, shrinkA=1.5, shrinkB=1.5))


def diagram_axes(width_in: float, height_in: float, xmax: float = 100.0):
    """Create a borderless axes whose data units are equal in x and y.

    Args:
        width_in: Figure width in inches.
        height_in: Figure height in inches.
        xmax: Width of the coordinate system; the height follows from the aspect.

    Returns:
        ``(fig, ax, ymax)`` where ``ymax`` is the top of the coordinate system.
    """
    fig = plt.figure(figsize=(width_in, height_in))
    ax = fig.add_axes((0, 0, 1, 1))
    ymax = xmax * height_in / width_in
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, ymax)
    ax.axis("off")
    return fig, ax, ymax


# ======================================================================================
# Figures
# ======================================================================================

def fig1_architecture() -> Path:
    """Figure 1: KvNode's process structure (clients, kernel, workers, shared store).

    Returns:
        Path of the written PNG.
    """
    fig, ax, ymax = diagram_axes(6.6, 3.55)
    # Clients and the kernel's listener fan-out.
    diagram_box(ax, 0.5, 19.5, 15.5, 15.5,
                "gRPC clients\n\n.NET client\nkvbench\nh2spec · grpc-dotnet", size=6.6)
    diagram_box(ax, 20.5, 17.5, 10.5, 19.5, "Linux\nkernel\n\nSO_REUSE-\nPORT\nlisteners", size=6.4)
    arrow(ax, (16.0, 27.25), (20.5, 27.25))
    ax.text(18.25, 29.4, "h2c\n:50051", ha="center", va="bottom", fontsize=5.8, color=INK2)

    # Process boundary: the accent hue marks the assembly process, text stays ink.
    ax.add_patch(FancyBboxPatch((34.0, 0.8), 65.2, ymax - 1.6,
                                boxstyle="round,pad=0,rounding_size=1.6", facecolor="none",
                                edgecolor=SERIES[0], linewidth=1.0, zorder=1))
    ax.text(35.4, ymax - 3.0, "kvnode process  ·  x86-64 assembly  ·  static-PIE, FROM scratch, non-root",
            ha="left", va="center", fontsize=6.5, color=INK, fontweight="semibold")

    diagram_box(ax, 36.0, 40.6, 38.6, 8.2,
                "main thread\nsettings → port probe → SipHash key → store → snapshot\n"
                "→ workers → sigwaitinfo; graceful drain on SIGTERM", size=5.9)
    diagram_box(ax, 80.0, 40.6, 17.4, 8.2, "snapshot file\nKVNSNAP1, verified", size=6.2)
    # The gap between the boxes is sized so the "startup" label fits without touching.
    arrow(ax, (80.0, 44.0), (74.6, 44.0))
    ax.text(77.3, 45.0, "startup", ha="center", va="bottom", fontsize=5.6, color=INK2)

    # Three worker rows: epoll loop -> native HTTP/2 engine (highlighted) -> gRPC layer.
    rows = [(27.8, "worker 0"), (15.8, "worker 1"), (2.4, "worker n−1")]
    for y, label in rows:
        diagram_box(ax, 36.0, y, 40.5, 10.4, "", fill=NEUTRAL_FILL, edge=BASELINE, round_size=1.0)
        ax.text(37.2, y + 9.0, label, ha="left", va="center", fontsize=5.9, color=INK2,
                fontweight="semibold")
        diagram_box(ax, 37.2, y + 1.2, 10.6, 6.2, "epoll loop\ntwo-phase", size=5.9, fill=SURFACE)
        diagram_box(ax, 50.6, y + 1.2, 12.2, 6.2, "HTTP/2 engine\nh2n · hpack", size=5.9,
                    fill=ACCENT_FILL, edge=SERIES[0])
        diagram_box(ax, 65.6, y + 1.2, 9.9, 6.2, "gRPC · proto\nbatched GETs", size=5.9,
                    fill=SURFACE)
        arrow(ax, (47.8, y + 4.3), (50.6, y + 4.3), lw=0.7)
        arrow(ax, (62.8, y + 4.3), (65.6, y + 4.3), lw=0.7)
        arrow(ax, (31.0, 27.25), (36.0, y + 5.2), lw=0.7)
        # Workers read the store lock-free and write through the one writer path.
        arrow(ax, (75.5, y + 4.3), (80.0, y + 4.3), lw=0.7, both=True)
    ax.text(56.2, 14.1, "⋮", ha="center", va="center", fontsize=9, color=INK2)

    diagram_box(ax, 80.0, 2.4, 17.4, 35.8,
                "shared store\n\nstore.asm · cuckoo.asm\n\n$2^k$ × 64-byte buckets\n"
                "version · 6 tags · 6 ptrs\n\nlock-free readers\n(seqlock per bucket)\n\n"
                "one futex-serialized\nwriter path\n\nimmutable records\nQSBR · Download pins",
                size=5.9)
    return save(fig, "fig1_architecture")


def fig2_loop() -> Path:
    """Figure 2: the measured-design loop as a ring of six steps around the human's role.

    Returns:
        Path of the written PNG.
    """
    fig, ax, ymax = diagram_axes(5.6, 2.95)
    cx, cy, rx, ry = 50.0, ymax / 2 - 0.6, 35.0, 16.0
    steps = [
        ("1  Measure", "perf · Callgrind · strace"),
        ("2  Hypothesize and size", "Amdahl ceiling first"),
        ("3  Build", "rivals behind one interface"),
        ("4  Verify", "unit · oracle · h2spec · fuzz"),
        ("5  Decide", "paired A/B · exact sign test"),
        ("6  Record", "ADR · commit · cookbook"),
    ]
    angles = [150, 90, 30, -30, -90, -150]  # clockwise from the upper left
    boxes = []
    w, h = 27.0, 9.6
    for (title, sub), deg in zip(steps, angles):
        x = cx + rx * math.cos(math.radians(deg)) - w / 2
        y = cy + ry * math.sin(math.radians(deg)) - h / 2
        box = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=1.4",
                             facecolor=NEUTRAL_FILL, edgecolor=BASELINE, linewidth=0.7, zorder=2)
        ax.add_patch(box)
        ax.text(x + w / 2, y + h * 0.66, title, ha="center", va="center", fontsize=7.0,
                color=INK, fontweight="semibold", zorder=3)
        ax.text(x + w / 2, y + h * 0.3, sub, ha="center", va="center", fontsize=6.0,
                color=INK2, zorder=3)
        boxes.append(box)
    for i, box in enumerate(boxes):
        # Step 6 closes the ring back to step 1.
        ax.add_patch(_ring_arrow(box, boxes[(i + 1) % len(boxes)]))
    ax.text(cx, cy + 2.6, "human: goals · bars · decisions · challenges", ha="center", va="center",
            fontsize=7.0, color=INK, fontweight="semibold")
    ax.text(cx, cy - 2.4, "records carry context across sessions:\nADRs · commit messages · cookbook skill · memory",
            ha="center", va="center", fontsize=6.0, color=INK2, linespacing=1.3)
    return save(fig, "fig2_loop")


def _ring_arrow(box_a: FancyBboxPatch, box_b: FancyBboxPatch) -> FancyArrowPatch:
    """Build the curved arrow from one loop step's box to the next.

    Args:
        box_a: The step the arrow leaves.
        box_b: The step the arrow enters.

    Returns:
        An arrow clipped to both boxes' outlines, bent clockwise around the ring.
    """
    # Aim center to center; patchA/patchB then clip the path at both boxes' outlines.
    start = (box_a.get_x() + box_a.get_width() / 2, box_a.get_y() + box_a.get_height() / 2)
    end = (box_b.get_x() + box_b.get_width() / 2, box_b.get_y() + box_b.get_height() / 2)
    return FancyArrowPatch(start, end, arrowstyle="-|>", mutation_scale=8, color=SERIES[0],
                           linewidth=1.0, zorder=1, connectionstyle="arc3,rad=-0.22",
                           patchA=box_a, patchB=box_b, shrinkA=2, shrinkB=2)


#: Every design decision with a recorded paired measurement (variant / baseline
#: throughput). ``lo``/``hi`` are the min-max of per-block ratios where the source
#: reports them (``None`` = point estimate only). ``state``: "adopted" (all paired
#: blocks agreed), "weak" (adopted, effect not significant), "rejected" (rejected or not
#: yet adopted). Sources in the trailing comments.
DECISIONS = [
    ("Native HTTP/2 engine vs nghttp2, 4 cores", 2.07, None, None, "adopted"),  # ADR 0007
    ("Native HTTP/2 engine vs nghttp2, 1 core", 2.03, None, None, "adopted"),   # ADR 0007
    ("256 KiB reads: 64 KiB uploads", 1.884, 1.867, 1.905, "adopted"),            # ab-readchunk.out
    ("Batched GETs + two-stage prefetch, 1 core", 1.349, 1.07, 1.81, "adopted"),  # ADR 0005
    ("Non-temporal record stores: 1 MiB uploads", 1.313, 1.303, 1.333, "adopted"),  # ab-nt.out
    ("Batched GETs + two-stage prefetch, 4 cores", 1.282, 1.07, 1.59, "adopted"),  # ADR 0005
    ("Two-phase event loop, 8 conns per client thread", 1.257, 1.217, 1.338, "adopted"),  # ADR 0009
    ("Folding CRC-32C: 1 MiB downloads", 1.253, 1.191, 1.337, "adopted"),          # ab-crc.out
    ("Two-phase event loop, 4 conns per client thread", 1.118, 1.031, 1.156, "adopted"),  # ADR 0009
    ("Per-worker pool allocator, 4 cores", 1.10, 0.61, 1.54, "weak"),            # ADR 0005
    ("SSE2 tag scan (store level, 4 Mi keys)", 1 / 0.954, None, None, "weak"),    # ADR 0006
    ("Third record-line prefetch", 1.001, 0.992, 1.010, "weak"),                  # ADR 0005
    ("Huge-page records via allocator tunables (lead)", 1.059, 1.038, 1.080, "rejected"),  # ac100eb
    ("Stream-lookup cache, 4 cores", 0.983, 0.36, 1.15, "rejected"),              # ADR 0005
    ("Bigger benchmark client, 24 vs 16 vCPUs", 0.877, 8.19 / 9.89, 8.70 / 9.67, "rejected"),  # client-ceiling
    ("Private L1/L2 record caches (store level, 1 Mi keys)", 1 / 1.261, None, None, "rejected"),  # ADR 0006
]


def fig3_decisions() -> Path:
    """Figure 3: forest plot of every measured design decision (log ratio axis).

    Returns:
        Path of the written PNG.
    """
    groups = [("Adopted", [d for d in DECISIONS if d[4] != "rejected"]),
              ("Rejected or not yet adopted", [d for d in DECISIONS if d[4] == "rejected"])]
    # Assign rows top-down with a header row per group.
    rows: list[tuple[float, tuple | str]] = []
    y = 0.0
    for title, items in groups:
        rows.append((y, title))
        y -= 1.0
        for item in items:
            rows.append((y, item))
            y -= 1.0
        y -= 0.4
    fig, ax = plt.subplots(figsize=(6.6, 4.2))
    fig.subplots_adjust(left=0.43, right=0.93, top=0.925, bottom=0.11)
    ax.set_xscale("log")
    ax.set_xlim(0.33, 2.7)
    ax.set_ylim(y + 0.6, 0.8)
    ax.axvline(1.0, color=BASELINE, linewidth=0.8, zorder=1)
    hairline_grid(ax, "x")
    ax.spines["left"].set_visible(False)
    ax.yaxis.set_major_locator(NullLocator())
    ticks = [0.5, 0.75, 1.0, 1.5, 2.0, 2.5]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}×"))
    ax.set_xlabel("variant ÷ baseline throughput (log scale; > 1 is faster)")
    for ypos, item in rows:
        if isinstance(item, str):
            ax.text(-0.02, ypos, item, transform=ax.get_yaxis_transform(), ha="right",
                    va="center", fontsize=7.3, color=INK, fontweight="semibold")
            continue
        label, ratio, lo, hi, state = item
        color = MUTED if state == "rejected" else SERIES[0]
        if lo is not None:
            ax.plot([lo, hi], [ypos, ypos], color=color, linewidth=1.3, solid_capstyle="round",
                    zorder=2)
        face = SURFACE if state == "weak" else color
        ax.plot([ratio], [ypos], marker="o", markersize=5.2, markerfacecolor=face,
                markeredgecolor=color if state == "weak" else SURFACE,
                markeredgewidth=1.2 if state == "weak" else 1.0, zorder=3, linestyle="none")
        ax.text(-0.02, ypos, label, transform=ax.get_yaxis_transform(), ha="right", va="center",
                fontsize=6.9, color=INK2)
        # The value sits past the whisker (or the marker) by a fixed offset in points,
        # so it never touches a ring on a log axis where data offsets vary in size.
        right = hi if hi is not None else ratio
        ax.annotate(f"{ratio:.2f}×", (right, ypos), xytext=(6, 0), textcoords="offset points",
                    ha="left", va="center", fontsize=6.6, color=INK2,
                    bbox=dict(facecolor=SURFACE, edgecolor="none", pad=0.4))
    # Legend built from proxy markers so all three states are explained.
    handles = [
        plt.Line2D([], [], marker="o", linestyle="none", markersize=5.2, markerfacecolor=SERIES[0],
                   markeredgecolor=SURFACE, label="adopted, every paired block agreed"),
        plt.Line2D([], [], marker="o", linestyle="none", markersize=5.2, markerfacecolor=SURFACE,
                   markeredgecolor=SERIES[0], markeredgewidth=1.2, label="adopted, not significant"),
        plt.Line2D([], [], marker="o", linestyle="none", markersize=5.2, markerfacecolor=MUTED,
                   markeredgecolor=SURFACE, label="rejected or not yet adopted"),
    ]
    # Above the plot, in one row: below it would collide with the bottom rows' labels.
    ax.legend(handles=handles, loc="lower left", bbox_to_anchor=(-0.02, 1.0), ncol=3,
              fontsize=6.5, handletextpad=0.3, columnspacing=1.4, labelcolor=INK2)
    return save(fig, "fig3_decisions")


# Published medians (benchmarks/results/2026-09-27-standard/README.md and REPORT.md).
S1_GETS_M = [8.84, 2.88, 3.20, 2.40, 3.86, 3.01]           # 4 cores, 10 blocks
S1_RATIO = [None, 3.07, 2.76, 3.68, 2.29, 2.94]             # KvNode ÷ system
N1_GETS_M = [1.76, 1.40, 1.42]                              # 1 core: KvNode, Redis, Valkey
N1_RATIO = [None, 1.26, 1.24]
N1_CPU_NS = [576, 722, 710]
E5_NOOP_NS = [472, 479, 525, 541, 774, 1837]                # 1 core, all six systems
E7_IN_FLIGHT = [1, 16, 64, 256, 1024]
E7_GETS = {                                                 # 4 cores, ops/s
    "KvNode": [35e3, 314e3, 714e3, 2.94e6, 9.11e6],
    "Redis": [19e3, 175e3, 297e3, 1.05e6, 2.91e6],
    "Valkey": [35e3, 275e3, 426e3, 1.56e6, 3.42e6],
    "Dragonfly": [21e3, 235e3, 423e3, 1.07e6, 2.50e6],
    "Garnet": [35e3, 250e3, 432e3, 1.50e6, 4.09e6],
    "Memcached": [37e3, 347e3, 519e3, 1.50e6, 3.15e6],
}


def _emphasis_bars(ax, names: list[str], values: list[float], labels: list[str],
                   ymax: float, yticks: list[float], fmt) -> None:
    """Draw an emphasis bar chart: KvNode in the accent hue, every rival in gray.

    Args:
        ax: Target axes (its layout must already be final; see :func:`rounded_bar`).
        names: Category names, KvNode first.
        values: Bar heights.
        labels: Text placed above each bar (may contain a second line).
        ymax: Upper y limit.
        yticks: Major tick positions.
        fmt: Callable formatting a tick value.
    """
    ax.set_xlim(-0.6, len(names) - 0.4)
    ax.set_ylim(0, ymax)
    ax.yaxis.set_major_locator(FixedLocator(yticks))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: fmt(v)))
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels(names)
    hairline_grid(ax, "y")
    ax.spines["left"].set_visible(False)
    for i, (name, value, label) in enumerate(zip(names, values, labels)):
        color = SERIES[0] if name == "KvNode" else MUTED
        rounded_bar(ax, 0, i - 0.29, 0.58, value, color)
        ax.text(i, value + ymax * 0.02, label, ha="center", va="bottom", fontsize=6.6,
                color=INK if name == "KvNode" else INK2,
                fontweight="semibold" if name == "KvNode" else "normal", linespacing=1.15)


def fig4_throughput() -> Path:
    """Figure 4: headline throughput and per-request CPU, KvNode emphasized.

    Returns:
        Path of the written PNG.
    """
    fig = plt.figure(figsize=(6.6, 4.7))
    top = fig.add_gridspec(1, 2, left=0.07, right=0.99, top=0.93, bottom=0.57, wspace=0.18,
                           width_ratios=[1.75, 1])
    bottom = fig.add_gridspec(1, 2, left=0.07, right=0.99, top=0.42, bottom=0.07, wspace=0.18,
                              width_ratios=[1, 1.75])
    a = fig.add_subplot(top[0, 0])
    b = fig.add_subplot(top[0, 1])
    c = fig.add_subplot(bottom[0, 0])
    d = fig.add_subplot(bottom[0, 1])
    mil = lambda v: "0" if v == 0 else f"{v:g} M"  # noqa: E731  (tick formatter)
    nsf = lambda v: f"{v:,.0f}"  # noqa: E731
    a.set_title("(a) GET/s, 4 cores (S1, 10 blocks)")
    _emphasis_bars(a, SYSTEMS, S1_GETS_M,
                   [f"{v:.2f} M" + (f"\n{r:.2f}×" if r else "") for v, r in zip(S1_GETS_M, S1_RATIO)],
                   10.6, [0, 2, 4, 6, 8, 10], mil)
    b.set_title("(b) GET/s, 1 core (N1, 6 blocks)")
    _emphasis_bars(b, SYSTEMS[:3], N1_GETS_M,
                   [f"{v:.2f} M" + (f"\n{r:.2f}×" if r else "") for v, r in zip(N1_GETS_M, N1_RATIO)],
                   2.15, [0, 0.5, 1.0, 1.5, 2.0], mil)
    c.set_title("(c) Server CPU per GET, 1 core (ns)")
    _emphasis_bars(c, SYSTEMS[:3], N1_CPU_NS, [f"{v:,}" for v in N1_CPU_NS], 880,
                   [0, 200, 400, 600, 800], nsf)
    d.set_title("(d) Server CPU per no-op request, 1 core (ns)")
    _emphasis_bars(d, SYSTEMS, E5_NOOP_NS, [f"{v:,}" for v in E5_NOOP_NS], 2150,
                   [0, 500, 1000, 1500, 2000], nsf)
    return save(fig, "fig4_throughput")


def fig5_concurrency() -> Path:
    """Figure 5: throughput against requests in flight, six systems, log-log axes.

    Returns:
        Path of the written PNG.
    """
    fig, ax = plt.subplots(figsize=(6.6, 3.0))
    fig.subplots_adjust(left=0.09, right=0.86, top=0.95, bottom=0.15)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(0.8, 1400)
    ax.set_ylim(1.4e4, 1.5e7)
    for name in SYSTEMS:
        ys = E7_GETS[name]
        ax.plot(E7_IN_FLIGHT, ys, color=SYSTEM_COLOR[name], linewidth=1.4,
                solid_capstyle="round", solid_joinstyle="round", marker=SYSTEM_MARKER[name],
                markersize=4.6, markerfacecolor=SYSTEM_COLOR[name], markeredgecolor=SURFACE,
                markeredgewidth=0.9, label=name, zorder=3 if name == "KvNode" else 2)
    ax.annotate("KvNode 9.11 M", (1024, 9.11e6), xytext=(7, 0), textcoords="offset points",
                ha="left", va="center", fontsize=6.8, color=INK, fontweight="semibold")
    ax.xaxis.set_major_locator(FixedLocator(E7_IN_FLIGHT))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.yaxis.set_major_locator(FixedLocator([1e4, 1e5, 1e6, 1e7]))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(
        lambda v, _: {1e4: "10 k", 1e5: "100 k", 1e6: "1 M", 1e7: "10 M"}.get(v, "")))
    hairline_grid(ax, "y")
    ax.spines["left"].set_visible(False)
    ax.set_xlabel("requests in flight (connections × depth)")
    ax.set_ylabel("GET/s, 4 cores")
    ax.legend(loc="upper left", ncol=2, handlelength=1.8, columnspacing=1.2, labelcolor=INK2)
    return save(fig, "fig5_concurrency")


def fig6_breakdown() -> Path:
    """Figure 6: per-request server time before/after the native engine, and the
    user-space instruction profile of one GET.

    Returns:
        Path of the written PNG.
    """
    fig = plt.figure(figsize=(6.6, 4.0))
    gs = fig.add_gridspec(2, 1, left=0.25, right=0.97, top=0.93, bottom=0.08, hspace=0.62,
                          height_ratios=[1, 1.9])
    a = fig.add_subplot(gs[0])
    b = fig.add_subplot(gs[1])

    # (a) Stacked time per request (ADR 0007; the "after" split is approximate).
    a.set_title("(a) Server time per request, 1 core (ns)")
    a.set_xlim(0, 1520)
    a.set_ylim(-0.6, 1.6)
    a.xaxis.set_major_locator(FixedLocator([0, 250, 500, 750, 1000, 1250, 1500]))
    a.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:,.0f}"))
    hairline_grid(a, "x")
    a.spines["left"].set_visible(False)
    a.set_yticks([1, 0])
    a.set_yticklabels(["with nghttp2\n(26 Sep)", "native engine\n(final)"])
    gap = 7.0  # surface gap between stacked segments, in ns of axis
    # (label candidates, longest first; the first that fits inside its segment is used)
    segs = {
        1: [(("kernel  485", "485"), 485, MUTED), (("nghttp2  672", "672"), 672, SERIES[1]),
            (("KvNode  222", "222"), 222, SERIES[0]), (("47",), 47, SERIES[2])],
        0: [(("kernel  ≈ 490", "≈ 490"), 490, MUTED),
            (("user space  ≈ 140", "≈ 140"), 140, SERIES[0])],
    }
    renderer = fig.canvas.get_renderer()
    for ypos, parts in segs.items():
        x = 0.0
        for i, (labels, width, color) in enumerate(parts):
            last = i == len(parts) - 1
            seg = width - (0 if last else gap)
            rounded_bar(a, x, ypos - 0.3, 0.6, seg, color, horizontal=True, round_end=last)
            # Measure first: a label goes inside only if it fits with padding on both
            # sides; otherwise a shorter candidate is tried, and if none fits the value
            # is left to the legend and the text (never clipped by its own mark).
            seg_px = a.transData.transform((x + seg, 0))[0] - a.transData.transform((x, 0))[0]
            for candidate in labels:
                t = a.text(x + seg / 2, ypos, candidate, ha="center", va="center", fontsize=6.4,
                           color=ink_on(color))
                if t.get_window_extent(renderer=renderer).width + 8 <= seg_px:
                    break
                t.remove()
            x += width
        total = "1,427" if ypos == 1 else "≈ 620–650"
        a.text(x + 18, ypos, total, ha="left", va="center", fontsize=6.6, color=INK,
               fontweight="semibold")
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for c in (MUTED, SERIES[1], SERIES[0], SERIES[2])]
    a.legend(handles, ["kernel", "nghttp2", "KvNode assembly", "libc"], loc="upper right",
             bbox_to_anchor=(1.0, -0.38), ncol=4, fontsize=6.5, handlelength=1.0,
             handleheight=0.8, columnspacing=1.0, labelcolor=INK2)

    # (b) Exact user-space instructions per GET (Callgrind; a0264f7 commit message).
    parts = [("HTTP/2 frame parsing", 280, True), ("SipHash-2-4-128", 254, False),
             ("response framing", 230, True), ("stream lifecycle", 210, True),
             ("store lookup", 160, True), ("gRPC + Protocol Buffers", 150, True),
             ("other (loop, pool, glue)", 141, True)]
    b.set_title("(b) User-space instructions per GET (Callgrind, total 1,425)")
    b.set_xlim(0, 330)
    b.set_ylim(len(parts) - 0.4, -0.6)
    b.xaxis.set_major_locator(FixedLocator([0, 100, 200, 300]))
    hairline_grid(b, "x")
    b.spines["left"].set_visible(False)
    b.set_yticks(range(len(parts)))
    b.set_yticklabels([p[0] for p in parts])
    for i, (name, count, approx) in enumerate(parts):
        rounded_bar(b, 0, i - 0.29, 0.58, count, SERIES[0], horizontal=True)
        b.text(count + 5, i, ("~" if approx else "") + f"{count}", ha="left", va="center",
               fontsize=6.6, color=INK2)
    return save(fig, "fig6_breakdown")


#: Milestones annotated on the timeline: (commit, label, label row 0/1).
MILESTONES = [
    ("d95793d", "v0.1.0", 0),
    ("9885e64", "own HTTP/2 engine;\nnghttp2 removed 1.5 h later", 1),
    ("c3dec96", "two-phase loop", 0),
    ("18adf13", "full suite:\n8.84 M GET/s", 1),
    ("1372f7d", "Upload/Download", 0),
    ("b751a97", "CRC folding,\nNT stores", 1),
]


def fig7_timeline() -> Path:
    """Figure 7: repository growth by artifact over hours since the first prompt,
    with milestones and a rug of the human's prompts.

    Returns:
        Path of the written PNG.
    """
    commits = study_commits()
    xs = [0.0] + [hours_since_start(r["date"]) for r in commits]
    series = [
        ("assembly", [r["asm"] for r in commits]),
        ("server tests", [r["server_tests"] for r in commits]),
        (".NET client", [r["dotnet"] for r in commits]),
        ("benchmark tooling", [r["bench_harness"] for r in commits]),
        ("documentation", [r["docs"] + r["results_docs"] for r in commits]),
    ]
    stats = load_json("session_stats.json")
    prompts = sorted(hours_since_start(t) for row in stats["rows"] for t in row["prompt_times"])
    end_h = xs[-1]

    fig = plt.figure(figsize=(6.6, 3.35))
    gs = fig.add_gridspec(2, 1, left=0.09, right=0.86, top=0.86, bottom=0.12, hspace=0.08,
                          height_ratios=[1, 0.09])
    ax = fig.add_subplot(gs[0])
    rug = fig.add_subplot(gs[1], sharex=ax)
    ax.set_xlim(-2, 118)
    ax.set_ylim(0, 19500)
    for i, (name, ys) in enumerate(series):
        ax.step(xs, [0] + ys, where="post", color=SERIES[i], linewidth=1.4, label=name,
                solid_joinstyle="round", zorder=3 if i == 0 else 2)
        if i:  # the assembly line already ends on its last milestone dot
            ax.plot([end_h], [ys[-1]], marker="o", markersize=4.4, color=SERIES[i],
                    markeredgecolor=SURFACE, markeredgewidth=0.9, zorder=4)
    ax.annotate(f"assembly {series[0][1][-1]:,}", (end_h, series[0][1][-1]), xytext=(6, 0),
                textcoords="offset points", ha="left", va="center", fontsize=6.8, color=INK,
                fontweight="semibold")
    ax.yaxis.set_major_locator(FixedLocator([0, 5000, 10000, 15000]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.set_ylabel("lines")
    hairline_grid(ax, "y")
    ax.spines["left"].set_visible(False)
    ax.tick_params(labelbottom=False)
    # The band between the full suite (52 h) and Upload/Download (88 h), above the
    # tooling lines and below the assembly line, holds no data: the legend goes there.
    ax.legend(loc="center", bbox_to_anchor=(70.0, 9350.0), bbox_transform=ax.transData,
              fontsize=6.6, labelcolor=INK2, handlelength=1.6, borderaxespad=0)
    # Milestones: a leader from the label down to the assembly line at that commit.
    by_commit = {r["commit"]: r for r in commits}
    for commit, label, row in MILESTONES:
        r = by_commit[commit]
        hx, hy = hours_since_start(r["date"]), r["asm"]
        ty = 21400 if row == 0 else 24600
        ax.annotate(label, (hx, hy), xytext=(hx, ty), textcoords="data", ha="center",
                    va="bottom", fontsize=6.2, color=INK2, annotation_clip=False,
                    arrowprops=dict(arrowstyle="-", color=BASELINE, linewidth=0.6,
                                    shrinkA=1, shrinkB=2))
        ax.plot([hx], [hy], marker="o", markersize=3.6, color=SERIES[0], markeredgecolor=SURFACE,
                markeredgewidth=0.7, zorder=5)
    # Rug of human prompts in its own strip, so it never collides with the series.
    rug.vlines(prompts, 0, 1, color=INK2, linewidth=0.7)
    rug.set_ylim(0, 1)
    rug.set_yticks([0.5])
    rug.set_yticklabels([f"prompts ({len(prompts)})"])
    for side in ("left", "top", "right"):
        rug.spines[side].set_visible(False)
    rug.xaxis.set_major_locator(FixedLocator([0, 24, 48, 72, 96, 120]))
    rug.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:.0f} h"))
    rug.set_xlabel("hours since the first prompt (25 Sep 2026, 17:38 UTC)")
    return save(fig, "fig7_timeline")


def build_figures() -> dict[str, Path]:
    """Render every figure of the paper.

    Returns:
        Mapping of figure file names (as referenced from ``paper.md``) to paths.
    """
    setup_matplotlib()
    paths = [fig1_architecture(), fig2_loop(), fig3_decisions(), fig4_throughput(),
             fig5_concurrency(), fig6_breakdown(), fig7_timeline()]
    return {f"figures/{p.name}": p for p in paths}


# ======================================================================================
# Markdown dialect -> blocks
# ======================================================================================

def parse_markdown(text: str) -> tuple[list[str], list[dict]]:
    """Parse ``paper.md`` into front matter and a list of typed blocks.

    The dialect: ``% `` lines (title, subtitle, author, meta), ``::: abstract`` …
    ``:::``, ``#``-headings, paragraphs, ``- `` bullets, ``1. `` numbered items,
    ``Table: caption`` + pipe table, ``Listing: caption`` + fenced code, bare fenced
    code, ``![caption](path){width=in}``, ``\\newpage``, ``{{placeholder}}`` and, after
    a ``# References`` heading, ``[n] text`` reference entries.

    Args:
        text: The whole Markdown source.

    Returns:
        ``(front, blocks)``: the front-matter lines and the blocks in document order;
        each block is a dict with a ``kind`` key and kind-specific fields.

    Raises:
        ValueError: On a malformed construct (a ``Listing:`` without a code fence,
            an unterminated fence or abstract), naming the line, so a typo cannot
            silently swallow the rest of the paper.
    """
    lines = text.splitlines()
    front: list[str] = []
    i = 0
    while i < len(lines) and lines[i].startswith("% "):
        front.append(lines[i][2:].strip())
        i += 1
    blocks: list[dict] = []
    in_refs = False
    starts_block = re.compile(r"^(#|Table: |Listing: |```|!\[|\\newpage|\{\{|::: |\s*[-*] |\d+\. )")

    def fence(start: int) -> tuple[list[str], int, str]:
        """Collect a fenced code block beginning at ``start``.

        Args:
            start: Index of the opening ```` ``` ```` line.

        Returns:
            ``(code_lines, index_after_closing_fence, language)``.

        Raises:
            ValueError: If the fence never closes.
        """
        lang = lines[start].strip()[3:].strip()
        body: list[str] = []
        j = start + 1
        while j < len(lines) and not lines[j].strip().startswith("```"):
            body.append(lines[j])
            j += 1
        if j >= len(lines):
            raise ValueError(f"unterminated code fence starting at line {start + 1}")
        return body, j + 1, lang

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith("::: abstract"):
            body: list[str] = []
            i += 1
            while i < len(lines) and lines[i].strip() != ":::":
                body.append(lines[i].strip())
                i += 1
            if i >= len(lines):
                raise ValueError("unterminated ::: abstract block")
            blocks.append({"kind": "abstract", "text": " ".join(b for b in body if b)})
            i += 1
            continue
        m = re.match(r"^(#{1,3})\s+(.*)$", line)
        if m:
            level, title = len(m.group(1)), m.group(2).strip()
            in_refs = title.lower() == "references"
            blocks.append({"kind": "heading", "level": level, "text": title})
            i += 1
            continue
        if line.startswith("Table: "):
            caption = line[len("Table: "):].strip()
            i += 1
            rows: list[list[str]] = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                rows.append(cells)
                i += 1
            if len(rows) < 3:
                raise ValueError(f"table '{caption[:40]}' needs a header, a rule and rows")
            align = ["right" if c.endswith(":") and not c.startswith(":") else
                     "center" if c.startswith(":") and c.endswith(":") else "left"
                     for c in rows[1]]
            blocks.append({"kind": "table", "caption": caption, "header": rows[0],
                           "align": align, "rows": rows[2:]})
            continue
        if line.startswith("Listing: "):
            caption = line[len("Listing: "):].strip()
            if i + 1 >= len(lines) or not lines[i + 1].strip().startswith("```"):
                raise ValueError(f"Listing at line {i + 1} is not followed by a code fence")
            body, i, lang = fence(i + 1)
            blocks.append({"kind": "code", "caption": caption, "lines": body, "lang": lang})
            continue
        if stripped.startswith("```"):
            body, i, lang = fence(i)
            blocks.append({"kind": "code", "caption": None, "lines": body, "lang": lang})
            continue
        m = re.match(r"^!\[(.+)\]\(([^)]+)\)(\{width=([\d.]+)\})?\s*$", stripped)
        if m:
            blocks.append({"kind": "figure", "caption": m.group(1), "path": m.group(2),
                           "width": float(m.group(4)) if m.group(4) else 6.5})
            i += 1
            continue
        if stripped == "\\newpage":
            blocks.append({"kind": "pagebreak"})
            i += 1
            continue
        m = re.match(r"^\{\{(\w+)\}\}$", stripped)
        if m:
            blocks.append({"kind": "placeholder", "name": m.group(1)})
            i += 1
            continue
        if in_refs and re.match(r"^\[\d+\] ", stripped):
            blocks.append({"kind": "reference", "text": stripped})
            i += 1
            continue
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if m:
            ordered = m.group(2)[0].isdigit()
            items: list[str] = []
            while i < len(lines):
                mm = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
                if mm and mm.group(2)[0].isdigit() == ordered:
                    items.append(mm.group(3).strip())
                elif lines[i].strip() and lines[i].startswith((" ", "\t")) and items:
                    items[-1] += " " + lines[i].strip()  # wrapped continuation line
                else:
                    break
                i += 1
            blocks.append({"kind": "list", "ordered": ordered, "items": items})
            continue
        # Plain paragraph: consecutive lines until a blank line or another construct.
        para = [stripped]
        i += 1
        while i < len(lines) and lines[i].strip() and not starts_block.match(lines[i]):
            para.append(lines[i].strip())
            i += 1
        blocks.append({"kind": "paragraph", "text": " ".join(para)})
    return front, blocks


# ======================================================================================
# Consistency checks
# ======================================================================================

def body_text(blocks: list[dict]) -> str:
    """Concatenate all prose a reader sees (headings, paragraphs, lists, captions, cells).

    Args:
        blocks: Parsed blocks.

    Returns:
        One string, used for citation and fact checks. Code listings and reference
        entries are excluded (a reference cites nothing).
    """
    out: list[str] = []
    for b in blocks:
        kind = b["kind"]
        if kind in ("paragraph", "abstract", "heading"):
            out.append(b["text"])
        elif kind == "list":
            out.extend(b["items"])
        elif kind == "table":
            out.append(b["caption"])
            out.extend(" | ".join(r) for r in [b["header"]] + b["rows"])
        elif kind in ("figure", "code") and b.get("caption"):
            out.append(b["caption"])
    return "\n".join(out)


def check_paper(blocks: list[dict]) -> None:
    """Abort the build if numbering, citations or key numbers are inconsistent.

    Args:
        blocks: Parsed blocks of ``paper.md``.

    Raises:
        SystemExit: With a list of every problem found, so all can be fixed at once.
    """
    problems: list[str] = []
    # 1. Figure/table/listing numbers must appear in order 1..n.
    for kind, word in (("figure", "Figure"), ("table", "Table"), ("code", "Listing")):
        numbers = []
        for b in blocks:
            if b["kind"] == kind and b.get("caption"):
                m = re.match(rf"^{word} (\d+)\.", b["caption"])
                if not m:
                    problems.append(f"{kind} caption lacks '{word} n.': {b['caption'][:50]}")
                    continue
                numbers.append(int(m.group(1)))
        if numbers != list(range(1, len(numbers) + 1)):
            problems.append(f"{word} numbers out of order: {numbers}")
    # 2. Every citation resolves; every reference is cited.
    refs = {int(re.match(r"^\[(\d+)\]", b["text"]).group(1)) for b in blocks if b["kind"] == "reference"}
    text = body_text(blocks)
    cited: set[int] = set()
    for group in re.findall(r"\[(\d+(?:,\s*\d+)*)\]", text):
        cited.update(int(n) for n in re.split(r",\s*", group))
    if refs != set(range(1, len(refs) + 1)):
        problems.append(f"references are not numbered 1..{len(refs)}")
    if cited - refs:
        problems.append(f"citations without a reference: {sorted(cited - refs)}")
    if refs - cited:
        problems.append(f"references never cited: {sorted(refs - cited)}")
    # 3. Key numbers quoted in the prose equal the values derived from data.
    stats = load_json("session_stats.json")["total"]
    commits = study_commits()
    expected = {
        "tool calls": f"{stats['tool_calls']:,}",
        "distinct prompts": f"{stats['distinct_human_prompts']} distinct",
        "prompt words": f"{stats['distinct_prompt_words']:,}",
        "assembly edits": f"{stats['asm_edits']}",
        "assembly lines": f"{commits[-1]['asm']:,}",
        "machine code bytes": f"{object_code_bytes():,}",
        "benchmark runs": f"{benchmark_run_records():,}",
        "commits": f"{int(git('rev-list', '--count', STUDY_REVISION).strip())} commits",
        "active hours": f"{stats['active_hours_union']} hours",
    }
    for what, needle in expected.items():
        if needle not in text:
            problems.append(f"{what}: the text never states '{needle}' (derived from data)")
    if problems:
        raise SystemExit("paper.md failed its consistency checks:\n  - " + "\n  - ".join(problems))


# ======================================================================================
# DOCX writer
# ======================================================================================

def set_run_font(run, name: str, size: float | None = None, color: str | None = None,
                 bold: bool | None = None, italic: bool | None = None) -> None:
    """Set a run's font on every script slot so Word never substitutes a theme font.

    Args:
        run: A python-docx run.
        name: Font family.
        size: Size in points, or None to inherit.
        color: ``#rrggbb`` or None to inherit.
        bold: True/False, or None to inherit.
        italic: True/False, or None to inherit.
    """
    run.font.name = name
    r_fonts = run._element.get_or_add_rPr().get_or_add_rFonts()
    for slot in ("w:ascii", "w:hAnsi", "w:cs", "w:eastAsia"):
        r_fonts.set(qn(slot), name)
    if size is not None:
        run.font.size = Pt(size)
    if color is not None:
        run.font.color.rgb = RGBColor.from_string(color.lstrip("#").upper())
    if bold is not None:
        run.font.bold = bold
    if italic is not None:
        run.font.italic = italic


def style_font(style, name: str, size: float, color: str = INK, bold: bool = False,
               italic: bool = False) -> None:
    """Pin a style's font, removing theme-font attributes that would override it.

    Built-in heading styles reference theme fonts (``w:asciiTheme``), which win over an
    explicit ``w:ascii``; leaving them in place makes headings render in Calibri Light
    whatever the style says. This strips them.

    Args:
        style: A python-docx style.
        name: Font family.
        size: Size in points.
        color: ``#rrggbb``.
        bold: Bold flag.
        italic: Italic flag.
    """
    style.font.name = name
    style.font.size = Pt(size)
    style.font.bold = bold
    style.font.italic = italic
    style.font.color.rgb = RGBColor.from_string(color.lstrip("#").upper())
    r_pr = style.element.get_or_add_rPr()
    r_fonts = r_pr.find(qn("w:rFonts"))
    if r_fonts is None:
        r_fonts = OxmlElement("w:rFonts")
        r_pr.append(r_fonts)
    for attr in ("w:asciiTheme", "w:hAnsiTheme", "w:eastAsiaTheme", "w:cstheme"):
        r_fonts.attrib.pop(qn(attr), None)
    for slot in ("w:ascii", "w:hAnsi", "w:cs", "w:eastAsia"):
        r_fonts.set(qn(slot), name)


def paragraph_style(doc, name: str, font: str, size: float, *, color: str = INK,
                    bold: bool = False, italic: bool = False, align=None,
                    before: float = 0, after: float = 4, line: float = 1.1,
                    keep_next: bool = False, base: str = "Normal"):
    """Create (or fetch) a custom paragraph style with the given typography.

    Args:
        doc: The document.
        name: Style name.
        font: Font family.
        size: Size in points.
        color: Text color.
        bold: Bold flag.
        italic: Italic flag.
        align: A ``WD_ALIGN_PARAGRAPH`` value or None.
        before: Space before, points.
        after: Space after, points.
        line: Line spacing multiple.
        keep_next: Keep with the next paragraph (captions above tables, headings).
        base: Name of the style it inherits from.

    Returns:
        The style object.
    """
    styles = doc.styles
    style = styles[name] if name in [s.name for s in styles] else styles.add_style(name, WD_STYLE_TYPE.PARAGRAPH)
    style.base_style = styles[base]
    style_font(style, font, size, color, bold, italic)
    pf = style.paragraph_format
    pf.space_before = Pt(before)
    pf.space_after = Pt(after)
    pf.line_spacing = line
    pf.keep_with_next = keep_next
    if align is not None:
        pf.alignment = align
    return style


def setup_document() -> Document:
    """Create the document with A4 geometry, styles, running head and page numbers.

    Returns:
        A python-docx Document ready for content.
    """
    doc = Document()
    section = doc.sections[0]
    section.page_width, section.page_height = Cm(21.0), Cm(29.7)
    for side in ("left_margin", "right_margin"):
        setattr(section, side, Cm(2.2))
    section.top_margin, section.bottom_margin = Cm(2.3), Cm(2.2)
    section.header_distance, section.footer_distance = Cm(1.1), Cm(1.0)

    normal = doc.styles["Normal"]
    style_font(normal, BODY_FONT, 10.5)
    normal.paragraph_format.space_after = Pt(5)
    normal.paragraph_format.line_spacing = 1.13
    normal.paragraph_format.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY

    for level, size, before, after in ((1, 13.5, 16, 6), (2, 11.5, 11, 4), (3, 10.5, 9, 3)):
        st = doc.styles[f"Heading {level}"]
        style_font(st, BODY_FONT, size, INK, bold=True, italic=(level == 3))
        pf = st.paragraph_format
        pf.space_before, pf.space_after = Pt(before), Pt(after)
        pf.keep_with_next = True
        pf.alignment = WD_ALIGN_PARAGRAPH.LEFT
        pf.line_spacing = 1.0

    paragraph_style(doc, "KV Title", BODY_FONT, 19, bold=True, align=WD_ALIGN_PARAGRAPH.CENTER,
                    after=6, line=1.05)
    paragraph_style(doc, "KV Subtitle", BODY_FONT, 11.5, color=INK2, italic=True,
                    align=WD_ALIGN_PARAGRAPH.CENTER, after=12, line=1.1)
    paragraph_style(doc, "KV Author", BODY_FONT, 11.5, align=WD_ALIGN_PARAGRAPH.CENTER, after=2)
    paragraph_style(doc, "KV Meta", BODY_FONT, 9, color=MUTED, align=WD_ALIGN_PARAGRAPH.CENTER,
                    after=14)
    paragraph_style(doc, "KV Abstract", BODY_FONT, 9.5, align=WD_ALIGN_PARAGRAPH.JUSTIFY,
                    after=0, line=1.12)
    paragraph_style(doc, "KV Keywords", BODY_FONT, 9, color=INK2, align=WD_ALIGN_PARAGRAPH.LEFT,
                    before=8, after=8)
    paragraph_style(doc, "KV Caption", BODY_FONT, 8.8, color=INK2, align=WD_ALIGN_PARAGRAPH.JUSTIFY,
                    before=3, after=10, line=1.08)
    paragraph_style(doc, "KV Caption Above", BODY_FONT, 8.8, color=INK2,
                    align=WD_ALIGN_PARAGRAPH.JUSTIFY, before=8, after=4, line=1.08, keep_next=True)
    paragraph_style(doc, "KV Table", TABLE_FONT, 8.3, align=WD_ALIGN_PARAGRAPH.LEFT, after=0,
                    line=1.0)
    paragraph_style(doc, "KV Code", CODE_FONT, 7.6, align=WD_ALIGN_PARAGRAPH.LEFT, after=0,
                    line=1.0)
    paragraph_style(doc, "KV Reference", BODY_FONT, 8.6, align=WD_ALIGN_PARAGRAPH.LEFT, after=2.5,
                    line=1.05)
    paragraph_style(doc, "KV Figure", BODY_FONT, 10, align=WD_ALIGN_PARAGRAPH.CENTER, before=8,
                    after=0, keep_next=True)
    bullet = doc.styles["List Bullet"]
    style_font(bullet, BODY_FONT, 10.5)
    bullet.paragraph_format.space_after = Pt(2.5)
    bullet.paragraph_format.line_spacing = 1.13
    bullet.paragraph_format.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY

    # Running head on every page but the first; page numbers on all pages.
    section.different_first_page_header_footer = True
    head = section.header.paragraphs[0]
    head.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    set_run_font(head.add_run(RUNNING_HEAD), BODY_FONT, 8, MUTED, italic=True)
    for footer in (section.footer, section.first_page_footer):
        p = footer.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        add_field(p.add_run(), "PAGE", BODY_FONT, 9, MUTED)
    return doc


def add_field(run, instruction: str, font: str, size: float, color: str) -> None:
    """Insert a Word field (e.g. ``PAGE``) into a run; Word computes it when rendering.

    Args:
        run: The run that will hold the field.
        instruction: Field code, such as ``"PAGE"``.
        font: Font family of the displayed result.
        size: Font size in points.
        color: Text color.
    """
    set_run_font(run, font, size, color)
    begin = OxmlElement("w:fldChar")
    begin.set(qn("w:fldCharType"), "begin")
    instr = OxmlElement("w:instrText")
    instr.set(qn("xml:space"), "preserve")
    instr.text = instruction
    sep = OxmlElement("w:fldChar")
    sep.set(qn("w:fldCharType"), "separate")
    placeholder = OxmlElement("w:t")
    placeholder.text = "1"  # shown until Word updates the field
    end = OxmlElement("w:fldChar")
    end.set(qn("w:fldCharType"), "end")
    for el in (begin, instr, sep, placeholder, end):
        run._r.append(el)


def add_hyperlink(paragraph, url: str, text: str) -> None:
    """Append a clickable external hyperlink run to a paragraph.

    python-docx has no hyperlink API; this writes the ``w:hyperlink`` element and its
    relationship by hand. The run inherits the paragraph's style and is colored with the
    sequential blue's darker step so it reads as a link in print and on screen.

    Args:
        paragraph: Target paragraph.
        url: Absolute URL.
        text: Visible text.
    """
    r_id = paragraph.part.relate_to(url, RELATIONSHIP_TYPE.HYPERLINK, is_external=True)
    link = OxmlElement("w:hyperlink")
    link.set(qn("r:id"), r_id)
    run = OxmlElement("w:r")
    r_pr = OxmlElement("w:rPr")
    color = OxmlElement("w:color")
    color.set(qn("w:val"), "1C5CAB")
    r_pr.append(color)
    run.append(r_pr)
    t = OxmlElement("w:t")
    t.set(qn("xml:space"), "preserve")
    t.text = text
    run.append(t)
    link.append(run)
    paragraph._p.append(link)


#: Inline markup: **bold**, *italic*, `code`, [text](url), ^superscript^.
INLINE = re.compile(
    r"\*\*(?P<bold>.+?)\*\*"
    r"|(?<![\w*])\*(?P<ital>[^*\s][^*]*?)\*(?![\w*])"
    r"|`(?P<code>[^`]+)`"
    r"|\[(?P<ltext>[^\]]+)\]\((?P<lurl>https?://[^)\s]+)\)"
    r"|\^(?P<sup>[^\^\s][^\^]*?)\^"
)


def add_inline(paragraph, text: str, *, font: str = BODY_FONT, size: float | None = None,
               color: str | None = None, bold: bool = False, italic: bool = False,
               code_size: float | None = None) -> None:
    """Append text with inline Markdown formatting to a paragraph.

    Args:
        paragraph: Target paragraph.
        text: Text that may contain the inline markup of :data:`INLINE`.
        font: Base font family of plain runs.
        size: Base size in points (None inherits the paragraph style).
        color: Base color (None inherits).
        bold: Make every run bold (table headers).
        italic: Make every run italic.
        code_size: Size of inline code runs; defaults to 0.9 × ``size`` (or 9 pt).
    """
    pos = 0
    for m in INLINE.finditer(text):
        if m.start() > pos:
            set_run_font(paragraph.add_run(text[pos:m.start()]), font, size, color, bold, italic)
        if m.group("bold") is not None:
            # Bold may itself contain inline code (e.g. **`h2n.asm`**): recurse once.
            add_inline(paragraph, m.group("bold"), font=font, size=size, color=color, bold=True,
                       italic=italic, code_size=code_size)
        elif m.group("ital") is not None:
            set_run_font(paragraph.add_run(m.group("ital")), font, size, color, bold, True)
        elif m.group("code") is not None:
            cs = code_size if code_size is not None else (size * 0.9 if size else 9)
            set_run_font(paragraph.add_run(m.group("code")), CODE_FONT, cs, color, bold, italic)
        elif m.group("lurl") is not None:
            add_hyperlink(paragraph, m.group("lurl"), m.group("ltext"))
        elif m.group("sup") is not None:
            run = paragraph.add_run(m.group("sup"))
            set_run_font(run, font, size, color, bold, italic)
            run.font.superscript = True
        pos = m.end()
    if pos < len(text):
        set_run_font(paragraph.add_run(text[pos:]), font, size, color, bold, italic)


def caption(doc, text: str, above: bool) -> None:
    """Write a caption whose leading "Figure n." / "Table n." / "Listing n." is bold.

    Args:
        doc: The document.
        text: Caption text from the Markdown source.
        above: True for table and listing captions (kept with what follows).
    """
    p = doc.add_paragraph(style="KV Caption Above" if above else "KV Caption")
    m = re.match(r"^((?:Figure|Table|Listing) [A-Z]?\d+\.)\s*(.*)$", text)
    if m:
        set_run_font(p.add_run(m.group(1) + " "), BODY_FONT, None, INK, bold=True)
        add_inline(p, m.group(2))
    else:
        add_inline(p, text)


def set_cell_border(cell, **edges: dict) -> None:
    """Set individual borders of a table cell.

    Args:
        cell: A python-docx cell.
        **edges: ``top``/``bottom``/``left``/``right`` → attribute dicts such as
            ``{"val": "single", "sz": 8, "color": "0B0B0B"}`` (``sz`` in eighths of a
            point; ``val="nil"`` removes the edge).
    """
    tc_pr = cell._tc.get_or_add_tcPr()
    borders = tc_pr.find(qn("w:tcBorders"))
    if borders is None:
        borders = OxmlElement("w:tcBorders")
        tc_pr.append(borders)
    for edge, attrs in edges.items():
        el = borders.find(qn(f"w:{edge}"))
        if el is None:
            el = OxmlElement(f"w:{edge}")
            borders.append(el)
        for key, value in attrs.items():
            el.set(qn(f"w:{key}"), str(value))


def shade_cell(cell, fill: str) -> None:
    """Fill a table cell with a solid background color.

    Args:
        cell: A python-docx cell.
        fill: Hex color without ``#``.
    """
    tc_pr = cell._tc.get_or_add_tcPr()
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), fill)
    tc_pr.append(shd)


def table_properties(table, *, borders_none: bool = True, cell_margin_cm: float = 0.12) -> None:
    """Remove a table's default grid and set compact cell margins.

    Args:
        table: A python-docx table.
        borders_none: Clear every table-level border (cells then add their own rules).
        cell_margin_cm: Left/right cell padding.
    """
    tbl_pr = table._tbl.tblPr
    if borders_none:
        borders = OxmlElement("w:tblBorders")
        for edge in ("top", "left", "bottom", "right", "insideH", "insideV"):
            el = OxmlElement(f"w:{edge}")
            el.set(qn("w:val"), "nil")
            borders.append(el)
        tbl_pr.append(borders)
    margins = OxmlElement("w:tblCellMar")
    for side, value in (("left", cell_margin_cm), ("right", cell_margin_cm), ("top", 0.03),
                        ("bottom", 0.03)):
        el = OxmlElement(f"w:{side}")
        el.set(qn("w:w"), str(int(value / 2.54 * 1440)))  # cm -> twentieths of a point
        el.set(qn("w:type"), "dxa")
        margins.append(el)
    tbl_pr.append(margins)


def row_flags(row, header: bool = False) -> None:
    """Keep a table row on one page; mark header rows to repeat after page breaks.

    Args:
        row: A python-docx row.
        header: True to repeat this row at the top of each page the table spans.
    """
    tr_pr = row._tr.get_or_add_trPr()
    cant = OxmlElement("w:cantSplit")
    tr_pr.append(cant)
    if header:
        hdr = OxmlElement("w:tblHeader")
        tr_pr.append(hdr)


def plain(text: str) -> str:
    """Strip inline markup, for measuring how much room a cell's text needs.

    Args:
        text: Cell text with Markdown markup.

    Returns:
        The visible characters only.
    """
    return re.sub(r"\*\*|`|\^", "", re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text))


def column_widths(header: list[str], rows: list[list[str]], total_cm: float) -> list[float]:
    """Distribute the text width over columns: a floor per column, the rest by content.

    Every column first gets a floor wide enough for its longest header word (a header
    such as "KvNode" must never break mid-word), then the remaining width is shared in
    proportion to each column's longest cell, softened by a power of 0.75 so one long
    prose column cannot starve the numeric ones.

    Args:
        header: Header cells.
        rows: Body rows.
        total_cm: Width available.

    Returns:
        Column widths in centimeters, summing to ``total_cm``. If the floors alone
        exceed the width, they are scaled down proportionally (words may then wrap).
    """
    n = len(header)
    # ~0.17 cm per bold 8.3 pt Calibri character, plus both cell margins and slack.
    floors = [0.17 * max((len(w) for w in plain(h).split()), default=1) + 0.4 for h in header]
    weights = []
    for c in range(n):
        longest = max(len(plain(r[c])) if c < len(r) else 0 for r in [header] + rows)
        weights.append(max(longest, 5) ** 0.75)
    spare = total_cm - sum(floors)
    if spare <= 0:
        scale = total_cm / sum(floors)
        return [f * scale for f in floors]
    return [f + spare * w / sum(weights) for f, w in zip(floors, weights)]


def add_table(doc, block: dict) -> None:
    """Write a booktabs-style table: rules above and below, a thin rule under the header.

    Args:
        doc: The document.
        block: A parsed ``table`` block (caption, header, align, rows).
    """
    caption(doc, block["caption"], above=True)
    header, rows, align = block["header"], block["rows"], block["align"]
    table = doc.add_table(rows=1 + len(rows), cols=len(header))
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    table_properties(table)
    widths = column_widths(header, rows, TEXT_WIDTH_CM)
    rule_heavy = {"val": "single", "sz": 10, "color": "0B0B0B"}
    rule_light = {"val": "single", "sz": 4, "color": "52514E"}
    for r_idx, cells in enumerate([header] + rows):
        row = table.rows[r_idx]
        row_flags(row, header=(r_idx == 0))
        for c_idx in range(len(header)):
            cell = row.cells[c_idx]
            cell.width = Cm(widths[c_idx])
            p = cell.paragraphs[0]
            p.style = doc.styles["KV Table"]
            p.alignment = {"right": WD_ALIGN_PARAGRAPH.RIGHT, "center": WD_ALIGN_PARAGRAPH.CENTER}.get(
                align[c_idx], WD_ALIGN_PARAGRAPH.LEFT)
            text = cells[c_idx] if c_idx < len(cells) else ""
            add_inline(p, text, font=TABLE_FONT, bold=(r_idx == 0), code_size=7.6)
            edges = {}
            if r_idx == 0:
                edges["top"] = rule_heavy
                edges["bottom"] = rule_light
            if r_idx == len(rows):
                edges["bottom"] = rule_heavy
            if edges:
                set_cell_border(cell, **edges)
    # A little air after the table before the next paragraph.
    spacer = doc.add_paragraph()
    spacer.paragraph_format.space_after = Pt(4)
    spacer.paragraph_format.line_spacing = 0.6


def add_code(doc, block: dict) -> None:
    """Write a code listing in a shaded, borderless one-cell table.

    Args:
        doc: The document.
        block: A parsed ``code`` block (optional caption, lines).
    """
    if block.get("caption"):
        caption(doc, block["caption"], above=True)
    table = doc.add_table(rows=1, cols=1)
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    table_properties(table, cell_margin_cm=0.25)
    cell = table.rows[0].cells[0]
    cell.width = Cm(TEXT_WIDTH_CM)
    shade_cell(cell, "F4F3F0")
    set_cell_border(cell, left={"val": "single", "sz": 12, "color": SERIES[0].lstrip("#").upper()})
    row_flags(table.rows[0])
    first = True
    for line in block["lines"]:
        p = cell.paragraphs[0] if first else cell.add_paragraph()
        first = False
        p.style = doc.styles["KV Code"]
        # Leading spaces carry the listing's indentation; python-docx preserves them.
        set_run_font(p.add_run(line if line else " "), CODE_FONT, 7.6, INK)
    spacer = doc.add_paragraph()
    spacer.paragraph_format.space_after = Pt(4)
    spacer.paragraph_format.line_spacing = 0.6


def add_figure(doc, block: dict, figures: dict[str, Path]) -> None:
    """Insert a figure image, give it alt text, and write its caption below.

    Args:
        doc: The document.
        block: A parsed ``figure`` block (caption, path, width in inches).
        figures: Rendered figures by the path written in the Markdown.

    Raises:
        FileNotFoundError: If the Markdown names a figure that was not rendered.
    """
    path = figures.get(block["path"], HERE / block["path"])
    if not Path(path).exists():
        raise FileNotFoundError(f"figure {block['path']} was not rendered")
    p = doc.add_paragraph(style="KV Figure")
    run = p.add_run()
    run.add_picture(str(path), width=Inches(min(block["width"], 6.5)))
    # Alt text for screen readers: the caption without its "Figure n." prefix.
    doc_pr = run._r.find(".//" + qn("wp:docPr"))
    if doc_pr is not None:
        doc_pr.set("descr", plain(re.sub(r"^Figure \d+\.\s*", "", block["caption"])))
    caption(doc, block["caption"], above=False)


def add_reference(doc, text: str) -> None:
    """Write one reference entry with a hanging indent and clickable URLs.

    Args:
        doc: The document.
        text: ``[n] Authors. Year. Title. Venue. URL``.
    """
    p = doc.add_paragraph(style="KV Reference")
    pf = p.paragraph_format
    pf.left_indent = Cm(0.85)
    pf.first_line_indent = Cm(-0.85)
    pos = 0
    for m in re.finditer(r"https?://\S+", text):
        if m.start() > pos:
            add_inline(p, text[pos:m.start()])
        add_hyperlink(p, m.group(0), m.group(0))
        pos = m.end()
    if pos < len(text):
        add_inline(p, text[pos:])


def add_abstract(doc, text: str) -> None:
    """Write the abstract in a lightly shaded panel with an accent rule on its left.

    Args:
        doc: The document.
        text: Abstract prose.
    """
    table = doc.add_table(rows=1, cols=1)
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    table_properties(table, cell_margin_cm=0.35)
    cell = table.rows[0].cells[0]
    cell.width = Cm(TEXT_WIDTH_CM)
    shade_cell(cell, "F6F6F4")
    set_cell_border(cell, left={"val": "single", "sz": 18, "color": SERIES[0].lstrip("#").upper()})
    head = cell.paragraphs[0]
    head.style = doc.styles["KV Abstract"]
    head.paragraph_format.space_before = Pt(4)
    head.paragraph_format.space_after = Pt(3)
    set_run_font(head.add_run("Abstract"), BODY_FONT, 10, INK, bold=True)
    body = cell.add_paragraph(style="KV Abstract")
    body.paragraph_format.space_after = Pt(5)
    add_inline(body, text)


def commit_timeline_table(doc) -> None:
    """Expand ``{{commit_timeline}}``: every studied commit with its hour and code size.

    Args:
        doc: The document.
    """
    rows = []
    for r in study_commits():
        subject = r["subject"].replace("|", "/")
        if len(subject) > 92:
            subject = subject[:89].rstrip() + "…"
        rows.append([f"{hours_since_start(r['date']):.1f}", f"`{r['commit']}`", subject,
                     f"{r['asm']:,}"])
    add_table(doc, {
        "caption": "Table A1. Commits on the first-parent line of the studied revision: hours "
                   "since the first prompt, commit, subject line and lines of assembly afterwards.",
        "header": ["Hours", "Commit", "Subject", "Assembly lines"],
        "align": ["right", "left", "left", "right"],
        "rows": rows,
    })


def write_docx(front: list[str], blocks: list[dict], figures: dict[str, Path]) -> Path:
    """Render the parsed paper into the Word document.

    Args:
        front: Title, subtitle, author and meta lines.
        blocks: Parsed blocks in order.
        figures: Rendered figures keyed by their Markdown path.

    Returns:
        Path of the written ``.docx``.

    Raises:
        ValueError: On an unknown block kind or placeholder (a parser/writer mismatch).
    """
    doc = setup_document()
    title, subtitle, author, meta = (front + ["", "", "", ""])[:4]
    doc.add_paragraph(title, style="KV Title")
    doc.add_paragraph(subtitle, style="KV Subtitle")
    doc.add_paragraph(author, style="KV Author")
    doc.add_paragraph(meta, style="KV Meta")
    for block in blocks:
        kind = block["kind"]
        if kind == "abstract":
            add_abstract(doc, block["text"])
        elif kind == "heading":
            doc.add_heading(block["text"], level=block["level"])
        elif kind == "paragraph":
            text = block["text"]
            style = "KV Keywords" if text.startswith("**Keywords:**") else None
            p = doc.add_paragraph(style=style) if style else doc.add_paragraph()
            add_inline(p, text)
        elif kind == "list":
            for n, item in enumerate(block["items"], start=1):
                if block["ordered"]:
                    p = doc.add_paragraph()
                    pf = p.paragraph_format
                    pf.left_indent, pf.first_line_indent = Cm(0.75), Cm(-0.5)
                    pf.space_after = Pt(3)
                    set_run_font(p.add_run(f"{n}.  "), BODY_FONT, None, INK)
                else:
                    p = doc.add_paragraph(style="List Bullet")
                add_inline(p, item)
            # The last item gets paragraph spacing, so the text after a list is not
            # glued to it (items themselves stay tight).
            p.paragraph_format.space_after = Pt(6)
        elif kind == "table":
            add_table(doc, block)
        elif kind == "code":
            add_code(doc, block)
        elif kind == "figure":
            add_figure(doc, block, figures)
        elif kind == "reference":
            add_reference(doc, block["text"])
        elif kind == "pagebreak":
            doc.add_page_break()
        elif kind == "placeholder":
            if block["name"] != "commit_timeline":
                raise ValueError(f"unknown placeholder {{{{{block['name']}}}}}")
            commit_timeline_table(doc)
        else:
            raise ValueError(f"unknown block kind {kind}")
    props = doc.core_properties
    props.title = title
    props.subject = subtitle
    props.author = author
    props.keywords = "LLM coding agents; assembly; key-value store; HTTP/2; gRPC; benchmarking"
    props.comments = "Generated by docs/paper/build_paper.py from docs/paper/paper.md"
    doc.save(OUTPUT)
    return OUTPUT


def main() -> int:
    """Check the paper, render its figures and write the .docx.

    Returns:
        Process exit code 0. Failures raise (``SystemExit`` with the list of problems
        for consistency failures).
    """
    front, blocks = parse_markdown(SOURCE.read_text(encoding="utf-8"))
    check_paper(blocks)
    figures = build_figures()
    out = write_docx(front, blocks, figures)
    print(f"wrote {out.relative_to(REPO)} and {len(figures)} figures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
