# Archaic

Path completion for Fish, Bash, and Zsh. A background daemon indexes your
tree once and answers Tab over a Unix socket.

## Install

### Arch / CachyOS

```bash
git clone https://github.com/Samoreilly/Archaic.git
cd Archaic/packaging/arch/archaic-git
makepkg -si
systemctl --user enable --now archaic.service
```

If you previously used `./install.sh`, run `./run.sh disable-service` first so the pacman unit is not shadowed.

Then open a **new** terminal, type `cd ` and press Tab.

To publish on the AUR later: push `packaging/arch/archaic-git` to `ssh://aur@aur.archlinux.org/archaic-git.git`. After that: `paru -S archaic-git`.

### Anywhere else

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

Keep the clone if you use `./install.sh` (the shell plugin is a symlink into it).

```bash
archaic-cli doctor
```

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

Default index is `~/src`, `~/projects`, `~/dev`, `~/code`, `~/git`, `~/samdev`, `~/work` if those directories exist — not all of `$HOME`. Add more without reinstalling:

```bash
archaic-cli watch ~/whatever
archaic-cli roots
```

- Socket: `$XDG_RUNTIME_DIR/archaic.sock` (else `/tmp/archaic-$UID.sock`)
- State: `$XDG_CACHE_HOME/archaic/state.bin` (else `~/.cache/archaic/state.bin`)
- `cd` / `mkdir` complete directories only
- Hidden files only if you typed `.`
- `sudo` / `make` / `python` complete paths only when the token looks like one

## Keybindings

When a dimmed ghost hint appears after your path, accept it without retyping:

| Keys | What it does |
|---|---|
| `Ctrl+Space` | Accept the hinted path (works in every terminal) |
| `Alt+Right` | Accept the hinted path (where your terminal passes it through) |
| `Alt+Down` / `Alt+Up` | Cycle through alternative completions |
| `Alt+Shift+Right` | Accept and keep completing (fish) |

Set `ARCHAIC_SUGGEST_ON_PROMPT=0` to turn ghost hints off (Tab completion keeps working).

## Troubleshooting

```bash
archaic-cli doctor
systemctl --user status archaic   # Linux
```

No completions: new shell, and the scan path must contain the files you want.
Daemon down: `./run.sh enable-service` or `./run.sh start`.

## License

MIT — see [LICENSE](LICENSE).
