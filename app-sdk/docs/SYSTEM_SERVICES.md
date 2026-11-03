# API 2.5 system services

The 228-byte RV32 descriptor and all previous imports remain unchanged. New
calls are grouped under `API 2.5: system services` in the public header,
allowlist and firmware resolver. `LAP_TARGET_API_24` still builds for 2.4.
All structures and buffers are copied synchronously, including callback-stack
inputs. Storage workers hold paths and staging buffers, never app pointers.

## Notifications and launch data

`lap_notify_post` copies a version-1 `lap_notification_t`: title, body, up to
two action labels and 128 opaque bytes. It returns a positive notice ID.
Eight notices are retained for this boot, with at most one post per second
per app. `lap_notify_dismiss` removes a notice owned by the calling app.

The firmware banner displays the source app identity, title/body and action
buttons. A body tap has action -1; buttons have action 0 or 1. The broker
retains the identity and bytes across unload. It opens the normal launcher,
including existing signature and capability review, or focuses the already
loaded app. `lap_launch_data` consumes the activation in enter, draw or a
service tick. Launch data expires after 60 seconds; callers can retry while
an existing permission review completes. No callback points into unloaded code.
The notice store and IPC store are RAM, not persistent scheduled alarms.

`reminder` schedules a background notice after one second. OPEN reports the
payload; SNOOZE schedules five seconds later. Closing its screen retains its
service. Opening another native app still stops that service, but the posted
notice can reopen the reminder.

## Scoped streaming and picker

Query `lap_caps` and `LAP_HW_SD` before opening storage. Scope 0 refers to
`/sdcard/apps_data/<app-id>/files/`, deliberately separate from cached flat
`lap_file_*` data and save/settings names. Nested relative paths are supported.
Names exclude hidden/dot components, control characters, colon and backslash.
Relative names are limited to 127 bytes and resolved paths to 255 bytes.
The worker rejects symlinks and Windows reparse points as well as non-files.

`lap_stream_open(scope,path,flags)` reserves one of four logical streams.
READ is mandatory; READ|WRITE allows overwrite and creation. Opening does
not touch the card; filesystem errors arrive on completion of a read or write.
Writes preserve the tail of an existing file. Seek selects an absolute offset,
up to INT32_MAX. Each read/write accepts 1..4096 bytes and returns a job ID.
There is one outstanding storage operation per instance. Read and write
positions advance when a successful completion is consumed. A stream with
pending work cannot be sought or closed. Write completion follows flush,
fsync and close, with an I/O error if any fail. FAT power-loss recovery still
requires hardware validation; this is not a transactional replace operation.

`lap_storage_poll(job,out,capacity,&result,&next)` returns 0 pending or 1
complete. `result` is bytes/rows or a negative error. Only poll copies read
data/rows into the supplied destination. A short destination returns EINVAL
and preserves the completion for retry. Write results need no output buffer.
Every completed job must be polled, including cancellations and errors.
`lap_storage_cancel` skips queued work; running writes can still finish and
are not rolled back. Unload cancels/orphans work; workers finish safely without
touching freed app state, and stale handles fail in the next generation.

`lap_dir_list(scope,path,cursor)` returns a job with at most sixteen
`lap_dir_entry_t` rows. Start at cursor 0; use poll's `next` until it is zero.
Directory enumeration order is the filesystem order. Mutating a directory
can invalidate its cursors. Long basenames that cannot fit are omitted.

`lap_file_picker` requests a file or folder, with READ or READ|WRITE access.
The firmware's existing LakeShark picker shows the source app identity and
requested mode. UP and NEXT PAGE browse worker-loaded pages; USE THIS FOLDER
grants the displayed folder. Picking a file grants only that exact file.
ESC/CANCEL and closing the app screen cancel. Background services cannot
open a picker. No file access is granted until the user selects it.

Shared roots eligible for explicit selection are `recordings`, `captures`,
`documents`, `downloads`, `music`, `tracks`, `exports` and `shared` under the
card. Installed apps, other app data, grants/keys and firmware directories
are excluded. The picker does not expose arbitrary card-wide permission.
`lap_file_picker_poll` consumes its completion and returns the positive scope
handle. Use an empty path for a selected file or the selected folder itself;
descendant relative paths are accepted only for folder grants. Grants last
until `lap_file_scope_close` or unload. A scope with open streams is busy.
The old SD API retains its old compatibility policy.

`scoped_files` demonstrates local pagination, selected file reads, folder
navigation, read/write grants, stream seek/write and cancellation. P selects
a file; F selects a folder; W changes the requested mode; H returns to the
app's own storage; S writes its own note; C cancels a job.

## Typed topic messaging and instance ownership

`lap_ipc_subscribe`, `lap_ipc_unsubscribe`, `lap_ipc_publish` and `lap_ipc_next`
implement boot-local named-topic mailboxes. Limits: eight app identities,
four topics each, eight queued messages each, 192 payload bytes, eight
publishes/second per identity. Topic names are 1..31 ASCII letters/digits or
`._/-`. Type is an application-defined 32-bit schema ID. Firmware fills sender
identity. Publish returns delivered mailbox count. Queue-full drops newest;
the next message reports the accumulated drop count. Publishing with no
subscriber returns zero. Duplicate subscriptions are idempotent; unsubscribe
also clears that topic's queued data. Topics are public and provide no secrecy.

Mailboxes and subscriptions intentionally survive native-app unload until
unsubscribe or reboot. They contain no ELF addresses. Run TOPIC READER once,
switch to TOPIC WRITER, press Enter, then reopen TOPIC READER. Services can
use the same APIs from their normal tick. No message automatically launches
or starts another app, and no app callback runs on a publisher's worker task.

The core runtime now has an explicit `ls_lap_instance_t` holding allocation
ownership/accounting, image/generation, callbacks, log queue, cached files,
input state, service callbacks/guard/budget/stop state and system extension
state (streams, scopes, picker and desktop queue). A lifecycle harness switches two quiescent contexts and proves that
allocations, held keys, streams and launch data do not bleed between them.
Worker requests carry copied owner identity/generation and use staging data.

API 2.6 extends these contexts through the loader and peripheral workers,
with multiple residents and individual Settings stop controls. See
[services and worker DSP](SERVICES26.md) for ownership, limits, cleanup,
compatibility and headless multi-instance scenarios.

## Desktop lifecycle

`lap_desktop_info` returns a snapshot. `lap_desktop_next` drains a sixteen-event
queue, including aggregate viewer attach/detach, client resize, firmware font
cell-size changes and input focus. Oldest events drop on overflow and the next
event reports loss. Polling also runs while an app service is hidden. Pixel
dimensions describe the app client, not an unrestricted desktop window.

The current remote-terminal protocol does not identify the Desktop product,
report PC DPI, or offer URL-open / save-dialog transactions. Accordingly,
`scale_milli` is zero (unknown host DPI); cell dimensions report firmware
render scale. `lap_desktop_open_url` and `lap_desktop_save_file` validate inputs
and return EUNSUPPORTED. A connected viewer alone never promises these features.
Adding these capabilities needs a separately negotiated viewer protocol.

## Headless simulator and board checks

The same broker, scoped worker, pagination and firmware picker run in lssim.
`LAPSIM_SCRIPT` commands include `desktop=0|1`, `focus=0|1`, `resize=W,H`,
`pick=INDEX`, `pickcancel`, `notice=ID,ACTION` and `noticeaction=ACTION` (latest
notice of this app). Picker rows 0..3 are CANCEL, UP, USE THIS FOLDER, NEXT/FIRST
PAGE; data starts at row 4. Put public fixtures under `sdcard/shared/` inside
the scenario working directory. `caps=none` simulates removal. Notice activation
in the shim feeds the broker; the C lifecycle harness unloads/reopens identities.
The simulator does not reproduce the signed firmware launcher or multiple
simultaneous native apps.

Board verification: tap targets on small/large screens, permission reviews on
notice relaunch, service hide/reopen, SD removal/full/durability and latency,
picker pagination through large real directories, viewer attach/timeout/focus,
font changes and own-client geometry. No firmware image build or flashing is
part of this worktree's validation.
