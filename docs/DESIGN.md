# Visual / interaction design

The reference is the *behavioral grammar* of iOS/macOS dynamic surfaces, not a pixel copy of Apple UI.

## Shape

Collapsed:

- 230 × 40 DIP
- near-black fill (`#050505`)
- full-height pill radius, matching the compact Dynamic Island proportions
- subtle one-pixel highlight border
- restrained shadow, never a large glass card

Expanded:

- 408 × 328 DIP for activities; settings grows to 390 DIP
- 42 DIP continuous-feeling corners
- the same dark material; album art, timeline, controls, and metrics expand from their compact counterparts

## Motion

Motion priorities:

1. silhouette first;
2. content cross-fades after the shape commits to expanding;
3. small controls appear last;
4. collapse reverses this hierarchy.

The native host uses springs instead of fixed 200 ms easing curves. It should be slightly lively but not rubbery.

## Typography

- bundled `Inter Variable` for consistent Apple-like proportions, weights, and spacing
- `Segoe UI Variable Text` as a runtime fallback when the bundled font cannot be loaded
- `Segoe Fluent Icons` for Windows-native glyphs

SF Pro is not redistributed; the interface uses the open-source Inter variable font instead.

## Color

The base UI is almost monochrome. Color is activity state, not decoration. Metric rings intentionally use saturated accents similar to the reference image, while the shell itself stays black/gray.

## Interaction

Collapsed state should be glanceable without stealing focus. Click expands. Hover only gives a small width response by default; an optional setting allows delayed hover expansion.

Expanded state remains non-activating so media controls do not pull keyboard focus from an editor/game. Settings that require text entry should eventually open a normal activated settings window rather than turning the overlay into a focus trap.
