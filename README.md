**A CLI Local LLM delegator.**

Crucible is a full CLI tool that works with 10 specially trained local LLM's called `Experts` and 1
delegator model to point your prompt to the correct expert agent.

`cd` into a project, then type `crucible`. Crucible is the delegator model. It
decides what expert model loads and computes your prompt. Load in specially trained models
in Physics, Mathematics, Programming, etc and Crucible will choose the best model for the prompt.

---

## Why

Running a single local model has to be small enough to fit onto your hardware,
so it isn't the best on its own. Using a `Mixture-of-Agents` approach, You can
get 10 specially trained models on specific subjects, and one delegator model to
choose which one gets the prompt. 

The purpose of this approach is to be able to get as much power out of Local models as
Possible. 

**Example**
If you choose 10, specially trained 30 billion parameter models, Essentially, you can 
have an AI setup that performs pretty much as well as a 300 billion parameter model using
30 billion parameters of space. The only downside to this setup is Just-In-Time loading overhead.

---

## Install

**One command**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/crucible/main/install.sh | bash
```

This will install Crucible and its dependencies. Re-running the installer upgrades in place.

## Uninstalling

**Uninstalling**

```sh
crucible --uninstall
```

If the binary is already gone, the installer can clean up instead:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/crucible/main/install.sh | bash -s -- --uninstall
```

### Installer options

You do not need to install runtimes before installing Crucible. The program has a built in runtime manager.

If you want to install runtimes with the initial install, pass options after `--`:

```sh
curl -fsSL .../install.sh | bash -s -- --gpu vulkan --prefix ~/.local
```

| option | |
|---|---|
| `--gpu cuda\|vulkan\|metal\|cpu\|auto` | which GPU SDK to install (default `auto`; `metal` on a Mac, and `cuda` is refused there) |
| `--prefix DIR` | install location (default `/usr/local`, or `~/.local` without sudo) |
| `--jobs N` | parallel build jobs |
| `--check` | report what would happen, change nothing, never ask for sudo |
| `--no-deps` | do not install system packages |
| `-y`, `--yes` | never prompt |
| `--uninstall` | remove Crucible (leaves your config and models) |

---

## Runtimes

Crucible supports `CUDA`, `Vulkan`, `Metal` and `CPU` runtimes.

They are stored in `~/.local/share/crucible/runtimes`.

## Multiple GPUs

| mode | what it does |
|---|---|
| `auto` | let llama.cpp decide (the default) |
| `even` | proportional to each card's memory, so they finish together |
| `priority` | fill the cards in the order you list, spilling into the next only when one is full |
| `single` | everything on **Main GPU** |

### Keeping the work on the GPU

Two settings under **HARDWARE** decide whether the processor and system memory
get involved at all.

| setting | default | what it does |
|---|---|---|
| **GPU-only compute** | on | every layer on the GPU, whatever **GPU layers** says |
| **Dedicated VRAM only** | off | refuse a model that will not fit in video memory |

---

## Build from source

If you would rather do it yourself: a C++20 compiler, CMake ≥ 3.24, and git.
Everything else — llama.cpp, FTXUI, nlohmann/json — is fetched and pinned
automatically.

```sh
git clone https://github.com/mattsaund/crucible.git
cd crucible
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/crucible
```

This builds the binary and no compute backend at all. Every runtime, CPU
included, is added afterwards from the settings screen — see
[Runtimes](#runtimes).

```sh
sudo cmake --install build --component crucible
# or, for a user prefix:
cmake --install build --component crucible --prefix ~/.local
```

The install is `bin/crucible` plus `lib/crucible/` holding llama.cpp's three shared
libraries. The binary's RPATH is relative, so it still works from anywhere on
`PATH`.

**Pass `--component crucible`.** llama.cpp and ggml carry their own install
rules, written for people installing llama.cpp as a library: a plain
`cmake --install` would also drop `libllama.so`, `libggml*.so`,
`ggml-config.cmake` and `ggml.pc` loose into `<prefix>/lib`. Crucible does not
use those copies — and on a system-wide install one of them could shadow
another llama.cpp. The component installs what Crucible actually needs, all of
it under `lib/crucible/`, which is also what makes `crucible --uninstall` able to
remove everything it put down.

## Build options

| option | default | what it does |
|---|---|---|
| `CRUCIBLE_BACKEND_DL` | `ON` | loadable GPU runtimes (see the note below) |
| `CRUCIBLE_NATIVE` | `ON` | tune for this machine; only consulted by monolithic builds, since a loadable backend cannot be built for one CPU |
| `CRUCIBLE_BUILD_TESTS` | `ON` | build the unit tests |
| `CRUCIBLE_BUILD_TOOLS` | `ON` | build `crucible-routebench` |
| `CRUCIBLE_WARNINGS` | `ON` | strict warnings on Crucible's own sources |
| `CRUCIBLE_CUDA` | `OFF` | monolithic builds only: compile CUDA in |
| `CRUCIBLE_VULKAN` | `OFF` | monolithic builds only: compile Vulkan in |

---

## Setup

As of right now, Crucible is BYO models. There are plans in the future to train specifically trained experts to open source.

You can either store the models in the default model directory or point Crucible to your own model directory

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

## Config

Type **`/settings`**. Everything in the config file is
editable there: choose the models directory with a browser, pick a model for
each expert seat and for the delegator from whatever is in it, and tune sampling.

```
╭ Settings ──────────────────────────────────────────────────────╮
│  ~/.local/share/crucible/models                  3 models found  │
├────────────────────────────────────────────────────────────────┤
│  MODELS                                                        │
│   Models directory    ~/.local/share/crucible/models             │
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
**Editing the config file**

`~/.config/crucible/config.json`:

```jsonc
{
  "models_dir": "~/.local/share/crucible/models",
  "router":   { "model": "LFM2-1.2B-Q8_0.gguf" },
  "defaults": { "n_ctx": 8192, "n_gpu_layers": -1, "temperature": 0.7 },
  "experts": {
    "mathematics": { "model": "math-expert-q4_k_m.gguf" },
    "physics":     { "model": "physics-expert-q4_k_m.gguf" },
    "programming": { "model": "/mnt/big/code-expert-q4_k_m.gguf" }
  }
}
```

```jsonc
"routing": {
  "min_confidence": 0.60,       // below this, treat the answer as undecided
  "use_fallback_expert": true   // empty seats send work to Fallback, not elsewhere
}
```

## The delegator model

The delegator never answers you; it only names a subject. 

It applies a score to the prompt to dictate what subject would be best suited to answer.
You can adjust the min scoring thresholds in settings. The more detailed the prompt the more
accurate the delegator is.

---

## Commands

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

Type `/` and the list folds up out of the prompt, narrowing as you type, with
the rest of the best match in grey after the cursor. `Tab` takes it:

```
 › /resume   reopen an earlier conversation about this project
   /release  unload the resident expert and free its memory
   tab completes   ↑↓ choose   esc dismiss
 › /resume
     ▲── typed "/re"; "sume" is the suggestion
```

| key | |
|---|---|
| `Tab` | accept the suggested command |
| `/settings` | assign models, move the models directory, tune sampling |
| `↑` `↓` · wheel | scroll the transcript a line at a time |
| `Ctrl-C` | cancel the current answer; again when idle to quit |
| `Ctrl-T` | show/hide the roundtable |
| `PgUp` / `PgDn` | scroll the transcript a page at a time |

### Resuming a conversation

Crucible keeps history per project — the directory you started it in. `/resume`
lists what it has for *this* project and nothing else:

```
 resume · crucible
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

History lives in `~/.local/share/crucible/projects/<name>-<hash>/`. The hash is
what keeps two different checkouts called `src` apart.

---

## File Structure

src/
├── main.cpp        parse, trust, hand off -- 40 lines
├── app/            things that happen instead of the TUI
│   ├── cli.cpp         argument parsing, banner, usage
│   ├── trust_gate.cpp  the folder-trust prompt
│   └── uninstall.cpp   crucible --uninstall
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
│   ├── subject.cpp     the subject table; labels and prompt come from it
│   ├── router.cpp      KeywordRouter and ModelRouter
│   └── completion.cpp  the slash-command list, and matching a prefix to it
├── llm/            everything that touches llama.cpp
│   ├── model_host.cpp    owns the backend; one expert resident at a time
│   ├── loaded_model.cpp  the generation loop
│   ├── sampling.cpp      building a sampler chain
│   ├── model_catalog.cpp reading the models directory
│   ├── model_shape.cpp   what a GGUF says about itself, before it is loaded
│   └── response_filter.cpp  sorting a model's working from its answer
├── tools/          what the experts can reach beyond the machine
│   └── web_search.cpp  looking something up, off by default
│
├── util/           the parts with no opinions
│   ├── markdown.cpp    reading the markdown a model wrote
│   ├── resources.cpp   what the GPUs and the processor are doing
│   ├── subprocess.cpp  fork/exec with a merged output pipe
│   ├── format.cpp      bytes, durations, paths
│   └── text.cpp        UTF-8 that arrives a fragment at a time
│
├── engine/         the delegation loop            [worker thread]
│   ├── engine.cpp        route -> JIT swap -> generate
│   ├── route_policy.cpp  what to do with the delegator's answer
│   └── state.cpp         the only memory the two threads share
├── ui/                                            [UI thread]
│   ├── app.cpp         shell, key handling, animation clock
│   ├── commands.cpp    slash commands
│   ├── transcript.cpp  drawing the conversation, markdown and all
│   ├── session_picker.cpp  the /resume list
│   ├── widgets/        crucible sprite, roundtable
│   └── settings/       view, runtimes panel, GPU priority panel,
│                       model manager, directory browser, model picker,
│                       line editor
└── util/           text and formatting helpers, subprocess

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
- [ ] Fine-tuned 1B Crucible router to replace the off-the-shelf one
- [ ] Curated subject experts, offered as a download you opt into — never bundled

---

**AI Policy**

I am open to AI and agentic coding, but the code written needs to follow specific guidelines:
1. MUST be human readable, acceptable variable/function names.
2. easily tracable, following a good program flow
3. Contributer MUST look at/document code and code changes. you need to understand the code that is being written.

---

## License

MIT — see [LICENSE](LICENSE). Every dependency Crucible links is MIT too, and
[THIRD_PARTY.md](THIRD_PARTY.md) lists them with their pinned versions. Nothing
is vendored into this repository; the build fetches each at a fixed tag.

Model weights are covered by none of it. Crucible ships no models and downloads
none — whatever GGUFs you put in your models directory carry their own licenses,
which are between you and whoever trained them.

---