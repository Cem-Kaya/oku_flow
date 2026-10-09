# OkuFlow — project rules for Claude

## No time estimates in task definitions

When writing plans, task definitions, or work items for coding agents
(`improvement_ideas/` docs, handoff reports, TODO items), NEVER include
effort or schedule estimates: no hours, days, weeks, sprints, team-size
guesses, or "this will take X" phrasing. Agents don't work on human
timescales and the numbers are meaningless noise.

Media/runtime timing details are fine — those are technical data, not
estimates (e.g. video durations, fragment cadence "~2s", timeouts,
watchdog intervals, frame budgets).

## Naming

The product is **OkuFlow**, formerly OpenZoom. Code identifiers use
`okuflow` / `oku_flow` / `OKUFLOW_`. The one rename record that intentionally
keeps the old name is `improvement_ideas/17-project-rename-plan.md`, along
with the `CHANGELOG.md` rename entry. See `agents.md` for details.
