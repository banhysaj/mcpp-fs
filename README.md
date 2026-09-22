# mcpp-fs

A filesystem MCP server for Windows, written in C++. It gives you a set of fast,
lightweight filesystem tools (search, outline, read_lines, find_files, list_dir,
apply_edit, create_file, create_dir, run, and a few more) that an MCP client can
call over stdio.

The server code itself lives in [`mcpp-fs/mcpp-fs.cpp`](mcpp-fs/mcpp-fs.cpp). The
MCP protocol engine behind it isn't copied into this repo. It comes from a
separate project called [mcpp_async](https://github.com/banhysaj/mcpp_async),
which gets pulled in as a git submodule. That project also ships rapidjson, so
you get that along with it. The short version is that this repo is the server,
and mcpp_async is the protocol library it runs on.

## Getting the code

Clone it with the submodule so you actually get mcpp_async and rapidjson too:

```bash
git clone --recursive https://github.com/banhysaj/mcpp-fs.git
```

If you already cloned it the normal way and the mcpp_async folder came up empty,
just pull it in after the fact:

```bash
git submodule update --init --recursive
```

## Building it

Open `mcpp-fs.sln` in Visual Studio 2022 and build. The project already knows how to find the mcpp_async sources and headers
from the submodule, and it's set to C++17, so there's nothing extra to set up.

When it finishes you'll have `mcpp-fs.exe`, which is a stdio MCP server. Point
your MCP client at that exe and you're good to go.

## Running it, and pointing it at a folder

Since you're working with a filesystem, you need to have a root directory,
so any searches or reads are contained within that path. If you want to give the AI 
the full ability to roam, just skip this. The flag for it is `--root`.

In a Claude config that looks like this:

```json
{
  "mcpServers": {
    "fs": {
      "command": "C:\\Users\\wow\\Projects\\mcpp-fs\\x64\\Release\\mcpp-fs.exe",
      "args": ["--root", "C:\\Users\\wow\\Projects\\my-very-secret-project"]
    }
  }
}
```

With that, every session starts already sitting in `C:\Users\wow\Projects\my-very-secret-project`.

If the folder you pass isn't actually there, it won't fall over. It prints a quick
note to stderr and keeps going with the default.

The bigger thing to know is that pinning a `--root` seals the whole session inside that folder. 
Every tool that touches the disk stays inside the root. If you ask `read_lines`, `search`, `list_dir`,
`create_file`, `apply_edit` or any of them for a path that lands outside the root,
whether it's an absolute path like `C:\Windows` or a sneaky `..\..\my-very-secret-project`, the
server turns it down and tells you it's outside the root. Nothing gets read, and
nothing gets written.

`set_workdir` and `load_dir` follow the same rule. You can move the working folder
around inside the root, but the moment you aim it outside, it says no and stays put.

### Turning command execution off

Out of the box the server can run shell commands for you with the `run` tool, plus
a few helpers for background jobs. That's great for things like `git` inside your
project, but a shell command can reach anywhere on the machine, so it does not
respect the `--root` sandbox the way the file tools do.

When you want it properly locked down, pass `--no-exec`. That drops `run` and its
helpers (`job`, `job_wait`, `read_log`) entirely. They never even get registered,
so the model has no way to shell out at all.


```json
{
  "mcpServers": {
    "fs": {
      "command": "C:\\Users\\wow\\Projects\\mcpp-fs\\x64\\Release\\mcpp-fs.exe",
      "args": ["--root", "C:\\Users\\wow\\Projects\\my-very-secret-project", "--no-exec"]
    }
  }
}
```

The two flags are independent, `--root` on its own gives you a sandboxed filesystem that can still run commands. 
`--no-exec` on its own gives you the full filesystem but no shell. Both together is the strict setup, and
neither one is the wide-open default.

## The flags, all of them

| flag | what it does |
|------|--------------|
| `--root <dir>` | Pin the session to `<dir>` and put every file tool inside this jail. |
| `--no-exec` | Drop the command tools: `run`, `job`, `job_wait`, `read_log`. |

## The tools

This is everything the server offers. Paths are taken relative to the root, or to
wherever `set_workdir` currently points, unless the client passes an absolute one.

Looking around

- `list_dir` reads the immediate contents of one folder, a level at a time
- `find_files` lists file paths matching a glob, names only, no contents
- `search` finds text or a regex across files and returns the matching lines

Reading files

- `read_lines` returns a numbered window of a file, or several files in one call
- `outline` gives a quick map of what a file declares, without the bodies, this is specifically used for codebases, reading code

Changing things

- `apply_edit` swaps an exact piece of text in a file
- `create_file` writes a new file
- `create_dir` makes a folder, creating any missing parents

Running commands (present only when exec is on)

- `run` runs a shell command, in the foreground or as a background job
- `job` checks on a background job
- `job_wait` waits for a background job to finish
- `read_log` reads the captured output of a run by its id

Keeping it fast

- `load_dir` pulls a folder into memory so later reads and searches are quicker
- `cache_status` shows what's cached and how any loading is going
- `cache_clear` frees what you cached

Session

- `set_workdir` moves the working folder, staying inside the root when one is set

## Caching a folder in memory

`load_dir` is the tool to use when you're about to work in a folder a lot. It
reads the files under it into memory in the background and returns right away, so
you're never left waiting on it. From then on, reads and searches over those files
come from memory instead of the disk. It stays honest too, if a file changes on
disk the cache notices and refreshes, so you never get served stale content.

It takes a handful of optional arguments:

- `path` the folder to load, defaults to the current one
- `memBudgetMB` how much to pull in before it stops, defaults to 256
- `maxFileSizeKB` skip any single file bigger than this, defaults to 2048
- `exts` only load certain extensions like `cpp,h`
- `setRoot` also make this folder the working root, same as setting it yourself

Check on it any time with `cache_status` tool, and to hand the memory back when you're done, the `cache_clear` tool is used.

