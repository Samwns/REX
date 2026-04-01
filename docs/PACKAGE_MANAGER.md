# REX Package Manager

REX has a built-in package manager for installing C++ libraries with a single command.

---

## Quick Start

```bash
rex add nlohmann-json          # install from built-in registry
rex add mathlib                # install from community registry
rex add nlohmann/json@v3.11.3  # install from GitHub
rex search json                # search for packages
rex list                       # show installed packages
rex remove nlohmann-json       # uninstall a package
```

---

## Package Sources

REX searches for packages in three registries, in order:

### 1. Built-in Registry

Hardcoded in REX with known download URLs. These are popular, stable libraries:

| Library | Type | Description |
|---|---|---|
| `nlohmann-json` | header-only | JSON for Modern C++ |
| `stb` | header-only | Single-file image loading/writing |
| `glm` | header-only | OpenGL Mathematics (vectors, matrices) |
| `magic-enum` | header-only | Static enum reflection |
| `toml11` | header-only | TOML parser and serializer |
| `argparse` | header-only | Command-line argument parser |
| `fmt` | compiled | Python-like formatting for C++ |
| `spdlog` | compiled | Fast, header-only-compatible logging |
| `termcolor` | header-only | Terminal color output |

### 2. Community Registry ([rex-packages](https://github.com/Samwns/rex-packages))

Community-maintained packages hosted at `https://github.com/Samwns/rex-packages`. REX fetches the `registry.json` index to discover available packages.

```bash
rex add mathlib           # downloads from community registry
rex search                # lists all community + built-in packages
```

### 3. GitHub Direct

Install any C++ library directly from GitHub by specifying `user/repo`:

```bash
rex add nlohmann/json               # latest release
rex add nlohmann/json@v3.11.3       # specific version/tag
rex add gabime/spdlog@v1.12.0       # specific version
```

REX downloads the repository archive from GitHub, extracts headers, and installs them.

---

## Commands

### `rex add <library>`

Install a library. REX tries each registry in order:

```bash
rex add nlohmann-json          # 1. Check built-in registry ✓
rex add mathlib                # 2. Check community registry ✓
rex add user/repo              # 3. Download from GitHub ✓
rex add user/repo@v1.0.0       # 3. Specific version from GitHub
```

If the library is found in the built-in or community registry, it's downloaded from the known URL. If not found, REX suggests using the `user/repo` format for direct GitHub installs.

### `rex remove <library>`

Uninstall a library:

```bash
rex remove nlohmann-json
rex rm nlohmann-json           # 'rm' is an alias for 'remove'
```

This removes the library directory from `~/.rex/libs/` and updates `rex.toml` if present.

### `rex list`

Show all installed libraries:

```bash
rex list
```

Lists every package directory under `~/.rex/libs/`.

### `rex registry`

Show all available libraries from all registries:

```bash
rex registry
```

Displays the full listing of built-in libraries and community packages.

### `rex search [query]`

Search for packages by name or description:

```bash
rex search            # list everything
rex search json       # search for "json"
rex search math       # search for "math"
```

Searches both the built-in registry and the community registry.

---

## How Packages Work

### Installation Location

All packages are installed to `~/.rex/libs/`:

```
~/.rex/
└── libs/
    ├── runtime.h                    # rexc runtime (auto-maintained)
    ├── nlohmann-json/
    │   └── include/
    │       └── nlohmann/
    │           └── json.hpp
    ├── glm/
    │   └── include/
    │       └── glm/
    │           ├── glm.hpp
    │           └── ...
    └── fmt/
        └── include/
            └── fmt/
                ├── core.h
                └── ...
```

### Automatic Include

REX automatically adds `-I` flags for each installed package directory, so you can `#include` headers directly:

```cpp
#include <nlohmann/json.hpp>    // works if nlohmann-json is installed
#include <glm/glm.hpp>          // works if glm is installed
```

No need to modify `rex.toml` or add manual include paths.

### rex.toml Integration

When you `rex add` a library in a project directory (where `rex.toml` exists), the dependency is also recorded:

```toml
[dependencies]
nlohmann-json = "latest"
```

---

## Publishing Packages

To publish your own C++ library to the community registry:

1. Create your library repository on GitHub
2. Follow the structure conventions in [rex-packages](https://github.com/Samwns/rex-packages)
3. Submit a pull request to the `rex-packages` registry

The community registry supports:

| Field | Description |
|---|---|
| `name` | Package name |
| `version` | Version string |
| `description` | Short description |
| `author` | Author name |
| `license` | License type |
| `path` | Archive download path |
| `include_dir` | Header directory within the archive |

---

## Header-Only vs Compiled Libraries

| Type | Include | Link | Examples |
|---|---|---|---|
| Header-only | `#include <lib/lib.hpp>` | Nothing | nlohmann-json, glm, stb |
| Compiled | `#include <lib/lib.h>` | Need to link `.a`/`.so` | fmt, spdlog |

For header-only libraries, `rex add` is all you need. For compiled libraries, you may need to add link flags to `rex.toml`:

```toml
[build]
flags = ["-lfmt"]
```
