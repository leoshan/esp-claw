# phone_agent prompt layers

`scripts/phone_agent.lua` builds each step's prompt from layers, so a rule only shows up where it
applies:

| Layer | Where | When it is sent |
|---|---|---|
| L0 core | in `phone_agent.lua` (`ACTOR_PROMPT`, `CONFIRM_PROMPT`) | every step: format, coordinates, action list, feedback rules |
| L1 platform | `platform.md` | every step |
| L2 widgets | `widgets/<tag>.md` | when the model reported that tag in its `CONTEXT:` line (this or the previous step), or a matched playbook preloads it |
| L3 playbooks | `playbooks/<app>.md` | when a `match:` keyword appears in the goal or in the model's `APP:` line |

A playbook starts with header lines, then the rules:

```
match: clock, alarm, 闹钟        (lower-case keywords, comma separated)
contexts: picker, sheet          (widget rules to preload, optional)
<rules>
```

Files under the DATA root's `phone/prompts/` (same layout) override these, so a prompt can be tuned
on the board through `/api/files/upload` without reflashing. Widget tags are fixed in the script:
keyboard, picker, sheet, list, toggle.
