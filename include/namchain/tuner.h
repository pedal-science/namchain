#pragma once

// nc.tuner: squatch-dsp's tuner as an engine node.
//
// The core is vendored in third_party/squatch-tuner (MIT, 2026 Squatch Stack) and compiled into
// src/nodes/tuner.cpp; this header exists only so the node type can be registered, and so nothing
// that includes namchain has to see the vendored headers. docs/tuner.md is the design: what the
// node is, the reading it publishes through `rig.state`, why it is not a palette block, and the
// ephemeral-node contract a client follows to mount one while its tuner is on screen.
//
// The node is a node like any other for audio processing -- one input, one output, compiled by the
// same compiler as every other -- with two differences that matter:
//
//  * **Its output is silence.** A pedal tuner takes the rig out of the signal path, and this one
//    does it by emitting zeros, so nothing downstream of it can be heard. The input is still read,
//    which is the whole point: the player tunes with the rig muted.
//  * **It is not a block.** A client mounts one while its tuner is visible and removes it after
//    (docs/tuner.md, "The ephemeral node"). `engine.schema` says so with `flags.in_palette: false`;
//    a document may still name the type, which is how a client mounts it.

namespace namchain
{
namespace render
{
struct NodeType;
/// The node type, registered in `src/nodes/builtin.cpp`.
extern const NodeType kTuner;
} // namespace render
} // namespace namchain
