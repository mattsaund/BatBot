```
   /\           /\
  /  \_________/  \      BatBot
 |   ___________   |     a local roundtable of experts
 |  |           |  |
 |  |  o     o  |  |
 |  |    \_/    |  |
 |  |___________|  |
 |_________________|
       |   |
      /|___|\
        | |
```

**A CLI LLM tool that delegates to an entire team of agents.**

`cd` into a project, then type `batbot`. BatBot is the delegator model. It
decides what expert model loads and computes your prompt. Load in specially trained models
in Physics, Mathematics, Programming, etc and BatBot will choose the best model for the prompt.

All local models run internally on the program. You just need to BYO LLM's

> Status: early. The delegation loop, the JIT model host, the router, the
> settings screen, and the TUI all work end to end. Agentic tool use is next.

---

## Why

Running a single local model has to be small enough to fit onto your hardware,
so it isn't the best on its own. Using a `Mixture-of-Agents` approach, You can
get 9 specially trained models on specific subjects, and one delegator model to
choose which one gets the prompt. 

**Example**
If you choose 9, specially trained 30 billion parameter models, Essentially, you can 
have an AI setup that performs pretty much as well as a 270 billion parameter model using
30 billion parameters of space. The only downside to this setup is Just-In-Time loading overhead.

## Nine experts and a fallback

`Mathematics` · `Programming` · `Physics` · `Chemistry` · `Biology` ·
`Engineering` · `Philosophy` · `Sociology` · `Language` · `Fallback`

Each seat is one GGUF file trained (or fine-tuned) on that subject alone.

the `Fallback` model is for the rare situations that the delegator model fails to choose
an expert. Having a `Fallback` model allows for there to never **not** be a response

## Markers

At the table, a seat reads:

| marker | meaning |
|---|---|
| `·` | no model assigned to this subject |
| `✗` | a model is assigned but the file is missing |
| `◇` | assigned, on disk, not loaded |
| `◴` | loading right now (with a percentage) |
| `◆` | resident and answering |

---

## Install

One command. It installs the build toolchain, picks a GPU backend your hardware
can actually use, builds, tests, and puts `batbot` on your PATH:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash
```

Re-running it upgrades in place.

### Uninstalling

```sh
batbot --uninstall
```

If the binary is already gone, the installer can clean up instead:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash -s -- --uninstall
```

### Installer options

Pass options after `--`:

```sh
curl -fsSL .../install.sh | bash -s -- --gpu vulkan --prefix ~/.local
```

| option | |
|---|---|
| `--gpu cuda\|vulkan\|cpu\|auto` | backend (default `auto`) |
| `--prefix DIR` | install location (default `/usr/local`, or `~/.local` without sudo) |
| `--jobs N` | parallel build jobs |
| `--check` | report what would happen, change nothing, never ask for sudo |
| `--no-deps` | do not install system packages |
| `-y`, `--yes` | never prompt |
| `--uninstall` | remove BatBot (leaves your config and models) |

### What `--gpu auto` decides

The installer reads your GPUs' compute capability and picks a backend it can
genuinely build for, rather than the one that sounds best:

- **CUDA**, if an available toolkit is new enough for your *newest* card.
- **Vulkan** otherwise, which works across NVIDIA, AMD and Intel through the
  driver and needs only a small shader compiler.
- **CPU**, if neither can be installed.

That check matters more than it sounds. A Blackwell card (RTX 50-series,
compute capability 12.0) needs **CUDA ≥ 12.8**, but most distributions still
ship CUDA 12.0 — which cannot generate code for it at all. Installing the
distro toolkit would give you a build that silently ignores that GPU. The
installer detects this, tells you, and uses Vulkan instead:

```
 !! CUDA 12.0 is too old for compute capability 12.0 (needs 12.8); using Vulkan
```

To force CUDA anyway, install a current toolkit from
[NVIDIA](https://developer.nvidia.com/cuda-downloads) and re-run with
`--gpu cuda`.

The installer also handles a CMake that is too old (several current LTS
releases ship < 3.24) by fetching an official build into `~/.cache/batbot`.

---

## Build from source

If you would rather do it yourself: a C++20 compiler, CMake ≥ 3.24, and git.
Everything else — llama.cpp, FTXUI, nlohmann/json — is fetched and pinned
automatically.

```sh
git clone https://github.com/mattsaund/batbot.git
cd batbot
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/batbot
```

The result is a single self-contained binary.

```sh
sudo cmake --install build          # or: cp build/bin/batbot ~/.local/bin/
```

### GPU backends

```sh
# NVIDIA -- needs the CUDA toolkit (nvcc)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBATBOT_CUDA=ON

# Anything Vulkan -- needs libvulkan-dev and glslc
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBATBOT_VULKAN=ON
```

With multiple GPUs, `split_mode` and `tensor_split` in the config control how an
expert is spread across them, so an expert can be larger than any single card.

### Build options

| option | default | what it does |
|---|---|---|
| `BATBOT_CUDA` | `OFF` | build llama.cpp's CUDA backend |
| `BATBOT_VULKAN` | `OFF` | build llama.cpp's Vulkan backend |
| `BATBOT_NATIVE` | `ON` | tune for this exact CPU (turn off for portable binaries) |
| `BATBOT_BUILD_TESTS` | `ON` | build the unit tests |
| `BATBOT_BUILD_TOOLS` | `ON` | build `batbot-routebench` |
| `BATBOT_WARNINGS` | `ON` | strict warnings on BatBot's own sources |

> **Note on filesystems:** everything links statically, partly so the binary is
> self-contained and partly because shared-library versioning needs symlinks —
> which exFAT and NTFS volumes do not support.

---

## Setup

On first run BatBot asks whether you trust the current folder (once per folder,
remembered in `~/.config/batbot/trust.json`), then writes a default config.

### The models directory

Every GGUF lives in one folder. The config names which file plays which role,
so moving models around never means editing paths:

```
~/.local/share/batbot/models/           <- your GGUFs, put there by you
    LFM2-1.2B-Q8_0.gguf                 <- the delegator
    math-expert-q4_k_m.gguf             <- Mathematics
    physics-expert-q4_k_m.gguf          <- Physics
```

The folder can live anywhere — an external drive, a NAS mount, a shared
partition. Pick it in the app with **`ctrl-e` → Models directory → Enter**, which
opens a browser showing how many GGUFs sit in each candidate folder:

```
╭ Models directory ──────────────────────────────────────────────╮
│ /mnt/scratch                                                   │
├────────────────────────────────────────────────────────────────┤
│ > [ use this directory ]                         no models here│
├────────────────────────────────────────────────────────────────┤
│   ..                                                           │
│   experts/                                              9 model│
│   archive/                                             2 models│
├────────────────────────────────────────────────────────────────┤
│ ↑↓  enter open  ← up  ~ home  e type  esc    . show 32 hidden  │
╰────────────────────────────────────────────────────────────────╯
```

Long lists scroll to follow the selection, in the folder browser and the model
picker both.

The model counts are the point: you can tell which folder is the one you meant
without opening each in turn.

**Or type the path.** Press `e` on the Models directory row to edit it as text —
the right move for a network mount, or anything easier pasted than navigated to.
The line editor takes `←→` to move, `ctrl-a`/`ctrl-e` for either end, `ctrl-w`
to drop the last path component, and `ctrl-u` to clear. A directory that is not
there is flagged inline rather than failing later.

Or set `models_dir` in the config file directly. An expert can also point
outside the folder with an absolute or `~` path when you do not want to move a
file.

### Configure it in the app

Press **`ctrl-e`** (or type `/config`). Everything in the config file is
editable there: choose the models directory with a browser, pick a model for
each expert seat and for the delegator from whatever is in it, and tune sampling.
**`ctrl-s`** saves and applies it to the running session — no restart, and the
expert reloads on the next prompt.

```
╭ Settings ──────────────────────────────────────────────────────╮
│  ~/.local/share/batbot/models                  3 models found  │
├────────────────────────────────────────────────────────────────┤
│  MODELS                                                        │
│   Models directory    ~/.local/share/batbot/models             │
│                                                                │
│  DELEGATOR                                                     │
│   Router model        LFM2-1.2B-Q8_0.gguf                      │
│                                                                │
│  EXPERTS                                                       │
│ > Mathematics         math-expert-q4_k_m.gguf                  │
│   Programming         (none)                                   │
│   Physics             physics-expert-q4_k_m.gguf               │
├────────────────────────────────────────────────────────────────┤
│ ↑↓ move   enter edit   r rescan   ctrl-s save & apply   esc    │
╰────────────────────────────────────────────────────────────────╯
```

A seat whose file has gone missing says so inline, in red, rather than failing
at load time.

### Or edit the file

`~/.config/batbot/config.json`:

```jsonc
{
  "models_dir": "~/.local/share/batbot/models",
  "router":   { "model": "LFM2-1.2B-Q8_0.gguf" },
  "defaults": { "n_ctx": 8192, "n_gpu_layers": -1, "temperature": 0.7 },
  "experts": {
    "mathematics": { "model": "math-expert-q4_k_m.gguf" },
    "physics":     { "model": "physics-expert-q4_k_m.gguf" },
    "programming": { "model": "/mnt/big/code-expert-q4_k_m.gguf" }
  }
}
```

Anything an expert omits is inherited from `defaults`, so filling a seat is a
one-line edit. Unfilled seats are drawn hollow, and prompts routed to them go to
`Fallback` — or, if that is empty too, to any filled seat, saying so either way.

### When the delegator cannot decide

```jsonc
"routing": {
  "min_confidence": 0.60,       // below this, treat the answer as undecided
  "use_fallback_expert": true   // empty seats send work to Fallback, not elsewhere
}
```

`min_confidence` is a floor, not a target: a delegator that answers below it is
treated as having made no decision, and the prompt goes to `Fallback` instead of
committing to a subject on a coin flip. Set it to `0` to take every answer at
face value.

How much this fires depends entirely on the delegator. LFM2-1.2B reports 0.95 on
most prompts including the ones it gets wrong, so the floor rarely triggers for
it — the reliable path there is the empty-seat fallback. A better-calibrated
router would get more out of it.

### The delegator

The delegator never answers you; it only names a subject. It stays resident for
the whole session, so it wants to be small — around 1B parameters.

One that works well: [LFM2-1.2B](https://huggingface.co/LiquidAI/LFM2-1.2B-GGUF)
at Q8_0 (~1.2 GB). It scores 63% on BatBot's routing benchmark against 42% for a
0.5B model, and routes in about a second on CPU. You fetch it yourself — BatBot
does not download models.

With no delegator configured, BatBot falls back to keyword routing, so the app
still works — and on unambiguous prompts the keyword router is not far behind a
small model.

Routing quality is the thing that decides whether BatBot sends your question to
the right expert, and it does not follow from a model's size or its published
benchmarks. Measure any candidate before trusting it:

```sh
./build/bin/batbot-routebench ~/.local/share/batbot/models/LFM2-1.2B-Q8_0.gguf
```

```
ok    Physics      (want Physics     )  980ms  why is the sky blue?
MISS  Mathematics  (want Sociology   ) 1010ms  what causes inflation in a modern economy
...
12/19 correct (63%), 1020ms per route
```

Nineteen prompts whose subject is not in doubt, sampled greedily so the number
is reproducible. Nine choices means chance is 11% — anything near that is a model
that is not reading the prompt at all, which in practice means a chat template or
prompt problem rather than a weak model.

`Fallback` never appears here: it is not in the grammar, so the delegator cannot
emit it. This measures the only job the delegator has — picking a specialist.

**Routing is deterministic.** The delegator runs at temperature 0, so the same
question always reaches the same expert.

---

## Using it

Type a prompt and BatBot picks the expert. Every reply is labelled with who
answered, how confident the routing was, and what the swap cost:

```
you ▸ why is the sky blue?
⟶ Physics · 0.92 · router model · 1474ms · swap 232ms
The sky is blue because shorter wavelengths scatter more strongly...
63 tok · 69.2 tok/s
```

| command | |
|---|---|
| `/<subject> <prompt>` | skip routing, send straight to one expert |
| `/config` | open the settings screen |
| `/models` | list the .gguf files in the models directory |
| `/experts` | which seats are filled, and with what |
| `/devices` | compute devices llama.cpp found |
| `/release` | unload the resident expert, freeing its memory |
| `/clear` | clear the transcript and the experts' history |
| `/paths` | where the config, models, log and trust files live |
| `/help`, `/quit` | |

| key | |
|---|---|
| `Ctrl-E` | settings: assign models, move the models directory, tune sampling |
| `Ctrl-C` | cancel the current answer; again when idle to quit |
| `Ctrl-T` | show/hide the roundtable |
| `PgUp` / `PgDn` | scroll the transcript |

The roundtable adapts to your terminal: the full ring above ~34 rows, a compact
bat above ~24, and a single status strip below that.

---

## Testing

### Try it for real

BatBot needs at least one GGUF before it can answer, and it will not fetch one
for you. Put a model in your models directory and assign it:

```sh
mkdir -p ~/.local/share/batbot/models
# copy or symlink your .gguf files there, then:
batbot
```

Press **`ctrl-e`**, assign a model to the delegator and to one or two expert
seats, **`ctrl-s`** to save, **`esc`** to go back. Then try, in order:

| type this | what you should see |
|---|---|
| `why is the sky blue?` | routes to **Physics**, that seat lights up, streams an answer |
| `prove the square root of 2 is irrational` | a **different** expert swaps in; the route line shows the cost |
| `/programming what is a segfault?` | pinned — no routing, and no swap if already resident |
| `/release` | the expert unloads and memory drops |
| `Ctrl-C` mid-answer | generation stops, partial text kept, app stays up |

Every reply is labelled with who answered and what it cost:

```
you ▸ why is the sky blue?
⟶ Physics · 0.95 · router model · 1050ms · swap 232ms
```

Any small instruct model works as a stand-in expert while you are trying things
out — one model can fill several seats. Each seat still loads and unloads
independently, so the swap behaviour is real even when the file is the same.

### Unit tests

The parts that need no model on disk — subject parsing, the router grammar,
keyword routing, config inheritance, the trust store, UTF-8 chunking — are
covered by a test suite that runs in well under a second:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Or run the binary directly to see each case:

```sh
./build/bin/batbot_tests
```

```
23 cases, 0 failed, 0 assertions failed
```

### Routing quality

```sh
./build/bin/batbot-routebench <a-model.gguf>
```

Reports how often a candidate delegator picks the right subject, greedily so the
score is reproducible. Exits non-zero below 50%. Run it whenever you change the
delegator, the subject list, or the router prompt.

### Uninstall safety

`batbot --uninstall` is the only destructive thing BatBot does, and the models
it could reach are yours and often large. Those rules are pinned down by running
the real binary against a sandboxed HOME:

```sh
bash tests/test_uninstall.sh
```

```
13 checks, 0 failed
```

It asserts that declining keeps everything, that `-y` removes the binary and
config but **never** the models, and that only an explicit yes to every question
removes the lot.

### Testing the installer

Run the whole install end to end without touching system packages, needing
sudo, or writing outside a scratch directory:

```sh
./install.sh --no-deps --gpu cpu --prefix /tmp/batbot-test -y
```

That exercises everything the real installer does — configure, compile, run the
test suite, install the binary — while `--no-deps` skips package installation,
`--gpu cpu` avoids a multi-gigabyte toolkit download, and `--prefix` keeps the
result out of your system directories. Check it, then clean up:

```sh
/tmp/batbot-test/bin/batbot --version
./install.sh --uninstall --prefix /tmp/batbot-test -y
```

To see what it *would* do — which backend it picks for your GPUs, which
packages it would install, where things would land — without changing anything
and without asking for sudo:

```sh
./install.sh --check
```

```
==> Dry run -- nothing will be changed

    distribution : Linux Mint 22.3
    GPU: NVIDIA GeForce RTX 4070, 8.9
    GPU: NVIDIA GeForce RTX 5060 Ti, 12.0
 !! CUDA 12.0 is too old for compute capability 12.0 (needs 12.8); using Vulkan

would install:
    toolchain : build-essential cmake git pkg-config curl ca-certificates
    backend   : glslc libvulkan-dev

would build and install:
    backend   : vulkan
    binary    : /home/matt/.local/bin/batbot
```

This also works straight from the URL, before you install anything:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash -s -- --check
```

The installer's own logic — version comparison, the CUDA-vs-compute-capability
table, CMake detection — has unit tests too, since a mistake there means a GPU
that silently goes unused rather than an error anyone would notice:

```sh
bash tests/test_install.sh
```

```
23 checks, 0 failed
```

### Checking your setup

```sh
batbot --version
batbot --config          # where the config lives
```

Inside BatBot:

| | |
|---|---|
| `/experts` | which seats are filled, and with what |
| `/devices` | the compute devices llama.cpp found — confirms your GPU backend is live |
| `/config` | config, log, and trust file locations |

If a model will not load, the reason is in the log — llama.cpp's output is
diverted there because anything on stderr would draw over the TUI:

```sh
tail -f ~/.local/share/batbot/batbot.log
```

---

## How it fits together

```
src/
├── main.cpp        parse, trust, hand off -- 40 lines
├── app/            things that happen instead of the TUI
│   ├── cli.cpp         argument parsing, banner, usage
│   ├── trust_gate.cpp  the folder-trust prompt
│   └── uninstall.cpp   batbot --uninstall
├── config/         what lives on disk
│   ├── config.cpp      the Config type's own behaviour
│   ├── config_io.cpp   reading and writing config.json
│   ├── paths.cpp       XDG locations
│   └── trust.cpp       the folder-trust store
├── routing/        deciding who answers
│   ├── subject.cpp     the subject table; grammar and prompt come from it
│   └── router.cpp      KeywordRouter and ModelRouter
├── llm/            everything that touches llama.cpp
│   ├── model_host.cpp    owns the backend; one expert resident at a time
│   ├── loaded_model.cpp  the generation loop
│   ├── sampling.cpp      building a sampler chain
│   └── model_catalog.cpp reading the models directory
├── engine/         the delegation loop            [worker thread]
│   ├── engine.cpp        route -> JIT swap -> generate
│   ├── route_policy.cpp  what to do with the delegator's answer
│   └── state.cpp         the only memory the two threads share
├── ui/                                            [UI thread]
│   ├── app.cpp         shell, key handling, animation clock
│   ├── commands.cpp    slash commands
│   ├── transcript.cpp  drawing the conversation
│   ├── widgets/        bat sprite, roundtable
│   └── settings/       view, directory browser, model picker, line editor
└── util/           text and formatting helpers
```

`include/batbot/` mirrors this exactly. Two threads, one boundary: the engine
never touches FTXUI and the UI never touches llama.cpp, and they meet only at
`AppState`. That is what lets the bat keep animating while a 30B expert loads.

Settings changes cross the same boundary as prompts. Saving from the settings
screen enqueues the new config on the engine's own queue, so it is applied
between requests and can never land in the middle of a generation.

**The subject table is the single source of truth.** The routing grammar, the
delegator's system prompt, its worked examples, and the order seats appear at
the roundtable are all generated from one array in `routing/subject.cpp`, so
they cannot drift apart. The one place that must be kept in step by hand -- the
keyword table -- is checked by a `static_assert`, after a missing entry once
silently zeroed Mathematics' score.

**The delegator cannot hallucinate a subject.** Its output is constrained by a
GBNF grammar generated from that table, so an invalid route is not merely
unlikely, it is unrepresentable:

```
root       ::= subject " " confidence
subject    ::= "MATH" | "PROG" | "PHYS" | "CHEM" | "BIO"
             | "ENG"  | "PHIL" | "SOC"  | "LANG"
confidence ::= "0." [0-9] [0-9] | "1.00"
```

`Fallback` is absent from it by design: the delegator is never offered a way to
decline. `routing/route_policy.cpp` decides when to fall back instead, and being
a pure function it is tested without loading a model.

`llama.cpp`'s own logging is redirected to `~/.local/share/batbot/batbot.log`,
since anything on stderr would draw straight over the TUI.

**The delegator is prompted with worked examples as real conversation turns**,
not as a block of text inside its system message. That distinction is worth more
than it sounds: on LFM2-1.2B, moving the same nine examples out of the system
prompt and into user/assistant turns took routing from 42% to 63%. An earlier
prompt that described each subject in prose and closed by naming a catch-all
scored 16% -- small models latched onto that closing line and answered it for
almost everything.

---

## Roadmap

- [x] JIT model host, router, roundtable TUI, streaming, cancellation
- [x] Models directory, and every setting editable in the app
- [x] Routing benchmark, and a delegator prompt tuned against it
- [x] `Fallback` seat for prompts the delegator cannot place
- [ ] **Agentic tools** — file read/edit, shell, web search, with a permission model
- [ ] Prefix caching so an unchanged conversation is not re-ingested every turn
- [ ] Predictive preloading of the likely next expert
- [ ] Fine-tuned 1B BatBot router to replace the off-the-shelf one
- [ ] Curated subject experts, offered as a download you opt into — never bundled


---

## License

MIT — see [LICENSE](LICENSE). Every dependency BatBot links is MIT too, and
[THIRD_PARTY.md](THIRD_PARTY.md) lists them with their pinned versions. Nothing
is vendored into this repository; the build fetches each at a fixed tag.

Model weights are covered by none of it. BatBot ships no models and downloads
none — whatever GGUFs you put in your models directory carry their own licenses,
which are between you and whoever trained them.

---

## Notes

**Temporary: LFM2-1.2B on the development machine.**
A copy of `LFM2-1.2B-Q8_0.gguf` sits in `~/.local/share/batbot/models` on the
dev box purely so routing changes can be measured against a known model with
`batbot-routebench`. **It is to be deleted.** It is not shipped, not referenced
by any script, and not required by anything in this repository. Nothing here
depends on that file existing; the benchmark takes whatever GGUF you hand it.

**Routing scores in this README were measured with that model**, greedily, on
the 19-prompt benchmark. They describe a stand-in delegator, not a ceiling.
Re-measure with your own before reading anything into them.
