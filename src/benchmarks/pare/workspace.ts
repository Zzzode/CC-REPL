import { mkdtemp, rm } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { execa } from 'execa'

export type BenchmarkWorkspace = {
  dir: string
  cleanup: () => Promise<void>
}

export async function createIsolatedWorkspace(params: {
  mode: 'tmp-git' | 'worktree' | 'current'
  sourceCwd: string
}): Promise<BenchmarkWorkspace> {
  if (params.mode === 'current') {
    return {
      dir: params.sourceCwd,
      cleanup: async () => {},
    }
  }

  const tempRoot = await mkdtemp(path.join(os.tmpdir(), 'pare-bench-'))
  const repoDir = path.join(tempRoot, 'repo')

  if (params.mode === 'tmp-git') {
    await execa('git', ['clone', '--local', '--no-hardlinks', '--no-checkout', params.sourceCwd, repoDir])
    await execa('git', ['-C', repoDir, 'checkout', '--force', 'HEAD'])

    return {
      dir: repoDir,
      cleanup: async () => {
        await rm(tempRoot, { recursive: true, force: true })
      },
    }
  }

  await execa('git', ['worktree', 'add', '--detach', repoDir, 'HEAD'], {
    cwd: params.sourceCwd,
  })

  return {
    dir: repoDir,
    cleanup: async () => {
      try {
        await execa('git', ['worktree', 'remove', '--force', repoDir], {
          cwd: params.sourceCwd,
        })
      } finally {
        await rm(tempRoot, { recursive: true, force: true })
      }
    },
  }
}
