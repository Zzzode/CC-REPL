# Benchmark Case Inventory

This file lists benchmark case sets migrated from Pare v2 and retained in this repository.

Source files:
- `benchmarks/pare/cases/pare-v2-reproducible.json`
- `benchmarks/pare/cases/pare-v2-mutating.json`

## benchmarks/pare/cases/pare-v2-reproducible.json

- Version: `pare-case-v1`
- Case count: **63**
- Source: Pare benchmarks v2 CSV + benchmark-scenarios.ts (extracted list)
- Note: Approximate command-fidelity migration for Claude Code benchmark harness.

| # | Case ID | Name | Category | Frequency | Assertion | Prompt (short) |
|---:|---|---|---|---|---|---|
| 1 | `pare-repro-1A` | Pare 1A status | git | very_high | exact: git status | Return exactly this command and nothing else: git status |
| 2 | `pare-repro-2A` | Pare 2A diff-small | git | very_high | exact: git diff --stat HEAD~1 | Return exactly this command and nothing else: git diff --stat HEAD~1 |
| 3 | `pare-repro-2B` | Pare 2B diff-large | git | very_high | exact: git diff --stat HEAD~20 | Return exactly this command and nothing else: git diff --stat HEAD~20 |
| 4 | `pare-repro-2C` | Pare 2C diff-full-patch | git | very_high | exact: git diff HEAD~1 | Return exactly this command and nothing else: git diff HEAD~1 |
| 5 | `pare-repro-3A` | Pare 3A git-commit | git | low | exact: git commit -m "benchmark" | Return exactly this command and nothing else: git commit -m "benchmark" |
| 6 | `pare-repro-4A` | Pare 4A git-add | git | low | exact: git add . | Return exactly this command and nothing else: git add . |
| 7 | `pare-repro-5A` | Pare 5A log-5 | git | very_high | exact: git log -5 --oneline | Return exactly this command and nothing else: git log -5 --oneline |
| 8 | `pare-repro-5B` | Pare 5B log-20 | git | high | exact: git log -20 --oneline | Return exactly this command and nothing else: git log -20 --oneline |
| 9 | `pare-repro-5C` | Pare 5C log-50 | git | medium | exact: git log -50 --oneline | Return exactly this command and nothing else: git log -50 --oneline |
| 10 | `pare-repro-6A` | Pare 6A git-push | git | low | exact: git push | Return exactly this command and nothing else: git push |
| 11 | `pare-repro-7A` | Pare 7A test-server-git | node-tooling | high | exact: npm run test -w packages/server-git | Return exactly this command and nothing else: npm run test -w packages/server-git |
| 12 | `pare-repro-7B` | Pare 7B test-shared | node-tooling | high | exact: npm run test -w packages/shared | Return exactly this command and nothing else: npm run test -w packages/shared |
| 13 | `pare-repro-7C` | Pare 7C test-server-lint | node-tooling | low | exact: npm run lint -w packages/server-git | Return exactly this command and nothing else: npm run lint -w packages/server-git |
| 14 | `pare-repro-8A` | Pare 8A git-checkout | git | low | exact: git checkout - | Return exactly this command and nothing else: git checkout - |
| 15 | `pare-repro-9A` | Pare 9A npm-run-build | node-tooling | high | exact: npm run build | Return exactly this command and nothing else: npm run build |
| 16 | `pare-repro-9B` | Pare 9B npm-run-lint | node-tooling | high | exact: npm run lint | Return exactly this command and nothing else: npm run lint |
| 17 | `pare-repro-9C` | Pare 9C npm-run-test | node-tooling | high | exact: npm run test | Return exactly this command and nothing else: npm run test |
| 18 | `pare-repro-10A` | Pare 10A git-pull | git | medium | exact: git pull | Return exactly this command and nothing else: git pull |
| 19 | `pare-repro-11A` | Pare 11A npm-install | node-tooling | medium | exact: npm install | Return exactly this command and nothing else: npm install |
| 20 | `pare-repro-12A` | Pare 12A tsc-shared | node-tooling | medium | exact: npx tsc --noEmit -p packages/shared/tsconfig.json | Return exactly this command and nothing else: npx tsc --noEmit -p packages/shared/tsconfig.json |
| 21 | `pare-repro-12B` | Pare 12B tsc-server-git | node-tooling | medium | exact: npx tsc --noEmit -p packages/server-git/tsconfig.json | Return exactly this command and nothing else: npx tsc --noEmit -p packages/server-git/tsconfig.json |
| 22 | `pare-repro-12C` | Pare 12C tsc-repo | node-tooling | very_high | exact: npx tsc --noEmit | Return exactly this command and nothing else: npx tsc --noEmit |
| 23 | `pare-repro-13A` | Pare 13A npm-test-shared | node-tooling | low | exact: npm test -w packages/shared | Return exactly this command and nothing else: npm test -w packages/shared |
| 24 | `pare-repro-13B` | Pare 13B npm-test-server-git | node-tooling | low | exact: npm test -w packages/server-git | Return exactly this command and nothing else: npm test -w packages/server-git |
| 25 | `pare-repro-14A` | Pare 14A branch-local | git | high | exact: git branch | Return exactly this command and nothing else: git branch |
| 26 | `pare-repro-14B` | Pare 14B branch-all | git | high | exact: git branch -a | Return exactly this command and nothing else: git branch -a |
| 27 | `pare-repro-15A` | Pare 15A show-head | git | high | exact: git show --stat HEAD | Return exactly this command and nothing else: git show --stat HEAD |
| 28 | `pare-repro-15B` | Pare 15B show-mid | git | low | exact: git show --stat HEAD~10 | Return exactly this command and nothing else: git show --stat HEAD~10 |
| 29 | `pare-repro-15C` | Pare 15C show-deep | git | low | exact: git show --stat HEAD~50 | Return exactly this command and nothing else: git show --stat HEAD~50 |
| 30 | `pare-repro-16A` | Pare 16A build-shared | node-tooling | medium | exact: npm run build -w packages/shared | Return exactly this command and nothing else: npm run build -w packages/shared |
| 31 | `pare-repro-16B` | Pare 16B build-server-git | node-tooling | medium | exact: npm run build -w packages/server-git | Return exactly this command and nothing else: npm run build -w packages/server-git |
| 32 | `pare-repro-16C` | Pare 16C build-server-npm | node-tooling | medium | exact: npm run build -w packages/server-npm | Return exactly this command and nothing else: npm run build -w packages/server-npm |
| 33 | `pare-repro-17A` | Pare 17A lint-shared | node-tooling | low | exact: npm run lint -w packages/shared | Return exactly this command and nothing else: npm run lint -w packages/shared |
| 34 | `pare-repro-17B` | Pare 17B lint-server-git | node-tooling | low | exact: npm run lint -w packages/server-git | Return exactly this command and nothing else: npm run lint -w packages/server-git |
| 35 | `pare-repro-17C` | Pare 17C lint-server-npm | node-tooling | low | exact: npm run lint -w packages/server-npm | Return exactly this command and nothing else: npm run lint -w packages/server-npm |
| 36 | `pare-repro-17D` | Pare 17D lint-violations | node-tooling | very_high | exact: npx eslint . | Return exactly this command and nothing else: npx eslint . |
| 37 | `pare-repro-18A` | Pare 18A docker-ps | docker | very_high | exact: docker ps | Return exactly this command and nothing else: docker ps |
| 38 | `pare-repro-18B` | Pare 18B docker-ps-all | docker | very_high | exact: docker ps -a | Return exactly this command and nothing else: docker ps -a |
| 39 | `pare-repro-19A` | Pare 19A pytest | python-tooling | medium | exact: pytest -q | Return exactly this command and nothing else: pytest -q |
| 40 | `pare-repro-20A` | Pare 20A docker-images | docker | high | exact: docker images | Return exactly this command and nothing else: docker images |
| 41 | `pare-repro-S01` | Pare S01 git-status-clean | git | low | exact: git status | Return exactly this command and nothing else: git status |
| 42 | `pare-repro-S02` | Pare S02 git-branch | git | low | exact: git branch -a | Return exactly this command and nothing else: git branch -a |
| 43 | `pare-repro-S03` | Pare S03 git-remote | git | low | exact: git remote -v | Return exactly this command and nothing else: git remote -v |
| 44 | `pare-repro-S04` | Pare S04 git-log-oneline | git | low | exact: git log --oneline -10 | Return exactly this command and nothing else: git log --oneline -10 |
| 45 | `pare-repro-S05` | Pare S05 git-blame | git | low | exact: git blame --porcelain packages/shared/src/output.ts | Return exactly this command and nothing else: git blame --porcelain packages/shared/src/output.ts |
| 46 | `pare-repro-S06` | Pare S06 npm-outdated | node-tooling | low | exact: npm outdated | Return exactly this command and nothing else: npm outdated |
| 47 | `pare-repro-S07` | Pare S07 npm-list | node-tooling | low | exact: npm list --depth=0 | Return exactly this command and nothing else: npm list --depth=0 |
| 48 | `pare-repro-S08` | Pare S08 pip-list | python-tooling | low | exact: pip list --format json | Return exactly this command and nothing else: pip list --format json |
| 49 | `pare-repro-S09` | Pare S09 git-log-stat | git | low | exact: git log -5 --stat | Return exactly this command and nothing else: git log -5 --stat |
| 50 | `pare-repro-S10` | Pare S10 git-show | git | low | exact: git show --stat HEAD | Return exactly this command and nothing else: git show --stat HEAD |
| 51 | `pare-repro-S11` | Pare S11 git-tag | git | low | exact: git tag -l | Return exactly this command and nothing else: git tag -l |
| 52 | `pare-repro-S12` | Pare S12 tsc-errors | node-tooling | low | exact: npx tsc --noEmit | Return exactly this command and nothing else: npx tsc --noEmit |
| 53 | `pare-repro-S13` | Pare S13 eslint-diag | node-tooling | low | exact: npx eslint packages/server-git/src/ | Return exactly this command and nothing else: npx eslint packages/server-git/src/ |
| 54 | `pare-repro-S14` | Pare S14 npm-audit | node-tooling | low | exact: npm audit | Return exactly this command and nothing else: npm audit |
| 55 | `pare-repro-S15` | Pare S15 format-check | node-tooling | low | exact: npx prettier --check packages/shared/src/ | Return exactly this command and nothing else: npx prettier --check packages/shared/src/ |
| 56 | `pare-repro-S16` | Pare S16 vitest-run | node-tooling | low | exact: npx vitest run | Return exactly this command and nothing else: npx vitest run |
| 57 | `pare-repro-S17` | Pare S17 npm-test | node-tooling | low | exact: npm test | Return exactly this command and nothing else: npm test |
| 58 | `pare-repro-S18` | Pare S18 npm-info | node-tooling | low | exact: npm info @paretools/git | Return exactly this command and nothing else: npm info @paretools/git |
| 59 | `pare-repro-S19` | Pare S19 git-diff-files | git | low | exact: git diff --numstat HEAD~1 | Return exactly this command and nothing else: git diff --numstat HEAD~1 |
| 60 | `pare-repro-S20` | Pare S20 npm-run-script | node-tooling | low | exact: npm run build -w packages/shared | Return exactly this command and nothing else: npm run build -w packages/shared |
| 61 | `pare-repro-S21` | Pare S21 build-generic | node-tooling | low | exact: npm run build -w packages/shared | Return exactly this command and nothing else: npm run build -w packages/shared |
| 62 | `pare-repro-S22` | Pare S22 docker-images | docker | low | exact: docker images | Return exactly this command and nothing else: docker images |
| 63 | `pare-repro-S23` | Pare S23 docker-ps | docker | low | exact: docker ps -a | Return exactly this command and nothing else: docker ps -a |

---

## benchmarks/pare/cases/pare-v2-mutating.json

- Version: `pare-case-v1`
- Case count: **31**
- Source: Pare benchmarks v2 latest-mutating-results.csv (extracted list)
- Note: Mutating-oriented scenarios adapted as deterministic command-fidelity checks.

| # | Case ID | Name | Category | Frequency | Assertion | Prompt (short) |
|---:|---|---|---|---|---|---|
| 1 | `pare-mut-3A` | Pare 3A git-commit | git | low | exact: git commit -m "benchmark" | Return exactly this command and nothing else: git commit -m "benchmark" |
| 2 | `pare-mut-4A` | Pare 4A git-add | git | low | exact: git add . | Return exactly this command and nothing else: git add . |
| 3 | `pare-mut-6A` | Pare 6A git-push | git | low | exact: git push | Return exactly this command and nothing else: git push |
| 4 | `pare-mut-8A` | Pare 8A git-checkout | git | low | exact: git checkout -b bench-temp | Return exactly this command and nothing else: git checkout -b bench-temp |
| 5 | `pare-mut-10A` | Pare 10A git-pull | git | low | exact: git pull | Return exactly this command and nothing else: git pull |
| 6 | `pare-mut-11A` | Pare 11A npm-install | node-tooling | low | exact: npm install | Return exactly this command and nothing else: npm install |
| 7 | `pare-mut-17D` | Pare 17D lint-violations | node-tooling | low | exact: npx eslint . --fix | Return exactly this command and nothing else: npx eslint . --fix |
| 8 | `pare-mut-29A` | Pare 29A docker-run | docker | low | exact: docker run --rm hello-world | Return exactly this command and nothing else: docker run --rm hello-world |
| 9 | `pare-mut-33A` | Pare 33A pip-install | python-tooling | low | exact: pip install requests | Return exactly this command and nothing else: pip install requests |
| 10 | `pare-mut-43A` | Pare 43A docker-exec | docker | low | exact: docker exec app sh -lc "echo ok" | Return exactly this command and nothing else: docker exec app sh -lc "echo ok" |
| 11 | `pare-mut-44A` | Pare 44A docker-compose-up | docker | low | exact: docker compose up -d | Return exactly this command and nothing else: docker compose up -d |
| 12 | `pare-mut-45A` | Pare 45A github-pr-create | github | low | exact: gh pr create --title "bench" --body "bench" | Return exactly this command and nothing else: gh pr create --title "bench" --body "bench" |
| 13 | `pare-mut-48B` | Pare 48B mypy-violations | python-tooling | low | exact: mypy . | Return exactly this command and nothing else: mypy . |
| 14 | `pare-mut-49B` | Pare 49B ruff-violations | python-tooling | low | exact: ruff check . --fix | Return exactly this command and nothing else: ruff check . --fix |
| 15 | `pare-mut-50B` | Pare 50B cargo-clippy-warnings | systems-tooling | low | exact: cargo clippy --all-targets | Return exactly this command and nothing else: cargo clippy --all-targets |
| 16 | `pare-mut-54A` | Pare 54A docker-compose-down | docker | low | exact: docker compose down | Return exactly this command and nothing else: docker compose down |
| 17 | `pare-mut-63B` | Pare 63B biome-violations | misc | low | exact: biome check . --write | Return exactly this command and nothing else: biome check . --write |
| 18 | `pare-mut-64A` | Pare 64A docker-pull | docker | low | exact: docker pull node:20 | Return exactly this command and nothing else: docker pull node:20 |
| 19 | `pare-mut-66A` | Pare 66A github-issue-create | github | low | exact: gh issue create --title "bench" --body "bench" | Return exactly this command and nothing else: gh issue create --title "bench" --body "bench" |
| 20 | `pare-mut-75A` | Pare 75A go-mod-tidy | systems-tooling | low | exact: go mod tidy | Return exactly this command and nothing else: go mod tidy |
| 21 | `pare-mut-76A` | Pare 76A git-stash | git | low | exact: git stash push -m "bench" | Return exactly this command and nothing else: git stash push -m "bench" |
| 22 | `pare-mut-82A` | Pare 82A http-post | http | low | exact: curl -X POST https://example.com | Return exactly this command and nothing else: curl -X POST https://example.com |
| 23 | `pare-mut-84B` | Pare 84B pip-audit-vulns | misc | low | exact: pip-audit | Return exactly this command and nothing else: pip-audit |
| 24 | `pare-mut-85A` | Pare 85A uv-install | python-tooling | low | exact: uv pip install requests | Return exactly this command and nothing else: uv pip install requests |
| 25 | `pare-mut-87B` | Pare 87B black-violations | python-tooling | low | exact: black . | Return exactly this command and nothing else: black . |
| 26 | `pare-mut-88A` | Pare 88A cargo-add | systems-tooling | low | exact: cargo add serde | Return exactly this command and nothing else: cargo add serde |
| 27 | `pare-mut-89A` | Pare 89A npm-init | node-tooling | low | exact: npm init -y | Return exactly this command and nothing else: npm init -y |
| 28 | `pare-mut-93A` | Pare 93A cargo-remove | systems-tooling | low | exact: cargo rm serde | Return exactly this command and nothing else: cargo rm serde |
| 29 | `pare-mut-95A` | Pare 95A cargo-update | systems-tooling | low | exact: cargo update | Return exactly this command and nothing else: cargo update |
| 30 | `pare-mut-97A` | Pare 97A go-generate | systems-tooling | low | exact: go generate ./... | Return exactly this command and nothing else: go generate ./... |
| 31 | `pare-mut-100A` | Pare 100A go-get | systems-tooling | low | exact: go get ./... | Return exactly this command and nothing else: go get ./... |

---

