/* eslint-disable eslint-plugin-n/no-unsupported-features/node-builtins */

import type { DirectConnectConfig } from './directConnectManager.js'

/**
 * Errors thrown by createDirectConnectSession when the connection fails.
 */
export class DirectConnectError extends Error {
  constructor(message: string) {
    super(message)
    this.name = 'DirectConnectError'
  }
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

function parseConnectResponse(value: unknown): {
  session_id: string
  ws_url: string
  work_dir?: string
} {
  if (typeof value !== 'object' || value === null) {
    throw new DirectConnectError('response body is not an object')
  }
  const record = value as Record<string, unknown>
  if (typeof record.session_id !== 'string' || record.session_id.length === 0) {
    throw new DirectConnectError('response is missing session_id')
  }
  if (typeof record.ws_url !== 'string' || record.ws_url.length === 0) {
    throw new DirectConnectError('response is missing ws_url')
  }
  if (
    record.work_dir !== undefined &&
    typeof record.work_dir !== 'string'
  ) {
    throw new DirectConnectError('response work_dir must be a string')
  }
  return {
    session_id: record.session_id,
    ws_url: record.ws_url,
    ...(typeof record.work_dir === 'string' && { work_dir: record.work_dir }),
  }
}

/**
 * Create a session on a direct-connect server.
 *
 * Posts to `${serverUrl}/sessions`, validates the response, and returns
 * a DirectConnectConfig ready for use by the REPL or headless runner.
 *
 * Throws DirectConnectError on network, HTTP, or response-parsing failures.
 */
export async function createDirectConnectSession({
  serverUrl,
  authToken,
  cwd,
  dangerouslySkipPermissions,
}: {
  serverUrl: string
  authToken?: string
  cwd: string
  dangerouslySkipPermissions?: boolean
}): Promise<{
  config: DirectConnectConfig
  workDir?: string
}> {
  const headers: Record<string, string> = {
    'content-type': 'application/json',
  }
  if (authToken) {
    headers['authorization'] = `Bearer ${authToken}`
  }

  let resp: Response
  try {
    resp = await fetch(`${serverUrl}/sessions`, {
      method: 'POST',
      headers,
      body: JSON.stringify({
        cwd,
        ...(dangerouslySkipPermissions && {
          dangerously_skip_permissions: true,
        }),
      }),
    })
  } catch (err) {
    throw new DirectConnectError(
      `Failed to connect to server at ${serverUrl}: ${errorMessage(err)}`,
    )
  }

  if (!resp.ok) {
    throw new DirectConnectError(
      `Failed to create session: ${resp.status} ${resp.statusText}`,
    )
  }

  let data: ReturnType<typeof parseConnectResponse>
  try {
    data = parseConnectResponse(await resp.json())
  } catch (err) {
    throw new DirectConnectError(
      `Invalid session response: ${errorMessage(err)}`,
    )
  }

  return {
    config: {
      serverUrl,
      sessionId: data.session_id,
      wsUrl: data.ws_url,
      authToken,
    },
    workDir: data.work_dir,
  }
}
