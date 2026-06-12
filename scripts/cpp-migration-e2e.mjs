#!/usr/bin/env node

import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, realpathSync, rmSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { tmpdir } from 'node:os'
import { spawn, spawnSync } from 'node:child_process'
import { createServer } from 'node:http'
import { createHash } from 'node:crypto'

const repoRoot = resolve(new URL('..', import.meta.url).pathname)
const nativeBinary = process.env.CC_REPL_NATIVE_BINARY
  ? resolve(repoRoot, process.env.CC_REPL_NATIVE_BINARY)
  : resolve(repoRoot, 'dist', process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl')

function run(label, command, args, expectedText, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? repoRoot,
    encoding: 'utf8',
    env: options.env ?? process.env,
    input: options.input,
  })

  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`
  if (result.error) throw result.error
  if (result.status !== 0) {
    process.stderr.write(output)
    throw new Error(`${label} failed with exit code ${result.status}`)
  }
  if (expectedText && !output.includes(expectedText)) {
    process.stderr.write(output)
    throw new Error(`${label} did not include expected text: ${expectedText}`)
  }
  console.log(`ok - ${label}`)
  return output
}

function runFailure(label, command, args, expectedText, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? repoRoot,
    encoding: 'utf8',
    env: options.env ?? process.env,
    input: options.input,
  })

  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`
  if (result.error) throw result.error
  if (result.status === 0) {
    process.stderr.write(output)
    throw new Error(`${label} succeeded unexpectedly`)
  }
  if (expectedText && !output.includes(expectedText)) {
    process.stderr.write(output)
    throw new Error(`${label} output did not include ${expectedText}`)
  }
  console.log(`ok - ${label}`)
  return output
}

function normalizeInventoryName(name) {
  return name
    .replace(/Tool$/, '')
    .replaceAll('MCP', 'Mcp')
    .replaceAll('LSP', 'Lsp')
    .replaceAll('REPL', 'Repl')
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/[^a-zA-Z0-9]+/g, '_')
    .toLowerCase()
    .replace(/^_+|_+$/g, '')
    .replace(/_tool$/, '')
    .replace(/^read$/, 'file_read')
    .replace(/^write$/, 'file_write')
    .replace(/^edit$/, 'file_edit')
    .replace(/^web_fetch$/, 'web_fetch')
    .replace(/^web_search$/, 'web_search')
}

function waitFor(predicate, timeoutMs, label) {
  return new Promise((resolve, reject) => {
    const started = Date.now()
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer)
        resolve()
        return
      }
      if (Date.now() - started > timeoutMs) {
        clearInterval(timer)
        reject(new Error(`timed out waiting for ${label}`))
      }
    }, 25)
  })
}

function webSocketAcceptKey(key) {
  return createHash('sha1')
    .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
    .digest('base64')
}

function base64UrlEncode(text) {
  return Buffer.from(text, 'utf8')
    .toString('base64')
    .replaceAll('+', '-')
    .replaceAll('/', '_')
    .replace(/=+$/g, '')
}

function serverWebSocketTextFrame(text) {
  const payload = Buffer.from(text)
  if (payload.length < 126) {
    return Buffer.concat([Buffer.from([0x81, payload.length]), payload])
  }
  if (payload.length <= 0xffff) {
    const header = Buffer.alloc(4)
    header[0] = 0x81
    header[1] = 126
    header.writeUInt16BE(payload.length, 2)
    return Buffer.concat([header, payload])
  }
  const header = Buffer.alloc(10)
  header[0] = 0x81
  header[1] = 127
  header.writeBigUInt64BE(BigInt(payload.length), 2)
  return Buffer.concat([header, payload])
}

async function runHeadlessIngressLifecycleE2e() {
  const requests = []
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/v1/sessions/session_1/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    ['--headless', '--session-id=session_1', '--task-id=work_1'],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: process.env.ANTHROPIC_API_KEY || 'dummy',
        CLAUDE_CODE_REMOTE_API_BASE_URL: `http://127.0.0.1:${port}`,
        CC_REMOTE_SESSION_ID: 'session_1',
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await waitFor(
      () => requests.some(request => request.body.includes('"status":"started"')),
      5000,
      'headless started lifecycle event',
    )
    child.kill('SIGINT')
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless process exit'))
      }, 5000)
      child.once('exit', (code, signal) => {
        clearTimeout(timer)
        if (code !== 0 && signal !== 'SIGINT') {
          reject(new Error(`headless process exited with code ${code ?? 'null'} signal ${signal ?? 'null'}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })
    await waitFor(
      () => requests.some(request => request.body.includes('"status":"stopped"')),
      5000,
      'headless stopped lifecycle event',
    )

    const lifecycleRequests = requests.filter(request => request.path === '/v1/sessions/session_1/events')
    if (lifecycleRequests.length < 2) {
      throw new Error(`expected at least two session lifecycle requests, saw ${lifecycleRequests.length}`)
    }
    for (const request of lifecycleRequests) {
      if (request.headers.authorization !== 'Bearer session-token-from-env') {
        throw new Error('headless lifecycle request did not use bearer session token')
      }
      if (!request.body.includes('"type":"session_lifecycle"') ||
          !request.body.includes('"bridge_work_id":"work_1"')) {
        throw new Error(`headless lifecycle request body was not a bridge lifecycle event: ${request.body}`)
      }
    }
    console.log('ok - native headless session ingress lifecycle e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessStreamJsonRemoteQueryE2e() {
  const requests = []
  const assistantText = 'native headless stream-json reply'
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/v1/messages') {
        res.writeHead(200, {
          'Content-Type': 'application/json',
        })
        res.end(JSON.stringify({
          id: 'msg_fake_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text: assistantText }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 5, output_tokens: 7 },
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/v1/sessions/session_1/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--replay-user-messages',
      '--session-id=session_1',
      '--task-id=work_1',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
        CLAUDE_CODE_REMOTE_API_BASE_URL: `http://127.0.0.1:${port}`,
        CC_REMOTE_SESSION_ID: 'session_1',
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
      },
      stdio: ['pipe', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    child.stdin.write(JSON.stringify({
      type: 'user',
      message: {
        role: 'user',
        content: 'run native headless stream-json e2e',
      },
      parent_tool_use_id: null,
      session_id: 'session_1',
    }) + '\n')
    child.stdin.end()

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless stream-json process exit'))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`headless stream-json exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const stdout = output.join('')
    const lines = stdout.trim().split(/\r?\n/).filter(Boolean).map(line => JSON.parse(line))
    if (!lines.some(line => line.type === 'user' && line.message?.content === 'run native headless stream-json e2e')) {
      throw new Error(`headless stream-json did not replay user message:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'assistant' && line.message?.content?.[0]?.text === assistantText)) {
      throw new Error(`headless stream-json did not emit assistant event:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'result' && line.result === assistantText && line.usage?.input_tokens === 5)) {
      throw new Error(`headless stream-json did not emit result event with usage:\n${stdout}`)
    }

    const anthropicRequest = requests.find(request => request.path === '/v1/messages')
    if (!anthropicRequest || !anthropicRequest.body.includes('run native headless stream-json e2e')) {
      throw new Error('headless stream-json did not send the stdin user message to the Anthropic API')
    }
    const ingressBodies = requests
      .filter(request => request.path === '/v1/sessions/session_1/events')
      .map(request => request.body)
    for (const expected of ['"status":"started"', '"type":"assistant"', '"type":"result"', '"status":"stopped"']) {
      if (!ingressBodies.some(body => body.includes(expected))) {
        throw new Error(`headless stream-json ingress did not include ${expected}`)
      }
    }
    console.log('ok - native headless stream-json remote query e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessStreamJsonTaskBudgetE2e() {
  const requests = []
  const assistantText = 'native headless task budget reply'
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/v1/messages') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          id: 'msg_task_budget_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text: assistantText }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 9, output_tokens: 10 },
        }))
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--task-budget=12000',
      '--session-id=session_task_budget',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
      },
      stdio: ['pipe', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    child.stdin.end(`${JSON.stringify({
      type: 'user',
      message: {
        role: 'user',
        content: 'run native task budget e2e',
      },
      parent_tool_use_id: null,
      session_id: 'session_task_budget',
    })}\n`)

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless task budget process exit'))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`headless task budget exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const anthropicRequest = requests.find(request => request.path === '/v1/messages')
    if (!anthropicRequest) {
      throw new Error('headless task budget did not call the Anthropic messages API')
    }
    const body = JSON.parse(anthropicRequest.body)
    const taskBudget = body.output_config?.task_budget
    if (!taskBudget || taskBudget.type !== 'tokens' || taskBudget.total !== 12000) {
      throw new Error(`headless task budget did not serialize output_config.task_budget:\n${anthropicRequest.body}`)
    }
    if ('remaining' in taskBudget) {
      throw new Error(`initial CLI task budget should not serialize remaining before compaction:\n${anthropicRequest.body}`)
    }
    if (!String(anthropicRequest.headers['anthropic-beta'] ?? '').includes('task-budgets-2026-03-13')) {
      throw new Error(`headless task budget did not send the task budget beta header:\n${JSON.stringify(anthropicRequest.headers)}`)
    }
    const stdout = output.join('')
    if (!stdout.includes(assistantText)) {
      throw new Error(`headless task budget did not emit assistant response:\n${stdout}`)
    }
    console.log('ok - native headless stream-json task budget e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessPersistedResumeE2e() {
  const requests = []
  const firstPrompt = 'persisted resume first turn'
  const secondPrompt = 'persisted resume second turn'
  const thirdPrompt = 'persisted continue third turn'
  const firstAssistant = 'persisted resume first assistant'
  const secondAssistant = 'persisted resume second assistant'
  const thirdAssistant = 'persisted continue third assistant'
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-persisted-resume-'))
  const sessionStore = join(root, 'conversations.json')

  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/v1/messages') {
        const text = body.includes(thirdPrompt)
          ? thirdAssistant
          : body.includes(secondPrompt)
            ? secondAssistant
            : firstAssistant
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          id: body.includes(thirdPrompt)
            ? 'msg_resume_3'
            : body.includes(secondPrompt)
              ? 'msg_resume_2'
              : 'msg_resume_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 5, output_tokens: 7 },
        }))
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const commonArgs = [
    '--headless',
    '--input-format',
    'stream-json',
    '--output-format',
    'stream-json',
    '--session-store',
    sessionStore,
  ]
  const commonEnv = {
    ...process.env,
    ANTHROPIC_API_KEY: 'fake-key',
    ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
  }
  const runNative = (label, args, input) => new Promise((resolve, reject) => {
    const child = spawn(nativeBinary, args, {
      cwd: repoRoot,
      env: commonEnv,
      stdio: ['pipe', 'pipe', 'pipe'],
    })
    const output = []
    child.stdout.on('data', chunk => output.push(String(chunk)))
    child.stderr.on('data', chunk => output.push(String(chunk)))
    child.on('error', reject)
    const timer = setTimeout(() => {
      child.kill('SIGKILL')
      reject(new Error(`${label} timed out\n${output.join('')}`))
    }, 8000)
    child.on('exit', code => {
      clearTimeout(timer)
      const text = output.join('')
      if (code !== 0) {
        reject(new Error(`${label} failed with exit code ${code}\n${text}`))
        return
      }
      resolve(text)
    })
    child.stdin.end(input)
  })

  try {
    await runNative(
      'first persisted resume run',
      [...commonArgs, '--session-id=session_resume'],
      `${JSON.stringify({
        type: 'user',
        message: { role: 'user', content: firstPrompt },
        parent_tool_use_id: null,
        session_id: 'session_resume',
      })}\n`,
    )
    if (!existsSync(sessionStore)) {
      throw new Error('persisted resume run did not create the session store')
    }
    const persisted = readFileSync(sessionStore, 'utf8')
    for (const expected of [firstPrompt, firstAssistant, 'session_resume']) {
      if (!persisted.includes(expected)) {
        throw new Error(`session store did not include ${expected}:\n${persisted}`)
      }
    }

    const secondOutput = await runNative(
      'second persisted resume run',
      [...commonArgs, '--resume=session_resume'],
      `${JSON.stringify({
        type: 'user',
        message: { role: 'user', content: secondPrompt },
        parent_tool_use_id: null,
        session_id: 'session_resume',
      })}\n`,
    )
    const secondBody = requests
      .filter(request => request.path === '/v1/messages')
      .at(-1)?.body
    if (!secondBody) {
      throw new Error('persisted resume did not send a second Anthropic request')
    }
    for (const expected of [firstPrompt, firstAssistant, secondPrompt]) {
      if (!secondBody.includes(expected)) {
        throw new Error(`resumed Anthropic request did not include ${expected}:\n${secondBody}`)
      }
    }
    if (!secondOutput.includes(secondAssistant)) {
      throw new Error(`persisted resume did not emit second assistant response:\n${secondOutput}`)
    }

    const thirdOutput = await runNative(
      'third persisted continue run',
      [...commonArgs, '--continue'],
      `${JSON.stringify({
        type: 'user',
        message: { role: 'user', content: thirdPrompt },
        parent_tool_use_id: null,
        session_id: 'session_resume',
      })}\n`,
    )
    const thirdBody = requests
      .filter(request => request.path === '/v1/messages')
      .at(-1)?.body
    if (!thirdBody) {
      throw new Error('persisted continue did not send a third Anthropic request')
    }
    for (const expected of [firstPrompt, firstAssistant, secondPrompt, secondAssistant, thirdPrompt]) {
      if (!thirdBody.includes(expected)) {
        throw new Error(`continued Anthropic request did not include ${expected}:\n${thirdBody}`)
      }
    }
    if (!thirdOutput.includes(thirdAssistant)) {
      throw new Error(`persisted continue did not emit third assistant response:\n${thirdOutput}`)
    }
    if (!thirdOutput.includes('"session_id":"session_resume"')) {
      throw new Error(`persisted continue did not reuse the active session id:\n${thirdOutput}`)
    }
    console.log('ok - native headless persisted resume context e2e')
  } finally {
    await new Promise(resolve => server.close(resolve))
    rmSync(root, { recursive: true, force: true })
  }
}

async function runHeadlessSdkUrlSseRemoteQueryE2e() {
  const requests = []
  const assistantText = 'native headless sdk-url sse reply'
  let streamConnectionCount = 0
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'GET' && req.url === '/v1/code/sessions/session_1/worker/events/stream') {
        streamConnectionCount += 1
        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          Connection: 'keep-alive',
        })
        if (streamConnectionCount === 1) {
          res.write('id: 1\n')
          res.write('event: client_event\n')
          res.write(`data: ${JSON.stringify({
            event_id: 'evt_reconnect_marker',
            sequence_num: 1,
            event_type: 'keep_alive',
            source: 'test',
            created_at: '2026-06-07T00:00:00Z',
            payload: {
              type: 'keep_alive',
              session_id: 'session_1',
            },
          })}\n\n`)
          res.end()
          return
        }
        if (req.headers['last-event-id'] !== '1') {
          res.write(`event: error\ndata: ${JSON.stringify({ error: 'missing Last-Event-ID' })}\n\n`)
          res.end()
          return
        }
        res.write('id: 2\n')
        res.write('event: client_event\n')
        res.write(`data: ${JSON.stringify({
          event_id: 'evt_1',
          sequence_num: 2,
          event_type: 'user',
          source: 'test',
          created_at: '2026-06-07T00:00:01Z',
          payload: {
            type: 'user',
            message: {
              role: 'user',
              content: 'run native headless sdk-url sse e2e',
            },
            parent_tool_use_id: null,
            session_id: 'session_1',
          },
        })}\n\n`)
        return
      }
      if (req.method === 'POST' && req.url === '/v1/messages') {
        res.writeHead(200, {
          'Content-Type': 'application/json',
        })
        res.end(JSON.stringify({
          id: 'msg_fake_sse_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text: assistantText }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 6, output_tokens: 8 },
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'PUT' && req.url === '/v1/code/sessions/session_1/worker') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/heartbeat') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/events/delivery') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/sessions/session_1/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--replay-user-messages',
      '--sdk-url',
      `http://127.0.0.1:${port}/v1/code/sessions/session_1`,
      '--session-id=session_1',
      '--task-id=work_1',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
        CLAUDE_CODE_REMOTE_API_BASE_URL: `http://127.0.0.1:${port}`,
        CC_REMOTE_SESSION_ID: 'session_1',
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
        CLAUDE_CODE_USE_CCR_V2: '1',
        CLAUDE_CODE_USE_CODE_SESSIONS: 'true',
        CLAUDE_CODE_WORKER_EPOCH: '42',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await waitFor(
      () => output.join('').includes(assistantText),
      8000,
      'headless sdk-url SSE assistant result',
    )

    child.kill('SIGTERM')
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless sdk-url SSE process exit'))
      }, 5000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`headless sdk-url SSE exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const stdout = output.join('')
    const lines = stdout.trim().split(/\r?\n/).filter(Boolean).map(line => JSON.parse(line))
    if (!lines.some(line => line.type === 'user' && line.message?.content === 'run native headless sdk-url sse e2e')) {
      throw new Error(`headless sdk-url SSE did not replay user message:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'assistant' && line.message?.content?.[0]?.text === assistantText)) {
      throw new Error(`headless sdk-url SSE did not emit assistant event:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'result' && line.result === assistantText && line.usage?.input_tokens === 6)) {
      throw new Error(`headless sdk-url SSE did not emit result event with usage:\n${stdout}`)
    }

    const streamRequests = requests.filter(request => request.path === '/v1/code/sessions/session_1/worker/events/stream')
    if (streamRequests.length < 2) {
      throw new Error('headless sdk-url SSE did not connect to worker event stream')
    }
    if (!streamRequests.every(request => request.headers.authorization === 'Bearer session-token-from-env')) {
      throw new Error('headless sdk-url SSE stream did not use bearer session token')
    }
    if (streamRequests[1].headers['last-event-id'] !== '1') {
      throw new Error(`headless sdk-url SSE reconnect did not resume with Last-Event-ID=1:\n${JSON.stringify(streamRequests.map(request => request.headers))}`)
    }
    const anthropicRequest = requests.find(request => request.path === '/v1/messages')
    if (!anthropicRequest || !anthropicRequest.body.includes('run native headless sdk-url sse e2e')) {
      throw new Error('headless sdk-url SSE did not send the remote user message to the Anthropic API')
    }
    const workerEventRequests = requests
      .filter(request => request.path === '/v1/code/sessions/session_1/worker/events')
    if (workerEventRequests.length === 0) {
      throw new Error(`headless sdk-url SSE did not write CCR v2 worker events; saw requests:\n${requests.map(request => `${request.method} ${request.path}`).join('\n')}`)
    }
    const workerEventBodies = workerEventRequests.map(request => JSON.parse(request.body))
    if (!workerEventBodies.every(body => body.worker_epoch === 42)) {
      throw new Error(`headless sdk-url SSE worker events did not include worker_epoch=42:\n${workerEventRequests.map(r => r.body).join('\n')}`)
    }
    const workerPayloads = workerEventBodies.flatMap(body => body.events?.map(event => event.payload) ?? [])
    for (const expected of ['started', 'user', 'assistant', 'result', 'stopped']) {
      if (!workerPayloads.some(payload => payload?.type === expected || payload?.status === expected)) {
        throw new Error(`headless sdk-url SSE worker events did not include ${expected}`)
      }
    }
    const workerStateBodies = requests
      .filter(request => request.method === 'PUT' && request.path === '/v1/code/sessions/session_1/worker')
      .map(request => JSON.parse(request.body))
    if (!workerStateBodies.some(body => body.worker_epoch === 42 && body.worker_status === 'idle' && body.external_metadata?.pending_action === null)) {
      throw new Error(`headless sdk-url SSE did not initialize worker idle state:\n${workerStateBodies.map(body => JSON.stringify(body)).join('\n')}`)
    }
    if (!workerStateBodies.some(body => body.worker_epoch === 42 && body.worker_status === 'running')) {
      throw new Error(`headless sdk-url SSE did not report running worker state:\n${workerStateBodies.map(body => JSON.stringify(body)).join('\n')}`)
    }
    const heartbeatBodies = requests
      .filter(request => request.method === 'POST' && request.path === '/v1/code/sessions/session_1/worker/heartbeat')
      .map(request => JSON.parse(request.body))
    if (!heartbeatBodies.some(body => body.worker_epoch === 42 && body.session_id === 'session_1')) {
      throw new Error(`headless sdk-url SSE did not send worker heartbeat:\n${heartbeatBodies.map(body => JSON.stringify(body)).join('\n')}`)
    }
    const deliveryUpdates = requests
      .filter(request => request.method === 'POST' && request.path === '/v1/code/sessions/session_1/worker/events/delivery')
      .flatMap(request => JSON.parse(request.body).updates ?? [])
    if (!deliveryUpdates.some(update => update.event_id === 'evt_1' && update.status === 'received')) {
      throw new Error(`headless sdk-url SSE did not ACK received delivery:\n${JSON.stringify(deliveryUpdates)}`)
    }
    if (!deliveryUpdates.some(update => update.event_id === 'evt_1' && update.status === 'processed')) {
      throw new Error(`headless sdk-url SSE did not ACK processed delivery:\n${JSON.stringify(deliveryUpdates)}`)
    }
    console.log('ok - native headless sdk-url SSE remote query e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessSdkUrlSseAuthFailureE2e() {
  const requests = []
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'GET' && req.url === '/v1/code/sessions/session_1/worker/events/stream') {
        res.writeHead(401, { 'Content-Type': 'application/json' })
        res.end('{"error":"unauthorized"}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--sdk-url',
      `http://127.0.0.1:${port}/v1/code/sessions/session_1`,
      '--session-id=session_1',
      '--task-id=work_1',
      '--debug',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless sdk-url SSE auth failure process exit'))
      }, 5000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code === 0) {
          reject(new Error(`headless sdk-url SSE auth failure exited successfully\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const streamRequest = requests.find(request => request.path === '/v1/code/sessions/session_1/worker/events/stream')
    if (!streamRequest) {
      throw new Error('headless sdk-url SSE auth failure did not open the worker event stream')
    }
    if (streamRequest.headers.authorization !== 'Bearer session-token-from-env') {
      throw new Error('headless sdk-url SSE auth failure did not use bearer session token')
    }
    const text = output.join('')
    if (!text.includes('authentication failed') || !text.includes('401')) {
      throw new Error(`headless sdk-url SSE auth failure did not report a 401 authentication error:\n${text}`)
    }
    console.log('ok - native headless sdk-url SSE auth failure e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessSdkUrlWebSocketRemoteQueryE2e() {
  const requests = []
  const upgradeRequests = []
  const wsSockets = new Set()
  const assistantText = 'native headless sdk-url websocket reply'
  let wsConnectionCount = 0
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/v1/messages') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          id: 'msg_fake_ws_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text: assistantText }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 7, output_tokens: 9 },
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/v1/sessions/session_1/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  server.on('upgrade', (req, socket) => {
    socket.on('error', () => {})
    upgradeRequests.push({
      path: req.url,
      headers: req.headers,
    })
    const key = req.headers['sec-websocket-key']
    if (!key || req.headers.authorization !== 'Bearer session-token-from-env') {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n')
      socket.destroy()
      return
    }

    wsSockets.add(socket)
    socket.on('close', () => wsSockets.delete(socket))
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Connection: Upgrade\r\n' +
      'Upgrade: websocket\r\n' +
      `Sec-WebSocket-Accept: ${webSocketAcceptKey(key)}\r\n\r\n`,
    )
    wsConnectionCount += 1
    if (wsConnectionCount === 1) {
      setTimeout(() => {
        socket.destroy()
      }, 25)
      return
    }
    setTimeout(() => {
      if (socket.destroyed) return
      socket.write(serverWebSocketTextFrame(JSON.stringify({
        type: 'user',
        message: {
          role: 'user',
          content: 'run native headless sdk-url websocket e2e',
        },
        parent_tool_use_id: null,
        session_id: 'session_1',
      })))
    }, 50)
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--replay-user-messages',
      '--sdk-url',
      `ws://127.0.0.1:${port}/v2/session_ingress/ws/session_1`,
      '--session-id=session_1',
      '--task-id=work_1',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
        CLAUDE_CODE_REMOTE_API_BASE_URL: `http://127.0.0.1:${port}`,
        CC_REMOTE_SESSION_ID: 'session_1',
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await waitFor(
      () => output.join('').includes(assistantText),
      8000,
      'headless sdk-url WebSocket assistant result',
    )

    child.kill('SIGTERM')
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless sdk-url WebSocket process exit'))
      }, 5000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`headless sdk-url WebSocket exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const stdout = output.join('')
    const lines = stdout.trim().split(/\r?\n/).filter(Boolean).map(line => JSON.parse(line))
    if (!lines.some(line => line.type === 'user' && line.message?.content === 'run native headless sdk-url websocket e2e')) {
      throw new Error(`headless sdk-url WebSocket did not replay user message:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'assistant' && line.message?.content?.[0]?.text === assistantText)) {
      throw new Error(`headless sdk-url WebSocket did not emit assistant event:\n${stdout}`)
    }
    if (!lines.some(line => line.type === 'result' && line.result === assistantText && line.usage?.input_tokens === 7)) {
      throw new Error(`headless sdk-url WebSocket did not emit result event with usage:\n${stdout}`)
    }

    const sessionIngressUpgrades = upgradeRequests.filter(request => request.path === '/v2/session_ingress/ws/session_1')
    if (sessionIngressUpgrades.length < 2) {
      throw new Error(`headless sdk-url WebSocket did not reconnect to session ingress:\n${JSON.stringify(upgradeRequests)}`)
    }
    if (!sessionIngressUpgrades.every(request => request.headers.authorization === 'Bearer session-token-from-env')) {
      throw new Error('headless sdk-url WebSocket did not use bearer session token')
    }
    const anthropicRequest = requests.find(request => request.path === '/v1/messages')
    if (!anthropicRequest || !anthropicRequest.body.includes('run native headless sdk-url websocket e2e')) {
      throw new Error('headless sdk-url WebSocket did not send the remote user message to the Anthropic API')
    }
    const ingressBodies = requests
      .filter(request => request.path === '/v1/sessions/session_1/events')
      .map(request => request.body)
    for (const expected of ['"status":"started"', '"type":"user"', '"type":"assistant"', '"type":"result"', '"status":"stopped"']) {
      if (!ingressBodies.some(body => body.includes(expected))) {
        throw new Error(`headless sdk-url WebSocket ingress did not include ${expected}`)
      }
    }
    if (requests.some(request => request.path === '/v1/code/sessions/session_1/worker/events')) {
      throw new Error('headless sdk-url WebSocket incorrectly wrote CCR v2 worker events')
    }
    console.log('ok - native headless sdk-url WebSocket remote query e2e')
  } finally {
    child.kill('SIGKILL')
    for (const socket of wsSockets) socket.destroy()
    await new Promise(resolve => server.close(resolve))
  }
}

async function runHeadlessSdkUrlWebSocketAuthFailureE2e() {
  const upgradeRequests = []
  const server = createServer((req, res) => {
    res.writeHead(404, { 'Content-Type': 'application/json' })
    res.end('{"error":"not found"}')
  })

  server.on('upgrade', (req, socket) => {
    socket.on('error', () => {})
    upgradeRequests.push({
      path: req.url,
      headers: req.headers,
    })
    socket.write('HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n\r\n{"error":"unauthorized"}')
    socket.destroy()
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--headless',
      '--input-format',
      'stream-json',
      '--output-format',
      'stream-json',
      '--sdk-url',
      `ws://127.0.0.1:${port}/v2/session_ingress/ws/session_1`,
      '--session-id=session_1',
      '--task-id=work_1',
      '--debug',
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
        CLAUDE_CODE_SESSION_ACCESS_TOKEN: 'session-token-from-env',
        CLAUDE_CODE_BRIDGE_WORK_ID: 'work_1',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for headless sdk-url WebSocket auth failure process exit'))
      }, 5000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code === 0) {
          reject(new Error(`headless sdk-url WebSocket auth failure exited successfully\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const upgradeRequest = upgradeRequests.find(request => request.path === '/v2/session_ingress/ws/session_1')
    if (!upgradeRequest) {
      throw new Error('headless sdk-url WebSocket auth failure did not open the session ingress socket')
    }
    if (upgradeRequest.headers.authorization !== 'Bearer session-token-from-env') {
      throw new Error('headless sdk-url WebSocket auth failure did not use bearer session token')
    }
    const text = output.join('')
    if (!text.includes('authentication failed') || !text.includes('401')) {
      throw new Error(`headless sdk-url WebSocket auth failure did not report a 401 authentication error:\n${text}`)
    }
    console.log('ok - native headless sdk-url WebSocket auth failure e2e')
  } finally {
    child.kill('SIGKILL')
    await new Promise(resolve => server.close(resolve))
  }
}

async function runBridgeDaemonProductLifecycleE2e({
  label,
  daemonFlag,
  extraArgs = [],
  expectedOutput = [],
  signalAfterAnthropicMessage = false,
  workAvailableAfterPolls = 1,
}) {
  const requests = []
  const assistantText = 'native bridge daemon product reply'
  let pollCount = 0
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      const { port } = server.address()
      const baseUrl = `http://127.0.0.1:${port}`
      const workSecret = base64UrlEncode(JSON.stringify({
        version: 1,
        session_ingress_token: 'session-token-from-secret',
        api_base_url: baseUrl,
        use_code_sessions: true,
        code_session_mode: 'code-session',
        sources: [{ type: 'github', id: 'source-1' }],
        auth: { records: [{ type: 'oauth', name: 'console' }] },
        mcp_config: { servers: { linear: { transport: 'http', url: 'https://mcp.example' } } },
        environment_variables: { REMOTE_FLAG: 'enabled', REMOTE_COUNT: 42 },
      }))

      if (req.method === 'GET' && req.url === '/v1/environments/env_backend_1/work/poll') {
        pollCount += 1
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(pollCount === workAvailableAfterPolls
          ? JSON.stringify({
              id: 'work_1',
              data: { type: 'session', id: 'session_1' },
              secret: workSecret,
            })
          : 'null')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/environments/env_backend_1/work/work_1/ack') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/environments/env_backend_1/work/work_1/heartbeat') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          lease_extended: true,
          state: 'active',
          last_heartbeat: '2026-06-07T00:00:00Z',
          ttl_seconds: 30,
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/register') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"worker_epoch":42}')
        return
      }
      if (req.method === 'GET' && req.url === '/v1/code/sessions/session_1/worker/events/stream') {
        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          Connection: 'keep-alive',
        })
        res.write('id: 1\n')
        res.write('event: client_event\n')
        res.write(`data: ${JSON.stringify({
          event_id: 'evt_daemon_1',
          sequence_num: 1,
          event_type: 'user',
          source: 'daemon-product-fixture',
          created_at: '2026-06-07T00:00:00Z',
          payload: {
            type: 'user',
            message: {
              role: 'user',
              content: 'run native bridge daemon product e2e',
            },
            parent_tool_use_id: null,
            session_id: 'session_1',
          },
        })}\n\n`)
        return
      }
      if (req.method === 'POST' && req.url === '/v1/messages') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          id: 'msg_daemon_product_1',
          type: 'message',
          role: 'assistant',
          model: 'claude-test',
          content: [{ type: 'text', text: assistantText }],
          stop_reason: 'end_turn',
          usage: { input_tokens: 11, output_tokens: 13 },
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/events') {
        res.writeHead(201, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'PUT' && req.url === '/v1/code/sessions/session_1/worker') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/heartbeat') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/code/sessions/session_1/worker/events/delivery') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/environments/env_backend_1/work/work_1/stop') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      if (req.method === 'POST' && req.url === '/v1/sessions/session_1/archive') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end('{"ok":true}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-bridge-daemon-e2e-'))
  let child
  try {
    const output = []
    child = spawn(nativeBinary, [
      daemonFlag,
      '--bridge-work-api-url',
      `http://127.0.0.1:${port}`,
      '--bridge-environment-id',
      'env_backend_1',
      '--bridge-environment-secret',
      'env_secret_1',
      '--bridge-access-token',
      'oauth_token',
      '--bridge-session-binary',
      nativeBinary,
      ...extraArgs,
    ], {
      cwd: repoRoot,
      env: {
        ...process.env,
        HOME: root,
        XDG_CONFIG_HOME: root,
        ANTHROPIC_API_KEY: 'fake-key',
        ANTHROPIC_BASE_URL: `http://127.0.0.1:${port}`,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    })
    child.stdout.on('data', chunk => output.push(String(chunk)))
    child.stderr.on('data', chunk => output.push(String(chunk)))
    if (signalAfterAnthropicMessage) {
      await waitFor(
        () => requests.some(request => request.method === 'POST' && request.path === '/v1/messages'),
        8000,
        `${label} Anthropic request`,
      )
      await waitFor(
        () => output.join('').includes('Bridge daemon spawned session'),
        5000,
        `${label} spawned session output`,
      )
      child.kill('SIGTERM')
    }
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error(`timed out waiting for ${label}\n${output.join('')}`))
      }, 15000)
      child.once('exit', code => {
        clearTimeout(timer)
        const text = output.join('')
        if (code !== 0) {
          reject(new Error(`${label} exited with code ${code}\n${text}`))
          return
        }
        resolve()
      })
    })
    child = undefined
    const outputText = output.join('')
    for (const expected of ['Bridge daemon spawned session', ...expectedOutput]) {
      if (!outputText.includes(expected)) {
        throw new Error(`${label} did not report ${expected}:\n${outputText}`)
      }
    }

    const saw = (method, path) => requests.some(request => request.method === method && request.path === path)
    for (const [method, path] of [
      ['GET', '/v1/environments/env_backend_1/work/poll'],
      ['POST', '/v1/environments/env_backend_1/work/work_1/ack'],
      ['POST', '/v1/environments/env_backend_1/work/work_1/heartbeat'],
      ['POST', '/v1/code/sessions/session_1/worker/register'],
      ['GET', '/v1/code/sessions/session_1/worker/events/stream'],
      ['POST', '/v1/code/sessions/session_1/worker/events'],
      ['PUT', '/v1/code/sessions/session_1/worker'],
      ['POST', '/v1/code/sessions/session_1/worker/heartbeat'],
      ['POST', '/v1/code/sessions/session_1/worker/events/delivery'],
      ['POST', '/v1/messages'],
      ['POST', '/v1/environments/env_backend_1/work/work_1/stop'],
      ['POST', '/v1/sessions/session_1/archive'],
    ]) {
      if (!saw(method, path)) {
        throw new Error(`${label} missed ${method} ${path}; saw:\n${requests.map(request => `${request.method} ${request.path}`).join('\n')}`)
      }
    }
    const requestFor = (method, path) => requests.find(request => request.method === method && request.path === path)
    if (requestFor('GET', '/v1/environments/env_backend_1/work/poll')?.headers.authorization !== 'Bearer env_secret_1') {
      throw new Error('native bridge daemon poll did not use environment secret')
    }
    if (requestFor('POST', '/v1/environments/env_backend_1/work/work_1/ack')?.headers.authorization !== 'Bearer session-token-from-secret') {
      throw new Error('native bridge daemon ack did not use session token from work secret')
    }
    if (requestFor('POST', '/v1/environments/env_backend_1/work/work_1/heartbeat')?.headers.authorization !== 'Bearer session-token-from-secret') {
      throw new Error('native bridge daemon heartbeat did not use session token from work secret')
    }
    if (requestFor('POST', '/v1/environments/env_backend_1/work/work_1/stop')?.headers.authorization !== 'Bearer oauth_token') {
      throw new Error('native bridge daemon stop did not use bridge OAuth token')
    }
    if (requestFor('POST', '/v1/sessions/session_1/archive')?.headers.authorization !== 'Bearer oauth_token') {
      throw new Error('native bridge daemon archive did not use bridge OAuth token')
    }
    if (!requestFor('POST', '/v1/messages')?.body.includes('run native bridge daemon product e2e')) {
      throw new Error('native bridge daemon child did not send remote user event to Anthropic')
    }
    const deliveryUpdates = requests
      .filter(request => request.method === 'POST' && request.path === '/v1/code/sessions/session_1/worker/events/delivery')
      .flatMap(request => JSON.parse(request.body).updates ?? [])
    if (!deliveryUpdates.some(update => update.event_id === 'evt_daemon_1' && update.status === 'received') ||
        !deliveryUpdates.some(update => update.event_id === 'evt_daemon_1' && update.status === 'processed')) {
      throw new Error(`native bridge daemon child did not ACK delivery lifecycle:\n${JSON.stringify(deliveryUpdates)}`)
    }
    console.log(`ok - ${label}`)
  } finally {
    child?.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

async function runBridgeDaemonOneShotProductE2e() {
  await runBridgeDaemonProductLifecycleE2e({
    label: 'native bridge daemon one-shot lifecycle e2e',
    daemonFlag: '--bridge-daemon-once',
    extraArgs: ['--bridge-daemon-wait-ms', '10000'],
    expectedOutput: ['completed with status completed'],
  })
}

async function runBridgeDaemonLongRunningProductE2e() {
  await runBridgeDaemonProductLifecycleE2e({
    label: 'native bridge daemon long-running lifecycle e2e',
    daemonFlag: '--bridge-daemon',
    expectedOutput: [
      'CC-REPL bridge daemon listening',
      'CC-REPL bridge daemon stopped',
    ],
    signalAfterAnthropicMessage: true,
  })
}

async function runBridgeDaemonDelayedWorkPollE2e() {
  await runBridgeDaemonProductLifecycleE2e({
    label: 'native bridge daemon delayed work poll e2e',
    daemonFlag: '--bridge-daemon',
    expectedOutput: [
      'CC-REPL bridge daemon listening',
      'CC-REPL bridge daemon stopped',
    ],
    signalAfterAnthropicMessage: true,
    workAvailableAfterPolls: 2,
  })
}

async function runBridgeDaemonAuthFailureBackoffE2e() {
  const requests = []
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'GET' && req.url === '/v1/environments/env_backend_1/work/poll') {
        res.writeHead(401, { 'Content-Type': 'application/json' })
        res.end('{"error":"unauthorized"}')
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-bridge-daemon-auth-e2e-'))
  const output = []
  const child = spawn(nativeBinary, [
    '--bridge-daemon',
    '--bridge-work-api-url',
    `http://127.0.0.1:${port}`,
    '--bridge-environment-id',
    'env_backend_1',
    '--bridge-environment-secret',
    'env_secret_1',
    '--bridge-access-token',
    'oauth_token',
    '--bridge-session-binary',
    nativeBinary,
  ], {
    cwd: repoRoot,
    env: {
      ...process.env,
      HOME: root,
      XDG_CONFIG_HOME: root,
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await waitFor(
      () => requests.filter(request => request.method === 'GET' && request.path === '/v1/environments/env_backend_1/work/poll').length >= 2,
      6000,
      'bridge daemon auth failure backoff poll retry',
    )
    child.kill('SIGTERM')
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error(`timed out waiting for native bridge daemon auth failure shutdown\n${output.join('')}`))
      }, 5000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`native bridge daemon auth failure exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })
    const text = output.join('')
    if (!text.includes('Bridge daemon poll failed') || !text.includes('HTTP 401')) {
      throw new Error(`native bridge daemon auth failure did not report HTTP 401:\n${text}`)
    }
    const pollRequests = requests.filter(request => request.method === 'GET' && request.path === '/v1/environments/env_backend_1/work/poll')
    for (const request of pollRequests) {
      if (request.headers.authorization !== 'Bearer env_secret_1') {
        throw new Error('native bridge daemon auth failure poll did not use environment secret')
      }
    }
    console.log('ok - native bridge daemon auth failure/backoff e2e')
  } finally {
    child.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

function runNativeMcpAuthRemoteOAuthConfigE2e() {
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-native-mcp-auth-'))
  mkdirSync(join(root, '.claude'), { recursive: true })
  writeFileSync(join(root, '.claude', 'config.json'), JSON.stringify({
    mcpServers: {
      remote_fixture: {
        type: 'http',
        url: 'https://mcp.example.test/mcp',
        oauth: {
          xaa: true,
        },
      },
    },
  }), 'utf8')

  try {
    const output = run(
      'native mcp_auth remote OAuth config',
      nativeBinary,
      [
        '--run-runtime-tool',
        'mcp_auth',
        '--runtime-tool-input',
        JSON.stringify({ server_name: 'remote_fixture' }),
      ],
      'Failed to start OAuth flow',
      {
        env: {
          ...process.env,
          HOME: root,
          CLAUDE_CODE_ENABLE_XAA: '0',
        },
        cwd: root,
      },
    )
    if (!output.includes('remote_fixture') || !output.includes('XAA is not enabled')) {
      throw new Error(`native mcp_auth did not load remote OAuth config and enter native auth flow:\n${output}`)
    }
    console.log('ok - native mcp_auth remote OAuth config e2e')
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
}

async function runNativeMcpAuthStandardOAuthUrlE2e() {
  const requests = []
  const server = createServer((req, res) => {
    requests.push({
      method: req.method,
      path: req.url,
      headers: req.headers,
    })
    if (req.method === 'GET' && req.url === '/.well-known/oauth-authorization-server') {
      const { port } = server.address()
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify({
        authorization_endpoint: `http://127.0.0.1:${port}/authorize`,
        token_endpoint: `http://127.0.0.1:${port}/token`,
        scope: 'tools',
      }))
      return
    }
    res.writeHead(404, { 'Content-Type': 'application/json' })
    res.end('{"error":"not found"}')
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-native-mcp-oauth-url-'))
  mkdirSync(join(root, '.claude'), { recursive: true })
  writeFileSync(join(root, '.claude', 'config.json'), JSON.stringify({
    mcpServers: {
      oauth_fixture: {
        type: 'http',
        url: `http://127.0.0.1:${port}/mcp`,
        oauth: {
          authServerMetadataUrl: `http://127.0.0.1:${port}/.well-known/oauth-authorization-server`,
          clientId: 'client-1',
        },
      },
    },
  }), 'utf8')

  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--run-runtime-tool',
      'mcp_auth',
      '--runtime-tool-input',
      JSON.stringify({ server_name: 'oauth_fixture' }),
    ],
    {
      cwd: root,
      env: {
        ...process.env,
        HOME: root,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for native mcp_auth standard OAuth URL e2e'))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`native mcp_auth standard OAuth URL exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const text = output.join('')
    if (!text.includes("Ask the user to open this URL") ||
        !text.includes(`http://127.0.0.1:${port}/authorize`) ||
        !text.includes('response_type=code') ||
        !text.includes('client_id=client-1') ||
        !text.includes('code_challenge_method=S256') ||
        !text.includes('scope=tools')) {
      throw new Error(`native mcp_auth did not emit a complete OAuth authorization URL:\n${text}`)
    }
    if (!requests.some(request => request.path === '/.well-known/oauth-authorization-server')) {
      throw new Error('native mcp_auth did not fetch OAuth server metadata')
    }
    console.log('ok - native mcp_auth standard OAuth authorization URL e2e')
  } finally {
    child.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

async function runNativeMcpAuthBrowserCallbackE2e() {
  const requests = []
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'GET' && req.url === '/.well-known/oauth-authorization-server') {
        const { port } = server.address()
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          authorization_endpoint: `http://127.0.0.1:${port}/authorize`,
          token_endpoint: `http://127.0.0.1:${port}/token`,
          revocation_endpoint: `http://127.0.0.1:${port}/revoke`,
          scope: 'tools',
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/token') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          access_token: 'callback-access-cli',
          refresh_token: 'callback-refresh-cli',
          expires_in: 3600,
          scope: 'tools',
        }))
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-native-mcp-oauth-callback-'))
  const urlFile = join(root, 'auth-url.txt')
  mkdirSync(join(root, '.claude'), { recursive: true })
  writeFileSync(join(root, '.claude', 'config.json'), JSON.stringify({
    mcpServers: {
      callback_fixture: {
        type: 'http',
        url: `http://127.0.0.1:${port}/mcp`,
        oauth: {
          authServerMetadataUrl: `http://127.0.0.1:${port}/.well-known/oauth-authorization-server`,
          clientId: 'client-1',
        },
      },
    },
  }), 'utf8')

  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--run-runtime-tool',
      'mcp_auth',
      '--runtime-tool-input',
      JSON.stringify({
        server_name: 'callback_fixture',
        wait_for_callback: true,
        authorization_url_file: urlFile,
      }),
    ],
    {
      cwd: root,
      env: {
        ...process.env,
        HOME: root,
        XDG_CONFIG_HOME: root,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await waitFor(
      () => existsSync(urlFile) && readFileSync(urlFile, 'utf8').includes('/authorize'),
      8000,
      'native mcp_auth browser callback authorization URL file',
    )
    const authUrl = readFileSync(urlFile, 'utf8')
    const parsedAuthUrl = new URL(authUrl)
    const redirectUri = parsedAuthUrl.searchParams.get('redirect_uri')
    const state = parsedAuthUrl.searchParams.get('state')
    if (!redirectUri || !state || parsedAuthUrl.searchParams.get('client_id') !== 'client-1') {
      throw new Error(`native mcp_auth browser callback emitted invalid URL:\n${authUrl}`)
    }
    const callbackUrl = new URL(redirectUri)
    callbackUrl.searchParams.set('code', 'callback-code-cli')
    callbackUrl.searchParams.set('state', state)
    const callbackResponse = await fetch(callbackUrl)
    if (!callbackResponse.ok) {
      throw new Error(`native mcp_auth browser callback returned HTTP ${callbackResponse.status}`)
    }

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error(`timed out waiting for native mcp_auth browser callback e2e\n${output.join('')}`))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`native mcp_auth browser callback exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const text = output.join('')
    if (!text.includes("Authentication completed with browser callback for 'callback_fixture'.")) {
      throw new Error(`native mcp_auth browser callback did not complete:\n${text}`)
    }
    const tokenRequest = requests.find(request => request.method === 'POST' && request.path === '/token')
    if (!tokenRequest ||
        !tokenRequest.body.includes('grant_type=authorization_code') ||
        !tokenRequest.body.includes('code=callback-code-cli') ||
        !tokenRequest.body.includes('client_id=client-1') ||
        !tokenRequest.body.includes('code_verifier=')) {
      throw new Error(`native mcp_auth browser callback sent invalid token request:\n${tokenRequest?.body}`)
    }

    const tokenDir = join(root, 'cc-repl', 'mcp')
    const tokenFiles = readdirSync(tokenDir).filter(name => name.endsWith('.json'))
    if (tokenFiles.length !== 1) {
      throw new Error(`native mcp_auth browser callback wrote ${tokenFiles.length} token files`)
    }
    const token = JSON.parse(readFileSync(join(tokenDir, tokenFiles[0]), 'utf8'))
    if (token.server_name !== 'callback_fixture' ||
        token.server_url !== `http://127.0.0.1:${port}/mcp` ||
        token.access_token !== 'callback-access-cli' ||
        token.refresh_token !== 'callback-refresh-cli' ||
        token.scope !== 'tools' ||
        token.client_id !== 'client-1') {
      throw new Error(`native mcp_auth browser callback persisted an invalid token: ${JSON.stringify(token)}`)
    }
    console.log('ok - native mcp_auth browser callback e2e')
  } finally {
    child.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

async function runNativeMcpAuthRefreshRemoteToolE2e() {
  const requests = []
  const parseRequestBody = body => {
    try {
      return JSON.parse(body)
    } catch {
      return null
    }
  }
  const jsonRpcId = request => {
    if (request?.id === undefined || request?.id === null) return 'null'
    return JSON.stringify(request.id)
  }
  const sendJson = (res, payload, status = 200) => {
    const body = typeof payload === 'string' ? payload : JSON.stringify(payload)
    res.writeHead(status, {
      'Content-Type': 'application/json',
      'Content-Length': String(Buffer.byteLength(body)),
    })
    res.end(body)
  }
  const sendEmpty = (res, status) => {
    res.writeHead(status, {
      'Content-Type': 'text/plain',
      'Content-Length': '0',
    })
    res.end('')
  }
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      const record = {
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      }
      requests.push(record)
      if (req.method === 'GET' && req.url === '/.well-known/oauth-authorization-server') {
        const { port } = server.address()
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          authorization_endpoint: `http://127.0.0.1:${port}/authorize`,
          token_endpoint: `http://127.0.0.1:${port}/token`,
          revocation_endpoint: `http://127.0.0.1:${port}/revoke`,
          scope: 'tools',
        }))
        return
      }
      if (req.method === 'POST' && req.url === '/token') {
        const params = new URLSearchParams(body)
        if (params.get('grant_type') === 'authorization_code') {
          res.writeHead(200, { 'Content-Type': 'application/json' })
          res.end(JSON.stringify({
            access_token: 'callback-access-cli',
            refresh_token: 'callback-refresh-cli',
            expires_in: 3600,
            scope: 'tools',
          }))
          return
        }
        if (params.get('grant_type') === 'refresh_token' &&
            params.get('refresh_token') === 'old-refresh-cli' &&
            params.get('client_id') === 'client-1') {
          res.writeHead(200, { 'Content-Type': 'application/json' })
          res.end(JSON.stringify({
            access_token: 'fresh-access-cli',
            refresh_token: 'fresh-refresh-cli',
            expires_in: 3600,
            scope: 'tools',
          }))
          return
        }
        res.writeHead(400, { 'Content-Type': 'application/json' })
        res.end('{"error":"invalid_request"}')
        return
      }
      if (req.method === 'POST' && req.url === '/mcp') {
        const authorization = req.headers.authorization
        if (authorization !== 'Bearer fresh-access-cli') {
          sendEmpty(res, 401)
          return
        }

        const rpc = parseRequestBody(body)
        if (!rpc) {
          sendEmpty(res, 400)
          return
        }
        if (rpc.method === 'notifications/initialized' || rpc.method === 'notifications/cancelled') {
          sendEmpty(res, 202)
          return
        }
        if (rpc.method === 'initialize') {
          sendJson(res, `{"jsonrpc":"2.0","id":${jsonRpcId(rpc)},"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"refresh-fixture","version":"1.0.0"}}}`)
          return
        }
        if (rpc.method === 'tools/list') {
          sendJson(res, `{"jsonrpc":"2.0","id":${jsonRpcId(rpc)},"result":{"tools":[{"name":"refresh_lookup","description":"Lookup after OAuth refresh","inputSchema":{"type":"object"}}]}}`)
          return
        }
        if (rpc.method === 'tools/call') {
          const query = rpc.params?.arguments?.query ?? 'missing'
          sendJson(res, {
            jsonrpc: '2.0',
            id: rpc.id,
            result: {
              content: [{
                type: 'text',
                text: `refresh:${query}:${authorization}`,
              }],
            },
          })
          return
        }

        sendEmpty(res, 404)
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-native-mcp-refresh-'))
  const urlFile = join(root, 'auth-url.txt')
  mkdirSync(join(root, '.claude'), { recursive: true })
  const mcpConfig = {
    mcpServers: {
      refresh_fixture: {
        type: 'http',
        url: `http://127.0.0.1:${port}/mcp`,
        oauth: {
          authServerMetadataUrl: `http://127.0.0.1:${port}/.well-known/oauth-authorization-server`,
          clientId: 'client-1',
        },
      },
    },
  }
  writeFileSync(join(root, '.claude', 'config.json'), JSON.stringify(mcpConfig), 'utf8')
  writeFileSync(join(root, '.claude', 'mcp_servers.json'), JSON.stringify(mcpConfig), 'utf8')

  const authOutput = []
  const child = spawn(
    nativeBinary,
    [
      '--run-runtime-tool',
      'mcp_auth',
      '--runtime-tool-input',
      JSON.stringify({
        server_name: 'refresh_fixture',
        wait_for_callback: true,
        authorization_url_file: urlFile,
      }),
    ],
    {
      cwd: root,
      env: {
        ...process.env,
        HOME: root,
        XDG_CONFIG_HOME: root,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => authOutput.push(String(chunk)))
  child.stderr.on('data', chunk => authOutput.push(String(chunk)))

  try {
    await waitFor(
      () => existsSync(urlFile) && readFileSync(urlFile, 'utf8').includes('/authorize'),
      8000,
      'native mcp_auth refresh fixture authorization URL file',
    )
    const authUrl = readFileSync(urlFile, 'utf8')
    const parsedAuthUrl = new URL(authUrl)
    const redirectUri = parsedAuthUrl.searchParams.get('redirect_uri')
    const state = parsedAuthUrl.searchParams.get('state')
    if (!redirectUri || !state || parsedAuthUrl.searchParams.get('client_id') !== 'client-1') {
      throw new Error(`native mcp_auth refresh fixture emitted invalid URL:\n${authUrl}`)
    }
    const callbackUrl = new URL(redirectUri)
    callbackUrl.searchParams.set('code', 'refresh-bootstrap-code-cli')
    callbackUrl.searchParams.set('state', state)
    const callbackResponse = await fetch(callbackUrl)
    if (!callbackResponse.ok) {
      throw new Error(`native mcp_auth refresh fixture callback returned HTTP ${callbackResponse.status}`)
    }

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error(`timed out waiting for native mcp_auth refresh fixture callback\n${authOutput.join('')}`))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`native mcp_auth refresh fixture exited with code ${code}\n${authOutput.join('')}`))
          return
        }
        resolve()
      })
    })

    if (!authOutput.join('').includes("Authentication completed with browser callback for 'refresh_fixture'.")) {
      throw new Error(`native mcp_auth refresh fixture did not complete:\n${authOutput.join('')}`)
    }

    const tokenDir = join(root, 'cc-repl', 'mcp')
    const tokenFiles = readdirSync(tokenDir).filter(name => name.endsWith('.json'))
    if (tokenFiles.length !== 1) {
      throw new Error(`native mcp_auth refresh fixture wrote ${tokenFiles.length} token files`)
    }
    const tokenPath = join(tokenDir, tokenFiles[0])
    const expiredToken = JSON.parse(readFileSync(tokenPath, 'utf8'))
    expiredToken.access_token = 'old-access-cli'
    expiredToken.refresh_token = 'old-refresh-cli'
    expiredToken.expires_at = 1
    writeFileSync(tokenPath, JSON.stringify(expiredToken, null, 2), 'utf8')

    const requestOffset = requests.length
    const runtimeArgs = [
      '--run-runtime-tool',
      'mcp',
      '--runtime-tool-input',
      JSON.stringify({
        server_name: 'refresh_fixture',
        tool_name: 'refresh_lookup',
        arguments: { query: 'after-refresh' },
      }),
    ]
    const runtimeOutput = []
    const runtimeChild = spawn(nativeBinary, runtimeArgs, {
      cwd: root,
      env: {
        ...process.env,
        HOME: root,
        XDG_CONFIG_HOME: root,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    })
    runtimeChild.stdout.on('data', chunk => runtimeOutput.push(String(chunk)))
    runtimeChild.stderr.on('data', chunk => runtimeOutput.push(String(chunk)))

    const runtimeCode = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        runtimeChild.kill('SIGKILL')
        reject(new Error(`timed out waiting for native MCP OAuth refresh remote tool e2e\n${runtimeOutput.join('')}\nRuntime requests: ${JSON.stringify(requests.slice(requestOffset), null, 2)}`))
      }, 10000)
      runtimeChild.once('exit', code => {
        clearTimeout(timer)
        resolve(code)
      })
      runtimeChild.once('error', error => {
        clearTimeout(timer)
        reject(error)
      })
    })
    const runtimeText = runtimeOutput.join('')
    const runtimeRequests = requests.slice(requestOffset)
    if (runtimeCode !== 0 ||
        !runtimeText.includes('refresh:after-refresh:Bearer fresh-access-cli')) {
      process.stderr.write(runtimeText)
      throw new Error(`native MCP OAuth refresh remote tool e2e failed with exit code ${runtimeCode}\nToken files: ${JSON.stringify(tokenFiles)}\nExpired token: ${readFileSync(tokenPath, 'utf8')}\nRuntime requests: ${JSON.stringify(runtimeRequests, null, 2)}`)
    }

    const refreshRequest = runtimeRequests.find(request =>
      request.method === 'POST' &&
      request.path === '/token' &&
      request.body.includes('grant_type=refresh_token') &&
      request.body.includes('refresh_token=old-refresh-cli') &&
      request.body.includes('client_id=client-1'))
    if (!refreshRequest) {
      throw new Error(`native runtime MCP call did not refresh the expired OAuth token:\n${JSON.stringify(runtimeRequests, null, 2)}`)
    }
    const runtimeMcpRequests = runtimeRequests.filter(request => request.method === 'POST' && request.path === '/mcp')
    if (runtimeMcpRequests.length < 3 ||
        !runtimeMcpRequests.every(request => request.headers.authorization === 'Bearer fresh-access-cli')) {
      throw new Error(`native runtime MCP call did not use the refreshed bearer for all remote requests:\n${JSON.stringify(runtimeMcpRequests, null, 2)}`)
    }
    const runtimeMethods = new Set(runtimeMcpRequests.map(request => parseRequestBody(request.body)?.method))
    for (const method of ['initialize', 'tools/list', 'tools/call']) {
      if (!runtimeMethods.has(method)) {
        throw new Error(`native runtime MCP refresh path did not send ${method}`)
      }
    }

    const refreshedToken = JSON.parse(readFileSync(tokenPath, 'utf8'))
    if (refreshedToken.access_token !== 'fresh-access-cli' ||
        refreshedToken.refresh_token !== 'fresh-refresh-cli' ||
        refreshedToken.server_name !== 'refresh_fixture' ||
        refreshedToken.server_url !== `http://127.0.0.1:${port}/mcp` ||
        refreshedToken.client_id !== 'client-1') {
      throw new Error(`native runtime MCP refresh persisted an invalid token: ${JSON.stringify(refreshedToken)}`)
    }
    console.log('ok - native MCP OAuth refresh remote tool e2e')
  } finally {
    child.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

async function runNativeMcpAuthXaaIdpE2e() {
  const requests = []
  const server = createServer((req, res) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', chunk => {
      body += chunk
    })
    req.on('end', () => {
      requests.push({
        method: req.method,
        path: req.url,
        headers: req.headers,
        body,
      })
      if (req.method === 'POST' && req.url === '/device/code') {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify({
          access_token: 'xaa-access-cli',
          refresh_token: 'xaa-refresh-cli',
          org_id: 'org-cli-1',
        }))
        return
      }
      res.writeHead(404, { 'Content-Type': 'application/json' })
      res.end('{"error":"not found"}')
    })
  })

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const { port } = server.address()
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-native-mcp-xaa-'))
  mkdirSync(join(root, '.claude'), { recursive: true })
  mkdirSync(join(root, '.cc-repl'), { recursive: true })
  writeFileSync(join(root, '.cc-repl', 'xaa-idp.txt'), [
    `idp_url=http://127.0.0.1:${port}`,
    'client_id=idp-client-1',
    'scope=openid profile mcp',
    '',
  ].join('\n'), 'utf8')
  writeFileSync(join(root, '.claude', 'config.json'), JSON.stringify({
    mcpServers: {
      xaa_fixture: {
        type: 'http',
        url: `http://127.0.0.1:${port}/mcp`,
        oauth: {
          xaa: true,
          clientId: 'as-client-1',
        },
      },
    },
  }), 'utf8')

  const output = []
  const child = spawn(
    nativeBinary,
    [
      '--run-runtime-tool',
      'mcp_auth',
      '--runtime-tool-input',
      JSON.stringify({ server_name: 'xaa_fixture' }),
    ],
    {
      cwd: root,
      env: {
        ...process.env,
        HOME: root,
        XDG_CONFIG_HOME: root,
        CLAUDE_CODE_ENABLE_XAA: '1',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  child.stdout.on('data', chunk => output.push(String(chunk)))
  child.stderr.on('data', chunk => output.push(String(chunk)))

  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        child.kill('SIGKILL')
        reject(new Error('timed out waiting for native mcp_auth XAA IdP e2e'))
      }, 8000)
      child.once('exit', code => {
        clearTimeout(timer)
        if (code !== 0) {
          reject(new Error(`native mcp_auth XAA IdP exited with code ${code}\n${output.join('')}`))
          return
        }
        resolve()
      })
    })

    const text = output.join('')
    if (!text.includes("Authentication completed silently for 'xaa_fixture'.")) {
      throw new Error(`native mcp_auth XAA IdP did not complete silently:\n${text}`)
    }
    const deviceCodeRequest = requests.find(request => request.method === 'POST' && request.path === '/device/code')
    if (!deviceCodeRequest) {
      throw new Error('native mcp_auth XAA IdP did not request a device code')
    }
    const deviceCodeBody = JSON.parse(deviceCodeRequest.body)
    if (deviceCodeBody.client_id !== 'idp-client-1' ||
        deviceCodeBody.scope !== 'openid profile mcp') {
      throw new Error(`native mcp_auth XAA IdP sent an invalid device code request: ${deviceCodeRequest.body}`)
    }

    const tokenDir = join(root, 'cc-repl', 'mcp')
    const tokenFiles = readdirSync(tokenDir).filter(name => name.endsWith('.json'))
    if (tokenFiles.length !== 1) {
      throw new Error(`native mcp_auth XAA IdP wrote ${tokenFiles.length} token files`)
    }
    const token = JSON.parse(readFileSync(join(tokenDir, tokenFiles[0]), 'utf8'))
    if (token.server_name !== 'xaa_fixture' ||
        token.server_url !== `http://127.0.0.1:${port}/mcp` ||
        token.access_token !== 'xaa-access-cli' ||
        token.refresh_token !== 'xaa-refresh-cli' ||
        token.scope !== 'openid profile mcp' ||
        token.client_id !== 'idp-client-1') {
      throw new Error(`native mcp_auth XAA IdP persisted an invalid token: ${JSON.stringify(token)}`)
    }
    console.log('ok - native mcp_auth XAA IdP e2e')
  } finally {
    child.kill('SIGKILL')
    rmSync(root, { recursive: true, force: true })
    await new Promise(resolve => server.close(resolve))
  }
}

function runRuntimePluginMcpE2e() {
  const root = mkdtempSync(join(tmpdir(), 'cc-repl-plugin-mcp-e2e-'))
  try {
    const pluginCache = join(root, '.claude', 'plugins')
    const pluginRoot = join(pluginCache, 'mcp-fixture')
    mkdirSync(join(root, '.claude'), { recursive: true })
    mkdirSync(pluginRoot, { recursive: true })

    const serverPath = join(pluginRoot, 'server.js')
    writeFileSync(serverPath, `
const readline = require('node:readline')
const rl = readline.createInterface({ input: process.stdin })

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\\n')
}

rl.on('line', line => {
  const request = JSON.parse(line)
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'plugin-mcp-fixture', version: '1.0.0' },
      },
    })
    return
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'plugin_echo', description: 'Echo from plugin MCP', inputSchema: { type: 'object' } }],
      },
    })
    return
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{
          type: 'text',
          text: [
            'plugin',
            request.params.arguments.value,
            process.env.PLUGIN_MCP_FIXTURE,
            process.env.PLUGIN_MCP_TOKEN,
            process.env.CLAUDE_PLUGIN_ROOT,
          ].join(':'),
        }],
      },
    })
  }
})
`)
    writeFileSync(join(root, '.claude', 'settings.json'), JSON.stringify({
      pluginConfigs: {
        'mcp-fixture': {
          options: {
            suffix: 'top-level',
            token: 'shared-token',
          },
          mcpServers: {
            echo: {
              suffix: 'configured',
              token: 'secret-token',
            },
          },
        },
      },
    }, null, 2))
    writeFileSync(join(pluginRoot, 'plugin.json'), JSON.stringify({
      name: 'mcp-fixture',
      version: '1.0.0',
      entry_point: 'plugin.js',
      mcpServers: {
        echo: {
          type: 'stdio',
          command: process.execPath,
          args: ['${CLAUDE_PLUGIN_ROOT}/server.js'],
          env: {
            PLUGIN_MCP_FIXTURE: '${user_config.suffix}',
            PLUGIN_MCP_TOKEN: '${user_config.token}',
          },
        },
      },
    }, null, 2))
    writeFileSync(join(pluginRoot, 'plugin.js'), 'process.exit(0)\n')

    run(
      'runtime tool plugin MCP stdio e2e',
      nativeBinary,
      [
        '--run-runtime-tool',
        'mcp',
        '--runtime-tool-input',
        JSON.stringify({
          server_name: 'plugin:mcp-fixture:echo',
          tool_name: 'plugin_echo',
          arguments: { value: 'hello' },
        }),
      ],
      `plugin:hello:configured:secret-token:${realpathSync(pluginRoot)}`,
      {
        cwd: root,
        env: {
          ...process.env,
          HOME: root,
          CLAUDE_CODE_PLUGIN_CACHE_DIR: pluginCache,
        },
      },
    )
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
}

if (!existsSync(nativeBinary)) {
  throw new Error(`Native binary not found at ${nativeBinary}. Run bun run build first.`)
}

const requestedScenario = process.env.CC_REPL_CPP_MIGRATION_E2E_ONLY
if (requestedScenario) {
  const scenarios = {
    'mcp-auth-refresh-remote-tool': runNativeMcpAuthRefreshRemoteToolE2e,
  }
  const scenario = scenarios[requestedScenario]
  if (!scenario) {
    throw new Error(`Unknown CC_REPL_CPP_MIGRATION_E2E_ONLY scenario: ${requestedScenario}`)
  }
  await scenario()
  process.exit(0)
}

run('native version', nativeBinary, ['--version'], 'cc-repl 1.0.0-cpp')
run('native help', nativeBinary, ['--help'], 'Usage: cc-repl [options]')
run('package start version', 'bun', ['run', 'start', '--', '--version'], 'cc-repl 1.0.0-cpp')
run('compat launcher version', 'bun', ['dist/cli.js', '--version'], 'cc-repl 1.0.0-cpp')
run('strict migration inventory', 'node', ['scripts/cpp-migration-inventory.mjs', '--strict'], 'CMake registered source/header entries')

const runtimeCommands = run('runtime command list', nativeBinary, ['--list-runtime-commands'], 'commit')
for (const command of ['/commit', '/mcp', '/review', '/skills', '/tasks', '/ant-trace', '/version', '/exit']) {
  if (!runtimeCommands.includes(command.slice(1))) {
    throw new Error(`runtime command list did not include ${command}`)
  }
}

const runtimeToolsOutput = run('runtime tool list', nativeBinary, ['--list-runtime-tools'], 'Bash')
const runtimeTools = runtimeToolsOutput.trim().split(/\r?\n/).filter(Boolean)
const normalizedTools = new Set(runtimeTools.map(normalizeInventoryName))
for (const tool of [
  'agent',
  'bash',
  'file_read',
  'file_write',
  'file_edit',
  'glob',
  'grep',
  'mcp',
  'lsp',
  'skill',
  'task_create',
  'script',
  'sleep',
  'web_fetch',
  'web_search',
  'computer_use',
]) {
  if (!normalizedTools.has(tool)) {
    throw new Error(`runtime tool list did not include ${tool}`)
  }
}

const computerUseRoot = mkdtempSync(join(tmpdir(), 'cc-repl-computer-use-e2e-'))
try {
  const hostPath = join(computerUseRoot, 'computer-use-host.mjs')
  const logPath = join(computerUseRoot, 'requests.jsonl')
  writeFileSync(hostPath, `
import { appendFileSync } from 'node:fs'

const request = JSON.parse(process.argv[2] || '{}')
appendFileSync(process.env.CC_REPL_COMPUTER_USE_LOG, JSON.stringify(request) + '\\n')
if (request.action === 'screenshot') {
  console.log(JSON.stringify({
    success: true,
    screenshot_base64: 'AQIDBA==',
    width: 2,
    height: 2,
    format: 'rgba',
  }))
} else {
  console.log(JSON.stringify({ success: true }))
}
`)
  const computerUseEnv = {
    ...process.env,
    CC_REPL_COMPUTER_USE_CMD: `${JSON.stringify(process.execPath)} ${JSON.stringify(hostPath)} {request}`,
    CC_REPL_COMPUTER_USE_LOG: logPath,
    CC_REPL_DISABLE_NATIVE_COMPUTER_INPUT: '1',
  }
  run(
    'runtime tool computer_use command screenshot',
    nativeBinary,
    [
      '--run-runtime-tool',
      'computer_use',
      '--runtime-tool-input',
      '{"action":"screenshot","x":7,"y":8,"width":2,"height":2}',
    ],
    'Captured screenshot 2x2.',
    { env: computerUseEnv },
  )
  run(
    'runtime tool computer_use command click',
    nativeBinary,
    [
      '--run-runtime-tool',
      'computer_use',
      '--runtime-tool-input',
      '{"action":"click","x":11,"y":12}',
    ],
    'Computer-use action completed: click',
    { env: computerUseEnv },
  )
  const computerRequests = readFileSync(logPath, 'utf8')
    .trim()
    .split(/\r?\n/)
    .filter(Boolean)
    .map(line => JSON.parse(line))
  if (computerRequests.length !== 2 ||
      computerRequests[0].action !== 'screenshot' ||
      computerRequests[0].region?.x !== 7 ||
      computerRequests[0].region?.y !== 8 ||
      computerRequests[0].region?.width !== 2 ||
      computerRequests[0].region?.height !== 2 ||
      computerRequests[1].action !== 'click' ||
      computerRequests[1].x !== 11 ||
      computerRequests[1].y !== 12) {
    throw new Error(`runtime computer_use command backend saw unexpected requests:\n${JSON.stringify(computerRequests, null, 2)}`)
  }
  console.log('ok - runtime tool computer_use command backend e2e')
} finally {
  rmSync(computerUseRoot, { recursive: true, force: true })
}

runFailure(
  'runtime tool powershell blocks dangerous command before platform execution',
  nativeBinary,
  [
    '--run-runtime-tool',
    'powershell',
    '--runtime-tool-input',
    JSON.stringify({
      command: 'Remove-Item -Recurse ./cc-repl-danger',
      cwd: repoRoot,
    }),
  ],
  'Dangerous cmdlet detected',
)
runFailure(
  'runtime tool powershell validates working directory before platform execution',
  nativeBinary,
  [
    '--run-runtime-tool',
    'powershell',
    '--runtime-tool-input',
    JSON.stringify({
      command: 'Write-Output cc-repl-powershell-e2e',
      cwd: join(computerUseRoot, 'missing-cwd'),
    }),
  ],
  'Working directory does not exist',
)
if (process.platform === 'win32') {
  run(
    'runtime tool powershell real Windows execution',
    nativeBinary,
    [
      '--run-runtime-tool',
      'powershell',
      '--runtime-tool-input',
      JSON.stringify({
        command: 'Write-Output cc-repl-powershell-e2e',
        cwd: repoRoot,
      }),
    ],
    'cc-repl-powershell-e2e',
  )
} else {
  runFailure(
    'runtime tool powershell non-Windows platform gate',
    nativeBinary,
    [
      '--run-runtime-tool',
      'powershell',
      '--runtime-tool-input',
      JSON.stringify({
        command: 'Write-Output cc-repl-powershell-e2e',
        cwd: repoRoot,
      }),
    ],
    'PowerShell execution is only available on Windows',
  )
}
runRuntimePluginMcpE2e()

const crossProcessRoot = mkdtempSync(join(tmpdir(), 'cc-repl-cross-process-team-'))
try {
  const crossProcessEnv = {
    ...process.env,
    CC_REPL_TEAM_RUNTIME_DIR: join(crossProcessRoot, 'teams'),
    CC_REPL_AGENT_RUNTIME_DIR: join(crossProcessRoot, 'agents'),
  }
  run(
    'runtime tool team_create cross-process producer',
    nativeBinary,
    [
      '--run-runtime-tool',
      'team_create',
      '--runtime-tool-input',
      JSON.stringify({
        team_id: 'cross-process-team-id',
        team_name: 'Cross Process Team',
        members: [
          { agent_id: 'reviewer@cross-process-team', role: 'reviewer' },
          { agent_id: 'planner@cross-process-team', role: 'worker' },
        ],
      }),
    ],
    '"team_name":"Cross Process Team"',
    { env: crossProcessEnv },
  )
  run(
    'runtime tool send_message cross-process consumer',
    nativeBinary,
    [
      '--run-runtime-tool',
      'send_message',
      '--runtime-tool-input',
      JSON.stringify({
        target_agent: 'reviewer',
        team_name: 'Cross Process Team',
        content: 'Cross-process native team delivery',
        summary: 'cross-process delivery',
      }),
    ],
    'reviewer@cross-process-team',
    { env: crossProcessEnv },
  )
  const record = JSON.parse(readFileSync(
    join(crossProcessRoot, 'agents', 'reviewer_cross-process-team.json'),
    'utf8',
  ))
  if (!Array.isArray(record.pending_messages) ||
      !record.pending_messages.some((message) => message.includes('Cross-process native team delivery'))) {
    throw new Error('cross-process send_message did not persist the native pending queue')
  }
  const inbox = JSON.parse(readFileSync(
    join(crossProcessRoot, 'teams', 'cross-process-team', 'inboxes', 'reviewer.json'),
    'utf8',
  ))
  if (!Array.isArray(inbox) ||
      inbox.length !== 1 ||
      inbox[0].text !== 'Cross-process native team delivery' ||
      inbox[0].summary !== 'cross-process delivery') {
    throw new Error('cross-process send_message did not write the teammate mailbox')
  }
  console.log('ok - runtime tool team send_message cross-process persistence')

  run(
    'runtime tool send_message structured shutdown request',
    nativeBinary,
    [
      '--run-runtime-tool',
      'send_message',
      '--runtime-tool-input',
      JSON.stringify({
        to: 'reviewer',
        team_name: 'Cross Process Team',
        message: {
          type: 'shutdown_request',
          reason: 'cross-process structured shutdown',
        },
      }),
    ],
    'request_id: shutdown-',
    { env: crossProcessEnv },
  )
  run(
    'runtime tool send_message structured plan response',
    nativeBinary,
    [
      '--run-runtime-tool',
      'send_message',
      '--runtime-tool-input',
      JSON.stringify({
        to: 'planner',
        team_name: 'Cross Process Team',
        message: {
          type: 'plan_approval_response',
          request_id: 'plan-cross-process-1',
          approve: true,
          permission_mode: 'acceptEdits',
        },
      }),
    ],
    'request_id: plan-cross-process-1',
    { env: crossProcessEnv },
  )

  const reviewerInbox = JSON.parse(readFileSync(
    join(crossProcessRoot, 'teams', 'cross-process-team', 'inboxes', 'reviewer.json'),
    'utf8',
  ))
  if (!Array.isArray(reviewerInbox) || reviewerInbox.length !== 2) {
    throw new Error('structured shutdown request did not append to reviewer mailbox')
  }
  const shutdownMessage = JSON.parse(reviewerInbox[1].text)
  if (shutdownMessage.type !== 'shutdown_request' ||
      shutdownMessage.from !== 'team-lead' ||
      shutdownMessage.reason !== 'cross-process structured shutdown' ||
      typeof shutdownMessage.requestId !== 'string' ||
      !shutdownMessage.requestId.startsWith('shutdown-')) {
    throw new Error(`structured shutdown request mailbox payload was invalid: ${reviewerInbox[1].text}`)
  }

  const plannerInbox = JSON.parse(readFileSync(
    join(crossProcessRoot, 'teams', 'cross-process-team', 'inboxes', 'planner.json'),
    'utf8',
  ))
  if (!Array.isArray(plannerInbox) || plannerInbox.length !== 1) {
    throw new Error('structured plan response did not write planner mailbox')
  }
  const planResponse = JSON.parse(plannerInbox[0].text)
  if (planResponse.type !== 'plan_approval_response' ||
      planResponse.requestId !== 'plan-cross-process-1' ||
      planResponse.approved !== true ||
      planResponse.permissionMode !== 'acceptEdits') {
    throw new Error(`structured plan response mailbox payload was invalid: ${plannerInbox[0].text}`)
  }
  console.log('ok - runtime tool team send_message structured cross-process protocol')
} finally {
  rmSync(crossProcessRoot, { recursive: true, force: true })
}

const simpleUiOutput = run(
  'simple ui command dispatch',
  nativeBinary,
  ['--simple-ui'],
  'Goodbye!',
  {
    env: { ...process.env, ANTHROPIC_API_KEY: process.env.ANTHROPIC_API_KEY || 'dummy' },
    input: '/help\n/commit\n/mcp list\n/ant-trace request-42\n/debug-tool-call {"name":"Bash","input":{"command":"pwd"}}\n/mock-limits 1\n/reset-limits all\n/extra-usage status\n/bridge status\n/onboarding status\n/version detail\n/exit\n',
  },
)
if (simpleUiOutput.includes('Unknown command: /commit')) {
  throw new Error('simple UI did not dispatch /commit through the migrated registry')
}
if (simpleUiOutput.includes('Unknown command: /mcp')) {
  throw new Error('simple UI did not dispatch /mcp through the migrated registry')
}
if (simpleUiOutput.includes('No dedicated local action')) {
  throw new Error('simple UI still dispatched a command through the surface fallback path')
}
for (const forbidden of ['command ready', 'workflow ready', 'adapter ready', 'workflow prepared']) {
  if (simpleUiOutput.includes(forbidden)) {
    throw new Error(`simple UI command dispatch still returned incomplete migration text: ${forbidden}`)
  }
}
for (const expected of [
  'ANT trace snapshot',
  'Tool-call payload valid',
  'Synthetic rate limit active',
  'Rate limit state reset: active=false',
  'Extra usage:',
  'Bridge status:',
  'Onboarding status',
]) {
  if (!simpleUiOutput.includes(expected)) {
    throw new Error(`simple UI command dispatch did not include expected runtime output: ${expected}`)
  }
}
if (!simpleUiOutput.includes('cc-repl 1.0.0-cpp (C++23')) {
  throw new Error('simple UI /version command did not report the native version')
}

const envWithoutApiKey = { ...process.env }
delete envWithoutApiKey.ANTHROPIC_API_KEY
const simpleUiNoKeyOutput = run(
  'simple ui slash commands without api key',
  nativeBinary,
  ['--simple-ui'],
  'Goodbye!',
  {
    env: envWithoutApiKey,
    input: '/version detail\n/exit\n',
  },
)
if (simpleUiNoKeyOutput.includes('ANTHROPIC_API_KEY environment variable is not set')) {
  throw new Error('simple UI still requires ANTHROPIC_API_KEY for slash-only command dispatch')
}
if (!simpleUiNoKeyOutput.includes('cc-repl 1.0.0-cpp (C++23')) {
  throw new Error('simple UI without api key did not dispatch /version')
}

run(
  'direct-connect TS client permission/reconnect e2e',
  'bun',
  ['scripts/cpp-direct-connect-client-e2e.mjs'],
  'ok - direct-connect TS client permission/reconnect e2e',
)
runNativeMcpAuthRemoteOAuthConfigE2e()
await runNativeMcpAuthStandardOAuthUrlE2e()
await runNativeMcpAuthBrowserCallbackE2e()
await runNativeMcpAuthRefreshRemoteToolE2e()
await runNativeMcpAuthXaaIdpE2e()

await runBridgeDaemonOneShotProductE2e()
await runBridgeDaemonLongRunningProductE2e()
await runBridgeDaemonDelayedWorkPollE2e()
await runBridgeDaemonAuthFailureBackoffE2e()
await runHeadlessIngressLifecycleE2e()
await runHeadlessStreamJsonRemoteQueryE2e()
await runHeadlessStreamJsonTaskBudgetE2e()
await runHeadlessPersistedResumeE2e()
await runHeadlessSdkUrlSseRemoteQueryE2e()
await runHeadlessSdkUrlSseAuthFailureE2e()
await runHeadlessSdkUrlWebSocketRemoteQueryE2e()
await runHeadlessSdkUrlWebSocketAuthFailureE2e()
