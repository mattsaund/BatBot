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

One command. It installs the build toolchain and whichever GPU SDK your
hardware can actually use, builds BatBot, tests it, and puts it on your PATH:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash
```

It runs in five parts, and shows two live bars: the whole install on top, the
part running now underneath, with what it is doing and how long it has been
doing it.

```
==> [5/5] Building BatBot
    ✓ configured
    compiling with 12 jobs
    install  █████████████░░░░░░░░░░░░░  51%
    [5/5]    █████░░░░░░░░░░░░░░░░░░░░░  21%    llama-model.cpp (37s)
```

The two are weighted differently on purpose. The parts are nothing like equal
in length — checking CMake is milliseconds, building is minutes — so the top
bar weights them by how long they actually take. One that moved a fifth per
part would read 80% with the entire build still ahead of it.

Within a part the same trick is applied again: each phase owns a slice of the
part, so the lower bar moves through the whole 0–100% whether that part is
downloading something with a real percentage or waiting on a package manager
that reports nothing at all.

**BatBot installs with no compute runtime.** Not even the CPU one. The first
thing to do after installing is to open the settings screen and pick one — see
[Runtimes](#runtimes).

Re-running the installer upgrades in place.

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
| `--gpu cuda\|vulkan\|cpu\|auto` | which GPU SDK to install (default `auto`) |
| `--prefix DIR` | install location (default `/usr/local`, or `~/.local` without sudo) |
| `--jobs N` | parallel build jobs |
| `--check` | report what would happen, change nothing, never ask for sudo |
| `--no-deps` | do not install system packages |
| `-y`, `--yes` | never prompt |
| `--uninstall` | remove BatBot (leaves your config and models) |

`--gpu` does not install a runtime — nothing does. It decides which *SDK* goes
in while the installer still has root, so that the runtime you pick later can
be built from the settings screen without needing a password inside a TUI.
Getting it wrong costs nothing: install the SDK yourself at any point and the
runtime appears in settings. See [Runtimes](#runtimes) below.

### What `--gpu auto` decides

The installer reads your GPUs' compute capability and installs the SDK for a
runtime it could genuinely build, rather than the one that sounds best:

- **CUDA**, if an available toolkit is new enough for your *newest* card. It is
  gigabytes, so this one is a question rather than a decision.
- **Vulkan** otherwise, which works across NVIDIA, AMD and Intel through the
  driver and needs only a small shader compiler. A few megabytes, so it goes in
  without asking.
- **Neither**, with `--gpu cpu`. The CPU runtime needs nothing but the compiler
  that is already being installed.

That check matters more than it sounds. A Blackwell card (RTX 50-series,
compute capability 12.0) needs **CUDA ≥ 12.8**, but most distributions still
ship CUDA 12.0 — which cannot generate code for it at all. Installing the
distro toolkit would give you a build that silently ignores that GPU. The
installer detects this, tells you, and uses Vulkan instead:

```
 !! CUDA 12.0 is too old for compute capability 12.0 (needs 12.8); using Vulkan
```

To use CUDA anyway, install a current toolkit from
[NVIDIA](https://developer.nvidia.com/cuda-downloads) — then add the CUDA
runtime from the settings screen, with no reinstall.

The installer also handles a CMake that is too old (several current LTS
releases ship < 3.24) by fetching an official build into `~/.cache/batbot`.

---

## Runtimes

A **runtime** is one compute backend: a shared library that teaches llama.cpp
how to talk to a piece of hardware. BatBot compiles none of them into the
binary. They are files in `~/.local/share/batbot/runtimes`, and the settings
screen adds and removes them.

**A fresh install has none of them.** That is deliberate: the backend is not a
decision you make once, at install time, and live with. Install on a laptop
with no GPU, add CUDA when you get a card, drop it again when a driver update
breaks it — none of that needs a reinstall, and none of it is decided for you
by a script that guessed at your hardware.

It does mean **no model will load until you install one**. BatBot says so
rather than failing obscurely:

```
no runtime installed -- press ctrl-e, open Runtimes, and install one
```

Open the panel with `/runtimes`, or `ctrl-e` → **HARDWARE** → **Runtimes**:

```
 runtimes · ~/.local/share/batbot/runtimes

 No runtime installed, so no model can load.
 Pick one and press enter; it is compiled here, which takes a few minutes.

 ▸ CPU     not installed
   CUDA    not installed · nvcc is not installed
   Vulkan  not installed · glslc is not installed

 devices llama.cpp can see
   no compute devices -- no runtime is loaded

 ↑↓ choose   enter install   d remove   r rescan   esc back
```

`enter` builds the selected runtime from the llama.cpp source the installer
saved, showing progress; it runs on its own thread, so BatBot stays usable and
`esc` leaves it running in the background. `d` twice removes one.

**A runtime is usable the moment it finishes building.** BatBot registers it
with ggml and reloads its models, and the panel says `ready to use`. A restart
is only mentioned when one is genuinely needed — when the module built but
would not load.

**Runtimes outlive the BatBot that built them.** They live in your data
directory, so `batbot --uninstall` keeps them unless you say otherwise, and a
reinstall picks them straight back up with no rebuild. The one case that needs
attention is a reinstall from source built against a *different* llama.cpp:
ggml is not ABI-stable across releases, so such a module would load and then
crash on the first tensor. Each runtime records the tag it was built from, and
the panel checks it:

```
 ▸ CPU     built for llama.cpp b9999 · press enter to rebuild
```

**Every runtime needs the CPU one.** llama.cpp keeps the output layer and
several buffer types on the host whatever GPU is doing the work, and refuses to
load a model without a CPU backend. So installing CUDA on a machine that has no
runtimes builds both, and says so:

```
building the CUDA runtime, and the CPU one with it -- every runtime needs it.
```

**Runtimes need an SDK to build.** `install.sh` installs the Vulkan one
automatically because it is small; CUDA is offered separately because it is
several gigabytes. If one is missing, the panel says which program it wants and
the exact command:

```
 CUDA: nvcc is not installed.  sudo apt install nvidia-cuda-toolkit
```

That check happens before the build starts, not ten minutes into it.

### Multiple GPUs

With more than one card, **HARDWARE → Multi-GPU split** decides how one expert
is divided across them:

| mode | what it does |
|---|---|
| `auto` | let llama.cpp decide (the default) |
| `even` | proportional to each card's memory, so they finish together |
| `priority` | fill the cards in the order you list, spilling into the next |
| `single` | everything on **Main GPU** |

`even` is even in *work*, not in count: a 16 GB card takes twice the layers of
an 8 GB one. Splitting evenly by count would fill the small card first and fail
a model that would otherwise have fitted.

`priority` reads **GPU priority order**, which is a screen of its own rather
than a list of numbers to type. `ctrl-e` → **HARDWARE** → **GPU priority
order** lists the cards in the order they will be filled:

```
 gpu priority order · first card is filled first

 ▸ 1.  NVIDIA GeForce RTX 4070 (12.0 GB)   ·  device 0  ·  CUDA
   2.  NVIDIA GeForce RTX 3060 (12.0 GB)   ·  device 1  ·  CUDA
   3.  NVIDIA GeForce RTX 5060 Ti (16.0 GB)  ·  device 2  ·  CUDA

 ↑↓ choose   enter pick up   esc back
```

`enter` picks a card up, `enter` on another swaps the two. Top is filled first,
bottom last. A card the config names that is no longer in the machine is
dropped; one that has appeared since is added at the bottom.

Only backends that can address several devices are affected — CUDA and Vulkan
can, CPU cannot. The delegator is never split: it is small, and spreading a 1B
model across three cards costs more in transfers than it saves in memory.

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

This builds the binary and no compute backend at all. Every runtime, CPU
included, is added afterwards from the settings screen — see
[Runtimes](#runtimes).

```sh
sudo cmake --install build --component batbot
# or, for a user prefix:
cmake --install build --component batbot --prefix ~/.local
```

The install is `bin/batbot` plus `lib/batbot/` holding llama.cpp's three shared
libraries. The binary's RPATH is relative, so it still works from anywhere on
`PATH`.

**Pass `--component batbot`.** llama.cpp and ggml carry their own install
rules, written for people installing llama.cpp as a library: a plain
`cmake --install` would also drop `libllama.so`, `libggml*.so`,
`ggml-config.cmake` and `ggml.pc` loose into `<prefix>/lib`. BatBot does not
use those copies — and on a system-wide install one of them could shadow
another llama.cpp. The component installs what BatBot actually needs, all of
it under `lib/batbot/`, which is also what makes `batbot --uninstall` able to
remove everything it put down.

### Build options

| option | default | what it does |
|---|---|---|
| `BATBOT_BACKEND_DL` | `ON` | loadable GPU runtimes (see the note below) |
| `BATBOT_NATIVE` | `ON` | tune for this machine; only consulted by monolithic builds, since a loadable backend cannot be built for one CPU |
| `BATBOT_BUILD_TESTS` | `ON` | build the unit tests |
| `BATBOT_BUILD_TOOLS` | `ON` | build `batbot-routebench` |
| `BATBOT_WARNINGS` | `ON` | strict warnings on BatBot's own sources |
| `BATBOT_CUDA` | `OFF` | monolithic builds only: compile CUDA in |
| `BATBOT_VULKAN` | `OFF` | monolithic builds only: compile Vulkan in |

> **Note on filesystems:** a loadable runtime is a shared library, and shared
> library versioning needs symlinks — which exFAT and NTFS volumes do not
> support. Building a checkout on such a volume fails at the link step, so
> **build somewhere else and leave the sources where they are**:
>
> ```sh
> cmake -S . -B ~/.cache/batbot/build -DCMAKE_BUILD_TYPE=Release
> ```
>
> `install.sh` detects this and relocates the build tree on its own. CMake
> stops with this advice rather than letting the linker fail a thousand lines
> into the log.

#### Monolithic builds

`-DBATBOT_BACKEND_DL=OFF` gives the older shape: one static binary with at most
one backend compiled in, chosen by `BATBOT_CUDA` / `BATBOT_VULKAN`. Nothing is
loadable and the Runtimes panel says so. It exists because it is the only mode
that builds on a filesystem without symlinks.

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

**And the way back.** A **Reset model path to default** row sits directly under
Models directory. It is what you want when the folder was on a drive that is no
longer plugged in, or when you simply want the standard layout again. It stores
*no* path at all — an empty `models_dir` means "the default", so a config
copied between machines still points somewhere sensible on each.

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
| `/resume` | reopen an earlier conversation about this project |
| `/new` | start a fresh conversation, keeping the current one on disk |
| `/usage` | tokens spent this session and on this project |
| `/config` | open the settings screen |
| `/runtimes` | install or remove compute backends |
| `/models` | list the .gguf files in the models directory |
| `/experts` | which seats are filled, and with what |
| `/devices` | compute devices, with the indices the GPU split uses |
| `/release` | unload the resident expert, freeing its memory |
| `/clear` | clear the transcript and the experts' history |
| `/paths` | where the config, models, runtimes, history and log live |
| `/help`, `/quit` | |

| key | |
|---|---|
| `Ctrl-E` | settings: assign models, move the models directory, tune sampling |
| `Ctrl-C` | cancel the current answer; again when idle to quit |
| `Ctrl-T` | show/hide the roundtable |
| `PgUp` / `PgDn` | scroll the transcript |

The roundtable adapts to your terminal: the full ring above ~34 rows, a compact
bat above ~24, and a single status strip below that.

### Tokens

The status bar carries a running count, and the rate of the reply arriving now:

```
● answering  │  resident: Physics    tok ↑ 1.4k  ↓ 830  ·  42.1 tok/s
```

`↑` is prompt tokens in, `↓` generated tokens out. While a reply streams, the
rate is that reply's; when idle it is the session average. `/usage` breaks it
down and adds the project total.

The rate is measured from the first token, not from when the prompt was sent —
otherwise a long prompt would make a fast expert look slow.

Nothing here is billed. BatBot runs locally; the numbers are for knowing which
expert is slow and how close a conversation is to filling its context.

### Resuming a conversation

BatBot keeps history per project — the directory you started it in. `/resume`
lists what it has for *this* project and nothing else:

```
 resume · batbot
 ▸ 2 hours ago    why does the JIT swap cost so little?     6 turns   4.1k tok
   yesterday      explain the grammar-constrained sampler   3 turns   2.2k tok
   12 Aug         first pass at the router prompt          14 turns  18.3k tok

 ↑↓ choose   enter resume   d delete   esc cancel
```

`enter` restores the transcript **and** hands the exchanges back to the expert,
so the next question continues the conversation rather than starting cold.
Further turns append to the same session. `d` twice deletes one.

A session is written after each completed turn, so a crash costs at most the
turn in flight; a reply still streaming is never saved, because it is not
something to resume into. `/new` starts a fresh one without discarding the old.

History lives in `~/.local/share/batbot/projects/<name>-<hash>/`. The hash is
what keeps two different checkouts called `src` apart.

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
keyword routing, config inheritance, the trust store, UTF-8 chunking, the
backend table, GPU splitting, token accounting and session history — are covered
by a test suite that runs in well under a second:

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
80 cases, 0 failed, 0 assertions failed
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
27 checks, 0 failed
```

It covers the shared libraries, the runtimes you built and the build cache too:
the binary is no longer the whole install, and leaving several hundred megabytes
of `lib/batbot` behind would make "yes to everything" a lie. The sandbox
includes `XDG_CACHE_HOME` for a reason — without it, running these tests deletes
the build directory of whoever is running them.

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
table, CMake detection, the package lists, and the build-directory relocation —
has unit tests too, since a mistake there means a GPU that silently goes unused
rather than an error anyone would notice:

```sh
bash tests/test_install.sh
```

```
77 checks, 0 failed
```

One of those checks exists because of a real bug: the Vulkan package list was
missing `spirv-headers`, and ggml's Vulkan backend does
`find_package(SPIRV-Headers CONFIG REQUIRED)`. The install failed several
minutes in, at configure time, naming a CMake package rather than anything you
could install.

### Checking your setup

```sh
batbot --version
batbot --config          # where the config lives
```

Inside BatBot:

| | |
|---|---|
| `/experts` | which seats are filled, and with what |
| `/runtimes` | which backends are installed, and which are active |
| `/devices` | the compute devices llama.cpp found — confirms your GPU runtime is live |
| `/usage` | tokens spent this session and on this project |
| `/paths` | config, models, runtimes, history and log locations |

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
│   ├── gpu_policy.cpp  turning a split mode into tensor_split
│   ├── paths.cpp       XDG locations
│   └── trust.cpp       the folder-trust store
├── runtime/        the loadable compute backends
│   ├── backend.cpp     the backend table; everything derives from it
│   ├── registry.cpp    what is installed, and handing it to ggml
│   ├── builder.cpp     compiling one on demand, off the UI thread
│   └── devices.cpp     device enumeration and the GPU split policy
├── session/        what a conversation costs, and remembering it
│   ├── usage.cpp       token counting and its readout
│   └── store.cpp       per-project session history
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
│   ├── session_picker.cpp  the /resume list
│   ├── widgets/        bat sprite, roundtable
│   └── settings/       view, runtimes panel, GPU priority panel,
│                       directory browser, model picker, line editor
└── util/           text and formatting helpers, subprocess
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

**No backend is compiled in at all**, CPU included. ggml is built with its
dlopen-based backend loader, so every runtime is a shared library in a
directory rather than a decision frozen at compile time. `ModelHost`'s
constructor hands that directory to ggml before `llama_backend_init`; on a
fresh install it is empty, and `ModelHost::load` says so in those words rather
than letting llama.cpp throw "no CPU backend found" from three frames down.

A runtime built while BatBot is running is registered immediately —
`RuntimeRegistry::activate` scores the modules the way ggml's own loader does
and calls `ggml_backend_load` on the winner — and the engine then reloads its
models, because a model picks its devices when it loads and would otherwise go
on running on the old ones. ggml's loader has no duplicate check, so `activate`
asks ggml's device registry whether that backend is already present rather than
remembering for itself; registering twice would give every device a twin.

The same dlopen design is what makes the CPU runtime fast without being
fragile: ggml refuses `-march=native` for a loadable backend, so the build
emits one module per x86-64 feature level (`sandybridge`, `haswell`, `zen4`, …)
and ggml scores them at load and picks the best this machine can actually run.

Installing a module is a copy to a temporary name and a `rename` over the
target, never a write in place. Rebuilding a runtime that is already loaded is
an ordinary thing to do, and by then the module is mapped into the process —
overwriting those bytes crashes BatBot on the next call into the backend, while
`rename` swaps the directory entry and leaves the running mapping alone.

Building a runtime is a real cmake invocation against the llama.cpp checkout the
installer saved, at the exact tag the binary was built from — `BATBOT_LLAMA_TAG`
is compiled in for that reason, since a backend from a different tag would load
and then crash on the first tensor. It runs under `util/subprocess.cpp` rather
than `popen`, because a build has to be abandonable: `popen` returns no process
id, so there would be nothing to signal when the user cancels.

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
- [x] Loadable runtimes — install and remove CUDA / Vulkan / CPU from settings
- [x] Token counts per turn, session and project, with a live tok/s readout
- [x] Multi-GPU splitting: even by memory, or a priority order you set
- [x] `/resume` — per-project conversation history
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
