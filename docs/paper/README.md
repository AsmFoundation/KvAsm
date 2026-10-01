# Case-study paper: KvNode, built by an LLM coding agent

**Measured, Not Argued: An LLM Coding Agent Builds an x86-64 Assembly Key-Value Server That Outperforms Redis on Reads.**

The paper is a case study of how Claude Code (Claude Opus 5.5) built KvNode in x86-64 assembly:

- the server's design, including its own HTTP/2, HPACK, gRPC, Protocol Buffers and CRC-32C implementations;
- the agent's measured-design method and its verification stack for assembly;
- the comparison with Redis, Valkey, Dragonfly, Garnet and Memcached;
- the division of labor between the developer and the agent;
- the limits and threats to validity.

It describes the repository at revision `6871e31`.

| File | What it is |
|---|---|
| [`KvNode-agentic-assembly-paper.docx`](KvNode-agentic-assembly-paper.docx) | the paper (24 pages in Word, A4) |
| [`paper.md`](paper.md) | its source text, in the small Markdown dialect described in `build_paper.py` |
| [`build_paper.py`](build_paper.py) | renders the figures and writes the `.docx`; checks numbering, citations and key numbers first |
| [`figures/`](figures) | the seven figures as PNG, regenerated on every build |
| [`data/`](data) | the measured inputs: code size per commit, session statistics, machine-code sizes |
| [`tools/`](tools) | the scripts that produced `data/` |

## Rebuild

```sh
pip install python-docx matplotlib
python docs/paper/build_paper.py
```

The build reads only committed files, and it stops with a list of problems if any check fails:

- figure, table or listing numbers out of order;
- a citation without a reference, or a reference never cited;
- a key number in the text that no longer matches `data/`.

Fonts are Cambria, Calibri and Consolas for the document and Segoe UI for the figures. Other systems fall back to DejaVu Sans in the figures.

## Where the numbers come from

- **Benchmarks.** The results are transcribed from the committed sessions under [`benchmarks/results/`](../../benchmarks/results) and from the ADRs. `build_paper.py` names the source of each value.
- **Code growth** (`data/loc_history.json`) comes from `tools/loc_history.py`, which counts lines per artifact at every first-parent commit.
- **Machine-code sizes** (`data/elf_sizes.json`) come from `tools/elf_sizes.py`.
  - Input: the objects of a clean build of `6871e31` (the upload-download worktree's `server/build/obj`).
  - Output: the ELF section sizes of each object.
- **Process statistics** (`data/session_stats.json`) come from `tools/session_stats.py`.
  - Input: the project's Claude Code transcripts.
  - Output: session counts, prompt counts and times, tool calls, the model id, token totals and active time.
  - Privacy: no prompt or tool text leaves the script. Prompts are de-duplicated by a hash that is never written out.
  - The session that drafted the paper is excluded.

Regenerate the data (only needed after new commits or sessions):

```sh
python docs/paper/tools/loc_history.py > docs/paper/data/loc_history.json
python docs/paper/tools/session_stats.py ~/.claude/projects/K--source-KvNode --exclude <paper-session-id> > docs/paper/data/session_stats.json
(cd <build dir> && python <repo>/docs/paper/tools/elf_sizes.py obj/*.o kvnode) > docs/paper/data/elf_sizes.json
```
