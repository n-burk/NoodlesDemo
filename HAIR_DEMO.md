# Hair grooming demo

The NoodlesDemo apps open on **Hair Groom**: a node-driven 3D hair grooming
tool. A procedural scalp, a seeded guide set, editable gizmos, and a styled
clump are rendered into the graph view's own OpenGL drawable, and the
transparent `noodles` graph is composited on top of them.

Every control is a real property row, evaluation follows the connections you
actually authored, and the tools are owned by the nodes whose switches arm them.

```text
Scalp ──┬──▶ Create Guides ──▶ Guide Sculpt ──▶ Clump ──▶ Generate Hair ──▶ Output
        └───────────────────────────────────────────────▶
```

## Layering

`NoodlesDemoCore` stays hair-agnostic, Apple-free, and OpenUSD-free. The groom
is an example consumer of it, exactly like the image-processing fixture:

| Piece | Where | Depends on |
| --- | --- | --- |
| Graph DTOs, editor, interaction | `NoodlesDemo::Core` | `noodles::noodles` |
| Groom systems (`noodles::demo::hair`) | `Examples/Hair`, target `NoodlesDemoHair` | Core + the C++17 standard library |
| GL viewport renderer | `Examples/Hair/HairGLRenderer.*` | the above + desktop GL 3.3 core / GL ES 3.0 |
| Event translation | `Examples/macOS`, `Examples/iPad` | AppKit / UIKit |

`NoodlesDemoHair` contains no GL, Apple, or OpenUSD types, so the CPU test
binary links exactly the code the apps run.

The groom draws into the graph view's own drawable, which requires
`editor->setOverlayBlendsWithBackground(true)`. That switch is generic Core API,
not hair-specific; see DESIGN.md's *Compositing* section.

## The graph

Six nodes, all connected, none carrying a stored position — so the editor's
deterministic layered auto-layout places the chain, exactly as it does for the
image graphs.

| Node | Schema | Inputs | Output |
| --- | --- | --- | --- |
| `/Hair/Scalp` | `HairScalp` | — | `mesh` (`mesh`) |
| `/Hair/CreateGuides` | `HairCreateGuides` | `scalp` | `guides` (`guides`) |
| `/Hair/GuideSculpt` | `HairGuideSculpt` | `guides` | `sculpted` (`guides`) |
| `/Hair/Clump` | `HairClump` | `curves`, `clumps` (optional) | `clumped` (`curves`) |
| `/Hair/GenerateHair` | `HairGenerate` | `scalp`, `guides` | `strands` (`hair`) |
| `/Hair/Output` | `HairOutput` | `strands` | — |

Property rows use the six documented group prefixes, which `noodles` renders as
independently foldable headers:

- `tool:` — `draw`, `editPoints` (Create Guides), `comb` (Guide Sculpt),
  `editClump`, `paintClump` (Clump)
- `draw:` — `snapRoot`, `spacing`, `smoothing`, `points`, `length`
- `brush:` — `radius`, `strength`, `falloff`, `smooth`
- `clump:` — `strength`, `tipBias`, `radius`, `noise`, `regions`,
  `regionSeed`, `paintRadius`, `paintAmount`, `paintErase`, `centerX/Y/Z`
- `hair:` — `density`, `segments`, `width`, `spread`, `variation`, `seed`
- `display:` — `visible`, `wireframe`, `gizmos`, `brush`, `gizmo`, `clumpMap`,
  `hair`, `guides`, `grid`, `tint`, `shine`, `ambient`

A group header's pin name is the bare prefix, so no port may be named after a
group. That is why Generate Hair's output is `strands` rather than `hair`: a
port called `hair` would be shadowed by the `hair:` group header and would
toggle the fold instead of authoring a link. `tests/Hair/HairGroomTests.cpp`
asserts no node ever reintroduces such a collision.

Boolean rows are authored scrubable, because `GraphEditor` maps a no-move row
tap to an atomic toggle only for scrubable rows; a non-scrubable value row
becomes a host activation instead.

## Typed data lives outside `GraphProperty`

The graph carries topology plus scalar and Boolean controls. Geometry does not:

```cpp
struct HairStores {
  std::map<std::string, GuideSet>     guides;      // per Create Guides node
  std::map<std::string, SculptDeltas> sculpt;      // per Guide Sculpt node
  std::map<std::string, ClumpPaint>   clumpPaint;  // per Clump node
};
```

All three are keyed by node id and owned by `HairScene`. The clump target is
the one piece of 3D data that *is* on the graph — as three scalars,
`clump:centerX/Y/Z` — because that is what makes the gizmo genuinely
node-driven: dragging it authors those rows, and the next evaluation reads them
straight back.

## Evaluation

`EvaluateHairGraph` starts at the node whose schema is `HairOutput` and walks
backwards through the edges that exist, dispatching on `schemaTypeName` rather
than on node ids. Rewiring the graph rewires the groom; a second `Clump` added
from the palette and chained in becomes the clump stage.

Each stage is independently valid. Losing Generate Hair's scalp input empties
the hair without pretending the guides vanished, and `status` names the first
break found. A missing input never yields the previous frame's geometry — the
result is empty and invalid, by construction.

`GraphEditor` deliberately does not clamp a scrub, so every range the renderer
depends on is enforced once, on read: density is clamped to 500–1500 strands
and segments to 12–24, and the clamped values are reported back on the result
so tests and the renderer read the same numbers generation used.

### Dirty generations

`HairScene` caches the evaluated result behind a key built from the graph's
topology hash, its parameter hash, and the three store versions. **Camera state
is deliberately not part of that key**, so orbiting, trucking, dollying, and
resizing never regenerate hair. `HairGLRenderer` re-uploads strand buffers only
when the evaluation generation moves; ribbons are turned to face the viewer in
the vertex shader, so camera motion touches no vertex buffer at all.

## Generation

Deterministic on every platform. The demo carries its own hash-based generator
because `std::uniform_real_distribution` is not portable even though
`std::mt19937` is.

1. Guides are arc-length resampled to the strand resolution.
2. Roots are distributed over the dome by sampling uniformly in normalized
   height — equal height bands carry equal area on a sphere — with a
   golden-angle azimuth and a sub-cell seeded dither, so raising the density
   refines the same distribution instead of reshuffling it.
3. Each root blends the three nearest guides' shapes by inverse-square distance.
   The shapes are blended as **world-space offsets**. Re-expressing them in each
   target root's surface frame would make every strand sweep away from its own
   root in the same local direction, which on a dome reads as a radial spike
   ball rather than as hair with a direction; the one artifact that costs — a
   strand dipping under a convex surface — is corrected by pushing points back
   out to the scalp.
4. Seeded per-strand length, width, shading, and a drift weighted by `t²` keep
   the variation off the root and let it accumulate toward the tip.
5. `strand.points[0]` is assigned the scalp point outright, so root attachment
   is exact rather than approximate.

## Clumping

Clumping is XGen-shaped rather than a single global attractor.

**Regions.** `clump:regions` Voronoi sites are distributed over the scalp by
the same area-uniform, golden-angle scheme the hair roots use, so regions and
roots stay well matched at any count. A curve belongs to the region whose site
is nearest **in world space**, which makes the partition a true Voronoi diagram
on the surface rather than one distorted by the UV parameterization.

**Convergence.** Each region's clump center is, by default, the mean curve of
its own members. Curves are pulled toward that center at the same normalized
position along their length, with influence rising as `t^tipBias`. Roots never
move. Deriving the center from the members is what makes a clump read as hair
gathering rather than as everything sliding toward one point.

**Explicit clump curves.** The Clump node's second input, `clumps`, takes a
curve set that *defines* the centers instead. Each region adopts the supplied
curve nearest its own site, so a whole cell converges onto one curve rather
than onto its own average — the usual workflow being a second, sparser Create
Guides whose only job is to shape the clumps. Matching per region rather than
per curve is what keeps a cell together. The input is optional: unconnected
falls back to the mean of members, and a connected-but-broken upstream is
reported by name rather than silently ignored. Supplied curves are drawn in the
viewport in the clump colour so it is obvious what the groom is gathering onto.

**Painted weight.** A `ClumpPaint` texture in scalp UV space scales the
influence per root, so `weight = strength × t^tipBias × painted(rootUV)`. It
starts fully painted — a Clump node that did nothing until you painted would
look broken — and the **Paint Clump** tool erases or restores it. The brush is
round in *world* space, not UV space, because UV space is badly distorted near
the pole; painting builds up across a stroke instead of stamping a hard disc.

**The gizmo** still matters: it attracts each region's mean curve, falling off
with the region site's distance from it over `clump:radius`. Dragging it sweeps
whole clumps toward a point, which is a legible styling action rather than a
uniform translation.

`display:clumpMap` draws the map on the scalp: every region gets its own pastel
— hues spaced by the golden ratio, so consecutive region indices never land on
neighbouring hues — with cell boundaries inked dark and the painted weight
fading a region toward the background where it has been erased. It ships off so the launch view shows a natural scalp; arming Paint
Clump turns the row on for you (by writing the node's own row, not by
overriding it). Hiding the scalp hides the map with it.

### Guides or generated curves

The Clump node's port is typed `curves`, not `guides`, because it accepts
either. `EvaluateGuides` and `EvaluateHair` both know how to pass a Clump, so
the same modifier can sit

- **before Generate Hair**, clumping the guide curves — the shipped wiring; or
- **after Generate Hair**, clumping the generated curves directly, by rewiring
  `Generate Hair.strands → Clump.curves → Output.strands`.

Both paths run the same `ApplyClumpToCurves`; guides and strands each project
onto a `ClumpCurves` view of point lists plus root UVs and back. `HairEvalResult::clumpsGeneratedCurves`
reports which position the evaluated Clump was in.

## Tools

Exactly one mutating tool may be armed. The `tool:*` switches are the single
source of truth: the control-bar picker only writes them, and it follows them
when you toggle a row in the graph instead. Turning one on turns the others
off, across every node in the graph — including ones added at runtime, because
the switch list is derived from the snapshot rather than from a fixed list of
ids.

A tool only operates on a node that is genuinely in the evaluated chain.
Arming Edit Clump on a Clump you have unplugged declines the gesture and names
both the armed node and the one that actually feeds the Output, rather than
silently editing geometry nobody can see.

### Which node a tool acts on

Every store — guides, comb deltas, clump paint — is keyed by node id, so
duplicated nodes never share state. Which node a tool targets is decided by the
chain that reaches the Output:

- **Chained Clumps.** The most downstream Clump is the stage: it is the one
  whose gizmo is drawn and whose rows the tools write. Arming Edit Clump or
  Paint Clump from the control panel picks it rather than the first Clump in
  graph order.
- **Side inputs never become stages.** A Clump's `clumps` input is a side
  branch. Nodes reached through it — typically a second Create Guides shaping
  the clump centers — are evaluated normally but are *not* recorded as the
  groom's Create Guides, Guide Sculpt, or Scalp. Without that rule the side
  branch, which evaluates last, would overwrite the recorded stage and the
  guide tools would silently start editing the clump curves instead of the
  groom.
- **Arming a tool on an off-chain node** is allowed — the switch is the
  document's to set — but the gesture is declined with a message naming the
  node that does feed the Output.

| Tool | Node | Behavior |
| --- | --- | --- |
| **Draw Guides** | Create Guides | Starts on a scalp ray hit, snaps the root, draws on a camera-facing plane frozen at pointer-down, previews live, smooths and resamples on release, clamps to `draw:length`. Missing the scalp declines the gesture. |
| **Edit Points** | Create Guides | Ray-picks a guide point in screen space and drags it in the camera plane, or along X/Y/Z by grabbing a handle tip. |
| **Comb Brush** | Guide Sculpt | Pushes and smooths non-root points within `brush:radius`, weighted by `brush:strength` and `brush:falloff`. Writes displacements, not positions. |
| **Edit Clump** | Clump | Drags the clump gizmo in the camera plane or along an axis, authoring `clump:centerX/Y/Z`. |
| **Paint Clump** | Clump | Ray-casts to the scalp and paints the clump-weight map with a world-space round brush; `clump:paintErase` flips it between restoring and erasing. Missing the scalp declines the gesture. |

The tools live in a **side panel on the leading edge of the viewport**, shown
only while the groom is up: they act on the 3D scene, not on the graph, so they
do not belong in the graph control bar. The panel is another way to write the
nodes' `tool:*` rows and it re-reads its state from them after every press, so
a tool the scene declines or reassigns to a different node is reflected rather
than assumed. On iPadOS the Graph Pan toggle sits in the same panel, being a
viewport interaction mode too.

While a guide-editing tool is armed, the viewport draws the **Create Guides
output** — the set whose indices map one-to-one onto that node's store — rather
than the post-clump guides, so what you pick is what you drag.

Edit Points solves rather than assigns. Smoothing and root snapping sit between
the store and what you see, so writing the target straight into the store would
leave the point trailing the cursor by the smoothing amount. Those stages are
linear and contractive, so a few correction passes converge and dragging stays
exact. Dragging a root is the exception: it cannot leave the scalp, so the
target is written directly and the snap stage slides the whole curve along the
surface.

Comb deltas are stored per point and re-conformed by normalized index when the
upstream point count changes, so changing `draw:points` after sculpting
re-applies the sculpt instead of discarding it.

## Input routing

Ownership is decided at pointer-down and never revised mid-gesture.

1. If the press hits graph content (`GraphEditor::hitsGraphElementAt`), the
   editor takes it — nodes, ports, links, relationship arrows, and the minimap.
2. Otherwise the host is offered the gesture. The active tool claims it if it
   can.
3. If no tool claims it, the camera does.

The graph views gained matching, hair-agnostic hooks for this
(`onBackgroundPointerDown/Move/Up`, `onBackgroundHover`, `onBackgroundZoom*`,
`onRenderBackground`, `onTeardownBackgroundGL`, `graphPanLock`), declared
identically on AppKit and UIKit so the decision logic stays in shared C++.

### macOS

| Input | Effect |
| --- | --- |
| Drag on graph content | Graph edit |
| Drag on background | Active tool, else orbit |
| Shift-drag on background | Camera truck |
| **Space-drag** | Graph pan, over background too |
| Trackpad scroll | Graph pan |
| Magnify on background | Camera dolly |
| Magnify on graph, or with Space | Graph zoom |
| Right-click | Remove node from canvas |
| Pointer motion | Hover feedback |

### iPadOS

| Input | Effect |
| --- | --- |
| Pencil or finger on graph content | Graph edit |
| Pencil or finger on background | Active tool, else orbit |
| **Graph Pan toggle** | One-finger background drag pans the graph — the touch equivalent of space-drag |
| Pinch starting on background | Camera dolly |
| Pinch starting on graph, or with Graph Pan on | Graph zoom |
| Long press | Remove node from canvas |
| Pencil hover | Hover feedback |

Pencil and finger route through the same decision point, so tool behavior is
identical for either. A background-owned Pencil stroke belongs to the tool for
its whole lifetime and never also removes a node.

## Rendering

The groom is drawn into the graph view's drawable immediately before
`GraphEditor::renderFrame()` composites the graph over it. Three programs, all
building against both desktop GL 3.3 core and GL ES 3.0:

- **scalp** — lit triangles with a small rim term
- **hair** — camera-facing triangle ribbons, expanded in the vertex shader and
  shaded with Kajiya-Kay (a strand has no surface normal, so shading comes from
  the tangent). 500–1500 strands, 12–24 segments, tapered toward the tip.
- **overlay** — grid, guide curves, guide and root points, X/Y/Z handles, the
  brush ring, the clump gizmo and its influence radius, the live stroke
  preview, and hover highlights.

Solid geometry is rebuilt only when the evaluation generation changes; overlay
geometry is small and rebuilt per frame. The renderer sets the state it needs
and restores what `GraphEditor` does not touch (cull face, program point size),
and releases its GL objects through `onTeardownBackgroundGL` while the context
is still current.

## Adding nodes

The Add Node control offers the `/Hair` palette while the groom is up and image
ops otherwise. Palette nodes are built by the same `MakeHairNode` the shipping
fixture is assembled from, so a node added at runtime is indistinguishable from
one that shipped — same rows, same groups, same ports. A newly added Create
Guides starts with no guides and says so; draw some.

## Tests

| Suite | Covers |
| --- | --- |
| `noodles_demo_hair` | Scalp mesh and ray hits, resampling and smoothing, seeded root attachment, deterministic generation, clamping, Voronoi regions and convergence, painted-weight gating, the clump map image, comb behavior, camera rays and axis constraints, graph shape and group-collision rules, topology-driven evaluation including the `clumps` side input, Clump on guides *and* on generated curves, supplied clump curves, chained Clumps resolving to the downstream stage, side branches never becoming stages, dirty generations, tool exclusivity, gesture ownership, all five tools end to end, gizmo visibility, ribbon geometry, overlay contents, the node palette |
| `noodles_demo_hair_render` | Real offscreen GL: the groom rasterizes, display switches remove geometry, the graph composite preserves the groom when blending and erases it when not, the graph draws over the groom, orbiting re-uploads nothing, and the clump map paints pastel regions on the scalp |

## Limitations

- One scalp shape: an axis-aligned ellipsoid dome. Ray picking and root
  distribution are analytic against that ellipsoid, so an arbitrary mesh would
  need a real acceleration structure.
- No collision between hair and the scalp beyond the push-out correction, and
  none between strands.
- The clump-weight map is a single channel at 192×96 in scalp UV space, and it
  is not persisted or exportable. Region assignment is recomputed per
  evaluation rather than cached, which is cheap at demo scale but linear in
  regions × curves.
- No undo. The editor opens `beginEdit`/`endEdit` envelopes and the host could
  bracket store mutations the same way, but the demo does not implement it.
- The groom is not persisted; the typed stores live for the life of the process.
- `hair:density` scrubs quickly because `GraphEditor` derives its step from the
  value at gesture start, and 900 is a large value. The clamp keeps it in range.
- Pencil hover needs hardware that reports it; there is no finger hover.
