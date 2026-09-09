import * as vscode from 'vscode';
import * as net from 'net';
import * as path from 'path';
import * as fs from 'fs';

// ── IPC Protocol Constants ──────────────────────────────────────────────────

const IPC_MAGIC = 0x41520001;
const IPC_MSG_COMPLETE = 3;
const IPC_MSG_COMPLETIONS = 102;
const IPC_MSG_ERROR = 101;

const HEADER_SIZE = 16; // 4 x uint32
const COMPLETE_REQ_SIZE = 8200; // 4096 + 4096 + 4 + 4
const COMPLETIONS_RESP_SIZE = 205804; // 4 + 50*4096 + 50*8 + 50*8 + 50*4

// ── Binary Protocol Helpers ─────────────────────────────────────────────────

function writeUint32LE(buf: Buffer, offset: number, value: number): void {
  buf.writeUInt32LE(value, offset);
}

function buildCompleteRequest(prefix: string, cwd: string, limit: number): Buffer {
  const buf = Buffer.alloc(COMPLETE_REQ_SIZE, 0);

  // prefix[4096]
  const prefixBuf = Buffer.from(prefix, 'utf8');
  prefixBuf.copy(buf, 0, 0, Math.min(prefixBuf.length, 4095));

  // cwd[4096]
  const cwdBuf = Buffer.from(cwd, 'utf8');
  cwdBuf.copy(buf, 4096, 0, Math.min(cwdBuf.length, 4095));

  // limit (uint32)
  writeUint32LE(buf, 8192, limit);

  // dirs_only (uint32) — always 0 for VS Code (we show both files and dirs)
  writeUint32LE(buf, 8196, 0);

  return buf;
}

function buildHeader(msgType: number, payloadLen: number, reqId: number): Buffer {
  const buf = Buffer.alloc(HEADER_SIZE);
  writeUint32LE(buf, 0, IPC_MAGIC);
  writeUint32LE(buf, 4, msgType);
  writeUint32LE(buf, 8, payloadLen);
  writeUint32LE(buf, 12, reqId);
  return buf;
}

function parseCompletionsResponse(buf: Buffer): Array<{ path: string; isDir: boolean }> {
  const results: Array<{ path: string; isDir: boolean }> = [];

  const count = buf.readUInt32LE(0);
  const safeCount = Math.min(count, 50);

  for (let i = 0; i < safeCount; i++) {
    // paths[i] starts at offset 4 + i * 4096
    const pathOffset = 4 + i * 4096;
    const pathEnd = buf.indexOf(0, pathOffset);
    const rawPath = buf.subarray(pathOffset, pathEnd > pathOffset ? pathEnd : pathOffset + 4096).toString('utf8');

    if (!rawPath) continue;

    // is_dirs[i] starts at offset 4 + 50*4096 + 50*8 + 50*8 + i*4
    const isDirOffset = 4 + 50 * 4096 + 50 * 8 + 50 * 8 + i * 4;
    const isDir = buf.readUInt32LE(isDirOffset) !== 0;

    results.push({ path: rawPath, isDir });
  }

  return results;
}

// ── Daemon Client ───────────────────────────────────────────────────────────

interface DaemonResponse {
  paths: Array<{ path: string; isDir: boolean }>;
}

function queryDaemon(socketPath: string, prefix: string, cwd: string, limit: number, timeoutMs: number): Promise<DaemonResponse> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(socketPath, () => {
      // Connection established — send request
      const payload = buildCompleteRequest(prefix, cwd, limit);
      const header = buildHeader(IPC_MSG_COMPLETE, payload.length, 1);
      socket.write(Buffer.concat([header, payload]));
    });

    const timer = setTimeout(() => {
      socket.destroy();
      reject(new Error('Daemon query timed out'));
    }, timeoutMs);

    let responseBuf = Buffer.alloc(0);

    socket.on('data', (chunk: Buffer) => {
      responseBuf = Buffer.concat([responseBuf, chunk]);

      // Need at least header
      if (responseBuf.length < HEADER_SIZE) return;

      const msgType = responseBuf.readUInt32LE(4);
      const payloadLen = responseBuf.readUInt32LE(8);

      // Need full message
      if (responseBuf.length < HEADER_SIZE + payloadLen) return;

      const payload = responseBuf.subarray(HEADER_SIZE, HEADER_SIZE + payloadLen);

      clearTimeout(timer);
      socket.destroy();

      if (msgType === IPC_MSG_COMPLETIONS) {
        const paths = parseCompletionsResponse(payload);
        resolve({ paths });
      } else if (msgType === IPC_MSG_ERROR) {
        const errorCode = payload.readInt32LE(0);
        const msgEnd = payload.indexOf(0, 4);
        const msg = payload.subarray(4, msgEnd > 4 ? msgEnd : 4 + 256).toString('utf8');
        reject(new Error(`Daemon error (${errorCode}): ${msg}`));
      } else {
        reject(new Error(`Unexpected response type: ${msgType}`));
      }
    });

    socket.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

// ── Path Prefix Extraction ──────────────────────────────────────────────────

/**
 * Extract the path prefix being typed from the current line and position.
 * Handles patterns like:
 *   import foo from "./src/ut|
 *   cd /home/user/proj|
 *   ./build/|
 */
function extractPathPrefix(line: string, position: number): { prefix: string; startCol: number } | null {
  const textBefore = line.substring(0, position);

  // Find the start of the current token (path segment being typed)
  // Path tokens can contain: alphanumeric, /, ., -, _, ~, @
  let startCol = position - 1;
  while (startCol >= 0) {
    const ch = textBefore[startCol];
    // Stop at whitespace, quotes, or common delimiters that aren't part of paths
    if (/\s/.test(ch) || ch === '"' || ch === "'" || ch === '`' || ch === '(' || ch === '{' || ch === '[') {
      break;
    }
    startCol--;
  }
  startCol++; // Move past the delimiter

  const prefix = textBefore.substring(startCol);

  // Only trigger if there's something that looks like a path
  if (!prefix || prefix.length === 0) return null;

  // Must contain at least one path-like character sequence
  // (starts with /, ./, ../, ~, or contains /)
  if (!prefix.startsWith('/') && !prefix.startsWith('./') && !prefix.startsWith('../') &&
      !prefix.startsWith('~') && !prefix.includes('/')) {
    return null;
  }

  return { prefix, startCol };
}

// ── Completion Item Provider ────────────────────────────────────────────────

class ArchaicCompletionProvider implements vscode.CompletionItemProvider {
  private socketPath: string;
  private timeoutMs: number;
  private maxResults: number;
  private enabled: boolean;

  constructor() {
    this.loadConfig();
  }

  private loadConfig(): void {
    const config = vscode.workspace.getConfiguration('archaic');
    this.socketPath = config.get<string>('socketPath', '/tmp/archaic-daemon.sock');
    this.timeoutMs = config.get<number>('timeoutMs', 100);
    this.maxResults = config.get<number>('maxResults', 50);
    this.enabled = config.get<boolean>('enabled', true);
  }

  async provideCompletionItems(
    document: vscode.TextDocument,
    position: vscode.Position,
    _token: vscode.CancellationToken,
    _context: vscode.CompletionContext
  ): Promise<vscode.CompletionItem[]> {
    if (!this.enabled) return [];

    // Reload config on each call to pick up changes
    this.loadConfig();

    const line = document.lineAt(position.line).text;
    const extracted = extractPathPrefix(line, position.character);

    if (!extracted) return [];

    const { prefix, startCol } = extracted;

    // Determine the working directory for the query
    const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
    const cwd = workspaceFolder ? workspaceFolder.uri.fsPath : path.dirname(document.uri.fsPath);

    try {
      const response = await queryDaemon(this.socketPath, prefix, cwd, this.maxResults, this.timeoutMs);

      return response.paths.map((entry) => {
        const item = new vscode.CompletionItem(
          entry.path,
          entry.isDir ? vscode.CompletionItemKind.Folder : vscode.CompletionItemKind.File
        );

        // Set the range to replace the current prefix
        const range = new vscode.Range(
          new vscode.Position(position.line, startCol),
          position
        );
        item.range = range;

        // Add trailing slash for directories
        if (entry.isDir && !entry.path.endsWith('/')) {
          item.insertText = entry.path + '/';
          item.filterText = entry.path;
        }

        // Add detail showing type
        item.detail = entry.isDir ? 'Directory' : 'File';

        return item;
      });
    } catch {
      // Daemon unavailable — return empty gracefully
      return [];
    }
  }
}

// ── Extension Activation ────────────────────────────────────────────────────

let provider: ArchaicCompletionProvider | undefined;

export function activate(context: vscode.ExtensionContext): void {
  provider = new ArchaicCompletionProvider();

  // Register for all languages
  const selector: vscode.DocumentSelector = { scheme: 'file' };

  const registration = vscode.languages.registerCompletionItemProvider(
    selector,
    provider,
    '/', '.', '-', '_'
  );

  context.subscriptions.push(registration);

  // Toggle command
  const toggleCmd = vscode.commands.registerCommand('archaic.toggle', async () => {
    const config = vscode.workspace.getConfiguration('archaic');
    const current = config.get<boolean>('enabled', true);
    await config.update('enabled', !current, vscode.ConfigurationTarget.Global);
    vscode.window.showInformationMessage(`Archaic autocomplete ${!current ? 'enabled' : 'disabled'}`);
  });
  context.subscriptions.push(toggleCmd);

  // Status command
  const statusCmd = vscode.commands.registerCommand('archaic.status', () => {
    const config = vscode.workspace.getConfiguration('archaic');
    const socketPath = config.get<string>('socketPath', '/tmp/archaic-daemon.sock');
    const enabled = config.get<boolean>('enabled', true);

    const socketExists = fs.existsSync(socketPath);

    const status = `Archaic Autocomplete\n` +
      `  Enabled: ${enabled}\n` +
      `  Socket: ${socketPath}\n` +
      `  Socket exists: ${socketExists ? 'Yes' : 'No'}\n` +
      `  Timeout: ${config.get<number>('timeoutMs', 100)}ms\n` +
      `  Max Results: ${config.get<number>('maxResults', 50)}`;

    vscode.window.showInformationMessage(status);
  });
  context.subscriptions.push(statusCmd);

  // Reload config when settings change
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('archaic') && provider) {
        provider['loadConfig']();
      }
    })
  );
}

export function deactivate(): void {
  provider = undefined;
}
