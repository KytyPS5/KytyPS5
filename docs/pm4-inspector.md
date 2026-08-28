# PM4 submission inspector

Enable **Command Buffer Dump** in the launcher, or pass:

```text
--command-buffer-dump true --command-buffer-dump-folder _Buffers
```

Kyty continues to create its existing text dumps and additionally writes one
`*.json` file per submitted graphics DCB or asynchronous-compute ACB.

Each JSON trace contains:

- The queue, frame, source address, and copied root packet stream.
- Readable nested indirect command buffers captured at submission time.
- Readable indirect context, shader, and uconfig register pairs.
- Register-state deltas and a persistent tracked-state hash per queue.
- Packet classifications: `Known`, `Partial`, `Inferred`, `Unknown`, `Unsupported`, or
  `UnreadableAtCaptureTime`.

`Known` describes the inspector's structural understanding. It does not claim that every
runtime behavior or register semantic is implemented. Resource-registration metadata and
general descriptor memory are not captured yet; the JSON records those limits explicitly.

The JSON uses `dcb` for graphics submissions and `acb` for asynchronous-compute submissions.
The older internal `constant_commands` stream is intentionally not relabeled or inspected.
