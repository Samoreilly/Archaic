# Archaic

Path completion for Fish, Bash, and Zsh. A background daemon indexes your
tree once and answers Tab over a Unix socket.

## Install

Needs CMake 3.22+, a C23 compiler, and [libfmt](https://fmt.dev).

```bash
# Debian/Ubuntu: sudo apt install cmake g++ libfmt-dev
# Fedora:        sudo dnf install cmake gcc fmt-devel
# macOS:         brew install cmake fmt

git clone https://github.com/Samoreilly/Archaic.git
cd Archaic
./install.sh              # indexes $HOME
# ./install.sh ~/projects # or a specific tree
```

Open a **new** terminal, type `cd ` and press Tab.

```bash
archaic-cli doctor        # socket, ping, scan path
```

Keep the clone. The shell plugin is a symlink into it.

## After install

| Command | What it does |
|---|---|
| `archaic-cli doctor` | Is the daemon up? |
| `./run.sh status` | Socket / running |
| `./run.sh rescan` | Re-index |
| `./run.sh restart` | Restart |
| `./run.sh disable-service` | Stop login autostart |

On Linux the user systemd unit starts the daemon at login.

## Config

Optional. Copy [config.example.toml](config.example.toml) to
`~/.config/archaic/config.toml`.

- Socket: `$XDG_RUNTIME_DIR/archaic.sock` (else `/tmp/archaic-$UID.sock`)
- State: `$XDG_CACHE_HOME/archaic/state.bin` (else `~/.cache/archaic/state.bin`)
- `cd` / `mkdir` complete directories only
- Hidden files only if you typed `.`
- `sudo` / `make` / `python` complete paths only when the token looks like one

## Troubleshooting

```bash
archaic-cli doctor
systemctl --user status archaic   # Linux
```

No completions: new shell, and the scan path must contain the files you want.
Daemon down: `./run.sh enable-service` or `./run.sh start`.

## License

MIT — see [LICENSE](LICENSE).
