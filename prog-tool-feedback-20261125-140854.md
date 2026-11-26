# progress-tool / say feedback — 2026-11-25

Written after a long multi-agent session on this repository (five concurrent
subagents, several hours, heavy device and build work). Purpose: give the
maintainer concrete, reproducible complaints and defaults to change. Nothing
here is about the *data model*, which is good — it is about when and how often
the nudges interrupt.

**One-line verdict:** the progress tree is genuinely useful; the nudges are
about a third noise, and the worst of them is a false positive that fires on
nearly every turn.

---

## 1. "Turn ending with N open tasks" is the biggest offender

Observed, in order, across the session: 14, 16, 17, 18, 22, 24, 16, 22 open
tasks — each with the same "Close finished or continue" advice, and each one
non-actionable because **most entries belonged to child subagent sessions**.

Why it happens: the parent session creates one entry per workstream, subagents
create their own entries (nested or sibling), and the parent cannot close a
child's entries. So a healthy, busy multi-agent session permanently looks like
an abandoned todo list.

**Impact:** roughly 8-10 turns ended with a one-line status message whose only
content was "all of these are the agents' work". That is the nudge writing my
reply for me.

**Recommendation**

- Fire only for entries **owned by the current session**.
- Treat descendants as `waiting` ("blocked on children"), not open.
- Only `block` when an owned entry is stale (no update for >10 min) or in
  `error`.
- Show ownership counts instead of a raw total: "3 running, 12 owned by
  children, 1 stale".

## 2. The `say` nudge is unsatisfiable

It fires after 3 consecutive tool calls with no `say`, but the documented
budget is **3 notes per turn, 15 s apart**. On a 20-call turn the first note
cannot legally be followed by enough notes to avoid the nudge, so the rule
keeps firing after the budget is spent.

**Recommendation**

- At most 2 nudges per turn, and none once the note budget is exhausted.
- Never re-fire in the same turn after one has been acted on.
- If the budget is the real constraint, say so in the nudge text rather than
  asking for something impossible.

## 3. The long-command hint arrives after the fact and repeats

"Long command — next time wrap it: `progress-relay --id <id> -- …`" appeared
repeatedly for the *same* command shape (`export JAVA_HOME=… gradlew
assembleDebug`), and only after the command had already completed. At that
point the advice cannot be applied, and the next invocation is a different
round trip where it is not shown.

**Recommendation**

- Detect a repeating pattern (same command prefix 2+ times per session) and
  emit the relay suggestion once, with the exact command, at the *start* of the
  next occurrence.
- Suppress entirely for commands that have already finished.
- Make the hint dismissible per pattern, not per invocation.

## 4. Parent staleness while blocked on children

"No progress for 5m / 6m / 8m / 16m" fired while the session was legitimately
waiting on subagents. Waiting is not stalling, and inventing a progress write
to silence it adds noise to the tree.

**Recommendation**

- Auto-mark a parent `waiting` while any child session runs, and exempt it from
  the staleness window for the duration.
- Raise the default window for non-shell work to 10-15 min; 5 min is shorter
  than a single build plus device pass.

## 5. Reminders do not coalesce

It was common to receive two or three separate context blocks in one turn
(`say-nudge` + `progress-reminder` + long-command hint). Each costs attention
and each asks for a different corrective action.

**Recommendation:** merge everything queued into one block at turn end, ordered
by severity, and enforce a global minimum gap (60-90 s) between any two nudges
regardless of which rule produced them.

## 6. The off switch disappeared

`progress_nudge` (session-scoped rule configuration) was removed from the tool
catalog mid-session. That was the only way for an agent to quiet a rule that is
misfiring for its particular workflow. Without it there is no escape hatch, and
the agent cannot even ask for one.

**Recommendation:** keep a session-scoped mute/snooze available to agents, even
if the general rule editor stays user-only. A single `progress_nudge {action:
"off", id}` equivalent would have removed 80 % of the noise in this session.

---

## Proposed defaults (summary)

| rule | current behaviour | proposed |
| --- | --- | --- |
| open-task block | every turn end, all entries | only owned + stale (>10 min) or error |
| say nudge | after 3 calls, ignores budget | max 2/turn, budget-aware, no re-fire |
| long command | per invocation, after completion | once per command pattern, before execution |
| staleness | 5 min, includes blocked parents | 10 min, suppressed while children run |
| delivery | separate blocks | one merged block, 60-90 s global gap |
| mute | none available | session-scoped off/snooze for agents |

## What should not change

- The tree: `parentId` nesting, status rollup, progress averaging.
- `entries[]` bulk writes — one call per phase is the right shape.
- `progress_touch` for long single-step work.
- The relay concept and its zero-token telemetry; the relay itself never became
  noise, only the reminder to use it did.
- Severity levels (info/warn/block) — the mapping is right, the thresholds are
  not.

## Method / caveats

Counts above are approximate, from this session's own transcript: roughly 15+
turn-end blocks, 8+ `say` nudges, 6+ long-command hints, 10+ staleness warnings
over about six hours of wall-clock work with five agents. I did not instrument
the plugin; the numbers are what appeared in context.

---

## Addendum, same day: the fix works, one gap remains

The maintainer shipped ownership-aware blocks (`N stale/error own tasks …
(M child-owned, K blocked on children excluded)`) and a session `mute`/`unmute`.
Both landed while this session was running, and both are the right shape: the
old false positive is gone, and the mute removed the say/staleness/relay noise
immediately.

Two observations from using them under load (9 parallel subagents):

- **A `progress_touch` does not reset the staleness clock.** Two "stale own
  task" blocks fired 12 and 25 minutes apart on parents whose only activity was
  subagent work, despite a touch in between. If touch is not an activity signal,
  say so in the nudge; otherwise the only way to silence a legitimately blocked
  parent is to fake a status change.
- **A parent cannot be marked `waiting` while children run**, because rollup
  forces it back to `running`. So the "blocked on children" exclusion can never
  apply to the parent that most needs it. Suggestion: treat "all running
  children are in other sessions" as waiting regardless of rollup, or exclude
  staleness for a parent whose children all updated within the window.

Neither is a regression — they are what shows up once the coarse false positive
is gone. With those two lines the block would be almost silent in a healthy
multi-agent session.

