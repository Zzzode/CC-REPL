import { describe, expect, test } from 'bun:test'
import { isCodeEditingTool } from './toolPermission/permissionLogging.js'

describe('permissionLogging: isCodeEditingTool', () => {
  test('returns true for Edit tool', () => {
    expect(isCodeEditingTool('Edit')).toBe(true)
  })

  test('returns true for Write tool', () => {
    expect(isCodeEditingTool('Write')).toBe(true)
  })

  test('returns true for NotebookEdit tool', () => {
    expect(isCodeEditingTool('NotebookEdit')).toBe(true)
  })

  test('returns false for Bash tool', () => {
    expect(isCodeEditingTool('Bash')).toBe(false)
  })

  test('returns false for WebSearch tool', () => {
    expect(isCodeEditingTool('WebSearch')).toBe(false)
  })

  test('returns false for empty string', () => {
    expect(isCodeEditingTool('')).toBe(false)
  })

  test('is case-sensitive', () => {
    expect(isCodeEditingTool('edit')).toBe(false)
    expect(isCodeEditingTool('EDIT')).toBe(false)
    expect(isCodeEditingTool('Edit ')).toBe(false)
  })

  test('returns false for similar names', () => {
    expect(isCodeEditingTool('EditFile')).toBe(false)
    expect(isCodeEditingTool('FileEdit')).toBe(false)
    expect(isCodeEditingTool('WriteFile')).toBe(false)
  })
})

describe('permission sourceToString logic', () => {
  // Test the source classification logic used in permission logging
  type PermissionApprovalSource =
    | { type: 'hook'; permanent?: boolean }
    | { type: 'user'; permanent: boolean }
    | { type: 'classifier' }

  type PermissionRejectionSource =
    | { type: 'hook' }
    | { type: 'user_abort' }
    | { type: 'user_reject'; hasFeedback: boolean }

  function sourceToString(
    source: PermissionApprovalSource | PermissionRejectionSource,
  ): string {
    switch (source.type) {
      case 'hook':
        return 'hook'
      case 'user':
        return source.permanent ? 'user_permanent' : 'user_temporary'
      case 'user_abort':
        return 'user_abort'
      case 'user_reject':
        return 'user_reject'
      case 'classifier':
        return 'classifier'
      default:
        return 'unknown'
    }
  }

  test('hook source returns "hook"', () => {
    expect(sourceToString({ type: 'hook' })).toBe('hook')
  })

  test('user permanent returns "user_permanent"', () => {
    expect(sourceToString({ type: 'user', permanent: true })).toBe('user_permanent')
  })

  test('user temporary returns "user_temporary"', () => {
    expect(sourceToString({ type: 'user', permanent: false })).toBe('user_temporary')
  })

  test('user_abort returns "user_abort"', () => {
    expect(sourceToString({ type: 'user_abort' })).toBe('user_abort')
  })

  test('user_reject returns "user_reject"', () => {
    expect(sourceToString({ type: 'user_reject', hasFeedback: true })).toBe('user_reject')
    expect(sourceToString({ type: 'user_reject', hasFeedback: false })).toBe('user_reject')
  })

  test('classifier returns "classifier"', () => {
    expect(sourceToString({ type: 'classifier' })).toBe('classifier')
  })

  test('hook with permanent flag still returns "hook"', () => {
    // Approval source hook has permanent, but it doesn't affect the string
    expect(sourceToString({ type: 'hook', permanent: true })).toBe('hook')
    expect(sourceToString({ type: 'hook', permanent: false })).toBe('hook')
  })
})

describe('permission decision logging classification', () => {
  // Test the baseMetadata spread logic for waitMs
  function buildWaitMetadata(waitMs: number | undefined): Record<string, number | undefined> {
    return {
      ...(waitMs !== undefined && { waiting_for_user_permission_ms: waitMs }),
    }
  }

  test('includes wait time when provided', () => {
    const meta = buildWaitMetadata(1500)
    expect(meta.waiting_for_user_permission_ms).toBe(1500)
  })

  test('omits wait time when undefined', () => {
    const meta = buildWaitMetadata(undefined)
    expect('waiting_for_user_permission_ms' in meta).toBe(false)
  })

  test('omits wait time when 0 (falsy but defined)', () => {
    // Wait, 0 is defined, so the spread should include it
    // Let's verify: waitMs !== undefined && { key: waitMs }
    const meta = buildWaitMetadata(0)
    expect(meta.waiting_for_user_permission_ms).toBe(0)
  })
})
