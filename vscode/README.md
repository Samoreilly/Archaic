# Archaic Autocomplete

Blazing-fast file path autocomplete for VS Code using the [archaic](https://github.com/Samoreilly/Archaic) daemon.

## Features

- **Sub-millisecond completions** — queries the archaic daemon via Unix socket
- **Intelligent ranking** — frequency, recency, and path-aware scoring
- **File & folder icons** — proper VS Code CompletionItemKind for each entry
- **Graceful fallback** — returns empty when daemon is unavailable
- **Configurable** — socket path, timeout, and result count via VS Code settings

## Prerequisites

The archaic daemon must be running. Install and start it:

```bash
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
./install.sh ~/projects  # or your project root
```

Verify the daemon is running:

```bash
./run.sh status
```

## Installation

### From Source

```bash
cd vscode
npm install
npm run compile
npm run package
```

This creates `archaic-autocomplete-0.1.0.vsix`. Install it:

```bash
code --install-extension archaic-autocomplete-0.1.0.vsix
```

### Development

```bash
cd vscode
npm install
```

Open the `vscode/` folder in VS Code and press `F5` to launch the Extension Development Host.

## Configuration

| Setting | Default | Description |
|---|---|---|
| `archaic.socketPath` | `/tmp/archaic-daemon.sock` | Unix socket path for the daemon |
| `archaic.timeoutMs` | `100` | Query timeout in milliseconds |
| `archaic.maxResults` | `50` | Maximum completions per query |
| `archaic.enabled` | `true` | Enable/disable autocomplete |

Configure via VS Code Settings (`Ctrl+,` / `Cmd+,`) or `.vscode/settings.json`:

```json
{
  "archaic.socketPath": "/tmp/archaic-daemon.sock",
  "archaic.timeoutMs": 100,
  "archaic.maxResults": 50,
  "archaic.enabled": true
}
```

## Usage

Autocomplete triggers automatically when typing path-like text containing `/`, `.`, `-`, or `_`.

Examples:
- `import foo from "./src/` — triggers after `/`
- `cd ~/projects/` — triggers after `/`
- `./build/` — triggers after `/`

### Commands

- **Archaic: Toggle Autocomplete** — enable/disable completions
- **Archaic: Show Daemon Status** — display daemon connection info

## How It Works

```
VS Code → Unix Socket → archaic-daemon
                          ├── Radix Tree
                          ├── Frequency Scoring
                          └── LRU Cache
```

The extension connects to the archaic daemon via Unix domain socket on each completion request, sends a binary `IPC_MSG_COMPLETE` query with the current path prefix and working directory, and parses the `IPC_MSG_COMPLETIONS` response into VS Code `CompletionItem` objects.

## License

MIT
