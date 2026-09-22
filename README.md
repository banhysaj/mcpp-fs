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

