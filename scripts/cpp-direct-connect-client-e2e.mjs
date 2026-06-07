#!/usr/bin/env bun

import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { spawn } from 'node:child_process'
import { once } from 'node:events'
import { DirectConnectSessionManager } from '../src/server/directConnectManager.ts'
import { createDirectConnectSession } from '../src/server/createDirectConnectSession.ts'

const repoRoot = resolve(new URL('..', import.meta.url).pathname)
const nativeBinary = resolve(repoRoot, 'dist', process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl')

function assert(condition, message) {
  if (!condition) throw new Error(message)
}

async function waitForValue(read, label, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    const value = read()
    if (value) return value
    await Bun.sleep(20)
  }
  throw new Error(`Timed out waiting for ${label}`)
}

function messageText(message) {
  const content = message?.message?.content
  if (!Array.isArray(content)) return ''
  return content
    .map(block => typeof block?.text === 'string' ? block.text : '')
    .filter(Boolean)
    .join('\n')
}

function anthropicMessage({ id, content, stopReason = 'end_turn' }) {
  return JSON.stringify({
    id,
    type: 'message',
    role: 'assistant',
    model: 'claude-test',
    content,
    stop_reason: stopReason,
    usage: { input_tokens: 1, output_tokens: 1 },
  })
}

function startAnthropicFixture(responses) {
  const bodies = []
  const server = Bun.serve({
    hostname: '127.0.0.1',
    port: 0,
    async fetch(request) {
      const url = new URL(request.url)
      if (request.method !== 'POST' || url.pathname !== '/v1/messages') {
        return new Response('not found', { status: 404 })
      }

      bodies.push(await request.text())
      const response = responses[Math.min(bodies.length - 1, responses.length - 1)]
      return new Response(response, {
        status: 200,
        headers: { 'content-type': 'application/json' },
      })
    },
  })

  return {
    baseUrl: `http://127.0.0.1:${server.port}`,
    bodies,
    async waitForBodies(count) {
      return waitForValue(
        () => bodies.length >= count ? bodies : null,
        `${count} Anthropic fixture request bodies`,
      )
    },
    stop() {
      server.stop(true)
    },
  }
}

async function startNativeServer(env) {
  const probe = Bun.serve({
    hostname: '127.0.0.1',
    port: 0,
    fetch() {
      return new Response('reserved')
    },
  })
  const port = probe.port
  probe.stop(true)

  const child = spawn(
    nativeBinary,
    ['--server', '--server-host', '127.0.0.1', '--server-port', String(port)],
    {
      cwd: repoRoot,
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )

  let output = ''
  child.stdout.on('data', chunk => {
    output += chunk.toString()
  })
  child.stderr.on('data', chunk => {
    output += chunk.toString()
  })

  const url = `http://127.0.0.1:${port}`
  const deadline = Date.now() + 10000
  while (Date.now() < deadline) {
    if (child.exitCode !== null) {
      throw new Error(`Native direct-connect server exited before listening: code=${child.exitCode}\n${output}`)
    }
    try {
      const response = await fetch(`${url}/health`)
      if (response.ok) return { child, url, output }
    } catch {
      // Keep polling until the server is ready or exits.
    }
    await Bun.sleep(50)
  }

  child.kill('SIGTERM')
  throw new Error(`Timed out waiting for native direct-connect server health check\n${output}`)
}

async function stopNativeServer(child) {
  if (!child || child.exitCode !== null) return
  child.kill('SIGTERM')
  await Promise.race([
    once(child, 'exit'),
    Bun.sleep(2000).then(() => {
      if (child.exitCode === null) child.kill('SIGKILL')
    }),
  ])
}

async function main() {
  assert(existsSync(nativeBinary), `Native binary not found at ${nativeBinary}. Run bun run build first.`)

  const root = mkdtempSync(join(tmpdir(), 'cc-repl-direct-ts-client-'))
  const allowedFile = join(root, 'allowed.txt')
  const deniedFile = join(root, 'denied.txt')
  const updatedFile = join(root, 'updated.txt')
  const allowedDir = join(root, 'allowed-dir')
  const directoryFile = join(allowedDir, 'directory.txt')
  mkdirSync(allowedDir)
  writeFileSync(allowedFile, 'original direct input content\n')
  writeFileSync(deniedFile, 'denied direct input content\n')
  writeFileSync(updatedFile, 'updated direct input content\n')
  writeFileSync(directoryFile, 'directory direct input content\n')

  const anthropic = startAnthropicFixture([
    anthropicMessage({
      id: 'msg_ts_read_allow_tool',
      stopReason: 'tool_use',
      content: [{
        type: 'tool_use',
        id: 'toolu_ts_read_allow',
        name: 'Read',
        input: { file_path: allowedFile },
      }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_allow_done',
      content: [{ type: 'text', text: 'ts client read allowed' }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_cached_allow_tool',
      stopReason: 'tool_use',
      content: [{
        type: 'tool_use',
        id: 'toolu_ts_read_cached_allow',
        name: 'Read',
        input: { file_path: allowedFile },
      }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_cached_allow_done',
      content: [{ type: 'text', text: 'ts client cached read allowed' }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_cached_directory_tool',
      stopReason: 'tool_use',
      content: [{
        type: 'tool_use',
        id: 'toolu_ts_read_cached_directory',
        name: 'Read',
        input: { file_path: directoryFile },
      }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_cached_directory_done',
      content: [{ type: 'text', text: 'ts client cached directory read allowed' }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_deny_tool',
      stopReason: 'tool_use',
      content: [{
        type: 'tool_use',
        id: 'toolu_ts_read_deny',
        name: 'Read',
        input: { file_path: deniedFile },
      }],
    }),
    anthropicMessage({
      id: 'msg_ts_read_deny_done',
      content: [{ type: 'text', text: 'ts client read denied' }],
    }),
  ])

  let native
  let manager
  try {
    native = await startNativeServer({
      ...process.env,
      ANTHROPIC_API_KEY: 'test-key',
      ANTHROPIC_BASE_URL: anthropic.baseUrl,
      CC_REPL_SERVER_SESSIONS_DIR: join(root, 'sessions'),
      CC_REPL_AGENT_RUNTIME_DIR: join(root, 'agents'),
      CC_REPL_TEAM_RUNTIME_DIR: join(root, 'teams'),
    })

    const { config, workDir } = await createDirectConnectSession({
      serverUrl: native.url,
      cwd: root,
    })
    assert(config.sessionId.length > 0, 'createDirectConnectSession did not return a session id')
    assert(config.wsUrl.startsWith('ws://127.0.0.1:'), `unexpected ws_url: ${config.wsUrl}`)
    assert(workDir === root, `unexpected workDir: ${workDir}`)

    const messages = []
    const permissionRequests = []
    const callbackErrors = []
    const expectedPermissions = [
      { behavior: 'allow', toolUseId: 'toolu_ts_read_allow', filePath: allowedFile },
      { behavior: 'deny', toolUseId: 'toolu_ts_read_deny', filePath: deniedFile },
    ]

    async function connectManager() {
      let connected = false
      manager = new DirectConnectSessionManager(config, {
        onConnected: () => {
          connected = true
        },
        onDisconnected: () => {},
        onError: error => {
          callbackErrors.push(error)
        },
        onMessage: message => {
          messages.push(message)
        },
        onPermissionRequest: (request, requestId) => {
          try {
            const expected = expectedPermissions.shift()
            assert(expected, `unexpected permission request ${requestId}`)
            assert(request.subtype === 'can_use_tool', `unexpected permission subtype: ${request.subtype}`)
            assert(request.tool_name === 'Read', `unexpected permission tool: ${request.tool_name}`)
            assert(request.tool_use_id === expected.toolUseId, `unexpected tool_use_id: ${request.tool_use_id}`)
            assert(request.input?.file_path === expected.filePath, `unexpected permission file path: ${request.input?.file_path}`)
            permissionRequests.push({ requestId, request, behavior: expected.behavior })
            manager.respondToPermissionRequest(
              requestId,
              expected.behavior === 'allow'
                ? {
                    behavior: 'allow',
                    updatedInput: { file_path: updatedFile },
                    updatedPermissions: [{
                      type: 'addRules',
                      rules: [{ toolName: 'Read', ruleContent: allowedFile }],
                      behavior: 'allow',
                      destination: 'session',
                    }, {
                      type: 'addDirectories',
                      directories: [allowedDir],
                      destination: 'session',
                    }],
                  }
                : { behavior: 'deny', message: 'denied by TS direct client e2e' },
            )
          } catch (error) {
            callbackErrors.push(error)
            manager.respondToPermissionRequest(requestId, {
              behavior: 'deny',
              message: error instanceof Error ? error.message : String(error),
            })
          }
        },
      })
      manager.connect()
      await waitForValue(() => connected, 'TS direct-connect client connection')
      return manager
    }

    async function sendAndExpect(prompt, expectedText) {
      const start = messages.length
      assert(manager.sendMessage(prompt), `sendMessage returned false for prompt: ${prompt}`)
      const result = await waitForValue(
        () => messages.slice(start).find(message => message.type === 'result' && message.result === expectedText),
        `result message ${expectedText}`,
      )
      assert(result.subtype === 'success', `unexpected result subtype: ${result.subtype}`)
      assert(result.session_id === config.sessionId, `unexpected result session: ${result.session_id}`)

      const assistant = messages.slice(start).find(message => message.type === 'assistant')
      assert(assistant, `missing assistant message for ${expectedText}`)
      assert(assistant.session_id === config.sessionId, `unexpected assistant session: ${assistant.session_id}`)
      assert(messageText(assistant) === expectedText, `unexpected assistant text: ${messageText(assistant)}`)
    }

    await connectManager()
    await sendAndExpect('read through TS direct client allow', 'ts client read allowed')
    manager.disconnect()

    await connectManager()
    await sendAndExpect('read through TS direct client cached allow after reconnect', 'ts client cached read allowed')
    await sendAndExpect('read through TS direct client cached directory after reconnect', 'ts client cached directory read allowed')
    await sendAndExpect('read through TS direct client deny after reconnect', 'ts client read denied')

    assert(callbackErrors.length === 0, callbackErrors.map(error => error.stack || error.message || String(error)).join('\n'))
    assert(permissionRequests.length === 2, `expected 2 permission requests, saw ${permissionRequests.length}`)
    const bodies = await anthropic.waitForBodies(8)
    assert(bodies[0].includes('read through TS direct client allow'), 'first model request did not include the allow prompt')
    assert(bodies[1].includes('updated direct input content'), 'updatedInput Read result did not reach the follow-up model request')
    assert(!bodies[1].includes('original direct input content'), 'original Read input was used despite updatedInput')
    assert(bodies[2].includes('read through TS direct client cached allow after reconnect'), 'cached allow model request did not include the prompt')
    assert(bodies[3].includes('original direct input content'), 'updatedPermissions allow rule did not let the cached Read result reach the model request')
    assert(bodies[4].includes('read through TS direct client cached directory after reconnect'), 'cached directory model request did not include the prompt')
    assert(bodies[5].includes('directory direct input content'), 'updatedPermissions addDirectories did not let the cached Read result reach the model request')
    assert(bodies[6].includes('read through TS direct client deny after reconnect'), 'deny model request did not include the prompt')
    assert(bodies[7].includes('denied by TS direct client e2e'), 'denied Read message did not reach the follow-up model request')
    assert(!bodies[7].includes('Permission denied for tool: Read'), 'default deny message was used despite SDK deny message')

    console.log('ok - direct-connect TS client permission/reconnect e2e')
  } finally {
    if (manager) manager.disconnect()
    await stopNativeServer(native?.child)
    anthropic.stop()
    rmSync(root, { recursive: true, force: true })
  }
}

main().catch(error => {
  console.error(error)
  process.exit(1)
})
