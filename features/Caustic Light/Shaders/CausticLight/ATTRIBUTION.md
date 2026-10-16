# Caustic Light attribution

Caustic Light is an integration of **CausticLight** by qtuna (Iwillbewarlock), GPL-3.0-only:
https://github.com/Iwillbewarlock/CausticLight

Taken from it:

- The six pattern atlases (`calm.dds`, `normal.dds`, `rough.dds` for pool floors, `refl_*.dds` for walls), unchanged.
- The optical model: the light's mirror image below the water, the glossy spread from the Cox-Munk slope, the
  pool floor focus, and the constants fitted against its Blender Cycles references.

Changed for Community Shaders: the pass runs inside the deferred pipeline. Lights come from Light Limit Fix's
clusters, the sun from the shared data with its shadow map at the water point, the surface normal and albedo from the
G-buffer. Water comes from the engine's water objects instead of Havok ray grids.

The atlas generator of the original started from **caustic-volume** by ScottieFox (MIT); its license is
`caustic-volume-LICENSE.txt` in this folder.
