'use strict';

/**
 * 最小 LSP 客户端：用 stdio 驱动语言服务器。
 *
 * 服务器通常是内嵌在生成器里的那份，启动命令是 `ParseEngine lsp --stdio`；
 * 想换成独立编译的 `lsp-server`，把 command/args 换成该可执行文件与 `['--stdio']` 即可。
 *
 * 协议要点：
 *   - 帧格式 `Content-Length: N\r\n\r\n` + UTF-8 正文；
 *   - 请求带自增 id，响应按 id 配对；
 *   - 服务器主动发的 `textDocument/publishDiagnostics` 走事件回调。
 *
 * 这个模块不依赖 `vscode`，可以在纯 Node 下跑（见 test/smoke.js）。
 */

const { EventEmitter } = require('node:events');
const { spawn } = require('node:child_process');

/** 默认请求超时（毫秒）：超过就当作失败，避免界面一直挂起。 */
const DEFAULT_TIMEOUT_MS = 15000;

class LspClient extends EventEmitter {
  /**
   * @param {string} command lsp-server 可执行文件路径。
   * @param {string[]} [args] 启动参数，默认 `["--stdio"]`。
   * @param {{cwd?: string, timeoutMs?: number}} [options] 启动选项。
   */
  constructor(command, args, options) {
    super();
    this.command = command;
    this.args = args && args.length ? args : ['--stdio'];
    this.cwd = (options && options.cwd) || undefined;
    this.timeoutMs = (options && options.timeoutMs) || DEFAULT_TIMEOUT_MS;

    /** @type {import('node:child_process').ChildProcess|null} */
    this.process = null;
    /** @type {Buffer} 还没解析完的字节流。 */
    this.buffer = Buffer.alloc(0);
    /** @type {Map<number, {resolve: Function, reject: Function, timer: NodeJS.Timeout}>} */
    this.pending = new Map();
    this.nextId = 1;
    this.stopped = false;
  }

  /** 服务器是否还在运行。 */
  get running() {
    return !!this.process && !this.stopped && this.process.exitCode === null;
  }

  /**
   * 启动子进程并挂上读写回调。
   *
   * @returns {Promise<void>} 进程起来后 resolve；起不来 reject。
   */
  start() {
    return new Promise((resolve, reject) => {
      // 1. 起进程：标准输入输出走管道，标准错误当作日志。
      let child;
      try {
        child = spawn(this.command, this.args, {
          cwd: this.cwd,
          stdio: ['pipe', 'pipe', 'pipe'],
          windowsHide: true,
        });
      } catch (error) {
        reject(error);
        return;
      }
      this.process = child;

      // 2. 进程起不来（路径错、缺依赖）时把错误抛给调用方。
      child.once('error', (error) => {
        this.stopped = true;
        this._failAll(error);
        reject(error);
      });

      // 3. 正常启动：挂事件。
      child.once('spawn', () => resolve());
      child.stdout.on('data', (chunk) => this._onData(chunk));
      child.stderr.on('data', (chunk) => this.emit('log', chunk.toString('utf8').trimEnd()));
      child.on('exit', (code) => {
        this.stopped = true;
        this._failAll(new Error(`语言服务器已退出（code=${code}）`));
        this.emit('exit', code);
      });
    });
  }

  /**
   * 发一个请求并等结果。
   *
   * @param {string} method LSP 方法名，例如 textDocument/hover。
   * @param {object} [params] 参数对象。
   * @param {object} [token] VS Code 取消令牌；取消时发送协议通知并清理等待项。
   * @returns {Promise<any>} 响应里的 result。
   */
  request(method, params, token) {
    return new Promise((resolve, reject) => {
      if (!this.running) { reject(new Error('语言服务器没有运行')); return; }
      const id = this.nextId++;
      const cancel = () => {
        const waiter = this.pending.get(id);
        if (!waiter) return;
        this.pending.delete(id); clearTimeout(waiter.timer); waiter.subscription?.dispose();
        this.notify('$/cancelRequest', { id });
        const error = new Error('请求已取消'); error.name = 'AbortError'; reject(error);
      };
      if (token?.isCancellationRequested) { const e = new Error('请求已取消'); e.name = 'AbortError'; reject(e); return; }
      const waiter = { resolve, reject, timer: setTimeout(() => {
        if (!this.pending.delete(id)) return;
        waiter.subscription?.dispose(); this.notify('$/cancelRequest', { id });
        reject(new Error(`请求超时：${method}`));
      }, this.timeoutMs) };
      this.pending.set(id, waiter);
      try {
        this._send({ jsonrpc: '2.0', id, method, params: params || {} });
        waiter.subscription = token?.onCancellationRequested(cancel);
        if (token?.isCancellationRequested) cancel();
        if (!this.pending.has(id)) waiter.subscription?.dispose();
      } catch (error) {
        clearTimeout(waiter.timer); waiter.subscription?.dispose(); this.pending.delete(id); reject(error);
      }
    });
  }

  /**
   * 发一个通知（不需要结果）。
   *
   * @param {string} method LSP 方法名。
   * @param {object} [params] 参数对象。
   */
  notify(method, params) {
    if (!this.running) return;
    try { this._send({ jsonrpc: '2.0', method, params: params || {} }); }
    catch (error) { this.emit('log', `通知失败：${error.message}`); }
  }

  /**
   * 握手：initialize + initialized。
   *
   * @returns {Promise<any>} 服务器的 capabilities。
   */
  async initialize() {
    const result = await this.request('initialize', {
      processId: process.pid,
      clientInfo: { name: 'vscode-gram-editor', version: '1.0.0' },
      rootUri: null,
      capabilities: {
        textDocument: {
          synchronization: { didSave: true, dynamicRegistration: false },
          publishDiagnostics: { relatedInformation: true },
          hover: { contentFormat: ['markdown', 'plaintext'] },
          completion: {
            completionItem: {
              snippetSupport: false,
              documentationFormat: ['markdown', 'plaintext'],
            },
          },
        },
        workspace: { workspaceFolders: false },
      },
    });
    this.capabilities = result.capabilities || {};
    this.notify('initialized', {});
    return result;
  }

  /**
   * 关闭服务器：先按协议请它退出，再兜底强杀。
   */
  async stop() {
    if (!this.process || this.stopped) return;
    let grace;
    try {
      await Promise.race([this.request('shutdown'), new Promise(resolve => { grace = setTimeout(resolve, 500); })]);
    } catch {
      // 服务器已经不响应也没关系，下面还有强杀。
    }
    clearTimeout(grace);
    this.notify('exit');
    this.stopped = true;
    const stopped = new Error('语言服务器已停止'); stopped.name = 'AbortError';
    this._failAll(stopped);

    // 给 300ms 让它自己退，然后强杀。
    setTimeout(() => {
      try {
        if (this.process && this.process.exitCode === null) this.process.kill();
      } catch {
        // 已经退了就无所谓。
      }
    }, 300);
  }

  /**
   * 写一帧。
   *
   * @param {object} message 要发送的 JSON 对象。
   */
  _send(message) {
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'ascii');
    this.process.stdin.write(Buffer.concat([header, body]));
  }

  /**
   * 收到一段字节：拼接后按帧切开，逐帧处理。
   *
   * @param {Buffer} chunk 新到的字节。
   */
  _onData(chunk) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;

    while (true) {
      // 1. 找帧头结束位置（兼容 \r\n\r\n 与 \n\n）。
      let headerEnd = this.buffer.indexOf('\r\n\r\n');
      let separatorLength = 4;
      if (headerEnd < 0) {
        headerEnd = this.buffer.indexOf('\n\n');
        separatorLength = 2;
      }
      if (headerEnd < 0) return;

      // 2. 读 Content-Length。
      const header = this.buffer.slice(0, headerEnd).toString('ascii');
      const match = /content-length:\s*(\d+)/i.exec(header);
      const bodyStart = headerEnd + separatorLength;
      if (!match) {
        this.buffer = this.buffer.slice(bodyStart);
        continue;
      }
      const length = Number(match[1]);

      // 3. 正文还没收齐就等下一段。
      if (this.buffer.length < bodyStart + length) return;
      const body = this.buffer.slice(bodyStart, bodyStart + length).toString('utf8');
      this.buffer = this.buffer.slice(bodyStart + length);

      // 4. 解析并分发。
      try {
        this._handle(JSON.parse(body));
      } catch {
        // 坏帧直接跳过，不打断整个连接。
      }
    }
  }

  /**
   * 分发一条消息。
   *
   * @param {any} message 解析后的消息。
   */
  _handle(message) {
    const isResponse = message.id !== undefined && message.method === undefined;

    // 1. 响应：唤醒等着的请求。
    if (isResponse) {
      const waiter = this.pending.get(message.id);
      if (waiter) {
        this.pending.delete(message.id);
        clearTimeout(waiter.timer);
        waiter.subscription?.dispose();
        if (message.error) {
          waiter.reject(new Error(message.error.message || 'LSP 请求失败'));
        } else {
          waiter.resolve(message.result);
        }
      }
      return;
    }

    // 2. 诊断推送：单独发一个事件，方便上层直接接。
    if (message.method === 'textDocument/publishDiagnostics') {
      const params = message.params || {};
      this.emit('diagnostics', { uri: params.uri, version: params.version, diagnostics: params.diagnostics || [] });
      return;
    }

    // 3. 其它通知一律透传。
    if (message.method) this.emit('notification', message);

    // 4. 服务器反向请求：回一个空结果，别让它等。
    if (message.method && message.id !== undefined) {
      try {
        this._send({ jsonrpc: '2.0', id: message.id, result: null });
      } catch {
        // 写不进去说明进程已经没了。
      }
    }
  }

  /**
   * 让所有等待中的请求以错误结束。
   *
   * @param {Error} error 要传给调用方的错误。
   */
  _failAll(error) {
    for (const [, waiter] of this.pending) {
      clearTimeout(waiter.timer);
      waiter.subscription?.dispose();
      waiter.reject(error);
    }
    this.pending.clear();
  }
}

module.exports = { LspClient };
