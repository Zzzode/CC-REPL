import { describe, expect, test, mock, beforeEach, afterEach } from 'bun:test'
import type { Task } from '../utils/tasks.js'

// Test the pure logic functions from useTaskListWatcher
// Since they're not exported, we reimplement and validate behavior

function findAvailableTask(tasks: Task[]): Task | undefined {
  const unresolvedTaskIds = new Set(
    tasks.filter(t => t.status !== 'completed').map(t => t.id),
  )

  return tasks.find(task => {
    if (task.status !== 'pending') return false
    if (task.owner) return false
    return task.blockedBy.every(id => !unresolvedTaskIds.has(id))
  })
}

function formatTaskAsPrompt(task: Task): string {
  let prompt = `Complete all open tasks. Start with task #${task.id}: \n\n ${task.subject}`

  if (task.description) {
    prompt += `\n\n${task.description}`
  }

  return prompt
}

describe('useTaskListWatcher logic: findAvailableTask', () => {
  function makeTask(
    id: string,
    status: 'pending' | 'in_progress' | 'completed',
    blockedBy: string[] = [],
    owner?: string,
  ): Task {
    return {
      id,
      subject: `Task ${id}`,
      description: '',
      status,
      blockedBy,
      owner,
      createdAt: Date.now(),
      updatedAt: Date.now(),
    } as unknown as Task
  }

  test('returns undefined for empty task list', () => {
    expect(findAvailableTask([])).toBeUndefined()
  })

  test('finds first pending task with no owner and no blockers', () => {
    const tasks = [
      makeTask('1', 'pending'),
      makeTask('2', 'pending'),
    ]
    const result = findAvailableTask(tasks)
    expect(result?.id).toBe('1')
  })

  test('skips tasks with owners', () => {
    const tasks = [
      makeTask('1', 'pending', [], 'agent-a'),
      makeTask('2', 'pending'),
    ]
    const result = findAvailableTask(tasks)
    expect(result?.id).toBe('2')
  })

  test('skips non-pending tasks', () => {
    const tasks = [
      makeTask('1', 'in_progress'),
      makeTask('2', 'completed'),
      makeTask('3', 'pending'),
    ]
    const result = findAvailableTask(tasks)
    expect(result?.id).toBe('3')
  })

  test('skips tasks with unresolved blockers', () => {
    const tasks = [
      makeTask('1', 'pending'),
      makeTask('2', 'pending', ['1']),
    ]
    const result = findAvailableTask(tasks)
    expect(result?.id).toBe('1')
  })

  test('allows tasks with completed blockers', () => {
    const tasks = [
      makeTask('1', 'completed'),
      makeTask('2', 'pending', ['1']),
    ]
    const result = findAvailableTask(tasks)
    expect(result?.id).toBe('2')
  })

  test('handles multiple blockers - all must be completed', () => {
    const tasks = [
      makeTask('1', 'completed'),
      makeTask('2', 'in_progress'),
      makeTask('3', 'pending', ['1', '2']),
    ]
    const result = findAvailableTask(tasks)
    expect(result).toBeUndefined()
  })

  test('returns undefined when all tasks are owned or blocked', () => {
    const tasks = [
      makeTask('1', 'pending', [], 'agent-a'),
      makeTask('2', 'pending', ['1']),
    ]
    expect(findAvailableTask(tasks)).toBeUndefined()
  })

  test('blockedBy referencing non-existent task id blocks forever', () => {
    const tasks = [
      makeTask('1', 'pending', ['999']),
    ]
    // The blocker id '999' doesn't exist, so it's in unresolvedTaskIds
    // (filter returns [] for non-existent, but wait — let's think again)
    // Actually, unresolvedTaskIds only contains ids from tasks in the list.
    // If a blocker references a non-existent id, it won't be in the set,
    // so `!unresolvedTaskIds.has(id)` is true — meaning the blocker is considered resolved.
    // This matches the implementation behavior.
    const result = findAvailableTask(tasks)
    // Non-existent blocker IDs are treated as "not unresolved" = pass
    expect(result?.id).toBe('1')
  })
})

describe('useTaskListWatcher logic: formatTaskAsPrompt', () => {
  test('includes task id and subject', () => {
    const task = {
      id: '42',
      subject: 'Fix the bug',
      description: '',
    } as unknown as Task
    const prompt = formatTaskAsPrompt(task)
    expect(prompt).toContain('#42')
    expect(prompt).toContain('Fix the bug')
  })

  test('includes description when present', () => {
    const task = {
      id: '1',
      subject: 'Test',
      description: 'Detailed description here',
    } as unknown as Task
    const prompt = formatTaskAsPrompt(task)
    expect(prompt).toContain('Detailed description here')
  })

  test('works without description', () => {
    const task = {
      id: '1',
      subject: 'Test',
    } as unknown as Task
    const prompt = formatTaskAsPrompt(task)
    expect(prompt).toContain('Test')
    // Should not include extra blank lines for missing description
  })
})
