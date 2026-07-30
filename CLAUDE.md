# OpenZoom — project rules for Claude

## No time estimates in task definitions

When writing plans, task definitions, or work items for coding agents
(`improvement_ideas/` docs, handoff reports, TODO items), NEVER include
effort or schedule estimates: no hours, days, weeks, sprints, team-size
guesses, or "this will take X" phrasing. Agents don't work on human
timescales and the numbers are meaningless noise.

Media/runtime timing details are fine — those are technical data, not
estimates (e.g. video durations, fragment cadence "~2s", timeouts,
watchdog intervals, frame budgets).
